#pragma once
// Optional world-loading measurements; disabled unless OPENGOTHIC_LOADPROBE=1.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <mutex>
#include <unordered_set>
#include <type_traits>

#include <Tempest/Log>
#include <Tempest/Pixmap>
#include <Tempest/Texture2d>

#include "resources.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#include <unistd.h>
#endif

bool tempestLoadProbeEnabled();
size_t tempestProbeGpuBytes();
size_t tempestProbeGpuBufferCount();
void tempestProbeUploadBuffers(size_t& stagingCount, size_t& stagingBytes);

namespace LoadProbe {

inline bool enabled() {
  return tempestLoadProbeEnabled();
  }

inline bool serial() {
  static const bool value = [] {
    if(!enabled())
      return false;
    const char* env = std::getenv("OPENGOTHIC_LOADPROBE_SERIAL");
    return env!=nullptr && std::strcmp(env,"1")==0;
    }();
  return value;
  }

inline std::chrono::steady_clock::time_point now() {
  return enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  }

inline std::atomic<uint64_t> texBytes{0};
inline std::atomic<uint64_t> texNs{0};
inline std::atomic<uint64_t> texCount{0};

inline std::chrono::steady_clock::time_point& origin() {
  static auto t = std::chrono::steady_clock::now();
  return t;
  }

inline double msSince(std::chrono::steady_clock::time_point t) {
  if(!enabled())
    return 0;
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
  }

// Only the synchronous authored-vob phase owns this window. Counters are atomic;
// nested scopes subtract children on their own thread, so neither vob subtrees
// nor nested resource operations are counted twice in the exclusive column.
namespace Vobs {
inline constexpr const char* typeNames[] = {
  "static_mesh", "decal", "pfx", "light", "sound", "mover", "mob_container",
  "mob_door", "mob_other", "item", "trigger", "zone", "other"
  };
inline constexpr const char* partNames[] = {
  "mesh_hit", "mesh_miss", "mesh_read", "mesh_pack", "mesh_gpu", "mesh_shape",
  "mesh_shape_cook", "mesh_proto_cpu", "texture_request", "texture_load", "texture_demand",
  "draw_instance", "physics_mesh", "physics_body", "script_item", "script_contents", "script_vm",
  "light", "sound", "sound_load", "pfx", "pfx_load"
  };
struct Counter {
  std::atomic<uint64_t> count{0}, ns{0}, inclusiveNs{0};
  void clear() { count=0; ns=0; inclusiveNs=0; }
  };
inline std::array<Counter,std::size(typeNames)> types;
inline std::array<Counter,std::size(partNames)> parts;
inline std::array<Counter,std::size(partNames)> missParts;
inline std::atomic<bool> active{false};
inline std::mutex meshSync;
inline std::unordered_set<std::string> meshes;
inline uint64_t meshHits=0, meshMisses=0;

template<bool IsType> class Timer {
  public:
    template<class Name> explicit Timer(Name name) {
      if(!enabled() || !active.load(std::memory_order_acquire))
        return;
      if constexpr(std::is_invocable_r_v<const char*,Name>)
        counter=find(name());
      else
        counter=find(name);
      previous=top;
      if constexpr(!IsType)
        inMiss=previous!=nullptr && (previous->inMiss || previous->counter==&parts[1]); // mesh_miss
      top=this;
      started=std::chrono::steady_clock::now();
      }
    ~Timer() { stop(); }
    Timer(const Timer&)=delete;
    Timer& operator=(const Timer&)=delete;

    void rename(const char* name) {
      if(counter!=nullptr)
        counter=find(name);
      }
    void stop() {
      if(counter==nullptr)
        return;
      const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now()-started).count());
      counter->count.fetch_add(1,std::memory_order_relaxed);
      counter->ns.fetch_add(ns-childNs,std::memory_order_relaxed);
      counter->inclusiveNs.fetch_add(ns,std::memory_order_relaxed);
      if constexpr(!IsType) {
        if(inMiss) {
          auto& miss=missParts[size_t(counter-parts.data())];
          miss.count.fetch_add(1,std::memory_order_relaxed);
          miss.ns.fetch_add(ns-childNs,std::memory_order_relaxed);
          miss.inclusiveNs.fetch_add(ns,std::memory_order_relaxed);
          }
        }
      top=previous;
      if(previous!=nullptr)
        previous->childNs+=ns;
      counter=nullptr;
      }
  private:
    static Counter* find(const char* name) {
      if constexpr(IsType) {
        for(size_t i=0;i<std::size(typeNames);++i)
          if(std::strcmp(name,typeNames[i])==0) return &types[i];
        } else {
        for(size_t i=0;i<std::size(partNames);++i)
          if(std::strcmp(name,partNames[i])==0) return &parts[i];
        }
      std::abort(); // Probe names are fixed at their call sites.
      }
    inline static thread_local Timer* top=nullptr;
    Counter* counter=nullptr;
    Timer* previous=nullptr;
    std::chrono::steady_clock::time_point started;
    uint64_t childNs=0;
    bool inMiss=false;
  };
