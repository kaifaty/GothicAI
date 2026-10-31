// Survey one original static prop for stage 2: where it stands in NEWWORLD.ZEN and
// the measured colour of the texels its mesh uses. Original data stays in memory;
// output is names and numbers only (positions, counts, linear RGB, OKLab).
// Build like target-measures.cpp (see measure_targets.py), then:
//   prop-survey <Gothic II directory> NW_HARBOUR_CRATE_01.MRM|BENCH_NW_CITY_01.ASC [metal R/G ratio, default 1.07]
// A model (.ASC/.MDL) is read from its MDL: every attachment and soft-skin mesh counts.
#include <zenkit/Model.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Texture.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/vobs/VirtualObject.hh>
#include "../material-measures.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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
std::string stem(const std::string& n) { const auto dot=n.rfind('.'); return dot==n.npos?n:n.substr(0,dot); }
double edge(const zenkit::Vec2& a,const zenkit::Vec2& b,double x,double y) {
  return (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x);
}
void instances(const std::vector<std::shared_ptr<zenkit::VirtualObject>>& vobs,const std::string& visual,
               size_t& index,bool& first) {
  for(const auto& v:vobs) {
    ++index;
    if(v->visual && stem(upper(v->visual->name))==visual) {
      const auto& p=v->position;
      const auto& r=v->rotation;
      std::cout<<(first?"":",")<<"{\"index\":"<<index<<",\"position_cm\":["<<p.x<<','<<p.y<<','<<p.z
               <<"],\"forward\":["<<r[2][0]<<','<<r[2][1]<<','<<r[2][2]<<"]}";
      first=false;
    }
    instances(v->children,visual,index,first);
  }
}
void zone(const char* key,const std::vector<uint8_t>& rgba,const std::vector<bool>& mask,int w,int h) {
  auto selected=rgba;
  size_t count=0;
  for(size_t i=0;i<mask.size();++i) {
    if(!mask[i]) selected[4*i+3]=0;
    else if(rgba[4*i+3]>=128) ++count;
  }
  std::cout<<"{\"zone\":\""<<key<<"\",\"pixels\":"<<count;
  if(count) {
    const auto m=material_measures::measure(selected.data(),w,h);
    std::cout<<",\"mean_linear_rgb\":["<<m.meanRgb[0]<<','<<m.meanRgb[1]<<','<<m.meanRgb[2]<<"],\"oklab\":["
             <<m.oklab[0]<<','<<m.oklab[1]<<','<<m.oklab[2]<<"],\"luminance\":"
             <<0.2126*m.meanRgb[0]+0.7152*m.meanRgb[1]+0.0722*m.meanRgb[2];
  }
  std::cout<<'}';
}
}

