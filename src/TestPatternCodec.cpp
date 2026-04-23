/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TestPatternCodec.h"

#include <kodi/Filesystem.h>
#include <kodi/General.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>

namespace
{
const char* VERTEX_SHADER_SOURCE =
    "#version 100\n"
    "attribute vec2 a_position;\n"
    "void main()\n"
    "{\n"
    "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

const float FULLSCREEN_QUAD[] = {
    -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
};
} // namespace

CTestPatternCodec::CTestPatternCodec(const kodi::addon::IInstanceInfo& instance)
  : CInstanceVideoCodec(instance)
{
}

CTestPatternCodec::~CTestPatternCodec()
{
  DestroyFBO();
  DestroyGLContext();
}

bool CTestPatternCodec::Open(const kodi::addon::VideoCodecInitdata& initData)
{
  m_width = initData.GetWidth();
  m_height = initData.GetHeight();

  kodi::Log(ADDON_LOG_INFO, "CTestPatternCodec::Open - %dx%d extraSize=%u",
            m_width, m_height, initData.GetExtraDataSize());

  // shader path is passed as extradata from the input stream
  const uint8_t* extra = initData.GetExtraData();
  unsigned int extraSize = initData.GetExtraDataSize();
  if (!extra || extraSize == 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: no shader path in extradata");
    return false;
  }

  // extradata: "shaderPath\0format\0bits\0transfer\0framecount"
  std::string extraStr(reinterpret_cast<const char*>(extra), extraSize);
  std::string shaderPath;
  size_t pos1 = extraStr.find('\0');
  if (pos1 != std::string::npos)
  {
    shaderPath = extraStr.substr(0, pos1);
    size_t pos2 = extraStr.find('\0', pos1 + 1);
    if (pos2 != std::string::npos)
    {
      m_format = extraStr.substr(pos1 + 1, pos2 - pos1 - 1);
      size_t pos3 = extraStr.find('\0', pos2 + 1);
      if (pos3 != std::string::npos)
      {
        m_bitDepth = std::atoi(extraStr.substr(pos2 + 1, pos3 - pos2 - 1).c_str());
        size_t pos4 = extraStr.find('\0', pos3 + 1);
        if (pos4 != std::string::npos)
        {
          m_transfer = extraStr.substr(pos3 + 1, pos4 - pos3 - 1);
          std::string frameCountStr = extraStr.substr(pos4 + 1);

          if (!frameCountStr.empty())
          {
            size_t dp = frameCountStr.find('-');
            if (dp != std::string::npos)
            {
              m_frameCountX = std::atoi(frameCountStr.substr(0, dp).c_str());
              m_frameCountY = std::atoi(frameCountStr.substr(dp + 1).c_str());
            }
          }
        }
      }
      else
      {
        m_bitDepth = std::atoi(extraStr.substr(pos2 + 1).c_str());
      }
      if (m_bitDepth < 8)
        m_bitDepth = 8;
    }
    else
    {
      m_format = extraStr.substr(pos1 + 1);
    }
  }
  else
  {
    shaderPath = extraStr;
  }
  // Normalize format string: extract bit depth if embedded in format name
  // Accepts: yuv420p, yuv420p10, yuv420p10le, yuv422p12, yuv444p10le, p010,
  // nv12, xrgb8888, xrgb2101010, xrgb16161616, xrgb16161616f
  struct { const char* name; const char* base; int bits; } formatAliases[] = {
    {"yuv420p10le", "yuv420p", 10}, {"yuv420p10", "yuv420p", 10},
    {"yuv420p12le", "yuv420p", 12}, {"yuv420p12", "yuv420p", 12},
    {"yuv422p10le", "yuv422p", 10}, {"yuv422p10", "yuv422p", 10},
    {"yuv422p12le", "yuv422p", 12}, {"yuv422p12", "yuv422p", 12},
    {"yuv444p10le", "yuv444p", 10}, {"yuv444p10", "yuv444p", 10},
    {"yuv444p12le", "yuv444p", 12}, {"yuv444p12", "yuv444p", 12},
    {"p010",         "p010",          10},
    {"nv12",         "nv12",           8},
    {"xrgb8888",     "xrgb8888",       8},
    {"xrgb2101010",  "xrgb2101010",   10},
    {"xrgb16161616", "xrgb16161616",  12},
    {"xrgb16161616f","xrgb16161616f", 16},
  };
  for (const auto& alias : formatAliases)
  {
    if (m_format == alias.name)
    {
      m_format = alias.base;
      if (m_bitDepth < alias.bits)
        m_bitDepth = alias.bits;
      break;
    }
  }
  m_bytesPerSample = m_bitDepth > 8 ? 2 : 1;

  // RGB detection: packed XRGB formats use a single plane, 4 or 8 bytes/pixel.
  m_isRGB = (m_format == "xrgb8888" || m_format == "xrgb2101010" ||
             m_format == "xrgb16161616" || m_format == "xrgb16161616f");
  if (m_isRGB)
    m_rgbBytesPerPixel = (m_format == "xrgb16161616" || m_format == "xrgb16161616f") ? 8 : 4;

  // load shader source
  kodi::vfs::CFile file;
  if (!file.OpenFile(shaderPath))
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: failed to open shader: %s",
              shaderPath.c_str());
    return false;
  }

  std::string shaderSource;
  char buf[4096];
  ssize_t bytesRead;
  while ((bytesRead = file.Read(buf, sizeof(buf))) > 0)
    shaderSource.append(buf, bytesRead);
  file.Close();

  if (!InitGLContext())
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: failed to init EGL context");
    return false;
  }

  if (!CompileShader(shaderSource))
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: failed to compile shader: %s",
              shaderPath.c_str());
    DestroyGLContext();
    return false;
  }

  if (!CreateFBO())
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: failed to create FBO");
    DestroyGLContext();
    return false;
  }

  // compute plane layout once
  if (m_isRGB)
  {
    // Packed RGB: single plane, no subsampling. decodedDataSize is the
    // total packed pixel buffer size for the RGB zero-copy path.
    m_subsampleX = 1;
    m_subsampleY = 1;
    m_yStride = m_width * m_rgbBytesPerPixel;
    m_ySize = m_yStride * m_height;
    m_uvStride = 0;
    m_uvSize = 0;
    m_yuvFrameSize = m_ySize;

    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    m_frameNumber = 0;
    m_hasFrame = false;
    m_opened = true;

    kodi::Log(ADDON_LOG_INFO, "CTestPatternCodec: opened RGB (%dx%d, format=%s, shader=%s)",
              m_width, m_height, m_format.c_str(), shaderPath.c_str());
    return true;
  }

  if (m_format == "yuv444p")
  { m_subsampleX = 1; m_subsampleY = 1; }
  else if (m_format == "yuv422p")
  { m_subsampleX = 2; m_subsampleY = 1; }
  else
  { m_subsampleX = 2; m_subsampleY = 2; } // yuv420p, nv12

  m_yStride = m_width * m_bytesPerSample;
  m_ySize = m_yStride * m_height;
  if (m_format == "nv12" || m_format == "p010")
  {
    m_uvStride = m_width * m_bytesPerSample; // interleaved UV
    m_uvSize = m_uvStride * (m_height / m_subsampleY);
    m_yuvFrameSize = m_ySize + m_uvSize;
  }
  else
  {
    m_uvStride = (m_width / m_subsampleX) * m_bytesPerSample;
    m_uvSize = m_uvStride * (m_height / m_subsampleY);
    m_yuvFrameSize = m_ySize + m_uvSize * 2;
  }

  // allocate working buffer for RGBA readback (16-bit for >8-bit)
  m_readbackBuffer.resize(m_width * m_height * 4 * m_bytesPerSample);

  eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  m_frameNumber = 0;
  m_hasFrame = false;
  m_opened = true;

  kodi::Log(ADDON_LOG_INFO, "CTestPatternCodec: opened (%dx%d, shader=%s)",
            m_width, m_height, shaderPath.c_str());

  return true;
}

