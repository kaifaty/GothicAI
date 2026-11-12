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
#include <ctime>
#include <string>
#include <string_view>
#include <mutex>
#include <unordered_set>
#include <type_traits>
#include <vector>

#include <Tempest/Log>
#include <Tempest/Pixmap>
#include <Tempest/Texture2d>

#include "resources.h"
#include <Tempest/LoadProbe>

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

struct MallocBytes { size_t inUse=0, allocated=0; };
inline MallocBytes mallocBytes() {
  size_t inUse=0, allocated=0;
#if defined(__APPLE__)
  vm_address_t* zones=nullptr;
  unsigned count=0;
  if(malloc_get_all_zones(mach_task_self(),nullptr,&zones,&count)!=KERN_SUCCESS) {
    Tempest::Log::e("[loadprobe-malloc] malloc_get_all_zones failed");
    return {};
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
  return {};
#endif
#else
  return {};
#endif
  return {inUse,allocated};
  }

inline void markMalloc(std::string_view stage, double elapsed) {
  const auto [inUse,allocated]=mallocBytes();
  char buf[192];
  std::snprintf(buf,sizeof(buf),
                "[loadprobe-malloc] stage=%.*s t_ms=%.1f malloc_in_use_mb=%.3f malloc_allocated_mb=%.3f",
                int(stage.size()),stage.data(),elapsed,double(inUse)/1048576.0,double(allocated)/1048576.0);
  Tempest::Log::i(buf);
  }


// STARTUP is synchronous. Exclusive clocks and signed allocator deltas subtract
// nested work on this thread; footprint is a process-wide observation, not ownership.
namespace Startup {
enum Kind : size_t { Script, Npc, Model, Skeleton, Animation, Texture, Item, NpcPlacement, Other, PhysicsPrepare, PhysicsBody, PlacementQueries, PhysicsRemove, Count };
inline constexpr const char* names[]={"script","npc","model","skeleton","animation","texture","item","npc_placement","other","physics_prepare","physics_body","placement_queries","physics_remove"};
struct Counter { uint64_t count=0, ns=0, inclusiveNs=0, bytes=0; int64_t malloc=0, footprint=0; };
inline thread_local std::array<Counter,Count> counters;
inline thread_local bool active=false;
inline thread_local const char* group="startup";
inline thread_local uint64_t samplingNs=0,samplingCount=0;

// Buffered STARTUP call records. No clocks, keys, allocations or output when
// the load probe is off; output happens after the existing STARTUP clocks stop.
namespace Detail {
struct Record {
  const char* name=nullptr;
  std::string key;
  size_t parent=size_t(-1);
  uint64_t ns=0, childNs=0, scriptNs=0, excludedNs=0, work=0, startedNs=0, tid=0;
  float x=0,y=0,z=0;
  bool positioned=false,hero=false;
  };
inline thread_local std::vector<Record> records;
inline thread_local size_t top=size_t(-1);
class Scope {
  public:
    template<class Key> Scope(const char* name,Key key) {
      if(!enabled() || !active || std::strcmp(group,"startup")!=0) return;
      index=records.size();
      records.push_back({name,key(),top});
      top=index;
      scriptBefore=counters[Script].ns;
      excludedBefore=excluded();
      started=std::chrono::steady_clock::now();
#if defined(__linux__)
      // gprofng timestamps use the raw monotonic clock, not steady_clock.
      if(std::strcmp(name,"script_phase")==0 || std::strcmp(name,"placement_phase")==0) {
        timespec raw{};
        ::clock_gettime(CLOCK_MONOTONIC_RAW,&raw);
        records[index].startedNs=uint64_t(raw.tv_sec)*1000000000+uint64_t(raw.tv_nsec);
        records[index].tid=uint64_t(::gettid());
        }
#endif
      }
    explicit Scope(const char* name):Scope(name,[]{return std::string{};}) {}
    ~Scope() { stop(); }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    void stop() {
      if(index==size_t(-1)) return;
      auto& row=records[index];
      if(row.startedNs==0)
        row.startedNs=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch()).count());
      row.ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now()-started).count());
      row.scriptNs=counters[Script].ns-scriptBefore;
      row.excludedNs=excluded()-excludedBefore;
      top=row.parent;
      if(top!=size_t(-1)) records[top].childNs+=row.ns;
      index=size_t(-1);
      }
    void position(float x,float y,float z,bool hero) {
      if(index==size_t(-1)) return;
      auto& row=records[index];
      row.x=x; row.y=y; row.z=z; row.hero=hero; row.positioned=true;
      }
    void work() { if(index!=size_t(-1)) ++records[index].work; }
  private:
    size_t index=size_t(-1);
    uint64_t scriptBefore=0,excludedBefore=0;
    std::chrono::steady_clock::time_point started;
    static uint64_t excluded() {
      uint64_t ns=0;
      for(size_t i=Npc;i<=Item;++i) ns+=counters[i].ns;
      return ns;
      }
  };
