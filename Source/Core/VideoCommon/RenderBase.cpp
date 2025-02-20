// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// ---------------------------------------------------------------------------------------------
// GC graphics pipeline
// ---------------------------------------------------------------------------------------------
// 3d commands are issued through the fifo. The GPU draws to the 2MB EFB.
// The efb can be copied back into ram in two forms: as textures or as XFB.
// The XFB is the region in RAM that the VI chip scans out to the television.
// So, after all rendering to EFB is done, the image is copied into one of two XFBs in RAM.
// Next frame, that one is scanned out and the other one gets the copy. = double buffering.
// ---------------------------------------------------------------------------------------------

#include "VideoCommon/RenderBase.h"

#include <cinttypes>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/FileUtil.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Profiler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/Movie.h"

#include "VideoCommon/AbstractRawTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/AVIDump.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Debugger.h"
#include "VideoCommon/FPSCounter.h"
#include "VideoCommon/FramebufferManagerBase.h"
#include "VideoCommon/ImageWrite.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

// TODO: Move these out of here.
int frameCount;
int OSDChoice;
static int OSDTime;

std::unique_ptr<Renderer> g_renderer;

// The maximum depth that is written to the depth buffer should never exceed this value.
// This is necessary because we use a 2^24 divisor for all our depth values to prevent
// floating-point round-trip errors. However the console GPU doesn't ever write a value
// to the depth buffer that exceeds 2^24 - 1.
const float Renderer::GX_MAX_DEPTH = 16777215.0f / 16777216.0f;

static float AspectToWidescreen(float aspect)
{
  return aspect * ((16.0f / 9.0f) / (4.0f / 3.0f));
}

Renderer::Renderer(int backbuffer_width, int backbuffer_height)
    : m_backbuffer_width(backbuffer_width), m_backbuffer_height(backbuffer_height),
      m_last_efb_scale(g_ActiveConfig.iEFBScale)
{
  UpdateActiveConfig();
  UpdateDrawRectangle();
  CalculateTargetSize();

  OSDChoice = 0;
  OSDTime = 0;

  if (SConfig::GetInstance().bWii)
  {
    m_aspect_wide = SConfig::GetInstance().m_wii_aspect_ratio != 0;
  }

  m_last_host_config_bits = ShaderHostConfig::GetCurrent().bits;
}

Renderer::~Renderer()
{
  ShutdownFrameDumping();
  if (m_frame_dump_thread.joinable())
    m_frame_dump_thread.join();
}

void Renderer::RenderToXFB(u32 xfbAddr, const EFBRectangle& sourceRc, u32 fbStride, u32 fbHeight,
                           float Gamma)
{
  CheckFifoRecording();

  if (!fbStride || !fbHeight)
    return;

  m_xfb_written = true;
}

int Renderer::EFBToScaledX(int x) const
{
  return x * (int)m_efb_scale_numeratorX / (int)m_efb_scale_denominatorX;
}

int Renderer::EFBToScaledY(int y) const
{
  return y * (int)m_efb_scale_numeratorY / (int)m_efb_scale_denominatorY;
}

float Renderer::EFBToScaledXf(float x) const
{
  return x * ((float)GetTargetWidth() / (float)EFB_WIDTH);
}

float Renderer::EFBToScaledYf(float y) const
{
  return y * ((float)GetTargetHeight() / (float)EFB_HEIGHT);
}

std::tuple<int, int> Renderer::CalculateTargetScale(int x, int y) const
{
  if (g_ActiveConfig.iEFBScale == SCALE_AUTO || g_ActiveConfig.iEFBScale == SCALE_AUTO_INTEGRAL)
  {
    return std::make_tuple(x, y);
  }

  const int scaled_x =
      x * static_cast<int>(m_efb_scale_numeratorX) / static_cast<int>(m_efb_scale_denominatorX);

  const int scaled_y =
      y * static_cast<int>(m_efb_scale_numeratorY) / static_cast<int>(m_efb_scale_denominatorY);

  return std::make_tuple(scaled_x, scaled_y);
}