bool CTestPatternCodec::AddData(const DEMUX_PACKET& packet)
{
  if (!m_opened)
    return false;

  m_pts = static_cast<int64_t>(packet.pts);
  m_hasFrame = true;
  return true;
}

void CTestPatternCodec::Reset()
{
  m_frameNumber = 0;
  m_hasFrame = false;
}

void CTestPatternCodec::SetPictureHdrType(VIDEOCODEC_PICTURE& picture) const
{
  if (m_transfer == "pq" || m_transfer == "smpte2084")
    picture.hdrType = VIDEOCODEC_HDR_TYPE_HDR10;
  else if (m_transfer == "hlg" || m_transfer == "arib-std-b67")
    picture.hdrType = VIDEOCODEC_HDR_TYPE_HLG;
  else
    picture.hdrType = VIDEOCODEC_HDR_TYPE_NONE;
}

VIDEOCODEC_RETVAL CTestPatternCodec::GetPicture(VIDEOCODEC_PICTURE& picture)
{
  if (picture.flags & VIDEOCODEC_PICTURE_FLAG_DRAIN)
    return VC_EOF;

  if (!m_opened || !m_hasFrame)
    return VC_BUFFER;

  m_hasFrame = false;

  // Common picture fields
  picture.width = m_width;
  picture.height = m_height;
  picture.pts = m_pts;
  picture.flags = 0;
  picture.decodedDataSize = m_yuvFrameSize;

  if (m_isRGB)
  {
    // ===== RGB zero-copy path =====
    // Set videoFormat first so GetFrameBuffer (called by RenderToDmaBuf) knows
    // what DRM fourcc to allocate.
    if (m_format == "xrgb8888")           picture.videoFormat = VIDEOCODEC_FORMAT_XRGB8888;
    else if (m_format == "xrgb2101010")   picture.videoFormat = VIDEOCODEC_FORMAT_XRGB2101010;
    else if (m_format == "xrgb16161616")  picture.videoFormat = VIDEOCODEC_FORMAT_XRGB16161616;
    else if (m_format == "xrgb16161616f") picture.videoFormat = VIDEOCODEC_FORMAT_XRGB16161616F;

    picture.stride[VIDEOCODEC_PICTURE_Y_PLANE] = m_width * m_rgbBytesPerPixel;
    picture.planeOffsets[VIDEOCODEC_PICTURE_Y_PLANE] = 0;
    picture.stride[VIDEOCODEC_PICTURE_U_PLANE] = 0;
    picture.planeOffsets[VIDEOCODEC_PICTURE_U_PLANE] = 0;
    picture.stride[VIDEOCODEC_PICTURE_V_PLANE] = 0;
    picture.planeOffsets[VIDEOCODEC_PICTURE_V_PLANE] = 0;

    SetPictureHdrType(picture);

    if (!RenderToDmaBuf(picture))
    {
      kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: RenderToDmaBuf failed");
      return VC_ERROR;
    }

    m_frameNumber++;
    return VC_PICTURE;
  }

  // ===== YUV path (original CPU-pack flow) =====

  // timing probes -- log every 30 frames
  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();

  // render shader per-frame (copy 1: GPU -> system memory RGBA)
  if (!eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext))
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: eglMakeCurrent failed");
    return VC_ERROR;
  }
  auto t1 = clk::now();

  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  RenderPattern();
  auto t2 = clk::now();

  glFinish();
  auto t3 = clk::now();

  if (m_bitDepth > 8)
    glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_SHORT, m_readbackBuffer.data());
  else
    glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, m_readbackBuffer.data());
  auto t4 = clk::now();

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  auto t5 = clk::now();

  picture.stride[VIDEOCODEC_PICTURE_Y_PLANE] = m_yStride;
  picture.planeOffsets[VIDEOCODEC_PICTURE_Y_PLANE] = 0;

  // set video format based on subsampling and bit depth
  if (m_format == "nv12")
    picture.videoFormat = VIDEOCODEC_FORMAT_NV12;
  else if (m_format == "p010")
    picture.videoFormat = VIDEOCODEC_FORMAT_P010;
  else if (m_subsampleX == 1 && m_subsampleY == 1) // 4:4:4
  {
    if (m_bitDepth == 12)      picture.videoFormat = VIDEOCODEC_FORMAT_YUV444P12;
    else if (m_bitDepth == 10) picture.videoFormat = VIDEOCODEC_FORMAT_YUV444P10;
    else                       picture.videoFormat = VIDEOCODEC_FORMAT_YUV444P;
  }
  else if (m_subsampleX == 2 && m_subsampleY == 1) // 4:2:2
  {
    if (m_bitDepth == 12)      picture.videoFormat = VIDEOCODEC_FORMAT_YUV422P12;
    else if (m_bitDepth == 10) picture.videoFormat = VIDEOCODEC_FORMAT_YUV422P10;
    else                       picture.videoFormat = VIDEOCODEC_FORMAT_YUV422P;
  }
  else // 4:2:0
  {
    if (m_bitDepth == 12)      picture.videoFormat = VIDEOCODEC_FORMAT_YUV420P12;
    else if (m_bitDepth == 10) picture.videoFormat = VIDEOCODEC_FORMAT_YUV420P10;
    else                       picture.videoFormat = VIDEOCODEC_FORMAT_I420;
  }

  picture.stride[VIDEOCODEC_PICTURE_U_PLANE] = m_uvStride;
  picture.planeOffsets[VIDEOCODEC_PICTURE_U_PLANE] = m_ySize;
  if (m_format == "nv12" || m_format == "p010")
  {
    picture.stride[VIDEOCODEC_PICTURE_V_PLANE] = 0;
    picture.planeOffsets[VIDEOCODEC_PICTURE_V_PLANE] = 0;
  }
  else
  {
    picture.stride[VIDEOCODEC_PICTURE_V_PLANE] = m_uvStride;
    picture.planeOffsets[VIDEOCODEC_PICTURE_V_PLANE] = m_ySize + m_uvSize;
  }

  SetPictureHdrType(picture);

  auto t6 = clk::now();

  // GetFrameBuffer calls SyncStart on the DMA buffer
  if (!GetFrameBuffer(picture))
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: GetFrameBuffer failed");
    return VC_ERROR;
  }
  auto t7 = clk::now();

  // Read a sample from the readback buffer (8-bit or 16-bit RGBA).
  // For >8-bit, GL_UNSIGNED_SHORT readback from GL_RGBA16_EXT gives [0,65535].
  // Scale to target bit depth: maxVal = (1 << bitDepth) - 1.
  int maxVal = (1 << m_bitDepth) - 1;
  int srcPixelBytes = 4 * m_bytesPerSample;
  int srcRowBytes = m_width * srcPixelBytes;
  auto readSample = [&](int row, int px, int channel) -> int {
    const uint8_t* base = m_readbackBuffer.data() + row * srcRowBytes + px * srcPixelBytes;
    if (m_bytesPerSample == 2)
    {
      uint16_t raw = reinterpret_cast<const uint16_t*>(base)[channel];
      return static_cast<int>((static_cast<uint32_t>(raw) * maxVal + 32767) / 65535);
    }
    return base[channel];
  };

  // P010 stores data in upper bits of 16-bit words; planar formats use lower bits
  // P010 stores data in upper bits of 16-bit words; planar formats use lower bits
  bool upperBits = (m_format == "p010");
  int shift = upperBits ? (16 - m_bitDepth) : 0;

  // extract Y plane
  uint8_t* yPlane = picture.decodedData;
  for (int y = 0; y < m_height; y++)
  {
    int srcRow = m_height - 1 - y;
    for (int x = 0; x < m_width; x++)
    {
      int val = readSample(srcRow, x, 0) << shift;
      if (m_bytesPerSample == 2)
        reinterpret_cast<uint16_t*>(yPlane + y * m_yStride)[x] = static_cast<uint16_t>(val);
      else
        yPlane[y * m_yStride + x] = static_cast<uint8_t>(val);
    }
  }

  // extract chroma planes using subsampling factors
  int chromaW = m_width / m_subsampleX;
  int chromaH = m_height / m_subsampleY;
  int count = m_subsampleX * m_subsampleY;

  if (m_format == "nv12" || m_format == "p010")
  {
    // semi-planar: interleaved UV
    uint8_t* uvPlane = picture.decodedData + m_ySize;
    for (int cy = 0; cy < chromaH; cy++)
    {
      for (int cx = 0; cx < chromaW; cx++)
      {
        int sumU = 0, sumV = 0;
        for (int sy = 0; sy < m_subsampleY; sy++)
        {
          int srcRow = m_height - 1 - (cy * m_subsampleY + sy);
          for (int sx = 0; sx < m_subsampleX; sx++)
          {
            int px = cx * m_subsampleX + sx;
            sumU += readSample(srcRow, px, 1);
            sumV += readSample(srcRow, px, 2);
          }
        }
        int u = ((sumU + count / 2) / count) << shift;
        int v = ((sumV + count / 2) / count) << shift;
        if (m_bytesPerSample == 2)
        {
          auto* dst = reinterpret_cast<uint16_t*>(uvPlane + cy * m_uvStride);
          dst[cx * 2] = static_cast<uint16_t>(u);
          dst[cx * 2 + 1] = static_cast<uint16_t>(v);
        }
        else
        {
          uint8_t* dst = uvPlane + cy * m_uvStride;
          dst[cx * 2] = static_cast<uint8_t>(u);
          dst[cx * 2 + 1] = static_cast<uint8_t>(v);
        }
      }
    }
  }
  else
  {
    // planar: separate U and V
    uint8_t* uPlane = picture.decodedData + m_ySize;
    uint8_t* vPlane = picture.decodedData + m_ySize + m_uvSize;
    for (int cy = 0; cy < chromaH; cy++)
    {
      for (int cx = 0; cx < chromaW; cx++)
      {
        int sumU = 0, sumV = 0;
        for (int sy = 0; sy < m_subsampleY; sy++)
        {
          int srcRow = m_height - 1 - (cy * m_subsampleY + sy);
          for (int sx = 0; sx < m_subsampleX; sx++)
          {
            int px = cx * m_subsampleX + sx;
            sumU += readSample(srcRow, px, 1);
            sumV += readSample(srcRow, px, 2);
          }
        }
        int u = (sumU + count / 2) / count;
        int v = (sumV + count / 2) / count;
        if (m_bytesPerSample == 2)
        {
          reinterpret_cast<uint16_t*>(uPlane + cy * m_uvStride)[cx] = static_cast<uint16_t>(u);
          reinterpret_cast<uint16_t*>(vPlane + cy * m_uvStride)[cx] = static_cast<uint16_t>(v);
        }
        else
        {
          uPlane[cy * m_uvStride + cx] = static_cast<uint8_t>(u);
          vPlane[cy * m_uvStride + cx] = static_cast<uint8_t>(v);
        }
      }
    }
  }
  // SyncEnd is called by CAddonVideoCodec after we return VC_PICTURE
  auto t8 = clk::now();

  if ((m_frameNumber % 30) == 0)
  {
    auto us = [](auto a, auto b) {
      return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    };
    kodi::Log(ADDON_LOG_INFO,
              "CTestPatternCodec timing frame %d: makeCurrent=%lldus render=%lldus "
              "glFinish=%lldus readPixels=%lldus releaseCurrent=%lldus getFb=%lldus "
              "cpuPack=%lldus total=%lldus",
              m_frameNumber, (long long)us(t0, t1), (long long)us(t1, t2),
              (long long)us(t2, t3), (long long)us(t3, t4), (long long)us(t4, t5),
              (long long)us(t6, t7), (long long)us(t7, t8), (long long)us(t0, t8));
  }

  m_frameNumber++;

  return VC_PICTURE;
}

