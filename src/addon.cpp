/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TestPatternCodec.h"
#include "TestPatternInputStream.h"

#include <kodi/General.h>

#include <cstdio>
#include <fstream>
#include <sstream>

//------------------------------------------------------------------------------
// CTestPatternInputStream
//------------------------------------------------------------------------------

CTestPatternInputStream::CTestPatternInputStream(const kodi::addon::IInstanceInfo& instance)
  : CInstanceInputStream(instance)
{
}

CTestPatternInputStream::~CTestPatternInputStream()
{
  Close();
}

bool CTestPatternInputStream::Open(const kodi::addon::InputstreamProperty& props)
{
  std::string url = props.GetURL();
  kodi::Log(ADDON_LOG_INFO, "CTestPatternInputStream::Open - URL: %s", url.c_str());

  if (!ParsePatternFile(url))
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternInputStream: failed to parse: %s", url.c_str());
    return false;
  }

  // Normalize format: extract bit depth if embedded (e.g. "yuv420p10le" -> "yuv420p" + bits=10)
  struct { const char* name; const char* base; int bits; } fmtAliases[] = {
    {"yuv420p10le", "yuv420p", 10}, {"yuv420p10", "yuv420p", 10},
    {"yuv420p12le", "yuv420p", 12}, {"yuv420p12", "yuv420p", 12},
    {"yuv422p10le", "yuv422p", 10}, {"yuv422p10", "yuv422p", 10},
    {"yuv422p12le", "yuv422p", 12}, {"yuv422p12", "yuv422p", 12},
    {"yuv444p10le", "yuv444p", 10}, {"yuv444p10", "yuv444p", 10},
    {"yuv444p12le", "yuv444p", 12}, {"yuv444p12", "yuv444p", 12},
    {"xrgb8888",      "xrgb8888",       8},
    {"xrgb2101010",   "xrgb2101010",   10},
    {"xrgb16161616",  "xrgb16161616",  12},
    {"xrgb16161616f", "xrgb16161616f", 16},
  };
  for (const auto& alias : fmtAliases)
  {
    if (m_config.format == alias.name)
    {
      m_config.format = alias.base;
      if (m_config.bitDepth < alias.bits)
        m_config.bitDepth = alias.bits;
      break;
    }
  }

  // compute frame size for queue management
  bool isRGB = (m_config.format == "xrgb8888" || m_config.format == "xrgb2101010" ||
                m_config.format == "xrgb16161616" || m_config.format == "xrgb16161616f");

  if (isRGB)
  {
    // Packed RGB: single plane, no subsampling.
    // 32 bpp for 8/10-bit, 64 bpp for 16-bit int and half-float.
    int bpp = (m_config.format == "xrgb16161616" || m_config.format == "xrgb16161616f") ? 8 : 4;
    m_frameSize = m_config.width * m_config.height * bpp;
  }
  else
  {
    int subX = 2, subY = 2; // default: 4:2:0
    if (m_config.format == "yuv422p")
    { subX = 2; subY = 1; }
    else if (m_config.format == "yuv444p")
    { subX = 1; subY = 1; }

    int bytesPerSample = m_config.bitDepth > 8 ? 2 : 1;
    int ySize = m_config.width * m_config.height * bytesPerSample;
    int chromaW = m_config.width / subX;
    int chromaH = m_config.height / subY;
    if (m_config.format == "nv12")
      m_frameSize = ySize + chromaW * 2 * chromaH * bytesPerSample;
    else
      m_frameSize = ySize + chromaW * chromaH * 2 * bytesPerSample;
  }

  m_totalFrames = m_config.durationSeconds * m_config.fpsNum / m_config.fpsDen;
  m_frameCount = 0;
  m_opened = true;

  kodi::Log(ADDON_LOG_INFO,
            "CTestPatternInputStream: opened %s (%dx%d %.3ffps %ds shader=%s frameSize=%d)",
            url.c_str(), m_config.width, m_config.height,
            static_cast<float>(m_config.fpsNum) / m_config.fpsDen, m_config.durationSeconds,
            m_config.shaderFile.c_str(), m_frameSize);

  return true;
}

