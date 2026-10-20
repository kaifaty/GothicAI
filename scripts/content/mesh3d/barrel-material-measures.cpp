// Read the original barrel in memory and emit only scalar zone measurements.
// Build like target-measures.cpp, linking ZenKit; no original image is written.
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Texture.hh>
#include <zenkit/Vfs.hh>
#include "../material-measures.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string upper(std::string value) {
  for(auto& c:value) c=char(std::toupper(static_cast<unsigned char>(c)));
  return value;
}
double edge(const zenkit::Vec2& a,const zenkit::Vec2& b,double x,double y) {
  return (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x);
}
bool inside(const std::array<zenkit::Vec2,3>& uv,double x,double y) {
  const auto a=edge(uv[0],uv[1],x,y),b=edge(uv[1],uv[2],x,y),c=edge(uv[2],uv[0],x,y);
  return (a>=0 && b>=0 && c>=0) || (a<=0 && b<=0 && c<=0);
}
void result(const char* key,const std::vector<uint8_t>& rgba,const std::vector<bool>& mask,int w,int h) {
  auto selected=rgba;
  size_t count=0;
  for(size_t i=0;i<mask.size();++i) {
    if(!mask[i]) selected[4*i+3]=0;
    if(mask[i] && rgba[4*i+3]>=128) ++count;
  }
  const auto m=material_measures::measure(selected.data(),w,h);
  const double y=0.2126*m.meanRgb[0]+0.7152*m.meanRgb[1]+0.0722*m.meanRgb[2];
  std::cout<<"{\"key\":\""<<key<<"\",\"pixels\":"<<count<<",\"oklab\":[";
  for(int i=0;i<3;++i) std::cout<<(i?",":"")<<m.oklab[i];
  std::cout<<"],\"mean_linear_rgb\":[";
  for(int i=0;i<3;++i) std::cout<<(i?",":"")<<m.meanRgb[i];
  std::cout<<"],\"luminance\":"<<y<<'}';
}
}
int main(int argc,char** argv) {
  try {
    if(argc!=2) throw std::runtime_error("usage: barrel-material-measures <Gothic II directory>");
    std::map<std::string,std::filesystem::path> archives;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::path(argv[1])/"Data")) {
      const auto name=upper(entry.path().filename().string());
      if(entry.is_regular_file() && name.ends_with(".VDF") &&
          (name.starts_with("MESHES") || name.starts_with("TEXTURES"))) archives[name]=entry.path();
    }
    if(archives.empty()) throw std::runtime_error("no Meshes/Textures VDF archives");
    zenkit::Vfs vfs;
    for(const auto& [name,path]:archives) vfs.mount_disk(path,zenkit::VfsOverwriteBehavior::OLDER);
    const auto* visual=vfs.find("NW_HARBOUR_BARREL_01.MRM");
    if(!visual) throw std::runtime_error("missing original barrel");
    auto stream=visual->open_read();
    zenkit::MultiResolutionMesh mesh; mesh.load(stream.get());
    std::string texture;
    std::vector<std::array<zenkit::Vec2,3>> heads;
    for(const auto& sub:mesh.sub_meshes) {
      const auto name=upper(sub.mat.texture);
      if(!texture.empty() && texture!=name) throw std::runtime_error("barrel atlas has changed");
      texture=name;
      for(const auto& triangle:sub.triangles) {
        double normalY=0;
        std::array<zenkit::Vec2,3> uv;
        for(int i=0;i<3;++i) {
          const auto& wedge=sub.wedges.at(triangle.wedges[i]);
          normalY+=wedge.normal.y/3;
          uv[i]=wedge.texture;
        }
        if(std::abs(normalY)>0.99) heads.push_back(uv);
      }
    }
    if(texture!="NW_CITY_BARREL_01.TGA" || heads.size()!=12)
      throw std::runtime_error("unexpected barrel atlas or head faces");
    const auto* file=vfs.find(texture.substr(0,texture.size()-4)+"-C.TEX");
    if(!file) throw std::runtime_error("missing original barrel texture");
    auto textureStream=file->open_read();
    zenkit::Texture tex; tex.load(textureStream.get());
    const int w=tex.width(),h=tex.height();
    if(w!=128 || h!=128) throw std::runtime_error("zone atlas expects 128x128 original");
    const auto rgba=tex.as_rgba8(0);
    std::vector<bool> stave(w*h),head(w*h),iron(w*h),wood(w*h);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
      const size_t i=y*w+x;
      const double u=(x+0.5)/w,v=(y+0.5)/h;
      // The MRM's side occupies U < 0.2; caps use the rest of the atlas.
      // Fixed hoop rows come from the side's numerical R/G profile (iron's
      // R/G near 1.0 vs wood near 1.15); include seam/shadow rows in each zone.
      if(u<0.2) {
        iron[i]=y<10 || (y>=28 && y<42) || (y>=84 && y<94) || y>=116;
        stave[i]=!iron[i];
      } else {
        head[i]=std::any_of(heads.begin(),heads.end(),[&](const auto& uv) { return inside(uv,u,v); });
      }
      wood[i]=stave[i] || head[i];
    }
    std::cout<<std::setprecision(12)<<"{\"visual\":\"NW_HARBOUR_BARREL_01.MRM\",\"texture\":\""<<texture
             <<"\",\"width\":"<<w<<",\"height\":"<<h<<",\"side_u\":[0,0.2],\"iron_rows_half_open\":[[0,10],[28,42],[84,94],[116,128]],\"zones\":[";
    result("OWN_PROP_WOOD_STAVE",rgba,stave,w,h); std::cout<<',';
    result("OWN_PROP_WOOD_HEAD",rgba,head,w,h); std::cout<<',';
    result("OWN_PROP_IRON_BAND",rgba,iron,w,h); std::cout<<',';
    result("WOOD_COMBINED",rgba,wood,w,h); std::cout<<"]\n";
  } catch(const std::exception& error) {
    std::cerr<<"barrel-material-measures: "<<error.what()<<'\n'; return 1;
  }
}
