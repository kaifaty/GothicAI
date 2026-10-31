#include "pixmapcodecdds.h"

#include <Tempest/IDevice>
#include <Tempest/Except>

#include <algorithm>
#include <cstring>
#include <limits>

#include "../ddsdef.h"

using namespace Tempest;

PixmapCodecDDS::PixmapCodecDDS() {  
  }

bool PixmapCodecDDS::testFormat(const PixmapCodec::Context &c) const {
  char buf[4]={};
  return c.peek(buf,4)==4 && std::memcmp(buf,"DDS ",4)==0;
  }

uint8_t* PixmapCodecDDS::load(PixmapCodec::Context &c, uint32_t &ow, uint32_t &oh,
                              TextureFormat& frm, uint32_t& mipCnt, size_t& dataSz, uint32_t &bpp) const {
  bpp = 0;
  return loadMipTail(c.device,0,0,ow,oh,frm,mipCnt,dataSz);
  }

uint8_t* PixmapCodecDDS::loadMipTail(IDevice& f, uint32_t firstMip, uint32_t minSize,
                                    uint32_t& ow, uint32_t& oh, TextureFormat& frm,
                                    uint32_t& mipCnt, size_t& dataSz) {
  using namespace Tempest::Detail;
  const auto invalid = []() {
    throw std::system_error(Tempest::SystemErrc::UnableToLoadAsset);
    };

  uint8_t head[4]={};
  if(f.read(head,4)!=4 || std::memcmp(head,"DDS ",4)!=0)
    invalid();

  DDSURFACEDESC2 ddsd={};
  if(f.read(&ddsd,sizeof(ddsd))!=sizeof(ddsd))
    invalid();
  if(ddsd.dwSize!=sizeof(ddsd) || ddsd.ddpfPixelFormat.dwSize!=sizeof(DDPIXELFORMAT) ||
     ddsd.dwWidth==0 || ddsd.dwHeight==0 || ddsd.dwDepth>1 ||
     (ddsd.ddsCaps.dwCaps2 & (0xfe00u | 0x200000u))!=0)
    invalid(); // Only a single 2D image is supported, not cube/volume textures.
  ow = ddsd.dwWidth;
  oh = ddsd.dwHeight;

  switch(ddsd.ddpfPixelFormat.dwFourCC) {
    case FOURCC_DXT1:
      frm = TextureFormat::DXT1;
      break;

    case FOURCC_DXT3:
      frm = TextureFormat::DXT3;
      break;

    case FOURCC_DXT5:
      frm = TextureFormat::DXT5;
      break;

    default:
      invalid();
    }

  mipCnt = std::max(1u, ddsd.dwMipMapCount);
  uint32_t maxMips = 1;
  for(uint32_t side=std::max(ow,oh); side>1; side/=2)
    ++maxMips;
  if(mipCnt>maxMips)
    invalid();

  // The retained base level must cover at least a BC block and minSize on
  // both axes. The rest of its original chain is kept, including 1x1 levels.
  const uint32_t floor = std::max(4u,minSize);
  uint32_t selected = 0;
  uint32_t sw = ow, sh = oh;
  while(selected<firstMip && selected+1<mipCnt && sw/2>=floor && sh/2>=floor) {
    ++selected;
    sw/=2;
    sh/=2;
    }

  const size_t blocksize = Pixmap::blockSizeForFormat(frm);
  size_t bufferSize = 0;
  size_t skipSize = 0;

  size_t w = size_t(ow), h = size_t(oh);
  for(size_t i=0; i<mipCnt; i++){
    const size_t bw = (w+3)/4, bh = (h+3)/4;
    if(bw>std::numeric_limits<size_t>::max()/bh/blocksize)
      invalid();
    const size_t bytes = bw*bh*blocksize;
    if(bytes>std::numeric_limits<size_t>::max()-bufferSize)
      invalid();
    bufferSize += bytes;
    if(i<selected)
      skipSize += bytes;
    w = std::max<size_t>(1,w/2);
    h = std::max<size_t>(1,h/2);
    }

  const size_t headerSize = sizeof(head)+sizeof(ddsd);
  const size_t fileSize = f.size();
  if(fileSize<headerSize || bufferSize>fileSize-headerSize)
    invalid();
  if(firstMip>0 && selected==0)
    return nullptr;
  if(f.seek(skipSize)!=skipSize)
    invalid();

  bufferSize -= skipSize;
  uint8_t* ddsv = reinterpret_cast<uint8_t*>(std::malloc(bufferSize));
  if(!ddsv)
    throw std::bad_alloc();
  if(f.read(ddsv,bufferSize)!=bufferSize) {
    std::free(ddsv);
    invalid();
    }

  ow = sw;
  oh = sh;
  mipCnt -= selected;
  dataSz = bufferSize;
  return ddsv;
  }

bool PixmapCodecDDS::save(ODevice &, const char* /*ext*/, const uint8_t *data, size_t dataSz,
                          uint32_t w, uint32_t h, TextureFormat frm) const {
  return false;
  }