void CTestPatternInputStream::Close()
{
  m_opened = false;
}

void CTestPatternInputStream::GetCapabilities(kodi::addon::InputstreamCapabilities& capabilities)
{
  capabilities.SetMask(INPUTSTREAM_SUPPORTS_IDEMUX | INPUTSTREAM_SUPPORTS_IDISPLAYTIME |
                       INPUTSTREAM_SUPPORTS_PAUSE | INPUTSTREAM_SUPPORTS_IPOSTIME);
}

bool CTestPatternInputStream::GetStreamIds(std::vector<unsigned int>& ids)
{
  if (!m_opened)
    return false;
  ids.push_back(0);
  return true;
}

bool CTestPatternInputStream::GetStream(int streamid, kodi::addon::InputstreamInfo& stream)
{
  if (streamid != 0 || !m_opened)
    return false;

  stream.SetStreamType(INPUTSTREAM_TYPE_VIDEO);
  stream.SetCodecName("rawvideo");
  stream.SetPhysicalIndex(0);
  stream.SetWidth(m_config.width);
  stream.SetHeight(m_config.height);
  stream.SetFpsRate(m_config.fpsNum);
  stream.SetFpsScale(m_config.fpsDen);
  stream.SetAspect(static_cast<float>(m_config.width) / m_config.height);
  stream.SetFeatures(INPUTSTREAM_FEATURE_DECODE);

  // pass fields through as NUL-separated extradata
  // format: "shaderPath\0format\0bits\0transfer\0framecount"
  // transfer: "pq"/"smpte2084"/"hlg"/"arib-std-b67"/"bt709"/... or empty
  // framecount: "X-Y" or empty
  std::string frameCountStr;
  if (m_config.frameCountX >= 0 && m_config.frameCountY >= 0)
    frameCountStr = std::to_string(m_config.frameCountX) + '-' +
                    std::to_string(m_config.frameCountY);
  std::string extraData = m_config.shaderPath + '\0' + m_config.format + '\0' +
                          std::to_string(m_config.bitDepth) + '\0' +
                          m_config.transfer + '\0' +
                          frameCountStr;
  stream.SetExtraData(reinterpret_cast<const uint8_t*>(extraData.data()),
                      extraData.size());

  // color metadata
  if (m_config.colorspace == "bt709")
    stream.SetColorSpace(INPUTSTREAM_COLORSPACE_BT709);
  else if (m_config.colorspace == "bt2020nc")
    stream.SetColorSpace(INPUTSTREAM_COLORSPACE_BT2020_NCL);
  else if (m_config.colorspace == "bt601" || m_config.colorspace == "smpte170m")
    stream.SetColorSpace(INPUTSTREAM_COLORSPACE_SMPTE170M);

  if (m_config.range == "limited" || m_config.range == "tv")
    stream.SetColorRange(INPUTSTREAM_COLORRANGE_LIMITED);
  else if (m_config.range == "full" || m_config.range == "pc")
    stream.SetColorRange(INPUTSTREAM_COLORRANGE_FULLRANGE);

  if (m_config.primaries == "bt709")
    stream.SetColorPrimaries(INPUTSTREAM_COLORPRIMARY_BT709);
  else if (m_config.primaries == "bt2020")
    stream.SetColorPrimaries(INPUTSTREAM_COLORPRIMARY_BT2020);

  if (m_config.transfer == "bt709")
    stream.SetColorTransferCharacteristic(INPUTSTREAM_COLORTRC_BT709);
  else if (m_config.transfer == "smpte2084" || m_config.transfer == "pq")
    stream.SetColorTransferCharacteristic(INPUTSTREAM_COLORTRC_SMPTE2084);
  else if (m_config.transfer == "hlg" || m_config.transfer == "arib-std-b67")
    stream.SetColorTransferCharacteristic(INPUTSTREAM_COLORTRC_ARIB_STD_B67);

  if (m_config.hasMasteringMetadata)
  {
    kodi::addon::InputstreamMasteringMetadata mastering;
    mastering.SetPrimaryR_ChromaticityX(m_config.primaryR[0]);
    mastering.SetPrimaryR_ChromaticityY(m_config.primaryR[1]);
    mastering.SetPrimaryG_ChromaticityX(m_config.primaryG[0]);
    mastering.SetPrimaryG_ChromaticityY(m_config.primaryG[1]);
    mastering.SetPrimaryB_ChromaticityX(m_config.primaryB[0]);
    mastering.SetPrimaryB_ChromaticityY(m_config.primaryB[1]);
    mastering.SetWhitePoint_ChromaticityX(m_config.whitePoint[0]);
    mastering.SetWhitePoint_ChromaticityY(m_config.whitePoint[1]);
    mastering.SetLuminanceMax(m_config.luminanceMax);
    mastering.SetLuminanceMin(m_config.luminanceMin);
    stream.SetMasteringMetadata(mastering);
  }

  if (m_config.hasContentLight)
  {
    kodi::addon::InputstreamContentlightMetadata contentLight;
    contentLight.SetMaxCll(static_cast<uint64_t>(m_config.maxCll));
    contentLight.SetMaxFall(static_cast<uint64_t>(m_config.maxFall));
    stream.SetContentLightMetadata(contentLight);
  }

  return true;
}