// return true if target size changed
bool Renderer::CalculateTargetSize()
{
  m_last_efb_scale = g_ActiveConfig.iEFBScale;

  int new_efb_width = 0;
  int new_efb_height = 0;

  // TODO: Ugly. Clean up
  switch (m_last_efb_scale)
  {
  case SCALE_AUTO:
  case SCALE_AUTO_INTEGRAL:
    new_efb_width = m_target_width;
    new_efb_height = m_target_height;

    if (m_last_efb_scale == SCALE_AUTO_INTEGRAL)
    {
      m_efb_scale_numeratorX = m_efb_scale_numeratorY =
          std::max((new_efb_width - 1) / EFB_WIDTH + 1, (new_efb_height - 1) / EFB_HEIGHT + 1);
      m_efb_scale_denominatorX = m_efb_scale_denominatorY = 1;
      new_efb_width = EFBToScaledX(EFB_WIDTH);
      new_efb_height = EFBToScaledY(EFB_HEIGHT);
    }
    else
    {
      m_efb_scale_numeratorX = new_efb_width;
      m_efb_scale_denominatorX = EFB_WIDTH;
      m_efb_scale_numeratorY = new_efb_height;
      m_efb_scale_denominatorY = EFB_HEIGHT;
    }
    break;

  case SCALE_1X:
    m_efb_scale_numeratorX = m_efb_scale_numeratorY = 1;
    m_efb_scale_denominatorX = m_efb_scale_denominatorY = 1;
    break;

  case SCALE_1_5X:
    m_efb_scale_numeratorX = m_efb_scale_numeratorY = 3;
    m_efb_scale_denominatorX = m_efb_scale_denominatorY = 2;
    break;

  case SCALE_2X:
    m_efb_scale_numeratorX = m_efb_scale_numeratorY = 2;
    m_efb_scale_denominatorX = m_efb_scale_denominatorY = 1;
    break;

  case SCALE_2_5X:
    m_efb_scale_numeratorX = m_efb_scale_numeratorY = 5;
    m_efb_scale_denominatorX = m_efb_scale_denominatorY = 2;
    break;

  default:
    m_efb_scale_numeratorX = m_efb_scale_numeratorY = m_last_efb_scale - 3;
    m_efb_scale_denominatorX = m_efb_scale_denominatorY = 1;

    const u32 max_size = g_ActiveConfig.backend_info.MaxTextureSize;
    if (max_size < EFB_WIDTH * m_efb_scale_numeratorX / m_efb_scale_denominatorX)
    {
      m_efb_scale_numeratorX = m_efb_scale_numeratorY = (max_size / EFB_WIDTH);
      m_efb_scale_denominatorX = m_efb_scale_denominatorY = 1;
    }

    break;
  }
  if (m_last_efb_scale > SCALE_AUTO_INTEGRAL)
    std::tie(new_efb_width, new_efb_height) = CalculateTargetScale(EFB_WIDTH, EFB_HEIGHT);

  if (new_efb_width != m_target_width || new_efb_height != m_target_height)
  {
    m_target_width = new_efb_width;
    m_target_height = new_efb_height;
    PixelShaderManager::SetEfbScaleChanged(EFBToScaledXf(1), EFBToScaledYf(1));
    return true;
  }
  return false;
}

std::tuple<TargetRectangle, TargetRectangle>
Renderer::ConvertStereoRectangle(const TargetRectangle& rc) const
{
  // Resize target to half its original size
  TargetRectangle draw_rc = rc;
  if (g_ActiveConfig.iStereoMode == STEREO_TAB)
  {
    // The height may be negative due to flipped rectangles
    int height = rc.bottom - rc.top;
    draw_rc.top += height / 4;
    draw_rc.bottom -= height / 4;
  }
  else
  {
    int width = rc.right - rc.left;
    draw_rc.left += width / 4;
    draw_rc.right -= width / 4;
  }

  // Create two target rectangle offset to the sides of the backbuffer
  TargetRectangle left_rc = draw_rc;
  TargetRectangle right_rc = draw_rc;
  if (g_ActiveConfig.iStereoMode == STEREO_TAB)
  {
    left_rc.top -= m_backbuffer_height / 4;
    left_rc.bottom -= m_backbuffer_height / 4;
    right_rc.top += m_backbuffer_height / 4;
    right_rc.bottom += m_backbuffer_height / 4;
  }
  else
  {
    left_rc.left -= m_backbuffer_width / 4;
    left_rc.right -= m_backbuffer_width / 4;
    right_rc.left += m_backbuffer_width / 4;
    right_rc.right += m_backbuffer_width / 4;
  }

  return std::make_tuple(left_rc, right_rc);
}

