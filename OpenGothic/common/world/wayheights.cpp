#include "wayheights.h"

#include "waypoint.h"
#include "physics/dynamicworld.h"
#include "sha256.h"

#include <Tempest/Log>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
constexpr std::string_view Magic = "GOTHIC-WAYHEIGHTS\n";
constexpr std::string_view ProfileMagic = "GOTHIC-WAYHEIGHT-PROFILES-3\n";
constexpr float MaxProfileSpacing = 25.f;
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

WayHeights::~WayHeights() {
  saveProfiles();
  }

float WayHeights::Profile::terminalHeightAt(const Tempest::Vec3& position) const {
  const auto& sample=samples.front();
  const auto offset=position-sample.position;
  return sample.position.y-(sample.normal.x*offset.x+sample.normal.z*offset.z)/sample.normal.y;
  }

float WayHeights::Profile::heightAt(const Tempest::Vec3& position) const {
  if(length<=0.f)
    return terminalHeightAt(position);
  const auto direction=(to-from)/length;
  const float distance=std::clamp((to.x-position.x)*direction.x+(to.z-position.z)*direction.z,0.f,length);
  const auto index=std::min(size_t(distance/spacing),samples.size()-2);
  const float begin=float(index)*spacing;
  const float end=index+2==samples.size() ? length : begin+spacing;
  const float fraction=end>begin ? std::clamp((distance-begin)/(end-begin),0.f,1.f) : 0.f;
  const auto center=samples[index].position+(samples[index+1].position-samples[index].position)*fraction;
  const auto normal=samples[index].normal+(samples[index+1].normal-samples[index].normal)*fraction;
  // Query the cached support plane at the actual X/Z, including the short
  // lateral handoff from a native body to the authored edge.
  const auto offset=position-center;
  return center.y-(normal.x*offset.x+normal.z*offset.z)/normal.y;
  }

const WayHeights::Profile& WayHeights::profile(const Tempest::Vec3& from,const Tempest::Vec3& to,
                                             float stepHeight,float slopeCos,const DynamicWorld& physics) {
  if(!profilesRead)
    readProfiles();
  const ProfileKey key={std::bit_cast<uint32_t>(from.x),std::bit_cast<uint32_t>(from.y),std::bit_cast<uint32_t>(from.z),
                        std::bit_cast<uint32_t>(to.x),std::bit_cast<uint32_t>(to.y),std::bit_cast<uint32_t>(to.z),
                        std::bit_cast<uint32_t>(stepHeight),std::bit_cast<uint32_t>(slopeCos)};
  if(const auto found=profiles.find(key);found!=profiles.end())
    return found->second;
  const auto support=[&](const Tempest::Vec3& at,float previousY) {
    auto begin=Tempest::Vec3(at.x,previousY+stepHeight+1.f,at.z);
    const auto end=Tempest::Vec3(at.x,previousY-stepHeight-1.f,at.z);
    // Search only the native step band around the preceding supported floor.
    // This preserves a ship/cave layer rather than selecting the topmost roof.
    // The first walkable contact matches native ground rays: a lower deck can
    // continue beneath stairs and must not win over their reachable ramp.
    for(unsigned layer=0;layer<32;++layer) {
      const auto hit=physics.rayStatic(begin,end);
      if(!hit.hasCol)
        break;
      const float difference=std::abs(hit.v.y-previousY);
      if(hit.n.y>0.f && hit.n.y>=slopeCos && difference<stepHeight+1.f)
        return Profile::Sample{hit.v,hit.n};
      begin=hit.v-Tempest::Vec3(0,.1f,0);
      }
    throw std::runtime_error("Simplified movement has no continuous ground support at "+
                             std::to_string(at.x)+","+std::to_string(previousY)+","+std::to_string(at.z)+
                             " on segment "+std::to_string(from.x)+","+std::to_string(from.y)+","+std::to_string(from.z)+
                             " -> "+std::to_string(to.x)+","+std::to_string(to.y)+","+std::to_string(to.z));
    };
  Profile result;
  result.from=from;
  result.to=to;
  result.length=std::hypot(to.x-from.x,to.z-from.z);
  // A continuous native slope must fit within the step band even for small guilds.
  result.spacing=std::min(MaxProfileSpacing,stepHeight*.5f);
  const auto direction=result.length>0.f ? (to-from)/result.length : Tempest::Vec3();
  const size_t count=std::max(size_t(1),size_t(std::ceil(result.length/result.spacing)));
  result.samples.reserve(count+1);
  float height=to.y;
  for(size_t i=0;i<count;++i) {
    const auto at=to-direction*(float(i)*result.spacing);
    auto sample=support(at,height);
    height=sample.position.y;
    result.samples.push_back(sample);
    }
  const auto first=support(from,height);
  auto source=from;
  const auto contact=physics.rayStatic(from+Tempest::Vec3(0,1,0),from-Tempest::Vec3(0,1,0));
  if(contact.hasCol && contact.n.y<slopeCos) {
    // Native spawn grounding can hit a furniture side. Only that invalid
    // foot uses the connected floor; a valid saved/current Y stays exact.
    source.y=first.position.y;
    }
  else if(std::abs(height-from.y)>stepHeight+1.f)
    throw std::runtime_error("Simplified ground profile reaches a different floor at "+
                             std::to_string(from.x)+","+std::to_string(from.y)+","+std::to_string(from.z)+
                             " from target "+std::to_string(to.x)+","+std::to_string(to.y)+","+std::to_string(to.z)+
                             "; support height "+std::to_string(first.position.y)+"; source contact normal "+
                             (contact.hasCol ? std::to_string(contact.n.x)+","+std::to_string(contact.n.y)+","+
                                               std::to_string(contact.n.z) : "none"));
  // Keep a valid source Y so a cold-cache load preserves its partial interval.
  result.samples.push_back({source,first.normal});
  profilesDirty=true;
  return profiles.emplace(key,std::move(result)).first->second;
  }