using Scope=Timer<false>;
using TypeScope=Timer<true>;

inline void mesh(std::string_view name, bool hit) {
  if(!enabled() || !active.load(std::memory_order_acquire))
    return;
  std::lock_guard<std::mutex> lock(meshSync);
  meshes.emplace(name);
  (hit ? meshHits : meshMisses)++;
  }

class Stage {
  public:
    Stage() {
      if(!enabled())
        return;
      for(auto& c:types) c.clear();
      for(auto& c:parts) c.clear();
      for(auto& c:missParts) c.clear();
      {
      std::lock_guard<std::mutex> lock(meshSync);
      meshes.clear();
      meshHits=meshMisses=0;
      }
      started=std::chrono::steady_clock::now();
      running=true;
      active.store(true,std::memory_order_release);
      }
    ~Stage() { stop(); }
    Stage(const Stage&)=delete;
    Stage& operator=(const Stage&)=delete;
    double stop() {
      if(!running)
        return 0;
      const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
      active.store(false,std::memory_order_release);
      running=false;
      auto log=[](const char* kind, const char* name, const Counter& c) {
        char line[256];
        std::snprintf(line,sizeof(line),"[loadprobe-vobs] kind=%s name=%s count=%llu ms=%.6f inclusive_ms=%.6f",
                      kind,name,(unsigned long long)c.count.load(),double(c.ns.load())/1e6,double(c.inclusiveNs.load())/1e6);
        Tempest::Log::i(line);
        };
      for(size_t i=0;i<types.size();++i) log("type",typeNames[i],types[i]);
      for(size_t i=0;i<parts.size();++i) log("part",partNames[i],parts[i]);
      for(size_t i=0;i<missParts.size();++i) log("miss_part",partNames[i],missParts[i]);
      {
      std::lock_guard<std::mutex> lock(meshSync);
      char line[192];
      std::snprintf(line,sizeof(line),"[loadprobe-vobs] kind=meshes unique=%zu hits=%llu misses=%llu",
                    meshes.size(),(unsigned long long)meshHits,(unsigned long long)meshMisses);
      Tempest::Log::i(line);
      }
      return elapsed;
      }
  private:
    bool running=false;
    std::chrono::steady_clock::time_point started;
  };
}

// resident and (on macOS) phys_footprint, which also counts Metal allocations of the process
inline void processMb(double& rss, double& footprint) {
  rss = footprint = 0;
#if defined(__APPLE__)
  task_vm_info_data_t info = {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if(task_info(mach_task_self(),TASK_VM_INFO,task_info_t(&info),&count)==KERN_SUCCESS) {
    rss       = double(info.resident_size)/1048576.0;
    footprint = double(info.phys_footprint)/1048576.0;
    }
#elif defined(__linux__)
  if(FILE* f = std::fopen("/proc/self/statm","r")) {
    long size=0, resident=0;
    if(std::fscanf(f,"%ld %ld",&size,&resident)==2)
      rss = double(resident)*double(sysconf(_SC_PAGESIZE))/1048576.0;
    std::fclose(f);
    }
  footprint = rss;
#endif
  }

inline void markMalloc(std::string_view stage, double elapsed) {
  if(stage!="session_end" && stage!="stream_warm" && stage!="stream_warm_5s")
    return;
  size_t inUse=0, allocated=0;
#if defined(__APPLE__)
  vm_address_t* zones=nullptr;
  unsigned count=0;
  if(malloc_get_all_zones(mach_task_self(),nullptr,&zones,&count)!=KERN_SUCCESS) {
    Tempest::Log::e("[loadprobe-malloc] malloc_get_all_zones failed");
    return;
    }
  for(unsigned i=0; i<count; ++i) {
    malloc_statistics_t stats={};
    malloc_zone_statistics(reinterpret_cast<malloc_zone_t*>(zones[i]),&stats);
    inUse     += stats.size_in_use;
    allocated += stats.size_allocated;
    }
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2,33)
  const auto stats = mallinfo2();
  inUse     = stats.uordblks;
  allocated = stats.arena+stats.hblkhd;
#else
  return;
#endif
#else
  return;
#endif
  char buf[192];
  std::snprintf(buf,sizeof(buf),
                "[loadprobe-malloc] stage=%.*s t_ms=%.1f malloc_in_use_mb=%.3f malloc_allocated_mb=%.3f",
                int(stage.size()),stage.data(),elapsed,double(inUse)/1048576.0,double(allocated)/1048576.0);
  Tempest::Log::i(buf);
  }