bool CTestPatternCodec::InitGLContext()
{
  m_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (m_eglDisplay == EGL_NO_DISPLAY)
    return false;

  EGLint major, minor;
  eglInitialize(m_eglDisplay, &major, &minor);

  const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0,
                                  EGL_RED_SIZE,        8,                   EGL_GREEN_SIZE,   8,
                                  EGL_BLUE_SIZE,       8,                   EGL_ALPHA_SIZE,   8,
                                  EGL_NONE};

  EGLint numConfigs = 0;
  if (!eglChooseConfig(m_eglDisplay, configAttribs, &m_eglConfig, 1, &numConfigs) ||
      numConfigs == 0)
    return false;

  const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  m_eglContext = eglCreateContext(m_eglDisplay, m_eglConfig, EGL_NO_CONTEXT, contextAttribs);
  if (m_eglContext == EGL_NO_CONTEXT)
    return false;

  if (!eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext))
  {
    eglDestroyContext(m_eglDisplay, m_eglContext);
    m_eglContext = EGL_NO_CONTEXT;
    return false;
  }

  kodi::Log(ADDON_LOG_DEBUG, "CTestPatternCodec: EGL context created");
  return true;
}

void CTestPatternCodec::DestroyGLContext()
{
  if (m_eglContext != EGL_NO_CONTEXT && m_eglDisplay != EGL_NO_DISPLAY)
  {
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_eglDisplay, m_eglContext);
    m_eglContext = EGL_NO_CONTEXT;
  }
}

