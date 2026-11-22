// Packs the finished textures of scripts/hd-textures.sh for the GPU, as the game's own are: DXT1 (DXT5
// with alpha) in a DDS file with all its mipmaps. As RGBA a 1024x1024 texture takes 5.6 MB of memory
// with its mipmaps, as DXT1 0.7: the game loads the textures of every npc in the world.
//
//   texpack <png dir> <out dir>
#include <squish.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

#pragma pack(push,1)
struct DdsPixelFormat {
  uint32_t size = 32, flags = 0x4 /*FOURCC*/, fourCC = 0, rgbBitCount = 0, r = 0, g = 0, b = 0, a = 0;
  };
struct DdsHeader {
  uint32_t       size = 124;
  uint32_t       flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000;  // caps, height, width, pixel format, mipmaps, linear size
  uint32_t       height = 0, width = 0, linearSize = 0, depth = 0, mipMapCount = 0;
  uint32_t       reserved1[11] = {};
  DdsPixelFormat format;
  uint32_t       caps = 0x1000 | 0x400000 | 0x8;  // texture, mipmap, complex
  uint32_t       caps2 = 0, caps3 = 0, caps4 = 0, reserved2 = 0;
  };
#pragma pack(pop)
static_assert(sizeof(DdsHeader)==124);

constexpr uint32_t fourCC(const char* c) {
  return uint32_t(uint8_t(c[0])) | uint32_t(uint8_t(c[1]))<<8 | uint32_t(uint8_t(c[2]))<<16 | uint32_t(uint8_t(c[3]))<<24;
  }

// the next mipmap: 2x2 averages (a side of 1 stays 1)
std::vector<uint8_t> half(const std::vector<uint8_t>& src, int w, int h, int& nw, int& nh) {
  nw = std::max(1,w/2);
  nh = std::max(1,h/2);
  std::vector<uint8_t> dst(size_t(nw)*size_t(nh)*4);
  for(int y=0; y<nh; ++y)
    for(int x=0; x<nw; ++x)
      for(int c=0; c<4; ++c) {
        int sum = 0, n = 0;
        for(int dy=0; dy<2; ++dy)
          for(int dx=0; dx<2; ++dx) {
            const int sx = std::min(w-1,x*2+dx), sy = std::min(h-1,y*2+dy);
            sum += src[(size_t(sy)*size_t(w)+size_t(sx))*4+size_t(c)];
            ++n;
            }
        dst[(size_t(y)*size_t(nw)+size_t(x))*4+size_t(c)] = uint8_t((sum+n/2)/n);
        }
  return dst;
  }

bool pack(const fs::path& src, const fs::path& dst) {
  int w = 0, h = 0, ch = 0;
  uint8_t* px = stbi_load(src.string().c_str(),&w,&h,&ch,4);
  if(px==nullptr)
    return false;
  std::vector<uint8_t> level(px,px+size_t(w)*size_t(h)*4);
  stbi_image_free(px);

  bool alpha = false;
  for(size_t i=3; i<level.size(); i+=4)
    alpha |= level[i]<255;
  const int flags = (alpha ? squish::kDxt5 : squish::kDxt1) | squish::kColourClusterFit;

  DdsHeader hdr;
  hdr.width         = uint32_t(w);
  hdr.height        = uint32_t(h);
  hdr.format.fourCC = fourCC(alpha ? "DXT5" : "DXT1");
  hdr.linearSize    = uint32_t(squish::GetStorageRequirements(w,h,flags));

  std::vector<uint8_t> data;
  int lw = w, lh = h;
  while(true) {
    std::vector<uint8_t> blocks(size_t(squish::GetStorageRequirements(lw,lh,flags)));
    squish::CompressImage(level.data(),lw,lh,blocks.data(),flags);
    data.insert(data.end(),blocks.begin(),blocks.end());
    ++hdr.mipMapCount;
    if(lw==1 && lh==1)
      break;
    int nw = 0, nh = 0;
    level = half(level,lw,lh,nw,nh);
    lw = nw;
    lh = nh;
    }

  std::FILE* f = std::fopen(dst.string().c_str(),"wb");
  if(f==nullptr)
    return false;
  std::fwrite("DDS ",1,4,f);
  std::fwrite(&hdr,1,sizeof(hdr),f);
  std::fwrite(data.data(),1,data.size(),f);
  std::fclose(f);
  return true;
  }

}

int main(int argc, char** argv) {
  if(argc<3) {
    std::fprintf(stderr,"usage: texpack <png dir> <out dir>\n");
    return 2;
    }
  const fs::path in = argv[1], out = argv[2];
  fs::create_directories(out);
  std::vector<fs::path> files;
  for(auto& e:fs::directory_iterator(in))
    if(e.path().extension()==".png")
      files.push_back(e.path());

  std::atomic<size_t> next{0}, done{0}, failed{0};
  std::vector<std::thread> pool(std::max(1u,std::thread::hardware_concurrency()));
  for(auto& t:pool)
    t = std::thread([&]() {
      for(size_t i; (i = next++)<files.size();) {
        if(pack(files[i],out/(files[i].stem().string()+".dds")))
          ++done; else
          ++failed;
        }
      });
  for(auto& t:pool)
    t.join();
  std::printf("texpack: %zu textures packed%s\n",done.load(),failed>0 ? ", some failed" : "");
  return failed>0 ? 1 : 0;
  }