void Renderer::SaveScreenshot(const std::string& filename, bool wait_for_completion)
{
  // We must not hold the lock while waiting for the screenshot to complete.
  {
    std::lock_guard<std::mutex> lk(m_screenshot_lock);
    m_screenshot_name = filename;
    m_screenshot_request.Set();
  }

  if (wait_for_completion)
  {
    // This is currently only used by Android, and it was using a wait time of 2 seconds.
    m_screenshot_completed.WaitFor(std::chrono::seconds(2));
  }
}

bool Renderer::CheckForHostConfigChanges()
{
  ShaderHostConfig new_host_config = ShaderHostConfig::GetCurrent();
  if (new_host_config.bits == m_last_host_config_bits)
    return false;

  OSD::AddMessage("Video config changed, reloading shaders.", OSD::Duration::NORMAL);
  m_last_host_config_bits = new_host_config.bits;
  return true;
}

// Create On-Screen-Messages
void Renderer::DrawDebugText()
{
  std::string final_yellow, final_cyan;

  if (g_ActiveConfig.bShowFPS || SConfig::GetInstance().m_ShowFrameCount)
  {
    if (g_ActiveConfig.bShowFPS)
      final_cyan += StringFromFormat("FPS: %.2f", m_fps_counter.GetFPS());

    if (g_ActiveConfig.bShowFPS && SConfig::GetInstance().m_ShowFrameCount)
      final_cyan += " - ";
    if (SConfig::GetInstance().m_ShowFrameCount)
    {
      final_cyan += StringFromFormat("Frame: %" PRIu64, Movie::GetCurrentFrame());
      if (Movie::IsPlayingInput())
        final_cyan += StringFromFormat("\nInput: %" PRIu64 " / %" PRIu64,
                                       Movie::GetCurrentInputCount(), Movie::GetTotalInputCount());
    }

    final_cyan += "\n";
    final_yellow += "\n";
  }

  if (SConfig::GetInstance().m_ShowLag)
  {
    final_cyan += StringFromFormat("Lag: %" PRIu64 "\n", Movie::GetCurrentLagCount());
    final_yellow += "\n";
  }

  if (SConfig::GetInstance().m_ShowInputDisplay)
  {
    final_cyan += Movie::GetInputDisplay();
    final_yellow += "\n";
  }

  if (SConfig::GetInstance().m_ShowRTC)
  {
    final_cyan += Movie::GetRTCDisplay();
    final_yellow += "\n";
  }

  // OSD Menu messages
  if (OSDChoice > 0)
  {
    OSDTime = Common::Timer::GetTimeMs() + 3000;
    OSDChoice = -OSDChoice;
  }

  if ((u32)OSDTime > Common::Timer::GetTimeMs())
  {
    std::string res_text;
    switch (g_ActiveConfig.iEFBScale)
    {
    case SCALE_AUTO:
      res_text = "Auto (fractional)";
      break;
    case SCALE_AUTO_INTEGRAL:
      res_text = "Auto (integral)";
      break;
    case SCALE_1X:
      res_text = "Native";
      break;
    case SCALE_1_5X:
      res_text = "1.5x";
      break;
    case SCALE_2X:
      res_text = "2x";
      break;
    case SCALE_2_5X:
      res_text = "2.5x";
      break;
    default:
      res_text = StringFromFormat("%dx", g_ActiveConfig.iEFBScale - 3);
      break;
    }
    const char* ar_text = "";
    switch (g_ActiveConfig.iAspectRatio)
    {
    case ASPECT_AUTO:
      ar_text = "Auto";
      break;
    case ASPECT_STRETCH:
      ar_text = "Stretch";
      break;
    case ASPECT_ANALOG:
      ar_text = "Force 4:3";
      break;
    case ASPECT_ANALOG_WIDE:
      ar_text = "Force 16:9";
    }

    const char* const efbcopy_text = g_ActiveConfig.bSkipEFBCopyToRam ? "to Texture" : "to RAM";
    const char* const xfbcopy_text = g_ActiveConfig.bSkipXFBCopyToRam ? "to Texture" : "to RAM";

    // The rows
    const std::string lines[] = {
        std::string("Internal Resolution: ") + res_text,
        std::string("Aspect Ratio: ") + ar_text + (g_ActiveConfig.bCrop ? " (crop)" : ""),
        std::string("Copy EFB: ") + efbcopy_text,
        std::string("Fog: ") + (g_ActiveConfig.bDisableFog ? "Disabled" : "Enabled"),
        SConfig::GetInstance().m_EmulationSpeed <= 0 ?
            "Speed Limit: Unlimited" :
            StringFromFormat("Speed Limit: %li%%",
                             std::lround(SConfig::GetInstance().m_EmulationSpeed * 100.f)),
        std::string("Copy XFB: ") + xfbcopy_text,
    };

    enum
    {
      lines_count = sizeof(lines) / sizeof(*lines)
    };

    // The latest changed setting in yellow
    for (int i = 0; i != lines_count; ++i)
    {
      if (OSDChoice == -i - 1)
        final_yellow += lines[i];
      final_yellow += '\n';
    }

    // The other settings in cyan
    for (int i = 0; i != lines_count; ++i)
    {
      if (OSDChoice != -i - 1)
        final_cyan += lines[i];
      final_cyan += '\n';
    }
  }

  final_cyan += Common::Profiler::ToString();

  if (g_ActiveConfig.bOverlayStats)
    final_cyan += Statistics::ToString();

  if (g_ActiveConfig.bOverlayProjStats)
    final_cyan += Statistics::ToStringProj();

  // and then the text
  RenderText(final_cyan, 20, 20, 0xFF00FFFF);
  RenderText(final_yellow, 20, 20, 0xFFFFFF00);
}

