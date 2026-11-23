#pragma once

#include <cstddef>
#include <cstdlib>

// TransEnd: the root's way over the last frame of a clip that is not a loop. A record of N frames plays
// N frame times, but its root has only N-1 steps; the way used to wrap there like a loop's and played the
// clip's first step again. A run start that begins slowly stalled for a frame as it handed over to the
// loop, and Gothic's own stop lurched 6 cm forward at its very end. Now the body keeps the speed it ended
// with, also over the rest of the game frame in which the clip ends (one frame of the clip at most): the
// next clip starts its own clock only on the next game frame. OPENGOTHIC_TRANSEND=0 brings the wrap back.
namespace TransEnd {

inline bool enabled() {
  static const bool on = []() {
    const char* v = std::getenv("OPENGOTHIC_TRANSEND");
    return v==nullptr || v[0]!='0';
    }();
  return on;
  }

// steps - how many steps the root has (frames - 1), frame - where the clip is, in frames.
// True past the last sample; over - how far past it, in frames
inline bool past(float frame, size_t steps, float& over) {
  if(steps==0 || frame<float(steps))
    return false;
  over = frame-float(steps);
  return true;
  }

// The way at `over` frames past the last sample: end - the last sample, prev - the one before it
inline float extend(float end, float prev, float over) {
  return end + (end-prev)*over;
  }

}
