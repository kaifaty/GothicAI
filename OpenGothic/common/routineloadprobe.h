#pragma once

#include <Tempest/Log>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace LoadProbe::Routines {
inline std::atomic<uint64_t> functions{0}, functionNs{0};
class Function {
  public:
    Function() {
      static const bool enabled=[] {
        const auto* value=std::getenv("OPENGOTHIC_LOADPROBE");
        return value!=nullptr && std::strcmp(value,"1")==0;
        }();
      active=enabled;
      if(active) {
        functions.fetch_add(1,std::memory_order_relaxed);
        started=std::chrono::steady_clock::now();
        }
      }
    ~Function() {
      if(active) functionNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-started).count()),std::memory_order_relaxed);
      }
  private:
    bool active=false;
    std::chrono::steady_clock::time_point started;
  };
inline void mark(std::string_view stage) {
  if(stage=="session_begin") { functions=0; functionNs=0; }
  char line[224];
  std::snprintf(line,sizeof(line),"[loadprobe-routines] stage=%.*s vm_functions=%llu vm_ms=%.6f",
                int(stage.size()),stage.data(),(unsigned long long)functions.load(),double(functionNs.load())/1e6);
  Tempest::Log::i(line);
  }
}
