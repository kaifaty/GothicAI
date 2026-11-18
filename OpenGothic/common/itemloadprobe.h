#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace LoadProbe::Items {
inline std::atomic<uint64_t> constructors{0}, constructorNs{0};
class Constructor {
  public:
    Constructor() {
      static const bool enabled=[] {
        const auto* value=std::getenv("OPENGOTHIC_LOADPROBE");
        return value!=nullptr && std::strcmp(value,"1")==0;
        }();
      active=enabled;
      if(active) {
        constructors.fetch_add(1,std::memory_order_relaxed);
        started=std::chrono::steady_clock::now();
        }
      }
    ~Constructor() {
      if(active) constructorNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-started).count()),std::memory_order_relaxed);
      }
  private:
    bool active=false;
    std::chrono::steady_clock::time_point started;
  };
}
