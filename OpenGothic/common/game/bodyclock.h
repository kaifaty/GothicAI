#pragma once

#include <chrono>
#include <optional>

// Body control and observations use this clock on the simulation/IPC thread.
class BodyClock final {
  public:
    using time_point = std::chrono::steady_clock::time_point;

    static time_point now() {
      return manual ? *manual : std::chrono::steady_clock::now();
      }

    // Tests install a fixed time before creating observations or leases.
    static void setManual(time_point value) { manual=value; }
    static void advance(std::chrono::milliseconds delta) { manual.value()+=delta; }
    static void useRealTime() { manual.reset(); }

  private:
    inline static std::optional<time_point> manual;
  };