inline void flush() {
  for(size_t i=0;i<records.size();++i) {
    const auto& r=records[i];
    char line[512];
    std::snprintf(line,sizeof(line),"[loadprobe-startup-call] id=%zu parent=%lld name=%s ns=%llu self_ns=%llu script_ns=%llu excluded_ns=%llu work=%llu",
                  i,r.parent==size_t(-1) ? -1LL : (long long)r.parent,r.name,
                  (unsigned long long)r.ns,(unsigned long long)(r.ns-r.childNs),
                  (unsigned long long)r.scriptNs,(unsigned long long)r.excludedNs,(unsigned long long)r.work);
    Tempest::Log::i(line);
    if(std::strcmp(r.name,"script_phase")==0 || std::strcmp(r.name,"placement_phase")==0) {
      std::snprintf(line,sizeof(line),"[loadprobe-startup-clock] id=%zu start_ns=%llu tid=%llu",i,
                    (unsigned long long)r.startedNs,(unsigned long long)r.tid);
      Tempest::Log::i(line);
      }
    if(!r.key.empty()) Tempest::Log::i("[loadprobe-startup-key] id=",i," key=",r.key);
    if(r.positioned) {
      std::snprintf(line,sizeof(line),"[loadprobe-startup-position] id=%zu hero=%u x=%.3f y=%.3f z=%.3f",i,unsigned(r.hero),r.x,r.y,r.z);
      Tempest::Log::i(line);
      }
    }
  records.clear();
  }
}
class Scope {
  public:
    explicit Scope(Kind kind,bool start=false,const char* onlyGroup=nullptr):kind(kind) {
      if(!enabled() || (!active && !start) || (!start && onlyGroup && std::strcmp(group,onlyGroup)!=0)) return;
      if(start && !active) {
        counters={};
        group=onlyGroup ? onlyGroup : "startup";
        samplingNs=samplingCount=0;
        if(std::strcmp(group,"startup")==0) {
          Detail::records.clear();
          Detail::top=size_t(-1);
          }
        active=true;
        root=true;
        }
      previous=top;
      top=this;
      if(serial()) {
        const auto sampleStart=std::chrono::steady_clock::now();
        allocation=int64_t(mallocBytes().inUse);
        double rss=0,fp=0;
        processMb(rss,fp);
        footprint=int64_t(fp*1048576.0);
        samplingNs+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-sampleStart).count());
        ++samplingCount;
        }
      started=std::chrono::steady_clock::now();
      running=true;
      }
    ~Scope() {
      if(!running) return;
      const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now()-started).count());
      int64_t delta=0,fpDelta=0;
      if(serial()) {
        const auto sampleStart=std::chrono::steady_clock::now();
        delta=int64_t(mallocBytes().inUse)-allocation;
        double rss=0,fp=0;
        processMb(rss,fp);
        fpDelta=int64_t(fp*1048576.0)-footprint;
        samplingNs+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-sampleStart).count());
        ++samplingCount;
        }
      auto& c=counters[kind];
      ++c.count; c.ns+=ns-childNs; c.inclusiveNs+=ns;
      c.malloc+=delta-childMalloc; c.footprint+=fpDelta-childFootprint;
      top=previous;
      if(previous) {
        previous->childNs+=ns;
        previous->childMalloc+=delta;
        previous->childFootprint+=fpDelta;
        }
      if(root) {
        active=false;
        for(size_t i=0;i<(std::strcmp(group,"startup")==0 ? size_t(Other) : size_t(Count));++i) {
          const auto& v=counters[i];
          char line[256];
          std::snprintf(line,sizeof(line),"[loadprobe-%s] name=%s count=%llu ms=%.6f inclusive_ms=%.6f bytes=%llu malloc_bytes=%lld footprint_bytes=%lld memory_sampled=%u",
                        group,names[i],(unsigned long long)v.count,double(v.ns)/1e6,double(v.inclusiveNs)/1e6,
                        (unsigned long long)v.bytes,(long long)v.malloc,(long long)v.footprint,unsigned(serial()));
          Tempest::Log::i(line);
          }
        Tempest::Log::i("[loadprobe-overhead] ",std::strcmp(group,"startup")==0 ? "startup" : "placed_items",
                       "_memory_ns=",samplingNs," ",std::strcmp(group,"startup")==0 ? "startup" : "placed_items",
                       "_memory_samples=",samplingCount);
        if(std::strcmp(group,"startup")==0) Detail::flush();
        }
      }
    static void bytes(Kind kind,uint64_t value) {
      if(enabled() && active) counters[kind].bytes+=value;
      }
  private:
    inline static thread_local Scope* top=nullptr;
    Scope* previous=nullptr;
    Kind kind;
    bool running=false,root=false;
    uint64_t childNs=0;
    int64_t allocation=0,footprint=0,childMalloc=0,childFootprint=0;
    std::chrono::steady_clock::time_point started;
  };
}

