// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/GL/GLUtil.h"

#include "VideoCommon/AbstractTexture.h"

namespace OGL
{
class OGLTexture final : public AbstractTexture
{
public:
  explicit OGLTexture(const TextureConfig& tex_config);
  ~OGLTexture();

  void Bind(unsigned int stage) override;

  void CopyRectangleFromTexture(const AbstractTexture* source,
                                const MathUtil::Rectangle<int>& srcrect,
                                const MathUtil::Rectangle<int>& dstrect) override;
  void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
            size_t buffer_size) override;

  GLuint GetRawTexIdentifier() const;
  GLuint GetFramebuffer() const;

  static void DisableStage(unsigned int stage);
  static void SetStage();

private:
  std::optional<RawTextureInfo> MapFullImpl() override;
  std::optional<RawTextureInfo> MapRegionImpl(u32 level, u32 x, u32 y, u32 width, 
                                              u32 height) override;
  GLuint m_texId;
  GLuint m_framebuffer = 0;
  std::vector<u8> m_staging_data;
};

}  // namespace OGL
