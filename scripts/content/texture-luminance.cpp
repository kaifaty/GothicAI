// Original texture pixels remain in memory. Only scalar properties leave the decoder.
// Build against the engine's ZenKit ABI as scripts/content-textures.sh does.
// texture-luminance [--properties|--measurements] <game dir> <TEXTURE.TGA>... | --self-test
#include <zenkit/Stream.hh>
#include <zenkit/Texture.hh>
#include <zenkit/Vfs.hh>
#include "material-measures.h"

#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
std::string upper(std::string value) {
  for(auto& ch:value) ch=char(std::toupper(static_cast<unsigned char>(ch)));
  return value;
}

double linear(uint8_t channel) {
  const double value=double(channel)/255;
  return value<=0.04045 ? value/12.92 : std::pow((value+0.055)/1.055,2.4);
}

double luminance(const std::vector<uint8_t>& rgba) {
  if(rgba.empty() || rgba.size()%4) throw std::runtime_error("invalid RGBA pixel buffer");
  double total=0, weights=0;
  for(size_t i=0;i<rgba.size();i+=4) {
    const double alpha=double(rgba[i+3])/255;
    total+=alpha*(0.2126*linear(rgba[i])+0.7152*linear(rgba[i+1])+0.0722*linear(rgba[i+2]));
    weights+=alpha;
  }
  if(weights==0) throw std::runtime_error("texture has no visible pixels");
  return total/weights;
}

double alphaFraction(const std::vector<uint8_t>& rgba) {
  if(rgba.empty() || rgba.size()%4) throw std::runtime_error("invalid RGBA pixel buffer");
  size_t nonOpaque=0;
  for(size_t i=3;i<rgba.size();i+=4) if(rgba[i]<255) ++nonOpaque;
  return double(nonOpaque)/(rgba.size()/4);
}

// main.frag discards alpha<0.5: byte 128 is the first surviving value.
double coverage(const std::vector<uint8_t>& rgba) {
  if(rgba.empty() || rgba.size()%4) throw std::runtime_error("invalid RGBA pixel buffer");
  size_t visible=0;
  for(size_t i=3;i<rgba.size();i+=4) if(rgba[i]>=128) ++visible;
  return double(visible)/(rgba.size()/4);
}

double visibleLuminance(const std::vector<uint8_t>& rgba) {
  if(rgba.empty() || rgba.size()%4) throw std::runtime_error("invalid RGBA pixel buffer");
  double total=0;
  size_t visible=0;
  for(size_t i=0;i<rgba.size();i+=4) {
    if(rgba[i+3]<128) continue;
    total+=0.2126*linear(rgba[i])+0.7152*linear(rgba[i+1])+0.0722*linear(rgba[i+2]);
    ++visible;
  }
  if(visible==0) throw std::runtime_error("texture has no pixels surviving alpha cutoff");
  return total/visible;
}

double halfCoverage(const std::vector<uint8_t>& rgba,uint32_t width,uint32_t height,int half) {
  size_t visible=0, count=0;
  for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x) {
    if((width>1 && ((half==0 && x>=width/2) || (half==1 && x<width/2)))
        || (height>1 && ((half==2 && y>=height/2) || (half==3 && y<height/2)))) continue;
    ++count;
    if(rgba[4*(size_t(y)*width+x)+3]>=128) ++visible;
  }
  if(!count) throw std::runtime_error("empty texture half");
  return double(visible)/count;
}

void selfTest() {
  const auto require=[](bool ok) { if(!ok) throw std::runtime_error("luminance self-test failed"); };
  require(luminance({0,0,0,255})==0);
  require(std::abs(luminance({255,255,255,255})-1)<1e-12);
  require(std::abs(luminance({255,255,255,255,255,0,0,0})-1)<1e-12);
  require(std::abs(luminance({255,255,255,255,0,0,0,128})-255.0/383)<1e-12);
  require(std::abs(luminance({128,128,128,255})-0.21586050011389926)<1e-12);
  require(std::abs(luminance({255,0,0,255})-0.2126)<1e-12);
  bool rejected=false;
  try { luminance({255,255,255,0}); } catch(const std::runtime_error&) { rejected=true; }
  require(rejected);
  require(alphaFraction({0,0,0,255})==0);
  require(alphaFraction({255,255,255,0,0,0,0,128})==1);
  require(alphaFraction({255,255,255,255,0,0,0,0})==0.5);
  require(coverage({255,255,255,127,0,0,0,128})==0.5);
  require(visibleLuminance({255,255,255,127,0,0,0,128})==0);
  require(visibleLuminance({255,255,255,128,0,0,0,127})==1);
  const std::vector<uint8_t> split={0,0,0,255,0,0,0,0,0,0,0,128,0,0,0,127};
  require(halfCoverage(split,2,2,0)==1 && halfCoverage(split,2,2,1)==0);
  require(halfCoverage(split,2,2,2)==0.5 && halfCoverage(split,2,2,3)==0.5);
  const auto flat=material_measures::measure(split.data(),2,2);
  require(flat.logSpread==0 && flat.detailK3==0 && flat.detailK9==0 && flat.detailK27==0);
  std::cout << "self-test: 16 checks passed\n";
}
}