inline uint64_t textureBytes(const Tempest::Texture2d& t) {
  if(t.isEmpty())
    return 0;
  uint64_t sum = 0;
  const auto frm   = t.format();
  const auto block = Tempest::Pixmap::blockSizeForFormat(frm);
  for(uint32_t i=0; i<std::max<uint32_t>(1,t.mipCount()); ++i) {
    const uint32_t w = std::max<uint32_t>(1,uint32_t(t.w())>>i);
    const uint32_t h = std::max<uint32_t>(1,uint32_t(t.h())>>i);
    const auto c = Tempest::Pixmap::blockCount(frm,w,h);
    sum += uint64_t(c.w)*uint64_t(c.h)*block;
    }
  return sum;
  }

inline void countTexture(const Tempest::Texture2d& t, std::chrono::steady_clock::time_point started) {
  if(!enabled())
    return;
  texCount++;
  texBytes += textureBytes(t);
  texNs    += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count());
  }

// one line per stage: name, stage time, wall time since the first mark, memory, textures so far
inline void mark(std::string_view stage, double stageMs = -1, std::string_view extra = {}) {
  if(!enabled())
    return;
  const double elapsed = msSince(origin());
  double rss=0, fp=0;
  processMb(rss,fp);
  size_t stagingCount=0, stagingBytes=0;
  tempestProbeUploadBuffers(stagingCount,stagingBytes);
  char buf[512];
  std::snprintf(buf,sizeof(buf),
                "[loadprobe] stage=%.*s stage_ms=%.1f t_ms=%.1f rss_mb=%.1f footprint_mb=%.1f gpu_mb=%.1f "
                "tex_n=%llu tex_mb=%.1f tex_ms=%.1f %.*s",
                int(stage.size()),stage.data(),stageMs,elapsed,rss,fp,
                double(tempestProbeGpuBytes())/1048576.0,
                (unsigned long long)texCount.load(),double(texBytes.load())/1048576.0,double(texNs.load())/1e6,
                int(extra.size()),extra.empty() ? "" : extra.data());
  Tempest::Log::i(buf);
  const auto textures = Resources::textureStats();
  char textureBuf[512];
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_live_bytes=%llu tex_retired_bytes=%llu "
                "tex_peak_bytes=%llu tex_live_n=%llu tex_evicted=%llu tex_reloaded=%llu",
                int(stage.size()),stage.data(),elapsed,
                (unsigned long long)textures.liveBytes,(unsigned long long)textures.retiredBytes,
                (unsigned long long)textures.peakBytes,(unsigned long long)textures.count,
                (unsigned long long)textures.evicted,(unsigned long long)textures.reloaded);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_full_bytes=%llu tex_tail_bytes=%llu "
                "tex_promoted=%llu tex_demoted=%llu tex_promotion_queue=%llu tex_near_tail_n=%llu tex_near_tail_seconds=%.9g",
                int(stage.size()),stage.data(),elapsed,
                (unsigned long long)textures.fullBytes,(unsigned long long)textures.tailBytes,
                (unsigned long long)textures.promoted,(unsigned long long)textures.demoted,
                (unsigned long long)textures.promotionQueue,(unsigned long long)textures.nearTailCount,textures.nearTailSeconds);
  Tempest::Log::i(textureBuf);
  const char* textureSources[]={"base","hd","material","other"};
  for(size_t i=0;i<textures.sourceBytes.size();++i) {
    std::snprintf(textureBuf,sizeof(textureBuf),
                  "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_source_%s_bytes=%llu tex_source_%s_full_bytes=%llu",
                  int(stage.size()),stage.data(),elapsed,textureSources[i],(unsigned long long)textures.sourceBytes[i],
                  textureSources[i],(unsigned long long)textures.sourceFullBytes[i]);
    Tempest::Log::i(textureBuf);
    }
  std::snprintf(textureBuf,sizeof(textureBuf),"[loadprobe-textures] stage=%.*s t_ms=%.1f tex_full_ring2_bytes=%llu",
                int(stage.size()),stage.data(),elapsed,(unsigned long long)textures.fullRing2Bytes);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f "
                "tex_landscape_bytes=%llu tex_landscape_n=%llu tex_static_bytes=%llu tex_static_n=%llu",
                int(stage.size()),stage.data(),elapsed,
                (unsigned long long)textures.categoryBytes[0],(unsigned long long)textures.categoryCount[0],
                (unsigned long long)textures.categoryBytes[1],(unsigned long long)textures.categoryCount[1]);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f "
                "tex_dynamic_bytes=%llu tex_dynamic_n=%llu tex_other_bytes=%llu tex_other_n=%llu",
                int(stage.size()),stage.data(),elapsed,
                (unsigned long long)textures.categoryBytes[2],(unsigned long long)textures.categoryCount[2],
                (unsigned long long)textures.categoryBytes[3],(unsigned long long)textures.categoryCount[3]);
  Tempest::Log::i(textureBuf);
  std::snprintf(buf,sizeof(buf),
                "[loadprobe-metal] stage=%.*s t_ms=%.1f staging_n=%zu staging_bytes=%zu",
                int(stage.size()),stage.data(),elapsed,stagingCount,stagingBytes);
  Tempest::Log::i(buf);
  markMalloc(stage,elapsed);
  }

}