float Renderer::CalculateDrawAspectRatio() const
{
  if (g_ActiveConfig.iAspectRatio == ASPECT_STRETCH)
  {
    // If stretch is enabled, we prefer the aspect ratio of the window.
    return (static_cast<float>(m_backbuffer_width) / static_cast<float>(m_backbuffer_height));
  }

  // The rendering window aspect ratio as a proportion of the 4:3 or 16:9 ratio
  if (g_ActiveConfig.iAspectRatio == ASPECT_ANALOG_WIDE ||
      (g_ActiveConfig.iAspectRatio != ASPECT_ANALOG && m_aspect_wide))
  {
    return AspectToWidescreen(VideoInterface::GetAspectRatio());
  }
  else
  {
    return VideoInterface::GetAspectRatio();
  }
}

std::tuple<float, float> Renderer::ScaleToDisplayAspectRatio(const int width,
                                                             const int height) const
{
  // Scale either the width or height depending the content aspect ratio.
  // This way we preserve as much resolution as possible when scaling.
  float scaled_width = static_cast<float>(width);
  float scaled_height = static_cast<float>(height);
  const float draw_aspect = CalculateDrawAspectRatio();
  if (scaled_width / scaled_height >= draw_aspect)
    scaled_height = scaled_width / draw_aspect;
  else
    scaled_width = scaled_height * draw_aspect;
  return std::make_tuple(scaled_width, scaled_height);
}

