#pragma once

#include "VideoBackends/Software/EfbCopy.h"
#include "VideoBackends/Software/SWTexture.h"
#include "VideoCommon/TextureCacheBase.h"

#include <memory>

namespace SW
{

class TextureCache : public TextureCacheBase
{
public:
  bool CompileShaders() override { return true; }
  void DeleteShaders() override {}
  void ConvertTexture(TCacheEntry* entry, TCacheEntry* unconverted, void* palette,
    TlutFormat format) override
  {
  }
  void CopyEFB(u8* dst, const EFBCopyFormat& format, u32 native_width, u32 bytes_per_row,
    u32 num_blocks_y, u32 memory_stride, bool is_depth_copy,
    const EFBRectangle& src_rect, bool scale_by_half, float y_scale) override
  {
    EfbCopy::CopyEfb();
  }

private:
  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config) override
  {
    return std::make_unique<SWTexture>(config);
  }
  
  void CopyEFBToCacheEntry(TCacheEntry* entry, bool is_depth_copy, const EFBRectangle& src_rect,
                           bool scale_by_half, unsigned int cbuf_id, const float* colmat) override
  {
    EfbCopy::CopyEfb();
  }
};

} // namespace SW
