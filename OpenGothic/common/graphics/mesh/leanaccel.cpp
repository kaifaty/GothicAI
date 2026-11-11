#include "leanaccel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

constexpr float Deg      = 3.14159265f/180.f;
constexpr float Gravity  = 981.f;    // cm/s^2
constexpr float Share    = 0.3f;     // of the lean the physics asks for: the clips lean by themselves too
constexpr float MaxFwd   = 7.f*Deg;
constexpr float MaxBack  = 5.f*Deg;
constexpr float Dead     = 200.f;    // cm/s^2: the ripple of a run loop's own root speed
constexpr float SpeedLag = 0.07f;    // seconds
constexpr float AccelLag = 0.09f;

float lag(float dt, float tau) {
  return 1.f - std::exp(-dt/tau);
  }

}

bool LeanAccel::enabled() {
  static const bool on = []() {
    const char* v = std::getenv("OPENGOTHIC_LEANACCEL");
    return v==nullptr || v[0]!='0';
    }();
  return on;
  }

void LeanAccel::reset(State& s) {
  s = State();
  }

void LeanAccel::step(State& s, float speed, float dt) {
  if(dt<=0.f)
    return;
  const float prev = s.speed;
  s.speed += (speed - s.speed)*lag(dt,SpeedLag);
  s.accel += ((s.speed-prev)/dt - s.accel)*lag(dt,AccelLag);
  }

float LeanAccel::pitch(const State& s) {
  const float a = std::max(0.f, std::abs(s.accel)-Dead);
  const float p = std::atan(a/Gravity)*Share;
  return s.accel>0 ? std::min(p,MaxFwd) : -std::min(p,MaxBack);
  }