TargetRectangle Renderer::CalculateFrameDumpDrawRectangle() const
{
  // No point including any borders in the frame dump image, since they'd have to be cropped anyway.
  TargetRectangle rc;
  rc.left = 0;
  rc.top = 0;

  // If full-resolution frame dumping is disabled, just use the window draw rectangle.
  if (!g_ActiveConfig.bInternalResolutionFrameDumps)
  {
    // But still remove the borders, since the caller expects this.
    rc.right = m_target_rectangle.GetWidth();
    rc.bottom = m_target_rectangle.GetHeight();
    return rc;
  }

  // Grab the dimensions of the EFB textures, we scale either of these depending on the ratio.
  u32 efb_width, efb_height;
  std::tie(efb_width, efb_height) = g_framebuffer_manager->GetTargetSize();

  float draw_width, draw_height;
  std::tie(draw_width, draw_height) = ScaleToDisplayAspectRatio(efb_width, efb_height);

  rc.right = static_cast<int>(std::ceil(draw_width));
  rc.bottom = static_cast<int>(std::ceil(draw_height));
  return rc;
}

void Renderer::UpdateDrawRectangle()
{
  // The rendering window size
  const float win_width = static_cast<float>(m_backbuffer_width);
  const float win_height = static_cast<float>(m_backbuffer_height);

  // Update aspect ratio hack values
  // Won't take effect until next frame
  // Don't know if there is a better place for this code so there isn't a 1 frame delay
  if (g_ActiveConfig.bWidescreenHack)
  {
    float source_aspect = VideoInterface::GetAspectRatio();
    if (m_aspect_wide)
      source_aspect = AspectToWidescreen(source_aspect);
    float target_aspect;

    switch (g_ActiveConfig.iAspectRatio)
    {
    case ASPECT_STRETCH:
      target_aspect = win_width / win_height;
      break;
    case ASPECT_ANALOG:
      target_aspect = VideoInterface::GetAspectRatio();
      break;
    case ASPECT_ANALOG_WIDE:
      target_aspect = AspectToWidescreen(VideoInterface::GetAspectRatio());
      break;
    default:
      // ASPECT_AUTO
      target_aspect = source_aspect;
      break;
    }

    float adjust = source_aspect / target_aspect;
    if (adjust > 1)
    {
      // Vert+
      g_Config.fAspectRatioHackW = 1;
      g_Config.fAspectRatioHackH = 1 / adjust;
    }
    else
    {
      // Hor+
      g_Config.fAspectRatioHackW = adjust;
      g_Config.fAspectRatioHackH = 1;
    }
  }
  else
  {
    // Hack is disabled
    g_Config.fAspectRatioHackW = 1;
    g_Config.fAspectRatioHackH = 1;
  }

  float draw_width, draw_height, crop_width, crop_height;

  // get the picture aspect ratio
  draw_width = crop_width = CalculateDrawAspectRatio();
  draw_height = crop_height = 1;

  // crop the picture to a standard aspect ratio
  if (g_ActiveConfig.bCrop && g_ActiveConfig.iAspectRatio != ASPECT_STRETCH)
  {
    float expected_aspect = (g_ActiveConfig.iAspectRatio == ASPECT_ANALOG_WIDE ||
                             (g_ActiveConfig.iAspectRatio != ASPECT_ANALOG && m_aspect_wide)) ?
                                (16.0f / 9.0f) :
                                (4.0f / 3.0f);
    if (crop_width / crop_height >= expected_aspect)
    {
      // the picture is flatter than it should be
      crop_width = crop_height * expected_aspect;
    }
    else
    {
      // the picture is skinnier than it should be
      crop_height = crop_width / expected_aspect;
    }
  }

  // scale the picture to fit the rendering window
  if (win_width / win_height >= crop_width / crop_height)
  {
    // the window is flatter than the picture
    draw_width *= win_height / crop_height;
    crop_width *= win_height / crop_height;
    draw_height *= win_height / crop_height;
    crop_height = win_height;
  }
  else
  {
    // the window is skinnier than the picture
    draw_width *= win_width / crop_width;
    draw_height *= win_width / crop_width;
    crop_height *= win_width / crop_width;
    crop_width = win_width;
  }

  // ensure divisibility by 4 to make it compatible with all the video encoders
  draw_width = std::ceil(draw_width) - static_cast<int>(std::ceil(draw_width)) % 4;
  draw_height = std::ceil(draw_height) - static_cast<int>(std::ceil(draw_height)) % 4;

  m_target_rectangle.left = static_cast<int>(std::round(win_width / 2.0 - draw_width / 2.0));
  m_target_rectangle.top = static_cast<int>(std::round(win_height / 2.0 - draw_height / 2.0));
  m_target_rectangle.right = m_target_rectangle.left + static_cast<int>(draw_width);
  m_target_rectangle.bottom = m_target_rectangle.top + static_cast<int>(draw_height);
}