namespace Requests {
inline std::atomic<bool> active{false};
inline std::mutex sync;
inline constexpr const char* names[]={"model","animation","texture"};
inline std::array<std::unordered_set<std::string>,3> unique;
inline std::array<uint64_t,3> requests{};
inline std::unordered_set<std::string> npcModels;
inline void begin() {
  if(!enabled()) return;
  std::lock_guard lock(sync);
  for(auto& set:unique) set.clear();
  requests={}; npcModels.clear(); active=true;
  }
inline void add(size_t kind,std::string_view name) {
  if(!enabled() || !active) return;
  std::lock_guard lock(sync);
  ++requests[kind]; unique[kind].emplace(name);
  if(kind==0 && Startup::active && std::strcmp(Startup::group,"startup")==0) npcModels.emplace(name);
  }
inline void end() {
  if(!enabled()) return;
  std::lock_guard lock(sync);
  active=false;
  for(size_t i=0;i<unique.size();++i) {
    Tempest::Log::i("[loadprobe-requests] name=",names[i]," count=",requests[i]," unique=",unique[i].size(),
                   " repeated=",requests[i]-unique[i].size());
    unique[i].clear(); unique[i].rehash(0);
    }
  }
}

inline std::chrono::steady_clock::time_point loadFinished;
inline bool firstFramePending=false, pipelinesPending=false;
inline void pipelines(std::string_view stage) {
  constexpr const char* names[]={"library","graphics","compute","convert"};
  for(size_t i=0;i<TempestLoadProbe::Count;++i) {
    const auto& c=TempestLoadProbe::pipelines[i];
    char line[256];
    std::snprintf(line,sizeof(line),"[loadprobe-pipelines] stage=%.*s name=%s count=%llu ms=%.6f post_load_count=%llu post_load_ms=%.6f",
                  int(stage.size()),stage.data(),names[i],(unsigned long long)c.calls.load(),double(c.ns.load())/1e6,
                  (unsigned long long)c.postLoadCalls.load(),double(c.postLoadNs.load())/1e6);
    Tempest::Log::i(line);
    }
  }
inline void loaded() {
  if(!enabled()) return;
  loadFinished=std::chrono::steady_clock::now();
  TempestLoadProbe::loadEndNs=TempestLoadProbe::clockNs();
  firstFramePending=pipelinesPending=true;
  pipelines("load_end");
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
  Startup::Scope::bytes(Startup::Texture,textureBytes(t));
  texNs    += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count());
  }

