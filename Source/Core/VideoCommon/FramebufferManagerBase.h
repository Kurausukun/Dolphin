// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <list>
#include <memory>
#include <utility>

#include "Common/CommonTypes.h"
#include "VideoCommon/VideoCommon.h"

inline bool AddressRangesOverlap(u32 aLower, u32 aUpper, u32 bLower, u32 bUpper)
{
  return !((aLower >= bUpper) || (bLower >= aUpper));
}

struct XFBSourceBase
{
  virtual ~XFBSourceBase() {}

  virtual void CopyEFB(float Gamma) = 0;

  u32 srcAddr;
  u32 srcWidth;
  u32 srcHeight;

  unsigned int texWidth;
  unsigned int texHeight;

  // TODO: only used by OGL
  TargetRectangle sourceRc;
};

class FramebufferManagerBase
{
public:
  enum
  {
    // There may be multiple XFBs in GameCube RAM. This is the maximum number to
    // virtualize.
    MAX_VIRTUAL_XFB = 8
  };

  FramebufferManagerBase();
  virtual ~FramebufferManagerBase();

  static void CopyToXFB(u32 xfbAddr, u32 fbStride, u32 fbHeight, const EFBRectangle& sourceRc,
                        float Gamma);

  static unsigned int GetEFBLayers() { return m_EFBLayers; }
  virtual std::pair<u32, u32> GetTargetSize() const = 0;

protected:
  struct VirtualXFB
  {
    VirtualXFB() {}
    // Address and size in GameCube RAM
    u32 xfbAddr = 0;
    u32 xfbWidth = 0;
    u32 xfbHeight = 0;

    std::unique_ptr<XFBSourceBase> xfbSource;
  };

  typedef std::list<VirtualXFB> VirtualXFBListType;

  static unsigned int m_EFBLayers;

private:
  virtual std::unique_ptr<XFBSourceBase>
  CreateXFBSource(unsigned int target_width, unsigned int target_height, unsigned int layers) = 0;

  static VirtualXFBListType::iterator FindVirtualXFB(u32 xfbAddr, u32 width, u32 height);

  static void ReplaceVirtualXFB();

  // TODO: merge these virtual funcs, they are nearly all the same
  virtual void CopyToRealXFB(u32 xfbAddr, u32 fbStride, u32 fbHeight, const EFBRectangle& sourceRc,
                             float Gamma = 1.0f) = 0;
  static void CopyToVirtualXFB(u32 xfbAddr, u32 fbWidth, u32 fbHeight, const EFBRectangle& sourceRc,
                               float Gamma = 1.0f);

  static std::unique_ptr<XFBSourceBase> m_realXFBSource;  // Only used in Real XFB mode
  static VirtualXFBListType m_virtualXFBList;             // Only used in Virtual XFB mode

  static std::array<const XFBSourceBase*, MAX_VIRTUAL_XFB> m_overlappingXFBArray;
};

extern std::unique_ptr<FramebufferManagerBase> g_framebuffer_manager;