void Renderer::SetWindowSize(int width, int height)
{
  width = std::max(width, 1);
  height = std::max(height, 1);

  // Scale the window size by the EFB scale.
  std::tie(width, height) = CalculateTargetScale(width, height);

  float scaled_width, scaled_height;
  std::tie(scaled_width, scaled_height) = ScaleToDisplayAspectRatio(width, height);

  if (g_ActiveConfig.bCrop)
  {
    // Force 4:3 or 16:9 by cropping the image.
    float current_aspect = scaled_width / scaled_height;
    float expected_aspect = (g_ActiveConfig.iAspectRatio == ASPECT_ANALOG_WIDE ||
                             (g_ActiveConfig.iAspectRatio != ASPECT_ANALOG && m_aspect_wide)) ?
                                (16.0f / 9.0f) :
                                (4.0f / 3.0f);
    if (current_aspect > expected_aspect)
    {
      // keep height, crop width
      scaled_width = scaled_height * expected_aspect;
    }
    else
    {
      // keep width, crop height
      scaled_height = scaled_width / expected_aspect;
    }
  }

  width = static_cast<int>(std::ceil(scaled_width));
  height = static_cast<int>(std::ceil(scaled_height));

  // UpdateDrawRectangle() makes sure that the rendered image is divisible by four for video
  // encoders, so do that here too to match it
  width -= width % 4;
  height -= height % 4;

  // Track the last values of width/height to avoid sending a window resize event every frame.
  if (width != m_last_window_request_width || height != m_last_window_request_height)
  {
    m_last_window_request_width = width;
    m_last_window_request_height = height;
    Host_RequestRenderWindowSize(width, height);
  }
}

void Renderer::CheckFifoRecording()
{
  bool wasRecording = g_bRecordFifoData;
  g_bRecordFifoData = FifoRecorder::GetInstance().IsRecording();

  if (g_bRecordFifoData)
  {
    if (!wasRecording)
    {
      RecordVideoMemory();
    }

    FifoRecorder::GetInstance().EndFrame(CommandProcessor::fifo.CPBase,
                                         CommandProcessor::fifo.CPEnd);
  }
}

void Renderer::RecordVideoMemory()
{
  const u32* bpmem_ptr = reinterpret_cast<const u32*>(&bpmem);
  u32 cpmem[256] = {};
  // The FIFO recording format splits XF memory into xfmem and xfregs; follow
  // that split here.
  const u32* xfmem_ptr = reinterpret_cast<const u32*>(&xfmem);
  const u32* xfregs_ptr = reinterpret_cast<const u32*>(&xfmem) + FifoDataFile::XF_MEM_SIZE;
  u32 xfregs_size = sizeof(XFMemory) / 4 - FifoDataFile::XF_MEM_SIZE;

  FillCPMemoryArray(cpmem);

  FifoRecorder::GetInstance().SetVideoMemory(bpmem_ptr, cpmem, xfmem_ptr, xfregs_ptr, xfregs_size,
                                             texMem);
}