// one line per stage: name, stage time, wall time since the first mark, memory, textures so far
inline void mark(std::string_view stage, double stageMs = -1, std::string_view extra = {}) {
  if(!enabled())
    return;
  const double elapsed = msSince(origin());
#if defined(__APPLE__) || defined(__linux__)
  if(stage=="session_begin" || stage=="change_begin") {
    double load[3]={};
    if(::getloadavg(load,3)==3) {
      char line[160];
      std::snprintf(line,sizeof(line),"[loadprobe-host] load_1=%.2f load_5=%.2f load_15=%.2f",load[0],load[1],load[2]);
      Tempest::Log::i(line);
      }
    }
#endif
  double rss=0, fp=0;
  processMb(rss,fp);
  size_t stagingCount=0, stagingBytes=0;
  tempestProbeUploadBuffers(stagingCount,stagingBytes);
  // Extra fields in order: landscape (main line), vob_near=... (vob
  // streaming), npc_full=... (NPC simulation tiers, world/npcsimtier.h).
  const auto npcStart=extra.find("npc_full=");
  const auto logicStart=extra.find("npc_logic_samples=");
  const auto beforeNpc=extra.substr(0,npcStart);
  const auto vobStart=beforeNpc.find("vob_near=");
  const auto landscapeExtra=beforeNpc.substr(0,vobStart);
  char buf[512];
  std::snprintf(buf,sizeof(buf),
                "[loadprobe] stage=%.*s stage_ms=%.1f t_ms=%.1f rss_mb=%.1f footprint_mb=%.1f gpu_mb=%.1f "
                "tex_n=%llu tex_mb=%.1f tex_ms=%.1f %.*s",
                int(stage.size()),stage.data(),stageMs,elapsed,rss,fp,
                double(tempestProbeGpuBytes())/1048576.0,
                (unsigned long long)texCount.load(),double(texBytes.load())/1048576.0,double(texNs.load())/1e6,
                int(landscapeExtra.size()),landscapeExtra.empty() ? "" : landscapeExtra.data());
  Tempest::Log::i(buf);
  // Tempest's log line is limited to 255 bytes. Keep the vob and NPC
  // measurements in short records with the same timestamp as the checkpoint.
  const auto records=[&](const char* tag, std::string_view fields) {
    while(!fields.empty() && fields.back()==' ')
      fields.remove_suffix(1);
    while(!fields.empty()) {
      size_t count=fields.size();
      if(count>150) {
        const auto space=fields.rfind(' ',150);
        count=(space==std::string_view::npos || space==0) ? 150 : space;
        }
      std::snprintf(buf,sizeof(buf),"[%s] stage=%.*s t_ms=%.1f %.*s",tag,
                    int(stage.size()),stage.data(),elapsed,int(count),fields.data());
      Tempest::Log::i(buf);
      fields.remove_prefix(count);
      if(!fields.empty())
        fields.remove_prefix(1);
      }
    };
  if(vobStart!=std::string_view::npos)
    records("loadprobe-vob-streaming",beforeNpc.substr(vobStart));
  if(npcStart!=std::string_view::npos)
    records("loadprobe-npc-tier",extra.substr(npcStart,logicStart==std::string_view::npos ? logicStart : logicStart-npcStart));
  if(logicStart!=std::string_view::npos)
    records("loadprobe-npc-logic",extra.substr(logicStart));
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
  const auto& level = textures.levelBytes;
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_level0_bytes=%llu tex_level1_bytes=%llu tex_level2_bytes=%llu "
                "tex_level3_bytes=%llu tex_level4_bytes=%llu tex_level5_bytes=%llu",
                int(stage.size()),stage.data(),elapsed,(unsigned long long)level[0],(unsigned long long)level[1],
                (unsigned long long)level[2],(unsigned long long)level[3],(unsigned long long)level[4],(unsigned long long)level[5]);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_shortfall_n=%llu tex_shortfall_seconds=%.9g tex_loads=%llu",
                int(stage.size()),stage.data(),elapsed,(unsigned long long)textures.shortfallCount,textures.shortfallSeconds,
                (unsigned long long)textures.loadsInFlight);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_jumps=%llu tex_jumps_open=%llu tex_jump_sync_max_ms=%.9g "
                "tex_jump_zero_max_ms=%.9g",
                int(stage.size()),stage.data(),elapsed,(unsigned long long)textures.jumps,(unsigned long long)textures.jumpsOpen,
                textures.jumpSyncMsMax,textures.jumpZeroMsMax);
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
  // The memory budget and the stage above the target (texturebudget.h); tex_target_source: TextureBudget::Source.
  // Tempest::Log breaks a line past 255 bytes and the summary drops the rest, so each line stays well below that.
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_device_budget_bytes=%llu tex_device_usage_bytes=%llu "
                "tex_device_shared=%d tex_device_measured=%d",
                int(stage.size()),stage.data(),elapsed,(unsigned long long)textures.device.budget,
                (unsigned long long)textures.device.usage,int(textures.device.shared),int(textures.device.measured));
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_target_bytes=%llu tex_target_source=%d tex_budget_bytes=%llu",
                int(stage.size()),stage.data(),elapsed,(unsigned long long)textures.targetBytes,int(textures.targetSource),
                (unsigned long long)textures.budgetBytes);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_stage=%u tex_stage_deepest=%u tex_stage_bytes0=%llu "
                "tex_stage_bytes=%llu",
                int(stage.size()),stage.data(),elapsed,textures.stage,textures.stageDeepest,
                (unsigned long long)textures.stageBytes0,(unsigned long long)textures.stageBytes);
  Tempest::Log::i(textureBuf);
  // Step 4 (textureflow.h): the shortfall's texture-seconds by state, the visible shortfall (reserve 0), the loads
  // in flight and their bytes, the latency demand -> publication, the wait for a slot and the bursts, since the start.
  const auto& flow = textures.flow;
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_shortfall_wait_seconds=%.9g tex_shortfall_read_seconds=%.9g "
                "tex_shortfall_upload_seconds=%.9g",
                int(stage.size()),stage.data(),elapsed,flow.seconds[TextureFlow::Waiting],flow.seconds[TextureFlow::Reading],
                flow.seconds[TextureFlow::Uploading]);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_shortfall_publish_seconds=%.9g tex_shortfall_stale_seconds=%.9g "
                "tex_shortfall_wait_n=%llu",
                int(stage.size()),stage.data(),elapsed,flow.seconds[TextureFlow::Publishing],flow.seconds[TextureFlow::Stale],
                (unsigned long long)flow.count[TextureFlow::Waiting]);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_visible_shortfall_n=%llu tex_visible_shortfall_seconds=%.9g "
                "tex_load_bytes=%llu tex_short_entries=%llu",
                int(stage.size()),stage.data(),elapsed,(unsigned long long)flow.visible,flow.visibleSeconds,
                (unsigned long long)flow.bytesInFlight,(unsigned long long)flow.entries);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_latency_n=%llu tex_latency_p50_ms=%.1f tex_latency_p90_ms=%.1f "
                "tex_latency_max_ms=%.1f",
                int(stage.size()),stage.data(),elapsed,(unsigned long long)flow.latencyN,flow.latencyP50,flow.latencyP90,
                flow.latencyMax);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_wait_p50_ms=%.1f tex_wait_p90_ms=%.1f tex_wait_max_ms=%.1f "
                "tex_bursts=%llu tex_burst_p90=%.0f tex_burst_max=%u",
                int(stage.size()),stage.data(),elapsed,flow.waitP50,flow.waitP90,flow.waitMax,
                (unsigned long long)flow.bursts,flow.burstP90,flow.burstMax);
  Tempest::Log::i(textureBuf);
  std::snprintf(textureBuf,sizeof(textureBuf),
                "[loadprobe-textures] stage=%.*s t_ms=%.1f tex_ahead_n=%llu tex_ahead_seconds=%.9g tex_lookahead_ms=%.1f "
                "tex_publications=%llu tex_publish_ms=%.9g",
                int(stage.size()),stage.data(),elapsed,(unsigned long long)flow.ahead,flow.aheadSeconds,1000*flow.horizon,
                (unsigned long long)flow.publications,flow.publishMs);
  Tempest::Log::i(textureBuf);
  // seconds on each stage reached so far, four a line
  for(uint32_t first=0; first<=textures.stageDeepest && first<textures.stageSeconds.size(); first+=4) {
    int at=std::snprintf(textureBuf,sizeof(textureBuf),"[loadprobe-textures] stage=%.*s t_ms=%.1f",
                         int(stage.size()),stage.data(),elapsed);
    for(uint32_t i=first; i<first+4 && i<=textures.stageDeepest && i<textures.stageSeconds.size(); ++i)
      if(at>0 && size_t(at)<sizeof(textureBuf))
        at+=std::snprintf(textureBuf+at,sizeof(textureBuf)-size_t(at)," tex_stage%u_seconds=%.9g",i,textures.stageSeconds[i]);
    Tempest::Log::i(textureBuf);
    }
  std::snprintf(buf,sizeof(buf),
                "[loadprobe-metal] stage=%.*s t_ms=%.1f staging_n=%zu staging_bytes=%zu",
                int(stage.size()),stage.data(),elapsed,stagingCount,stagingBytes);
  Tempest::Log::i(buf);
  markMalloc(stage,elapsed);
  }


inline void presented() {
  if(!enabled() || !pipelinesPending) return;
  if(firstFramePending) {
    firstFramePending=false;
    mark("first_frame",msSince(loadFinished));
    pipelines("first_frame");
    }
  if(msSince(loadFinished)>=10000) {
    pipelinesPending=false;
    mark("pipelines_10s",msSince(loadFinished));
    pipelines("pipelines_10s");
    }
  }

}
