#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>

// Body control and observations use this clock on the simulation/IPC thread.
class BodyClock final {
  public:
    using time_point = std::chrono::steady_clock::time_point;

    static time_point now() {
      return manual ? *manual : std::chrono::steady_clock::now()+offset;
      }

    // Freeze the current control time; switching modes never changes its epoch.
    static void useManualTime() { manual=now(); }
    static void advance(std::chrono::milliseconds delta) {
      if(delta.count()<0) throw std::invalid_argument("body clock cannot advance backwards");
      manual.value()+=delta;
      }
    static void useRealTime() {
      if(!manual) return;
      offset=*manual-std::chrono::steady_clock::now();
      manual.reset();
      }

  private:
    inline static std::chrono::steady_clock::duration offset{};
    inline static std::optional<time_point> manual;
  };
