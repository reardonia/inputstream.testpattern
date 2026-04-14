/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <kodi/addon-instance/Inputstream.h>

#include <string>
#include <vector>

struct TestPatternConfig
{
  std::string shaderFile;
  std::string shaderPath; // resolved absolute path to .frag file
  int width = 1920;
  int height = 1080;
  int fpsNum = 30000;
  int fpsDen = 1001;
  int bitDepth = 8;
  std::string format = "yuv420p";
  int durationSeconds = 30;

  std::string colorspace = "bt709";
  std::string range = "limited";
  std::string primaries = "bt709";
  std::string transfer = "bt709";
  std::string hdr;

  // HDR10 static metadata (mastering display)
  bool hasMasteringMetadata = false;
  double primaryR[2] = {0, 0};
  double primaryG[2] = {0, 0};
  double primaryB[2] = {0, 0};
  double whitePoint[2] = {0, 0};
  double luminanceMin = 0;
  double luminanceMax = 0;

  // content light metadata
  bool hasContentLight = false;
  double maxCll = 0;
  double maxFall = 0;

  // On-screen frame counter (top-left pixel position in video coords; -1 disables)
  int frameCountX = -1;
  int frameCountY = -1;
};

class ATTR_DLL_LOCAL CTestPatternInputStream : public kodi::addon::CInstanceInputStream
{
public:
  CTestPatternInputStream(const kodi::addon::IInstanceInfo& instance);
  ~CTestPatternInputStream() override;

  bool Open(const kodi::addon::InputstreamProperty& props) override;
  void Close() override;

  void GetCapabilities(kodi::addon::InputstreamCapabilities& capabilities) override;

  bool GetStreamIds(std::vector<unsigned int>& ids) override;
  bool GetStream(int streamid, kodi::addon::InputstreamInfo& stream) override;
  void EnableStream(int streamid, bool enable) override {}
  bool OpenStream(int streamid) override { return true; }

  DEMUX_PACKET* DemuxRead() override;
  bool DemuxSeekTime(double time, bool backwards, double& startpts) override;
  void DemuxSetSpeed(int speed) override {}
  void DemuxAbort() override {}
  void DemuxFlush() override {}

  int GetTotalTime() override;
  int GetTime() override;

  bool PosTime(int ms) override;
  bool IsRealTimeStream() override { return false; }

private:
  bool ParsePatternFile(const std::string& path);

  TestPatternConfig m_config;
  int m_frameCount = 0;
  int m_totalFrames = 0;
  int m_frameSize = 0; // YUV420P frame size in bytes
  bool m_opened = false;
};