void WayHeights::readProfiles() {
  profilesRead=true;
  if(file.empty())
    return;
  profileFile=file;
  profileFile+=".profiles";
  profileIdentity=Sha256::hash(identity+std::string(ProfileMagic));
  try {
    std::error_code error;
    const auto size=fs::file_size(profileFile,error);
    if(error || size>MaxFileSize)
      return;
    std::ifstream input(profileFile,std::ios::binary);
    std::string bytes(size_t(size),'\0');
    if(!input.read(bytes.data(),std::streamsize(bytes.size())))
      return;
    std::string_view view=bytes;
    if(!view.starts_with(ProfileMagic))
      return;
    view.remove_prefix(ProfileMagic.size());
    if(view.size()<128 || view.substr(0,64)!=profileIdentity)
      return;
    const auto checksum=view.substr(64,64);
    view.remove_prefix(128);
    if(Sha256::hash(view)!=checksum)
      return;
    const auto count=take(view);
    std::map<ProfileKey,Profile> loaded;
    for(uint32_t i=0;i<count;++i) {
      ProfileKey key;
      for(auto& value:key)
        value=take(view);
      Profile profile;
      profile.from={std::bit_cast<float>(key[0]),std::bit_cast<float>(key[1]),std::bit_cast<float>(key[2])};
      profile.to={std::bit_cast<float>(key[3]),std::bit_cast<float>(key[4]),std::bit_cast<float>(key[5])};
      profile.length=std::hypot(profile.to.x-profile.from.x,profile.to.z-profile.from.z);
      profile.spacing=std::min(MaxProfileSpacing,std::bit_cast<float>(key[6])*.5f);
      const auto points=take(view);
      if(!std::isfinite(profile.length) || !std::isfinite(profile.spacing) || profile.spacing<=0.f ||
         points!=std::max(size_t(1),size_t(std::ceil(profile.length/profile.spacing)))+1 ||
         points>view.size()/16)
        return;
      const auto direction=profile.length>0.f ? (profile.to-profile.from)/profile.length : Tempest::Vec3();
      for(uint32_t n=0;n<points;++n) {
        auto position=n+1==points ? profile.from : profile.to-direction*(float(n)*profile.spacing);
        position.y=std::bit_cast<float>(take(view));
        Tempest::Vec3 normal;
        normal.x=std::bit_cast<float>(take(view));
        normal.y=std::bit_cast<float>(take(view));
        normal.z=std::bit_cast<float>(take(view));
        if(!std::isfinite(position.y) || !std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z) || normal.y<=0.f)
          return;
        profile.samples.push_back({position,normal});
        }
      if(!loaded.emplace(key,std::move(profile)).second)
        return;
      }
    if(view.empty())
      profiles=std::move(loaded);
    }
  catch(const std::exception&) {
    // Profiles are derived from immutable collision, independently of saves.
    }
  }

void WayHeights::saveProfiles() {
  if(profileFile.empty() || !profilesDirty)
    return;
  fs::path temporary;
  try {
    std::string payload;
    append(payload,uint32_t(profiles.size()));
    for(const auto& [key,profile]:profiles) {
      for(const auto value:key)
        append(payload,value);
      append(payload,uint32_t(profile.samples.size()));
      for(const auto& sample:profile.samples)
        for(const auto value:{sample.position.y,sample.normal.x,sample.normal.y,sample.normal.z})
          append(payload,std::bit_cast<uint32_t>(value));
      }
    const auto bytes=std::string(ProfileMagic)+profileIdentity+Sha256::hash(payload)+payload;
    fs::create_directories(profileFile.parent_path());
    temporary=profileFile;
    temporary+=".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
               "-"+std::to_string(std::random_device{}());
    std::ofstream output(temporary,std::ios::binary);
    output.write(bytes.data(),std::streamsize(bytes.size()));
    output.close();
    if(!output)
      throw std::runtime_error("cannot write way height profiles");
    fs::rename(temporary,profileFile);
    }
  catch(const std::exception& error) {
    if(!temporary.empty()) {
      std::error_code ignored;
      fs::remove(temporary,ignored);
      }
    Tempest::Log::e("waypoints: profile cache write failed: ",error.what());
    }
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
  auto key=pointKey(point);
  if(const auto found=points.find(key); found!=points.end()) {
    point.groundPos=found->second;
    ++hitCount;
    return;
    }
  const auto ray=physics.landRay(point.position());
  point.groundPos=ray.hasCol ? ray.v : point.pos;
  ++missCount;
  if(!file.empty())
    baked.emplace_back(std::move(key),point.groundPos);
  }

void WayHeights::save() {
  if(file.empty() || missCount==0)
    return;
  fs::path temporary;
  try {
    for(auto& [key,ground]:baked)
      points.insert_or_assign(std::move(key),ground);
    baked.clear();
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