DEMUX_PACKET* CTestPatternInputStream::DemuxRead()
{
  if (!m_opened || (m_totalFrames > 0 && m_frameCount >= m_totalFrames))
    return nullptr;

  // allocate packet with the expected decoded frame size so the video player
  // queue level calculation works correctly (it uses iSize for buffering)
  DEMUX_PACKET* packet = AllocateDemuxPacket(m_frameSize);
  if (!packet)
    return nullptr;

  // the packet data is unused -- the addon codec generates frames via shader
  // but iSize must reflect the decoded frame size for proper queue management
  std::memset(packet->pData, 0, m_frameSize);

  double frameDuration =
      STREAM_TIME_BASE * static_cast<double>(m_config.fpsDen) / static_cast<double>(m_config.fpsNum);

  packet->iStreamId = 0;
  packet->iSize = m_frameSize;
  packet->pts = frameDuration * m_frameCount;
  packet->dts = packet->pts;
  packet->duration = frameDuration;

  m_frameCount++;

  return packet;
}

bool CTestPatternInputStream::PosTime(int ms)
{
  if (!m_opened || m_config.fpsDen == 0)
    return false;
  m_frameCount = static_cast<int>(static_cast<double>(ms) * m_config.fpsNum /
                                  (m_config.fpsDen * 1000.0));
  return true;
}

bool CTestPatternInputStream::DemuxSeekTime(double time, bool backwards, double& startpts)
{
  if (!m_opened || m_config.fpsDen == 0)
    return false;
  m_frameCount = static_cast<int>(time * m_config.fpsNum / (m_config.fpsDen * 1000.0));
  startpts = STREAM_MSEC_TO_TIME(time);
  return true;
}

int CTestPatternInputStream::GetTotalTime()
{
  return m_config.durationSeconds * 1000;
}

int CTestPatternInputStream::GetTime()
{
  return static_cast<int>(1000.0 * m_frameCount * m_config.fpsDen / m_config.fpsNum);
}