int main(int argc,char** argv) {
  try {
    if(argc==2 && std::string(argv[1])=="--self-test") { selfTest(); return 0; }
    const bool measurements=argc>1 && std::string(argv[1])=="--measurements";
    const bool properties=measurements || (argc>1 && std::string(argv[1])=="--properties");
    const int gameArg=properties ? 2 : 1;
    if(argc<gameArg+2) throw std::runtime_error("usage: texture-luminance [--properties|--measurements] <game dir> <TEXTURE.TGA>... | --self-test");
    std::map<std::string,fs::path> archives;
    for(const auto& entry:fs::directory_iterator(fs::path(argv[gameArg])/"Data")) {
      const auto name=upper(entry.path().filename().string());
      if(entry.is_regular_file() && name.starts_with("TEXTURES") && name.ends_with(".VDF"))
        archives[name]=entry.path();
    }
    if(archives.empty()) throw std::runtime_error("no Textures*.vdf archives");
    zenkit::Vfs vfs;
    for(const auto& [name,path]:archives)
      vfs.mount_disk(path,zenkit::VfsOverwriteBehavior::OLDER); // same archive policy as texdump.cpp
    std::set<std::string> seen;
    struct Result { std::string name; double luminance, alphaFraction, coverage; uint32_t width, height;
                    double left, right, top, bottom; material_measures::Result measurements; };
    std::vector<Result> results;
    for(int i=gameArg+1;i<argc;++i) {
      const std::string texture=upper(argv[i]);
      if(!texture.ends_with(".TGA") || texture.find_first_of("/\\,\r\n")!=std::string::npos)
        throw std::runtime_error("expected a texture basename ending in .TGA");
      if(!seen.insert(texture).second) throw std::runtime_error("duplicate texture "+texture);
      const auto compiled=texture.substr(0,texture.size()-4)+"-C.TEX";
      const auto* file=vfs.find(compiled);
      if(!file) throw std::runtime_error("missing original texture "+compiled);
      zenkit::Texture tex;
      auto stream=file->open_read();
      tex.load(stream.get());
      // Use the exact top-mip decode path of scripts/hd/texdump.cpp.
      const auto rgba=tex.as_rgba8(0);
      results.push_back({texture,properties ? visibleLuminance(rgba) : luminance(rgba),alphaFraction(rgba),
          coverage(rgba),tex.width(),tex.height(),halfCoverage(rgba,tex.width(),tex.height(),0),
          halfCoverage(rgba,tex.width(),tex.height(),1),halfCoverage(rgba,tex.width(),tex.height(),2),
          halfCoverage(rgba,tex.width(),tex.height(),3),
          measurements ? material_measures::measure(rgba.data(),tex.width(),tex.height()) : material_measures::Result{}});
    }
    std::cout << "texture,target_luminance";
    if(properties) std::cout << ",nonopaque_fraction,opaque,width,height,target_coverage,coverage_left_half,coverage_right_half,coverage_top_half,coverage_bottom_half";
    if(measurements) std::cout << ",target_period_u_uv,target_period_v_uv,target_direction,target_direction_confidence,target_oklab_l,target_oklab_a,target_oklab_b,target_log_spread,target_detail_k3,target_detail_k9,target_detail_k27";
    std::cout << '\n' << std::setprecision(12);
    for(const auto& result:results) {
      std::cout << result.name << ',' << result.luminance;
      if(properties) std::cout << ',' << result.alphaFraction << ',' << (result.alphaFraction==0 ? "true" : "false")
                               << ',' << result.width << ',' << result.height << ',' << result.coverage
                               << ',' << result.left << ',' << result.right << ',' << result.top << ',' << result.bottom;
      if(measurements) {
        const auto& m=result.measurements;
        std::cout << ',' << m.periodU << ',' << m.periodV << ',' << (m.direction==1?"U":m.direction==2?"V":"none")
                  << ',' << m.directionConfidence << ',' << m.oklab[0] << ',' << m.oklab[1] << ',' << m.oklab[2]
                  << ',' << m.logSpread << ',' << m.detailK3 << ',' << m.detailK9 << ',' << m.detailK27;
      }
      std::cout << '\n';
    }
  } catch(const std::exception& error) {
    std::cerr << "texture-luminance: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
