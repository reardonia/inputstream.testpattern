/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <kodi/addon-instance/VideoCodec.h>

#include <string>
#include <utility>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

class ATTR_DLL_LOCAL CTestPatternCodec : public kodi::addon::CInstanceVideoCodec
{
public:
  CTestPatternCodec(const kodi::addon::IInstanceInfo& instance);
  ~CTestPatternCodec() override;

  bool Open(const kodi::addon::VideoCodecInitdata& initData) override;
  bool AddData(const DEMUX_PACKET& packet) override;
  VIDEOCODEC_RETVAL GetPicture(VIDEOCODEC_PICTURE& picture) override;
  const char* GetName() override { return "testpattern"; }
  void Reset() override;

private:
  bool InitGLContext();
  void DestroyGLContext();
  bool CompileShader(const std::string& shaderSource);
  bool CreateFBO();
  void DestroyFBO();
  void RenderPattern();
  // Zero-copy GPU render directly into a DMA-BUF backed by an EGLImage.
  // Returns true on success, false on any EGL/GL error (caller can fall back).
  bool RenderToDmaBuf(VIDEOCODEC_PICTURE& picture);

  // Set picture.hdrType explicitly per VIDEOCODEC_PICTURE API, from
  // m_transfer. Overrides Kodi's stream-hint derivation.
  void SetPictureHdrType(VIDEOCODEC_PICTURE& picture) const;

  int m_width = 0;
  int m_height = 0;
  int m_frameNumber = 0;
  bool m_hasFrame = false;
  int64_t m_pts = 0;
  bool m_opened = false;

  // format and cached plane layout (computed once in Open)
  std::string m_format = "yuv420p";
  int m_bitDepth = 8;
  int m_bytesPerSample = 1;
  int m_yStride = 0;
  int m_uvStride = 0;
  int m_ySize = 0;
  int m_uvSize = 0;
  int m_subsampleX = 2; // horizontal chroma subsampling factor
  int m_subsampleY = 2; // vertical chroma subsampling factor
  bool m_isRGB = false; // true for xrgb8888/xrgb2101010/xrgb16161616/xrgb16161616f
  int m_rgbBytesPerPixel = 0; // 4 for 8/10-bit, 8 for 16-bit int and half-float

  // EGL context
  EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
  EGLContext m_eglContext = EGL_NO_CONTEXT;
  EGLConfig m_eglConfig = nullptr;

  // GL resources
  GLuint m_fbo = 0;
  GLuint m_fboTexture = 0;
  GLuint m_shaderProgram = 0;
  GLuint m_vertexShader = 0;
  GLuint m_fragmentShader = 0;
  GLint m_locResolution = -1;
  GLint m_locTime = -1;
  GLint m_locFrame = -1;
  GLint m_locFrameCountPos = -1;
  GLint m_locPosition = -1;

  // buffers
  std::vector<uint8_t> m_readbackBuffer; // RGBA from glReadPixels
  int m_yuvFrameSize = 0;

  // Frame counter overlay position (video top-down pixels, -1 = disabled)
  int m_frameCountX = -1;
  int m_frameCountY = -1;

  // Transfer characteristic name ("pq", "smpte2084", "hlg", "arib-std-b67", "bt709", ...)
  // used to set VIDEOCODEC_PICTURE.hdrType explicitly per frame.
  std::string m_transfer;
};
