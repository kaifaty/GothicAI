#pragma once

#include <Tempest/Log>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace LoadProbe::Npcs {
inline std::atomic<uint64_t> constructors{0}, constructorNs{0}, deferredCalls{0}, scriptCalls{0};
inline thread_local unsigned initializationDepth=0;
inline bool enabled() {
  static const bool value=[] {
    const auto* env=std::getenv("OPENGOTHIC_LOADPROBE");
    return env!=nullptr && std::strcmp(env,"1")==0;
    }();
  return value;
  }
class Initialization {
  public:
    Initialization() { ++initializationDepth; }
    ~Initialization() { --initializationDepth; }
  };
class Constructor {
  public:
    Constructor() {
      if(enabled()) {
        constructors.fetch_add(1,std::memory_order_relaxed);
        started=std::chrono::steady_clock::now();
        }
      }
    ~Constructor() {
      if(enabled()) constructorNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-started).count()),std::memory_order_relaxed);
      }
  private:
    std::chrono::steady_clock::time_point started;
  };
inline void deferred() {
  if(enabled()) deferredCalls.fetch_add(1,std::memory_order_relaxed);
  }
inline void mark(std::string_view stage) {
  if(stage=="session_begin") { constructors=0; constructorNs=0; deferredCalls=0; scriptCalls=0; }
  char line[224];
  std::snprintf(line,sizeof(line),"[loadprobe-npcs] stage=%.*s vm_constructors=%llu vm_ms=%.6f deferred_calls=%llu script_calls=%llu",
                int(stage.size()),stage.data(),(unsigned long long)constructors.load(),
                double(constructorNs.load())/1e6,(unsigned long long)deferredCalls.load(),(unsigned long long)scriptCalls.load());
  Tempest::Log::i(line);
  }
}
