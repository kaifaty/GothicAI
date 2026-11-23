#pragma once

#include "../pixmapcodec.h"

namespace Tempest {

class PixmapCodecDDS : public PixmapCodec {
  public:
    PixmapCodecDDS();

    static uint8_t* loadMipTail(IDevice& f, uint32_t firstMip, uint32_t minSize,
                               uint32_t& w, uint32_t& h, TextureFormat& frm,
                               uint32_t& mipCnt, size_t& dataSz);

  protected:
    bool     testFormat(const Context& c) const override;
    uint8_t* load(PixmapCodec::Context &c,uint32_t& w,uint32_t& h,TextureFormat& frm,uint32_t& mipCnt,size_t& dataSz,uint32_t& bpp) const override;
    bool     save(ODevice& f,const char* ext, const uint8_t *data, size_t dataSz, uint32_t w, uint32_t h, TextureFormat frm) const override;
  };

}

