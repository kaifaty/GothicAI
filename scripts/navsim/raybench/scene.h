#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sched.h>

constexpr size_t RayCount = 238;
struct Vec {
  float x=0,y=0,z=0;
  Vec operator+(Vec b) const {return {x+b.x,y+b.y,z+b.z};}
  Vec operator-(Vec b) const {return {x-b.x,y-b.y,z-b.z};}
  Vec operator*(float a) const {return {x*a,y*a,z*a};}
};
struct Triangle {Vec v[3];};
static_assert(sizeof(Triangle)==36);
struct Hit {float distance=0, normalY=0; uint32_t kind=0, flags=0;};
static_assert(sizeof(Hit)==16);
struct Ray {Vec direction; float range; Hit reference;};
struct Update {Vec origin; bool far; std::array<Ray,RayCount> rays;};
struct Input {float nearRange=0; std::vector<Triangle> triangles; std::vector<uint8_t> categories; std::vector<Update> updates;};
using Clock=std::chrono::steady_clock;
inline double us(Clock::time_point t) {return std::chrono::duration<double,std::micro>(Clock::now()-t).count();}
inline void require(bool ok,const std::string& what) {if(!ok)throw std::runtime_error(what);}
template<class T> T read(std::istream& f) {T v{};f.read(reinterpret_cast<char*>(&v),sizeof v);require(bool(f),"Truncated input");return v;}
inline void magic(std::istream& f,const char* expected) {char b[8];f.read(b,8);require(bool(f)&&std::memcmp(b,expected,8)==0,"Invalid snapshot magic");}
inline uint32_t kind(uint8_t category) {return category==1?1:category==3?5:2;}
inline Input load(const std::filesystem::path& root) {
  Input in;
  std::ifstream f(root/"static.bin",std::ios::binary);magic(f,"NAVSTAT1");
  auto count=read<uint32_t>(f);require(read<uint32_t>(f)==0,"Invalid static reserved field");
  require(std::filesystem::file_size(root/"static.bin")==16+uint64_t(count)*37,"Invalid static size");
  in.triangles.resize(count);in.categories.resize(count);
  f.read(reinterpret_cast<char*>(in.triangles.data()),count*36ull);
  f.read(reinterpret_cast<char*>(in.categories.data()),count);
  for(auto c:in.categories)require(c>=1&&c<=4,"Invalid category");
  std::ifstream r(root/"rays.bin",std::ios::binary);magic(r,"NAVRAYS1");
  require(read<uint32_t>(r)==RayCount,"Expected 238 rays");count=read<uint32_t>(r);in.nearRange=read<float>(r);
  require(std::filesystem::file_size(root/"rays.bin")==20+uint64_t(count)*(21+RayCount*27),"Invalid rays size");
  in.updates.resize(count);
  for(auto& u:in.updates) {
    read<uint64_t>(r);u.origin=read<Vec>(r);u.far=read<uint8_t>(r)!=0;
    for(auto& ray:u.rays) {
      ray.direction=read<Vec>(r);ray.range=read<float>(r);ray.reference.distance=read<float>(r);
      ray.reference.kind=read<uint8_t>(r);ray.reference.normalY=read<float>(r);
      ray.reference.flags=read<uint8_t>(r);ray.reference.flags|=uint32_t(read<uint8_t>(r))<<1;
    }
  }
  require(!in.updates.empty(),"Empty ray corpus");return in;
}
inline std::vector<int> cpus() {
  cpu_set_t set;CPU_ZERO(&set);require(sched_getaffinity(0,sizeof set,&set)==0,"Cannot read CPU affinity");
  std::vector<int> out;for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&set))out.push_back(i);return out;
}
inline void pin(int cpu) {cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);require(sched_setaffinity(0,sizeof set,&set)==0,"Cannot pin worker");}
inline double quantile(std::vector<double> v,double p) {require(!v.empty(),"Empty distribution");std::sort(v.begin(),v.end());return v[size_t(std::ceil(p*v.size()))-1];}
inline void distribution(std::ostream& f,const std::vector<double>& v) {
  f<<"{\"median\":"<<quantile(v,.5)<<",\"p90\":"<<quantile(v,.9)<<",\"samples\":"<<v.size()<<'}';
}
// Each lane belongs to one eye update. Far queries are compacted only within it.
template<class Scene> uint32_t eye(const Scene& scene,const Update& u,float nearRange,Hit* result,int width,bool cull) {
  std::array<Vec,RayCount> origins,ends;
  std::array<float,RayCount> lengths;
  for(size_t i=0;i<RayCount;++i) {origins[i]=u.origin;lengths[i]=std::min(nearRange,u.rays[i].range);ends[i]=u.origin+u.rays[i].direction*lengths[i];}
  scene.cast(origins.data(),ends.data(),RayCount,result,width,cull);
  std::array<size_t,RayCount> indices;size_t farCount=0;
  for(size_t i=0;i<RayCount;++i) {
    const float end=lengths[i];const auto& r=u.rays[i];
    result[i].flags|=((result[i].flags&1)||end>=r.range)?2:0;
    if(result[i].flags&1) {
      const auto delta=(ends[i]-u.origin)*result[i].distance;
      result[i].distance=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    } else result[i].distance=end;
    if(!(result[i].flags&2)&&u.far) {
      indices[farCount]=i;origins[farCount]=u.origin+r.direction*end;
      ends[farCount]=u.origin+r.direction*r.range;lengths[farCount]=r.range-end;++farCount;
    }
  }
  std::array<Hit,RayCount> farHits;
  if(farCount)scene.cast(origins.data(),ends.data(),farCount,farHits.data(),width,cull);
  for(size_t j=0;j<farCount;++j) {
    size_t i=indices[j];result[i]=farHits[j];result[i].flags|=2;
    if(result[i].flags&1) {
      const auto delta=origins[j]+(ends[j]-origins[j])*result[i].distance-u.origin;
      result[i].distance=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    } else result[i].distance=u.rays[i].range;
  }
  return RayCount+farCount;
}
