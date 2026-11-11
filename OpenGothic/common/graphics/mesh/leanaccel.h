#pragma once

// LeanAccel: a body leans with its own acceleration - forward while it speeds up, back while it brakes
// (the whole-body lean of a runner follows atan(a/g), about 5.8 degrees per m/s^2). Lean adds it to the
// trunk's pitch, so a start, a stop and a change of pace show their inertia whatever clip plays; a clip
// that leans by itself gets a little more of it, held a moment longer. OPENGOTHIC_LEANACCEL=0 turns it off.
namespace LeanAccel {

struct State final {
  float speed = 0; // cm/s along the facing, smoothed
  float accel = 0; // cm/s^2, smoothed
  };

bool  enabled();
void  reset(State& s);
// One frame: the body's speed along its facing (cm/s, minus is backwards) over dt seconds
void  step(State& s, float speed, float dt);
// The trunk's pitch this acceleration asks for, radians, + forward. Steps of a run loop speed the root up
// and down a little: an acceleration under the dead zone leans nothing
float pitch(const State& s);

}
