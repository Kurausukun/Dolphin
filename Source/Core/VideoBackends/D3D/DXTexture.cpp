// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cstddef>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DTexture.h"
#include "VideoBackends/D3D/D3DUtil.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3D/FramebufferManager.h"
#include "VideoBackends/D3D/GeometryShaderCache.h"
#include "VideoBackends/D3D/PixelShaderCache.h"
#include "VideoBackends/D3D/TextureCache.h"
#include "VideoBackends/D3D/VertexShaderCache.h"

#include "VideoCommon/ImageWrite.h"
#include "VideoCommon/TextureConfig.h"

namespace DX11
{
namespace
{
DXGI_FORMAT GetDXGIFormatForHostFormat(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::DXT1:
    return DXGI_FORMAT_BC1_UNORM;
  case AbstractTextureFormat::DXT3:
    return DXGI_FORMAT_BC2_UNORM;
  case AbstractTextureFormat::DXT5:
    return DXGI_FORMAT_BC3_UNORM;
  case AbstractTextureFormat::RGBA8:
  default:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}
}  // Anonymous namespace

DXTexture::DXTexture(const TextureConfig& tex_config)
    : AbstractTexture(tex_config) , m_staging_texture(nullptr)
{
  DXGI_FORMAT dxgi_format = GetDXGIFormatForHostFormat(m_config.format);
  if (m_config.rendertarget)
  {
    m_texture = D3DTexture2D::Create(
        m_config.width, m_config.height,
        (D3D11_BIND_FLAG)((int)D3D11_BIND_RENDER_TARGET | (int)D3D11_BIND_SHADER_RESOURCE),
        D3D11_USAGE_DEFAULT, dxgi_format, 1, m_config.layers);
  }
  else
  {
    const D3D11_TEXTURE2D_DESC texdesc =
        CD3D11_TEXTURE2D_DESC(dxgi_format, m_config.width, m_config.height, 1, m_config.levels,
                              D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);

    ID3D11Texture2D* pTexture;
    const HRESULT hr = D3D::device->CreateTexture2D(&texdesc, nullptr, &pTexture);
    CHECK(SUCCEEDED(hr), "Create texture of the TextureCache");

    m_texture = new D3DTexture2D(pTexture, D3D11_BIND_SHADER_RESOURCE);

    // TODO: better debug names
    D3D::SetDebugObjectName((ID3D11DeviceChild*)m_texture->GetTex(),
                            "a texture of the TextureCache");
    D3D::SetDebugObjectName((ID3D11DeviceChild*)m_texture->GetSRV(),
                            "shader resource view of a texture of the TextureCache");

    SAFE_RELEASE(pTexture);
  }
}

DXTexture::~DXTexture()
{
  m_texture->Release();
  SAFE_RELEASE(m_staging_texture);
}

D3DTexture2D* DXTexture::GetRawTexIdentifier() const
{
  return m_texture;
}

void DXTexture::Bind(unsigned int stage)
{
  D3D::stateman->SetTexture(stage, m_texture->GetSRV());
}

std::optional<AbstractTexture::RawTextureInfo> DXTexture::MapFullImpl()
{
  CD3D11_TEXTURE2D_DESC staging_texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, m_config.width, m_config.height, 1,
                                             1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);

  HRESULT hr = D3D::device->CreateTexture2D(&staging_texture_desc, nullptr, &m_staging_texture);
  if (FAILED(hr))
  {
    WARN_LOG(VIDEO, "Failed to create texture dumping readback texture: %X", static_cast<u32>(hr));
    return {};
  }

  // Copy the selected data to the staging texture
  D3D::context->CopyResource(m_staging_texture, m_texture->GetTex());

  // Map the staging texture to client memory, and encode it as a .png image.
  D3D11_MAPPED_SUBRESOURCE map;
  hr = D3D::context->Map(m_staging_texture, 0, D3D11_MAP_READ, 0, &map);
  if (FAILED(hr))
  {
    WARN_LOG(VIDEO, "Failed to map texture dumping readback texture: %X", static_cast<u32>(hr));
    return {};
  }

  return AbstractTexture::RawTextureInfo{reinterpret_cast<u8*>(map.pData), map.RowPitch,
                                         m_config.width, m_config.height};
}

