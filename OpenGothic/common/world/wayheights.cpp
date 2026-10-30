#include "wayheights.h"

#include "waypoint.h"
#include "physics/dynamicworld.h"
#include "sha256.h"

#include <Tempest/Log>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
constexpr std::string_view Magic = "GOTHIC-WAYHEIGHTS\n";
constexpr size_t MaxFileSize = 16*1024*1024;

void append(std::string& bytes, uint32_t value) {
  for(unsigned shift=0;shift<32;shift+=8)
    bytes.push_back(char((value>>shift)&0xff));
  }

uint32_t take(std::string_view& bytes) {
  if(bytes.size()<4)
    throw std::runtime_error("truncated way heights");
  uint32_t value=0;
  for(unsigned i=0;i<4;++i)
    value|=uint32_t(uint8_t(bytes[i]))<<(i*8);
  bytes.remove_prefix(4);
  return value;
  }
}

WayHeights::WayHeights(const fs::path& cacheRoot, const Key& key) {
  const auto* enabled=std::getenv("OPENGOTHIC_WAYHEIGHT_CACHE");
  if((enabled && std::string_view(enabled)=="0") || cacheRoot.empty() || key.base.empty() || key.content.empty())
    return;
  // Names are also file components, so never accept a path in an identity.
  if(key.base.find_first_not_of("0123456789abcdef")!=std::string::npos ||
     key.world.empty() || key.world.find_first_of("/\\")!=std::string::npos || key.world=="..")
    return;
  std::string bytes=key.base+"\n"+key.world+"\n"+key.content+"\n";
  append(bytes,key.version);
  append(bytes,key.collisionFlags);
  identity=Sha256::hash(bytes);
  file=cacheRoot/"derived"/key.base/(key.world+".wayheights");
  read();
  }

WayHeights::PointKey WayHeights::pointKey(const WayPoint& point) {
  return {point.name,{std::bit_cast<uint32_t>(point.pos.x),std::bit_cast<uint32_t>(point.pos.y),
                      std::bit_cast<uint32_t>(point.pos.z)}};
  }

void WayHeights::read() {
  try {
    std::error_code error;
    const auto size=fs::file_size(file,error);
    if(error || size>MaxFileSize)
      return;
    std::ifstream input(file,std::ios::binary);
    std::string bytes(size_t(size),'\0');
    if(!input.read(bytes.data(),std::streamsize(bytes.size())))
      return;
    std::string_view view=bytes;
    if(!view.starts_with(Magic))
      return;
    view.remove_prefix(Magic.size());
    if(view.size()<128 || view.substr(0,64)!=identity)
      return;
    const auto checksum=view.substr(64,64);
    view.remove_prefix(128);
    if(Sha256::hash(view)!=checksum)
      return;
    const auto count=take(view);
    if(count>view.size()/28)
      return;
    std::map<PointKey,Tempest::Vec3> loaded;
    for(uint32_t i=0;i<count;++i) {
      const auto length=take(view);
      if(length>4096 || length>view.size())
        return;
      std::string name(view.substr(0,length));
      view.remove_prefix(length);
      std::array<uint32_t,3> position;
      for(auto& v:position)
        v=take(view);
      Tempest::Vec3 ground;
      ground.x=std::bit_cast<float>(take(view));
      ground.y=std::bit_cast<float>(take(view));
      ground.z=std::bit_cast<float>(take(view));
      if(!loaded.emplace(PointKey{std::move(name),position},ground).second)
        return;
      }
    if(view.empty())
      points=std::move(loaded);
    }
  catch(const std::exception&) {
    // A derived file can always be rebuilt from the initial collision world.
    }
  }

void WayHeights::adjust(WayPoint& point, const DynamicWorld& physics) {
  const auto key=pointKey(point);
  if(const auto found=points.find(key); found!=points.end()) {
    point.groundPos=found->second;
    ++hitCount;
    return;
    }
  const auto ray=physics.landRay(point.position());
  point.groundPos=ray.hasCol ? ray.v : point.pos;
  ++missCount;
  if(!file.empty())
    points.insert_or_assign(key,point.groundPos);
  }

void WayHeights::save() {
  if(file.empty() || missCount==0)
    return;
  fs::path temporary;
  try {
    std::string payload;
    append(payload,uint32_t(points.size()));
    for(const auto& [key,ground]:points) {
      const auto& [name,position]=key;
      append(payload,uint32_t(name.size()));
      payload+=name;
      for(const auto v:position)
        append(payload,v);
      for(const auto v:{ground.x,ground.y,ground.z})
        append(payload,std::bit_cast<uint32_t>(v));
      }
    const std::string bytes=std::string(Magic)+identity+Sha256::hash(payload)+payload;
    fs::create_directories(file.parent_path());
    temporary=file;
    temporary+=".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
               "-"+std::to_string(std::random_device{}());
    {
      std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
      output.write(bytes.data(),std::streamsize(bytes.size()));
      output.close();
      if(!output)
        throw std::runtime_error("cannot write way heights");
    }
    fs::rename(temporary,file);
    }
  catch(const std::exception& error) {
    if(!temporary.empty()) {
      std::error_code ignored;
      fs::remove(temporary,ignored);
      }
    Tempest::Log::e("waypoints: cache write failed: ",error.what());
    }
  }
