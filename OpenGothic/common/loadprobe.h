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

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

bool tempestLoadProbeEnabled();
size_t tempestProbeGpuBytes();
size_t tempestProbeGpuBufferCount();

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
  char buf[512];
  std::snprintf(buf,sizeof(buf),
                "[loadprobe] stage=%.*s stage_ms=%.1f t_ms=%.1f rss_mb=%.1f footprint_mb=%.1f gpu_mb=%.1f "
                "tex_n=%llu tex_mb=%.1f tex_ms=%.1f %.*s",
                int(stage.size()),stage.data(),stageMs,elapsed,rss,fp,
                double(tempestProbeGpuBytes())/1048576.0,
                (unsigned long long)texCount.load(),double(texBytes.load())/1048576.0,double(texNs.load())/1e6,
                int(extra.size()),extra.empty() ? "" : extra.data());
  Tempest::Log::i(buf);
  }

}