bool CTestPatternCodec::CompileShader(const std::string& shaderSource)
{
  m_vertexShader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(m_vertexShader, 1, &VERTEX_SHADER_SOURCE, nullptr);
  glCompileShader(m_vertexShader);
  GLint compiled = 0;
  glGetShaderiv(m_vertexShader, GL_COMPILE_STATUS, &compiled);
  if (!compiled)
  {
    char log[512];
    glGetShaderInfoLog(m_vertexShader, sizeof(log), nullptr, log);
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: vertex shader error: %s", log);
    return false;
  }

  m_fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
  const char* fragSource = shaderSource.c_str();
  glShaderSource(m_fragmentShader, 1, &fragSource, nullptr);
  glCompileShader(m_fragmentShader);
  glGetShaderiv(m_fragmentShader, GL_COMPILE_STATUS, &compiled);
  if (!compiled)
  {
    char log[1024];
    glGetShaderInfoLog(m_fragmentShader, sizeof(log), nullptr, log);
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: fragment shader error: %s", log);
    return false;
  }

  m_shaderProgram = glCreateProgram();
  glAttachShader(m_shaderProgram, m_vertexShader);
  glAttachShader(m_shaderProgram, m_fragmentShader);
  glLinkProgram(m_shaderProgram);
  GLint linked = 0;
  glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &linked);
  if (!linked)
  {
    char log[512];
    glGetProgramInfoLog(m_shaderProgram, sizeof(log), nullptr, log);
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: link error: %s", log);
    return false;
  }

  m_locResolution = glGetUniformLocation(m_shaderProgram, "u_resolution");
  m_locTime = glGetUniformLocation(m_shaderProgram, "u_time");
  m_locFrame = glGetUniformLocation(m_shaderProgram, "u_frame");
  m_locFrameCountPos = glGetUniformLocation(m_shaderProgram, "u_frameCountPos");
  m_locPosition = glGetAttribLocation(m_shaderProgram, "a_position");

  kodi::Log(ADDON_LOG_DEBUG, "CTestPatternCodec: shader compiled and linked");
  return true;
}