int main(int argc,char** argv) {
  try {
    if(argc<3 || argc>4) throw std::runtime_error("usage: prop-survey <game dir> <VISUAL.MRM|VISUAL.ASC> [metal R/G ratio]");
    const auto visual=upper(argv[2]);
    const double metalRatio=argc==4?std::stod(argv[3]):1.07;
    std::map<std::string,std::filesystem::path> archives;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::path(argv[1])/"Data")) {
      const auto name=upper(entry.path().filename().string());
      if(entry.is_regular_file() && name.ends_with(".VDF") &&
         (name.starts_with("MESHES") || name.starts_with("TEXTURES") || name.starts_with("WORLDS") ||
          name.starts_with("ANIMS"))) archives[name]=entry.path();
    }
    if(archives.empty()) throw std::runtime_error("no Meshes/Textures/Worlds VDF archives");
    zenkit::Vfs vfs;
    for(const auto& [name,path]:archives) vfs.mount_disk(path,zenkit::VfsOverwriteBehavior::OLDER);
    const auto* worldFile=vfs.find("NEWWORLD.ZEN");
    const bool model=visual.ends_with(".ASC") || visual.ends_with(".MDL");
    const auto* meshFile=vfs.find(model ? stem(visual)+".MDL" : visual);
    if(!worldFile || !meshFile) throw std::runtime_error("missing NEWWORLD.ZEN or "+visual);
    std::cout<<std::setprecision(10)<<"{\"visual\":\""<<visual<<"\",\"newworld\":[";
    {
      auto stream=worldFile->open_read();
      zenkit::World world; world.load(stream.get(),zenkit::GameVersion::GOTHIC_2);
      size_t index=0; bool first=true;
      instances(world.world_vobs,stem(visual),index,first);
    }
    std::cout<<"],\"textures\":[";
    auto stream=meshFile->open_read();
    std::vector<zenkit::MultiResolutionMesh> meshes;
    if(model) {
      zenkit::Model mdl; mdl.load(stream.get());
      for(auto& [name,mesh]:mdl.mesh.attachments) meshes.push_back(mesh);
      for(auto& skin:mdl.mesh.meshes) meshes.push_back(skin.mesh);
    } else {
      zenkit::MultiResolutionMesh mesh; mesh.load(stream.get()); meshes.push_back(std::move(mesh));
    }
    // One entry per texture: the texels used by every sub-mesh that samples it.
    std::map<std::string,std::vector<std::array<zenkit::Vec2,3>>> byTexture;
    for(const auto& mesh:meshes)
      for(const auto& sub:mesh.sub_meshes)
        for(const auto& triangle:sub.triangles) {
          std::array<zenkit::Vec2,3> uv;
          for(int i=0;i<3;++i) uv[i]=sub.wedges.at(triangle.wedges[i]).texture;
          byTexture[upper(sub.mat.texture)].push_back(uv);
        }
    bool firstTexture=true;
    for(const auto& [texture,triangles]:byTexture) {
      const auto* file=vfs.find(stem(texture)+"-C.TEX");
      std::cout<<(firstTexture?"":",")<<"{\"texture\":\""<<texture<<"\",\"triangles\":"<<triangles.size();
      firstTexture=false;
      if(!file) { std::cout<<",\"missing\":true}"; continue; }
      auto textureStream=file->open_read();
      zenkit::Texture tex; tex.load(textureStream.get());
      const int w=tex.width(),h=tex.height();
      const auto rgba=tex.as_rgba8(0);
      std::vector<bool> used(size_t(w)*h,false);
      for(auto uv:triangles) {
        const double ox=std::floor(std::min({uv[0].x,uv[1].x,uv[2].x})), oy=std::floor(std::min({uv[0].y,uv[1].y,uv[2].y}));
        for(auto& p:uv) { p.x-=float(ox); p.y-=float(oy); }  // one tile of a repeating UV
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
          const double u=(x+0.5)/w,v=(y+0.5)/h;
          const auto a=edge(uv[0],uv[1],u,v),b=edge(uv[1],uv[2],u,v),c=edge(uv[2],uv[0],u,v);
          if((a>=0 && b>=0 && c>=0) || (a<=0 && b<=0 && c<=0)) used[size_t(y)*w+x]=true;
        }
      }
      std::vector<bool> metal(used.size()),wood(used.size());
      for(size_t i=0;i<used.size();++i) {
        if(!used[i]) continue;
        const double r=material_measures::linear(rgba[4*i]),g=material_measures::linear(rgba[4*i+1]);
        (r<metalRatio*g ? metal : wood)[i]=true;
      }
      std::cout<<",\"width\":"<<w<<",\"height\":"<<h<<",\"metal_rg_below\":"<<metalRatio<<",\"zones\":[";
      zone("ALL_USED",rgba,used,w,h); std::cout<<',';
      zone("WOOD_LIKE",rgba,wood,w,h); std::cout<<',';
      zone("METAL_LIKE",rgba,metal,w,h); std::cout<<"]}";
    }
    std::cout<<"]}\n";
  } catch(const std::exception& error) {
    std::cerr<<"prop-survey: "<<error.what()<<'\n'; return 1;
  }
}