std::optional<AbstractTexture::RawTextureInfo> DXTexture::MapRegionImpl(u32 level, u32 x, u32 y,
                                                                        u32 width, u32 height)
{
  CD3D11_TEXTURE2D_DESC staging_texture_desc(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1,
    1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);

  HRESULT hr = D3D::device->CreateTexture2D(&staging_texture_desc, nullptr, &m_staging_texture);
  if (FAILED(hr))
  {
    WARN_LOG(VIDEO, "Failed to create texture dumping readback texture: %X", static_cast<u32>(hr));
    return {};
  }

  // Copy the selected data to the staging texture
  CD3D11_BOX src_box(x, y, 0, width, height, 1);
  D3D::context->CopySubresourceRegion(m_staging_texture, 0, 0, 0, 0, m_texture->GetTex(),
    D3D11CalcSubresource(level, 0, m_config.levels), &src_box);

  // Map the staging texture to client memory, and encode it as a .png image.
  D3D11_MAPPED_SUBRESOURCE map;
  hr = D3D::context->Map(m_staging_texture, 0, D3D11_MAP_READ, 0, &map);
  if (FAILED(hr))
  {
    WARN_LOG(VIDEO, "Failed to map texture dumping readback texture: %X", static_cast<u32>(hr));
    return {};
  }

  return AbstractTexture::RawTextureInfo{reinterpret_cast<u8*>(map.pData), map.RowPitch,
                                         m_config.width, m_config.height};
}

void DXTexture::Unmap()
{
  if (m_staging_texture)
  {
    D3D::context->Unmap(m_staging_texture, 0);
  }
}

void DXTexture::CopyRectangleFromTexture(const AbstractTexture* source,
                                         const MathUtil::Rectangle<int>& srcrect,
                                         const MathUtil::Rectangle<int>& dstrect)
{
  const DXTexture* srcentry = static_cast<const DXTexture*>(source);
  if (srcrect.GetWidth() == dstrect.GetWidth() && srcrect.GetHeight() == dstrect.GetHeight())
  {
    D3D11_BOX srcbox;
    srcbox.left = srcrect.left;
    srcbox.top = srcrect.top;
    srcbox.right = srcrect.right;
    srcbox.bottom = srcrect.bottom;
    srcbox.front = 0;
    srcbox.back = srcentry->m_config.layers;

    D3D::context->CopySubresourceRegion(m_texture->GetTex(), 0, dstrect.left, dstrect.top, 0,
                                        srcentry->m_texture->GetTex(), 0, &srcbox);
    return;
  }
  else if (!m_config.rendertarget)
  {
    return;
  }
  g_renderer->ResetAPIState();  // reset any game specific settings

  const D3D11_VIEWPORT vp = CD3D11_VIEWPORT(float(dstrect.left), float(dstrect.top),
                                            float(dstrect.GetWidth()), float(dstrect.GetHeight()));

  D3D::stateman->UnsetTexture(m_texture->GetSRV());
  D3D::stateman->Apply();

  D3D::context->OMSetRenderTargets(1, &m_texture->GetRTV(), nullptr);
  D3D::context->RSSetViewports(1, &vp);
  D3D::SetLinearCopySampler();
  D3D11_RECT srcRC;
  srcRC.left = srcrect.left;
  srcRC.right = srcrect.right;
  srcRC.top = srcrect.top;
  srcRC.bottom = srcrect.bottom;
  D3D::drawShadedTexQuad(srcentry->m_texture->GetSRV(), &srcRC, srcentry->m_config.width,
                         srcentry->m_config.height, PixelShaderCache::GetColorCopyProgram(false),
                         VertexShaderCache::GetSimpleVertexShader(),
                         VertexShaderCache::GetSimpleInputLayout(),
                         GeometryShaderCache::GetCopyGeometryShader(), 1.0, 0);

  D3D::context->OMSetRenderTargets(1, &FramebufferManager::GetEFBColorTexture()->GetRTV(),
                                   FramebufferManager::GetEFBDepthTexture()->GetDSV());

  g_renderer->RestoreAPIState();
}

void DXTexture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                     size_t buffer_size)
{
  size_t src_pitch = CalculateHostTextureLevelPitch(m_config.format, row_length);
  D3D::context->UpdateSubresource(m_texture->GetTex(), level, nullptr, buffer,
                                  static_cast<UINT>(src_pitch), 0);
}
}  // namespace DX11