bool CTestPatternInputStream::ParsePatternFile(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open())
    return false;

  std::string patternDir;
  size_t lastSlash = path.rfind('/');
  if (lastSlash != std::string::npos)
    patternDir = path.substr(0, lastSlash + 1);

  std::string line;
  while (std::getline(file, line))
  {
    size_t commentPos = line.find('#');
    if (commentPos != std::string::npos)
      line = line.substr(0, commentPos);

    size_t start = line.find_first_not_of(" \t\r\n");
    size_t end = line.find_last_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    line = line.substr(start, end - start + 1);

    size_t eqPos = line.find('=');
    if (eqPos == std::string::npos)
      continue;

    std::string key = line.substr(0, eqPos);
    std::string value = line.substr(eqPos + 1);

    auto trim = [](std::string& s) {
      size_t a = s.find_first_not_of(" \t");
      size_t b = s.find_last_not_of(" \t");
      s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
    };
    trim(key);
    trim(value);

    if (key == "shader")
    {
      m_config.shaderFile = value;
      if (!value.empty() && value[0] != '/')
        m_config.shaderPath = patternDir + value;
      else
        m_config.shaderPath = value;
    }
    else if (key == "width")
      m_config.width = std::stoi(value);
    else if (key == "height")
      m_config.height = std::stoi(value);
    else if (key == "fps")
    {
      size_t slashPos = value.find('/');
      if (slashPos != std::string::npos)
      {
        m_config.fpsNum = std::stoi(value.substr(0, slashPos));
        m_config.fpsDen = std::stoi(value.substr(slashPos + 1));
      }
      else
      {
        m_config.fpsNum = static_cast<int>(std::stof(value) * 1000);
        m_config.fpsDen = 1000;
      }
    }
    else if (key == "bits")
      m_config.bitDepth = std::stoi(value);
    else if (key == "format")
      m_config.format = value;
    else if (key == "colorspace")
      m_config.colorspace = value;
    else if (key == "range")
      m_config.range = value;
    else if (key == "primaries")
      m_config.primaries = value;
    else if (key == "transfer")
      m_config.transfer = value;
    else if (key == "duration")
      m_config.durationSeconds = std::stoi(value);
    else if (key == "hdr")
      m_config.hdr = value;
    else if (key == "display_primaries")
    {
      double vals[6] = {};
      if (sscanf(value.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf",
                 &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) == 6)
      {
        m_config.primaryR[0] = vals[0]; m_config.primaryR[1] = vals[1];
        m_config.primaryG[0] = vals[2]; m_config.primaryG[1] = vals[3];
        m_config.primaryB[0] = vals[4]; m_config.primaryB[1] = vals[5];
        m_config.hasMasteringMetadata = true;
      }
    }
    else if (key == "white_point")
    {
      sscanf(value.c_str(), "%lf,%lf", &m_config.whitePoint[0], &m_config.whitePoint[1]);
      m_config.hasMasteringMetadata = true;
    }
    else if (key == "luminance")
    {
      sscanf(value.c_str(), "%lf,%lf", &m_config.luminanceMin, &m_config.luminanceMax);
      m_config.hasMasteringMetadata = true;
    }
    else if (key == "max_cll")
    {
      m_config.maxCll = std::stod(value);
      m_config.hasContentLight = true;
    }
    else if (key == "max_fall")
    {
      m_config.maxFall = std::stod(value);
      m_config.hasContentLight = true;
    }
    else if (key == "framecount")
    {
      // Format: "X-Y" top-left of box in video pixel coords
      size_t dashPos = value.find('-');
      if (dashPos != std::string::npos)
      {
        m_config.frameCountX = std::stoi(value.substr(0, dashPos));
        m_config.frameCountY = std::stoi(value.substr(dashPos + 1));
      }
    }
  }

  if (m_config.shaderFile.empty())
  {
    kodi::Log(ADDON_LOG_ERROR, "CTestPatternInputStream: no shader specified in pattern file");
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// Addon entry point
//------------------------------------------------------------------------------

class ATTR_DLL_LOCAL CTestPatternAddon : public kodi::addon::CAddonBase
{
public:
  CTestPatternAddon() = default;

  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override
  {
    if (instance.IsType(ADDON_INSTANCE_INPUTSTREAM))
    {
      hdl = new CTestPatternInputStream(instance);
      return ADDON_STATUS_OK;
    }
    else if (instance.IsType(ADDON_INSTANCE_VIDEOCODEC))
    {
      hdl = new CTestPatternCodec(instance);
      return ADDON_STATUS_OK;
    }
    return ADDON_STATUS_NOT_IMPLEMENTED;
  }
};

ADDONCREATOR(CTestPatternAddon)