bool CTestPatternCodec::CreateFBO()
{
  GLenum internalFormat = GL_RGBA;
  GLenum type = GL_UNSIGNED_BYTE;
  if (m_bitDepth > 8)
  {
    // GL_RGBA16_EXT from EXT_texture_norm16 -- 16-bit unorm, color-renderable,
    // exact precision for 10-bit and 12-bit values. Requires GLES 3.0+.
    internalFormat = GL_RGBA16_EXT;
    type = GL_UNSIGNED_SHORT;
  }

  glGenTextures(1, &m_fboTexture);
  glBindTexture(GL_TEXTURE_2D, m_fboTexture);
  glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, m_width, m_height, 0, GL_RGBA, type, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fboTexture, 0);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: FBO incomplete: 0x%x",
              static_cast<unsigned>(status));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  kodi::Log(ADDON_LOG_DEBUG, "CTestPatternCodec: FBO created (%dx%d)", m_width, m_height);
  return true;
}

void CTestPatternCodec::DestroyFBO()
{
  if (m_eglDisplay != EGL_NO_DISPLAY && m_eglContext != EGL_NO_CONTEXT)
  {
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext);
    if (m_shaderProgram)
    {
      glDeleteProgram(m_shaderProgram);
      m_shaderProgram = 0;
    }
    if (m_vertexShader)
    {
      glDeleteShader(m_vertexShader);
      m_vertexShader = 0;
    }
    if (m_fragmentShader)
    {
      glDeleteShader(m_fragmentShader);
      m_fragmentShader = 0;
    }
    if (m_fbo)
    {
      glDeleteFramebuffers(1, &m_fbo);
      m_fbo = 0;
    }
    if (m_fboTexture)
    {
      glDeleteTextures(1, &m_fboTexture);
      m_fboTexture = 0;
    }
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
}

