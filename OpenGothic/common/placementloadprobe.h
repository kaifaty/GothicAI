#pragma once

#include "npcloadprobe.h"

namespace LoadProbe::Placement {
inline std::atomic<uint64_t> functions{0}, deferredCalls{0}, elapsedNs{0};
inline thread_local unsigned depth=0, fallbackDepth=0;
inline bool enabled() { return Npcs::enabled(); }

class Scope {
  public:
    explicit Scope(bool fallback):fallback(fallback) {
      ++depth;
      if(fallback) ++fallbackDepth;
      if(enabled()) started=std::chrono::steady_clock::now();
      }
    ~Scope() {
      if(enabled()) elapsedNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-started).count()),std::memory_order_relaxed);
      if(fallback) --fallbackDepth;
      --depth;
      }
  private:
    bool fallback;
    std::chrono::steady_clock::time_point started;
  };

inline void mark(std::string_view stage) {
  if(stage=="session_begin") { functions=0; deferredCalls=0; elapsedNs=0; }
  char line[224];
  std::snprintf(line,sizeof(line),"[loadprobe-placement] stage=%.*s vm_functions=%llu placement_ms=%.6f deferred_calls=%llu",
                int(stage.size()),stage.data(),(unsigned long long)functions.load(),
                double(elapsedNs.load())/1e6,(unsigned long long)deferredCalls.load());
  Tempest::Log::i(line);
  }
}
