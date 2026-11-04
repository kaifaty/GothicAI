// Measurement-only reference for the GPU memory port (scripts/navsim/gpumem).
// Replays a memory-cost.bin capture through the engine's NavigationMemory and
// NavPolicy::encode, exposing each readout's HD inputs through a C ABI.
#define NAV_MEMORY_COST_REPLAY_ONLY
#include "navmemorycosttrace.h"
#include "navpolicy.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace {
std::string lastError;

struct Replay {
  NavPolicy policy;
  NavigationMemory memory;
  NavigationMemory::FamiliarMap map;
  NavigationMemory::FamiliarMask mask;
  std::ifstream input;
  std::vector<NavigationEye::Ray> rays;
  std::vector<NavigationEye::Ray> eye;
  NavigationMemory::View view;
  uint64_t updates=0, views=0;

  Replay(const char* trace,const char* weights,const char* journey):policy(weights),input(trace,std::ios::binary),eye(NavDemoWriter::rayCount) {
    if(!input)throw std::runtime_error(std::string("cannot open navigation memory cost capture: ")+trace);
    std::array<char,8> magic;
    NavMemoryCost::read(input,magic);
    if(magic!=NavMemoryCost::Magic)throw std::runtime_error("invalid navigation memory cost capture");
    uint32_t poseBytes=0,rayBytes=0,tileBytes=0;
    NavMemoryCost::read(input,poseBytes);NavMemoryCost::read(input,rayBytes);NavMemoryCost::read(input,tileBytes);
    if(poseBytes!=sizeof(NavigationMemory::Pose) || rayBytes!=sizeof(NavigationEye::Ray) ||
       tileBytes!=sizeof(NavigationMemory::FamiliarTile))
      throw std::runtime_error("navigation memory cost capture ABI differs");
    uint64_t tiles=0,words=0;
    NavMemoryCost::read(input,tiles);NavMemoryCost::read(input,words);
    if(words!=(tiles+63)/64)throw std::runtime_error("navigation memory cost capture mask differs from atlas");
    map.tiles.resize(size_t(tiles));mask.resize(size_t(words));
    for(auto& tile:map.tiles)NavMemoryCost::read(input,tile);
    for(auto& word:mask)NavMemoryCost::read(input,word);
    map.finish();
    // Same order as the replay benchmark: policy defaults, then the captured map.
    policy.configureMemory(memory,journey);
    if(!mask.empty())memory.setFamiliarMap(&map,mask);
    }

  // Applies events up to and including the next readout.
  bool next(float* near,float* far,float* vector) {
    while(input.peek()!=std::char_traits<char>::eof()) {
      uint8_t kind=0;
      NavMemoryCost::read(input,kind);
      if(kind==NavMemoryCost::Update) {
        NavigationMemory::Pose pose;
        uint32_t count=0,changed=0;
        NavMemoryCost::read(input,pose);NavMemoryCost::read(input,count);NavMemoryCost::read(input,changed);
        if(changed>count)throw std::runtime_error("invalid navigation memory cost capture ray count");
        rays.resize(count);
        for(uint32_t i=0;i<changed;++i) {
          uint32_t index=0;NavigationEye::Ray ray;
          NavMemoryCost::read(input,index);NavMemoryCost::read(input,ray);
          if(index>=count)throw std::runtime_error("invalid navigation memory cost capture ray slot");
          rays[index]=ray;
          }
        memory.update(rays,pose);
        ++updates;
        }
      else if(kind==NavMemoryCost::View) {
        memory.view(view);
        if(rays.size()!=2*eye.size())throw std::runtime_error("capture does not have the current + cached eye layout");
        std::copy_n(rays.begin(),eye.size(),eye.begin());
        NavDemoWriter::Snapshot observation;
        observation.eye=&eye;observation.memory=&view;
        observation.position={view.pose.position.x,view.pose.position.y,view.pose.position.z};
        const auto features=policy.encode(observation,false);
        std::memcpy(near,features.near.data(),features.near.size()*sizeof(float));
        std::memcpy(far,features.far.data(),features.far.size()*sizeof(float));
        std::memcpy(vector,features.vector.data(),features.vector.size()*sizeof(float));
        ++views;
        return true;
        }
      else throw std::runtime_error("invalid navigation memory cost capture event");
      }
    return false;
    }
  };
}

extern "C" {
const char* navref_error() { return lastError.c_str(); }

// journey: nullptr keeps the policy's memory version; "0" or "1" overrides it.
void* navref_open(const char* trace,const char* weights,const char* journey) {
  try { return new Replay(trace,weights,journey); }
  catch(const std::exception& error) { lastError=error.what(); return nullptr; }
  }

void navref_close(void* handle) { delete static_cast<Replay*>(handle); }

int64_t navref_sizes(void* handle,int64_t* near,int64_t* far,int64_t* vector) {
  const auto& policy=static_cast<Replay*>(handle)->policy;
  *near=int64_t(policy.nearInputSize());*far=int64_t(policy.farInputSize());*vector=int64_t(policy.vectorInputSize());
  return int64_t(policy.weightVersion());
  }

int navref_journey(void* handle) { return static_cast<Replay*>(handle)->memory.journeyMemoryEnabled() ? 1 : 0; }

// 1: a readout was written; 0: end of capture; -1: error (see navref_error).
int navref_next(void* handle,float* near,float* far,float* vector,uint64_t* updates) {
  try {
    auto& replay=*static_cast<Replay*>(handle);
    const bool more=replay.next(near,far,vector);
    *updates=replay.updates;
    return more ? 1 : 0;
    }
  catch(const std::exception& error) { lastError=error.what(); return -1; }
  }

// libm's float tangent, as used by NavigationMemory::classificationLimits.
float navref_tanf(float value) { return std::tan(value); }
}
