#include "islandimage.h"

#include <squish.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace island {

namespace {

#pragma pack(push,1)
struct DdsPixelFormat {
  uint32_t size = 32, flags = 0x4 /*FOURCC*/, fourCC = 0, rgbBitCount = 0, r = 0, g = 0, b = 0, a = 0;
  };
struct DdsHeader {
  uint32_t       size = 124;
  uint32_t       flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000; // caps, height, width, format, mips, size
  uint32_t       height = 0, width = 0, linearSize = 0, depth = 0, mipMapCount = 0;
  uint32_t       reserved1[11] = {};
  DdsPixelFormat format;
  uint32_t       caps = 0x1000 | 0x400000 | 0x8; // texture, mipmap, complex
  uint32_t       caps2 = 0, caps3 = 0, caps4 = 0, reserved2 = 0;
  };
#pragma pack(pop)
static_assert(sizeof(DdsHeader)==124);

constexpr uint32_t fourCC(const char* c) {
  return uint32_t(uint8_t(c[0])) | uint32_t(uint8_t(c[1]))<<8 | uint32_t(uint8_t(c[2]))<<16 |
         uint32_t(uint8_t(c[3]))<<24;
  }

uint8_t byte(float v) {
  return uint8_t(std::lround(std::min(1.f, std::max(0.f, v))*255.f));
  }

// IEC 61966-2-1, the exact curve
float srgb(float linear) {
  linear = std::min(1.f, std::max(0.f, linear));
  return linear<=0.0031308f ? linear*12.92f : 1.055f*std::pow(linear, 1.f/2.4f)-0.055f;
  }

Image half(const Image& src, bool normal) {
  Image dst(std::max(1, src.w/2), std::max(1, src.h/2));
  for(int y=0; y<dst.h; ++y)
    for(int x=0; x<dst.w; ++x) {
      float* out = dst.at(x, y);
      for(int dy=0; dy<2; ++dy)
        for(int dx=0; dx<2; ++dx) {
          const float* in = src.at(std::min(src.w-1, x*2+dx), std::min(src.h-1, y*2+dy));
          for(int c=0; c<4; ++c)
            out[c] += in[c]*0.25f;
          }
      if(normal) {
        const float l = std::sqrt(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]);
        if(l>1e-6f) {
          out[0] /= l;
          out[1] /= l;
          out[2] /= l;
          } else {
          out[0] = out[1] = 0;
          out[2] = 1;
          }
        }
      }
  return dst;
  }

std::vector<uint8_t> bytes(const Image& image, Encoding encoding) {
  std::vector<uint8_t> out(size_t(image.w)*size_t(image.h)*4);
  for(size_t i=0; i<out.size(); i+=4) {
    const float* in = image.px.data()+i;
    switch(encoding) {
      case Encoding::Colour:
      case Encoding::ColourAlpha:
        out[i+0] = byte(srgb(in[0]));
        out[i+1] = byte(srgb(in[1]));
        out[i+2] = byte(srgb(in[2]));
        out[i+3] = encoding==Encoding::Colour ? 255 : byte(in[3]);
        break;
      case Encoding::Data:
      case Encoding::DataAlpha:
        out[i+0] = byte(in[0]);
        out[i+1] = byte(in[1]);
        out[i+2] = byte(in[2]);
        out[i+3] = encoding==Encoding::Data ? 255 : byte(in[3]);
        break;
      case Encoding::Normal:
        // the colour block carries y alone, so its two end points serve one channel
        out[i+0] = 0;
        out[i+1] = byte(in[1]*0.5f+0.5f);
        out[i+2] = 0;
        out[i+3] = byte(in[0]*0.5f+0.5f);
        break;
      }
    }
  return out;
  }

}

void parallelRows(int rows, const std::function<void(int)>& row) {
  const unsigned count = std::min(unsigned(std::max(rows, 1)), std::max(1u, std::thread::hardware_concurrency()));
  std::atomic<int> next{0};
  auto work = [&]() {
    for(int r=next.fetch_add(1); r<rows; r=next.fetch_add(1))
      row(r);
    };
  std::vector<std::thread> threads;
  for(unsigned i=1; i<count; ++i)
    threads.emplace_back(work);
  work();
  for(auto& t:threads)
    t.join();
  }

float meanLuminance(const Image& image) {
  double sum = 0;
  for(size_t i=0; i<image.px.size(); i+=4)
    sum += 0.2126*double(image.px[i]) + 0.7152*double(image.px[i+1]) + 0.0722*double(image.px[i+2]);
  return float(sum/double(image.px.size()/4));
  }

void writeDds(const std::filesystem::path& path, const Image& top, Encoding encoding) {
  if(top.w<=0 || top.h<=0 || top.px.size()!=size_t(top.w)*size_t(top.h)*4)
    throw std::runtime_error("island: invalid image for "+path.string());
  const bool alpha = encoding==Encoding::ColourAlpha || encoding==Encoding::DataAlpha || encoding==Encoding::Normal;
  // the colour of a material is fitted by clusters; the rest by the range of the block, many times faster
  const int flags = (alpha ? squish::kDxt5 : squish::kDxt1) |
                    (encoding==Encoding::Colour || encoding==Encoding::ColourAlpha ? squish::kColourClusterFit
                                                                                   : squish::kColourRangeFit) |
                    (encoding==Encoding::Colour || encoding==Encoding::ColourAlpha ? squish::kColourMetricPerceptual
                                                                                   : squish::kColourMetricUniform);
  const size_t blockBytes = alpha ? 16 : 8;

  DdsHeader header;
  header.width         = uint32_t(top.w);
  header.height        = uint32_t(top.h);
  header.format.fourCC = fourCC(alpha ? "DXT5" : "DXT1");
  header.linearSize    = uint32_t(squish::GetStorageRequirements(top.w, top.h, flags));

  std::vector<uint8_t> data;
  Image level = top;
  while(true) {
    const auto rgba = bytes(level, encoding);
    const int  bw = (level.w+3)/4, bh = (level.h+3)/4;
    const size_t at = data.size();
    data.resize(at+size_t(bw)*size_t(bh)*blockBytes);
    const int w = level.w, h = level.h;
    parallelRows(bh, [&](int by) {
      for(int bx=0; bx<bw; ++bx) {
        uint8_t block[64];
        // a level smaller than a block repeats its edge
        for(int y=0; y<4; ++y)
          for(int x=0; x<4; ++x) {
            const size_t from = (size_t(std::min(h-1, by*4+y))*size_t(w)+size_t(std::min(w-1, bx*4+x)))*4;
            for(size_t c=0; c<4; ++c)
              block[size_t(y*4+x)*4+c] = rgba[from+c];
            }
        squish::Compress(block, data.data()+at+(size_t(by)*size_t(bw)+size_t(bx))*blockBytes, flags);
        }
      });
    ++header.mipMapCount;
    if(level.w==1 && level.h==1)
      break;
    level = half(level, encoding==Encoding::Normal);
    }

  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write("DDS ", 4);
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  if(!out)
    throw std::runtime_error("island: unable to write "+path.string());
  }

}
