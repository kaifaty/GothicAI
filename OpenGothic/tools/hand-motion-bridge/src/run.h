#pragma once

// The two ways packets reach the bridge: a record played at its pace, or the live SDL gamepads.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include "motion.h"
#include "record.h"

namespace Bridge {

using Clock = std::chrono::steady_clock;

int64_t nowUs();

struct ReplayOptions {
  // 0: play forever; one pass follows the other without a pause, the times go on growing
  int                       passes = 1;
  // 1 is the record's pace; <= 0: as fast as possible (the self-test)
  double                    speed  = 1;
  const std::atomic<bool>*  stop   = nullptr;
  // after every packet
  std::function<void()>     tick;
  };

// The arrivals are moved onto the current steady_clock, so the frames carry today's times.
void replay(const std::vector<RecordEvent>& events, Bridge& bridge, const ReplayOptions& o);

// a line per hand once a second: device, samples per second, the bias, roll, pitch and yaw
class StatusLine final {
  public:
    explicit StatusLine(Bridge& b);
    void tick();

  private:
    Bridge&           bridge;
    Clock::time_point mark;
  };

// SDL gamepads until `stop`; 0 or 1 to return from main
int runLive(Bridge& bridge, RecordWriter* rec, const std::atomic<bool>& stop, bool status);

}
