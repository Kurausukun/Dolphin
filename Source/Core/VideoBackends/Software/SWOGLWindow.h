// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/AbstractTexture.h"

class SWOGLWindow
{
public:
  static void Init(void* window_handle);
  static void Shutdown();
  void Prepare();

  // Will be printed on the *next* image
  void PrintText(const std::string& text, int x, int y, u32 color);

  // Image to show, will be swapped immediately
  void ShowImage(AbstractTexture* image, float aspect);

  int PeekMessages();

  static std::unique_ptr<SWOGLWindow> s_instance;

private:
  SWOGLWindow() {}

  struct TextData
  {
    std::string text;
    int x, y;
    u32 color;
  };
  std::vector<TextData> m_text;

  bool m_init{false};

  u32 m_image_program, m_image_vao;
};
