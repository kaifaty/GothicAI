#pragma once
// Optional world-loading measurements; disabled unless OPENGOTHIC_LOADPROBE=1.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

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