void Renderer::Swap(u32 xfbAddr, u32 fbWidth, u32 fbStride, u32 fbHeight, const EFBRectangle& rc,
                    u64 ticks, float Gamma)
{
  // Heuristic to detect if a GameCube game is in 16:9 anamorphic widescreen mode.
  if (!SConfig::GetInstance().bWii)
  {
    size_t flush_count_4_3, flush_count_anamorphic;
    std::tie(flush_count_4_3, flush_count_anamorphic) =
        g_vertex_manager->ResetFlushAspectRatioCount();
    size_t flush_total = flush_count_4_3 + flush_count_anamorphic;

    // Modify the threshold based on which aspect ratio we're already using: if
    // the game's in 4:3, it probably won't switch to anamorphic, and vice-versa.
    if (m_aspect_wide)
      m_aspect_wide = !(flush_count_4_3 > 0.75 * flush_total);
    else
      m_aspect_wide = flush_count_anamorphic > 0.75 * flush_total;
  }

  // The FinishFrameData call here is necessary even after frame dumping is stopped.
  // If left out, screenshots are "one frame" behind, as an extra frame is dumped and buffered.
  FinishFrameData();
  if (IsFrameDumping())
  {
    auto result = m_last_xfb_texture->Map();
    if (result.has_value())
    {
      auto raw_data = result.value();
      DumpFrameData(raw_data.data, raw_data.width, raw_data.height, raw_data.stride,
                    AVIDump::FetchState(ticks));
    }
  }

  if (xfbAddr && fbWidth && fbStride && fbHeight)
  {
    static const int force_safe_texture_cache_hash = 0;
    // Get the current XFB from texture cache
    auto* xfb_entry = g_texture_cache->GetTexture(xfbAddr, fbWidth, fbHeight, GX_CTF_XFB,
                                                  force_safe_texture_cache_hash);

    if (xfb_entry)
    {
      // TODO, check if xfb_entry is a duplicate of the previous frame and skip SwapImpl

      m_last_xfb_texture = xfb_entry->texture.get();

      // TODO: merge more generic parts into VideoCommon
      g_renderer->SwapImpl(xfb_entry->texture.get(), rc, ticks, Gamma);
    }
  }

  if (m_xfb_written)
    m_fps_counter.Update();

  frameCount++;
  GFX_DEBUGGER_PAUSE_AT(NEXT_FRAME, true);

  // Begin new frame
  // Set default viewport and scissor, for the clear to work correctly
  // New frame
  stats.ResetFrame();

  Core::Callback_VideoCopiedToXFB(m_xfb_written || !g_ActiveConfig.bSkipXFBCopyToRam);
  m_xfb_written = false;
}

bool Renderer::IsFrameDumping()
{
  if (m_screenshot_request.IsSet())
    return true;

#if defined(HAVE_FFMPEG)
  if (SConfig::GetInstance().m_DumpFrames)
    return true;
#endif

  ShutdownFrameDumping();
  return false;
}

void Renderer::ShutdownFrameDumping()
{
  if (!m_frame_dump_thread_running.IsSet())
    return;

  FinishFrameData();
  m_frame_dump_thread_running.Clear();
  m_frame_dump_start.Set();
}

void Renderer::DumpFrameData(const u8* data, int w, int h, int stride, const AVIDump::Frame& state)
{
  m_frame_dump_config = FrameDumpConfig{ m_last_xfb_texture, data, w, h, stride, state };

  if (!m_frame_dump_thread_running.IsSet())
  {
    if (m_frame_dump_thread.joinable())
      m_frame_dump_thread.join();
    m_frame_dump_thread_running.Set();
    m_frame_dump_thread = std::thread(&Renderer::RunFrameDumps, this);
  }

  m_frame_dump_start.Set();
  m_frame_dump_frame_running = true;
}

void Renderer::FinishFrameData()
{
  if (!m_frame_dump_frame_running)
    return;

  m_frame_dump_done.Wait();
  m_frame_dump_frame_running = false;
  m_frame_dump_config.texture->Unmap();
}

