// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>

#include "Common/GL/GLUtil.h"
#include "VideoCommon/RenderBase.h"

struct XFBSourceBase;

namespace OGL
{
void ClearEFBCache();

enum GLSL_VERSION
{
  GLSL_130,
  GLSL_140,
  GLSL_150,
  GLSL_330,
  GLSL_400,  // and above
  GLSL_430,
  GLSLES_300,  // GLES 3.0
  GLSLES_310,  // GLES 3.1
  GLSLES_320,  // GLES 3.2
};
enum class ES_TEXBUF_TYPE
{
  TEXBUF_NONE,
  TEXBUF_CORE,
  TEXBUF_OES,
  TEXBUF_EXT
};

// ogl-only config, so not in VideoConfig.h
struct VideoConfig
{
  bool bSupportsGLSLCache;
  bool bSupportsGLPinnedMemory;
  bool bSupportsGLSync;
  bool bSupportsGLBaseVertex;
  bool bSupportsGLBufferStorage;
  bool bSupportsMSAA;
  GLSL_VERSION eSupportedGLSLVersion;
  bool bSupportViewportFloat;
  bool bSupportsAEP;
  bool bSupportsDebug;
  bool bSupportsCopySubImage;
  u8 SupportedESPointSize;
  ES_TEXBUF_TYPE SupportedESTextureBuffer;
  bool bSupportsTextureStorage;
  bool bSupports2DTextureStorageMultisample;
  bool bSupports3DTextureStorageMultisample;
  bool bSupportsConservativeDepth;
  bool bSupportsImageLoadStore;
  bool bSupportsAniso;
  bool bSupportsBitfield;

  const char* gl_vendor;
  const char* gl_renderer;
  const char* gl_version;

  s32 max_samples;
};
extern VideoConfig g_ogl_config;

class Renderer : public ::Renderer
{
public:
  Renderer();
  ~Renderer() override;

  void Init();
  void Shutdown();

  void SetBlendMode(bool forceUpdate) override;
  void SetScissorRect(const EFBRectangle& rc) override;
  void SetGenerationMode() override;
  void SetDepthMode() override;
  void SetSamplerState(int stage, int texindex, bool custom_tex) override;
  void SetInterlacingMode() override;
  void SetViewport() override;

  void RenderText(const std::string& text, int left, int top, u32 color) override;

  u32 AccessEFB(EFBAccessType type, u32 x, u32 y, u32 poke_data) override;
  void PokeEFB(EFBAccessType type, const EfbPokeData* points, size_t num_points) override;

  u16 BBoxRead(int index) override;
  void BBoxWrite(int index, u16 value) override;

  void ResetAPIState() override;
  void RestoreAPIState() override;

  TargetRectangle ConvertEFBRectangle(const EFBRectangle& rc) override;

  void SwapImpl(AbstractTexture* texture, const EFBRectangle& rc, u64 ticks, float Gamma) override;

  void ClearScreen(const EFBRectangle& rc, bool colorEnable, bool alphaEnable, bool zEnable,
                   u32 color, u32 z) override;

  void ReinterpretPixelData(unsigned int convtype) override;

  void ChangeSurface(void* new_surface_handle) override;

private:
  void UpdateEFBCache(EFBAccessType type, u32 cacheRectIdx, const EFBRectangle& efbPixelRc,
                      const TargetRectangle& targetPixelRc, const void* data);

  void DrawEFB(GLuint framebuffer, const TargetRectangle& target_rc, const TargetRectangle& source_rc);

  void BlitScreen(TargetRectangle src, TargetRectangle dst, GLuint src_texture, int src_width,
                  int src_height);

  void FlushFrameDump();
  void DumpFrame(const TargetRectangle& flipped_trc, u64 ticks);
  void DumpFrameUsingFBO(const TargetRectangle& source_rc, u64 ticks);

  // Frame dumping framebuffer, we render to this, then read it back
  void PrepareFrameDumpRenderTexture(u32 width, u32 height);
  void DestroyFrameDumpResources();
  GLuint m_frame_dump_render_texture = 0;
  GLuint m_frame_dump_render_framebuffer = 0;
  u32 m_frame_dump_render_texture_width = 0;
  u32 m_frame_dump_render_texture_height = 0;

  // avi dumping state to delay one frame
  std::array<u32, 2> m_frame_dumping_pbo = {};
  std::array<bool, 2> m_frame_pbo_is_mapped = {};
  std::array<int, 2> m_last_frame_width = {};
  std::array<int, 2> m_last_frame_height = {};
  bool m_last_frame_exported = false;
  AVIDump::Frame m_last_frame_state;
};
}