void CTestPatternCodec::RenderPattern()
{
  // Caller must bind m_fbo before calling.
  glViewport(0, 0, m_width, m_height);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(m_shaderProgram);

  if (m_locResolution >= 0)
    glUniform2f(m_locResolution, static_cast<float>(m_width), static_cast<float>(m_height));
  if (m_locTime >= 0)
    glUniform1f(m_locTime, static_cast<float>(m_pts) / 1000000.0f);
  if (m_locFrame >= 0)
    glUniform1i(m_locFrame, m_frameNumber);
  if (m_locFrameCountPos >= 0)
  {
    glUniform2f(m_locFrameCountPos,
                m_frameCountX >= 0 ? static_cast<float>(m_frameCountX) : -1.0f,
                m_frameCountY >= 0 ? static_cast<float>(m_frameCountY) : -1.0f);
  }

  glVertexAttribPointer(m_locPosition, 2, GL_FLOAT, GL_FALSE, 0, FULLSCREEN_QUAD);
  glEnableVertexAttribArray(m_locPosition);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(m_locPosition);

  glFinish();
}

bool CTestPatternCodec::RenderToDmaBuf(VIDEOCODEC_PICTURE& picture)
{
  // Allocate a DMA-BUF of the requested RGB format from Kodi's pool. Kodi
  // builds a CVideoBufferDMA with the matching DRM fourcc.
  if (!GetFrameBuffer(picture))
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: GetFrameBuffer failed");
    return false;
  }

  // Query the platform-native handle so we can render directly into the
  // underlying DMA-BUF without going through CPU memory.
  VIDEOCODEC_PLATFORM_BUFFER pb = {};
  if (!GetFrameBufferPlatformHandle(picture.videoBufferHandle, pb) ||
      pb.type != VIDEOCODEC_PLATFORM_BUFFER_DRM_PRIME || !pb.handle)
  {
    kodi::Log(ADDON_LOG_ERROR,
              "CTestPatternCodec: no DRM_PRIME handle (type=%d) -- "
              "is useprimedecoder=true?", static_cast<int>(pb.type));
    return false;
  }

  auto* desc = static_cast<KODI_DRM_FRAME_DESCRIPTOR*>(pb.handle);
  if (desc->nb_objects < 1 || desc->nb_layers < 1 || desc->layers[0].nb_planes < 1)
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: malformed DRM frame descriptor");
    return false;
  }

  int fd = desc->objects[0].fd;
  uint32_t fourcc = desc->layers[0].format;
  uint64_t modifier = desc->objects[0].format_modifier;
  int offset = static_cast<int>(desc->layers[0].planes[0].offset);
  int pitch = static_cast<int>(desc->layers[0].planes[0].pitch);

  if (!eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext))
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: eglMakeCurrent failed");
    return false;
  }

  // Resolve EGL/GL extension functions on first use.
  static auto eglCreateImageKHRFn =
      reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  static auto eglDestroyImageKHRFn =
      reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  static auto glEGLImageTargetRenderbufferStorageOESFn =
      reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
          eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
  if (!eglCreateImageKHRFn || !eglDestroyImageKHRFn ||
      !glEGLImageTargetRenderbufferStorageOESFn)
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: missing EGL/GL extension entry points");
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return false;
  }

  // Build the EGLImage attribute list. Modifier attribs are optional but most
  // drivers prefer them when known.
  EGLint attribs[24];
  int i = 0;
  attribs[i++] = EGL_WIDTH;                       attribs[i++] = m_width;
  attribs[i++] = EGL_HEIGHT;                      attribs[i++] = m_height;
  attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;        attribs[i++] = static_cast<EGLint>(fourcc);
  attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;       attribs[i++] = fd;
  attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;   attribs[i++] = offset;
  attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;    attribs[i++] = pitch;
  if (modifier != DRM_FORMAT_MOD_INVALID)
  {
    attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attribs[i++] = static_cast<EGLint>(modifier & 0xFFFFFFFFu);
    attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attribs[i++] = static_cast<EGLint>(modifier >> 32);
  }
  attribs[i++] = EGL_NONE;

  EGLImageKHR eglImage = eglCreateImageKHRFn(m_eglDisplay, EGL_NO_CONTEXT,
                                              EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
  if (eglImage == EGL_NO_IMAGE_KHR)
  {
    kodi::Log(ADDON_LOG_ERROR,
              "CTestPatternCodec: eglCreateImageKHR failed (egl=%#x fourcc=%c%c%c%c)",
              eglGetError(),
              fourcc & 0xff, (fourcc >> 8) & 0xff,
              (fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff);
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return false;
  }

  // Bind the EGLImage as a renderbuffer attached to a transient FBO.
  GLuint rb = 0;
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  glEGLImageTargetRenderbufferStorageOESFn(GL_RENDERBUFFER, eglImage);

  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternCodec: dma-buf FBO incomplete: 0x%x",
              static_cast<unsigned>(status));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &rb);
    eglDestroyImageKHRFn(m_eglDisplay, eglImage);
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return false;
  }

  // Render the shader directly into the DMA-BUF.
  RenderPattern();

  // Tear down the transient FBO and EGLImage. The DMA-BUF itself remains
  // owned by Kodi and will be released by the renderer/SyncEnd path.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
  glDeleteRenderbuffers(1, &rb);
  eglDestroyImageKHRFn(m_eglDisplay, eglImage);

  eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return true;
}

