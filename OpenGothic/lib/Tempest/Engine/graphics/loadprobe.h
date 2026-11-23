#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

bool tempestLoadProbeEnabled();

namespace TempestLoadProbe {
// Convert: SPIR-V to MSL translation (Metal only), measured to compare with the library compile.
enum Operation : size_t { Library, Graphics, Compute, Convert, Count };
struct Counter {
  std::atomic<uint64_t> calls{0}, ns{0}, postLoadCalls{0}, postLoadNs{0};
  };
inline std::array<Counter,Count> pipelines;
inline std::atomic<int64_t> loadEndNs{0};
inline int64_t clockNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::steady_clock::now().time_since_epoch()).count();
  }
class PipelineScope {
  public:
    explicit PipelineScope(Operation op):op(op) {
      if(tempestLoadProbeEnabled()) started=clockNs();
      }
    ~PipelineScope() { stop(); }
    void stop() {
      if(started==0) return;
      const auto elapsed=uint64_t(clockNs()-started);
      auto& c=pipelines[op];
      c.calls.fetch_add(1,std::memory_order_relaxed);
      c.ns.fetch_add(elapsed,std::memory_order_relaxed);
      const auto end=loadEndNs.load(std::memory_order_relaxed);
      if(end!=0 && started>=end && started<end+10000000000LL) {
        c.postLoadCalls.fetch_add(1,std::memory_order_relaxed);
        c.postLoadNs.fetch_add(elapsed,std::memory_order_relaxed);
        }
      started=0;
      }
  private:
    Operation op;
    int64_t started=0;
  };
}