void Renderer::RunFrameDumps()
{
  Common::SetCurrentThreadName("FrameDumping");
  bool dump_to_avi = !g_ActiveConfig.bDumpFramesAsImages;
  bool frame_dump_started = false;

// If Dolphin was compiled without libav, we only support dumping to images.
#if !defined(HAVE_FFMPEG)
  if (dump_to_avi)
  {
    WARN_LOG(VIDEO, "AVI frame dump requested, but Dolphin was compiled without libav. "
                    "Frame dump will be saved as images instead.");
    dump_to_avi = false;
  }
#endif

  while (true)
  {
    m_frame_dump_start.Wait();
    if (!m_frame_dump_thread_running.IsSet())
      break;

    auto config = m_frame_dump_config;

    // Save screenshot
    if (m_screenshot_request.TestAndClear())
    {
      std::lock_guard<std::mutex> lk(m_screenshot_lock);

      if (TextureToPng(config.data, config.stride, m_screenshot_name, config.width, config.height,
                       false))
        OSD::AddMessage("Screenshot saved to " + m_screenshot_name);

      // Reset settings
      m_screenshot_name.clear();
      m_screenshot_completed.Set();
    }

    if (SConfig::GetInstance().m_DumpFrames)
    {
      if (!frame_dump_started)
      {
        if (dump_to_avi)
          frame_dump_started = StartFrameDumpToAVI(config);
        else
          frame_dump_started = StartFrameDumpToImage(config);

        // Stop frame dumping if we fail to start.
        if (!frame_dump_started)
          SConfig::GetInstance().m_DumpFrames = false;
      }

      // If we failed to start frame dumping, don't write a frame.
      if (frame_dump_started)
      {
        if (dump_to_avi)
          DumpFrameToAVI(config);
        else
          DumpFrameToImage(config);
      }
    }

    m_frame_dump_done.Set();
  }

  if (frame_dump_started)
  {
    // No additional cleanup is needed when dumping to images.
    if (dump_to_avi)
      StopFrameDumpToAVI();
  }
}

#if defined(HAVE_FFMPEG)

bool Renderer::StartFrameDumpToAVI(const FrameDumpConfig& config)
{
  return AVIDump::Start(config.width, config.height);
}

void Renderer::DumpFrameToAVI(const FrameDumpConfig& config)
{
  AVIDump::AddFrame(config.data, config.width, config.height, config.stride, config.state);
}

void Renderer::StopFrameDumpToAVI()
{
  AVIDump::Stop();
}

#else

bool Renderer::StartFrameDumpToAVI(const FrameDumpConfig& config)
{
  return false;
}

void Renderer::DumpFrameToAVI(const FrameDumpConfig& config)
{
}

void Renderer::StopFrameDumpToAVI()
{
}

#endif  // defined(HAVE_FFMPEG)

std::string Renderer::GetFrameDumpNextImageFileName() const
{
  return StringFromFormat("%sframedump_%u.png", File::GetUserPath(D_DUMPFRAMES_IDX).c_str(),
                          m_frame_dump_image_counter);
}

bool Renderer::StartFrameDumpToImage(const FrameDumpConfig& config)
{
  m_frame_dump_image_counter = 1;
  if (!SConfig::GetInstance().m_DumpFramesSilent)
  {
    // Only check for the presence of the first image to confirm overwriting.
    // A previous run will always have at least one image, and it's safe to assume that if the user
    // has allowed the first image to be overwritten, this will apply any remaining images as well.
    std::string filename = GetFrameDumpNextImageFileName();
    if (File::Exists(filename))
    {
      if (!AskYesNoT("Frame dump image(s) '%s' already exists. Overwrite?", filename.c_str()))
        return false;
    }
  }

  return true;
}

void Renderer::DumpFrameToImage(const FrameDumpConfig& config)
{
  std::string filename = GetFrameDumpNextImageFileName();
  TextureToPng(config.data, config.stride, filename, config.width, config.height, false);
  m_frame_dump_image_counter++;
}

bool Renderer::UseVertexDepthRange() const
{
  // We can't compute the depth range in the vertex shader if we don't support depth clamp.
  if (!g_ActiveConfig.backend_info.bSupportsDepthClamp)
    return false;

  // We need a full depth range if a ztexture is used.
  if (bpmem.ztex2.type != ZTEXTURE_DISABLE && !bpmem.zcontrol.early_ztest)
    return true;

  // If an inverted depth range is unsupported, we also need to check if the range is inverted.
  if (!g_ActiveConfig.backend_info.bSupportsReversedDepthRange && xfmem.viewport.zRange < 0.0f)
    return true;

  // If an oversized depth range or a ztexture is used, we need to calculate the depth range
  // in the vertex shader.
  return fabs(xfmem.viewport.zRange) > 16777215.0f || fabs(xfmem.viewport.farZ) > 16777215.0f;
}
