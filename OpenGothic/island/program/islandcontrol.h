#pragma once

// Where the hero's Control comes from, step by step (engine layers, step 8.6): the keys and the mouse, or a track
// that is the same every run. A recorded track (8.7: --script) is one more ControlSource.

#include "islandhero.h"

#include <Tempest/Event>

#include <cstdint>

namespace island {

class ControlSource {
  public:
    virtual ~ControlSource() = default;
    // The control for the step [time, time+dt) of the program's own time, milliseconds.
    virtual Control next(uint64_t time, uint64_t dt) = 0;
    // The track has nothing more to play at this time (the program may stop).
    virtual bool    finished(uint64_t /*time*/) const { return false; }
  };

// WASD, Shift to run, Space to jump; the mouse turns the hero and tilts the camera.
class Keys final : public ControlSource {
  public:
    void    key(Tempest::KeyEvent::KeyType key, bool down);
    // pixels the mouse moved by
    void    mouse(int dx, int dy);
    void    clear();
    Control next(uint64_t time, uint64_t dt) override;

  private:
    bool  ahead = false, back = false, left = false, right = false, shift = false;
    bool  jumpPressed = false;
    float turn = 0, pitch = 0;
  };

// The built-in demo, 13.5 s: a breath, a run up towards the summit across a chunk border, a turn to the right, two
// jumps on the run, a walk, a stop. World::start places the hero where this track fits.
class DemoTrack final : public ControlSource {
  public:
    static constexpr uint64_t Length = 13500;
    // the jumps' moments, milliseconds
    static constexpr uint64_t Jumps[2] = {8000, 10000};

    Control next(uint64_t time, uint64_t dt) override;
    bool    finished(uint64_t time) const override { return time>=Length; }
  };

}
