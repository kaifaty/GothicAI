#include "graphics/mesh/leanaccel.h"
#include "graphics/mesh/transend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
  constexpr float Deg = 3.14159265f/180.f;

  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  // the body's speed goes from `from` to `to` cm/s over `ramp` seconds, then holds; returns the largest lean
  // forward and back on the way and leaves the state as it is `total` seconds in
  void run(LeanAccel::State& s, float from, float to, float ramp, float total, float& fwd, float& back) {
    const float dt = 1.f/30.f;
    fwd = 0; back = 0;
    for(float t=0; t<total; t+=dt) {
      const float k = ramp>0 ? std::min(1.f,t/ramp) : 1.f;
      LeanAccel::step(s,from+(to-from)*k,dt);
      const float p = LeanAccel::pitch(s);
      fwd  = std::max(fwd, p);
      back = std::min(back,p);
      }
    }
  }

int main() {
  float fwd = 0, back = 0;

  // standing still and a steady run lean nothing
  LeanAccel::State s;
  run(s,0,0,0,1,fwd,back);
  require(fwd==0.f && back==0.f, "a body at rest does not lean");
  LeanAccel::reset(s);
  run(s,345,345,0,0.1f,fwd,back);       // the state is fed a run from nowhere: a start, it leans forward
  require(fwd>0.f, "a speed that appears is an acceleration");
  run(s,345,345,0,2,fwd,back);
  require(std::abs(LeanAccel::pitch(s))<1e-4f, "a steady run does not lean by this");

  // a run start: 0 to 300 cm/s in 0.45 s leans forward, not past the cap, and lets go within a second
  LeanAccel::reset(s);
  run(s,0,300,0.45f,0.5f,fwd,back);
  require(fwd>3.f*Deg && fwd<=7.f*Deg+1e-4f, "a start leans forward by 3 to 7 degrees");
  require(back==0.f, "and never back");
  run(s,300,300,0,1,fwd,back);
  require(std::abs(LeanAccel::pitch(s))<1e-4f, "the lean is gone a second after the speed is reached");

  // a stop: 345 cm/s to rest in 0.3 s leans back, less than a start leans forward
  LeanAccel::reset(s);
  run(s,345,345,0,2,fwd,back);
  run(s,345,0,0.3f,0.4f,fwd,back);
  require(back<-2.f*Deg && back>=-5.f*Deg-1e-4f, "a stop leans back by 2 to 5 degrees");
  require(fwd==0.f, "and never forward");
  run(s,0,0,0,1,fwd,back);
  require(std::abs(LeanAccel::pitch(s))<1e-4f, "and stands straight again");

  // the ripple of a run loop's root (345 +- 20 cm/s at the step rate) stays in the dead zone
  LeanAccel::reset(s);
  run(s,345,345,0,2,fwd,back);
  fwd = 0; back = 0;
  for(int i=0; i<90; ++i) {
    LeanAccel::step(s,345.f+20.f*std::sin(float(i)/30.f*2.f*3.14159265f*2.85f),1.f/30.f);
    fwd  = std::max(fwd, LeanAccel::pitch(s));
    back = std::min(back,LeanAccel::pitch(s));
    }
  require(fwd<1.f*Deg && back>-1.f*Deg, "steps of a run do not rock the trunk by a degree");

  // walking backwards: speeding up backwards leans back
  LeanAccel::reset(s);
  run(s,0,-150,0.2f,0.3f,fwd,back);
  require(back<0.f && fwd==0.f, "a start backwards leans back");

  // a clip of 14 frames has 13 steps of its root: frames 0..13 are its own, past 13 the way goes on
  float over = -1;
  require(!TransEnd::past(0.f,13,over) && !TransEnd::past(12.9f,13,over), "inside the clip the way is its own");
  require(TransEnd::past(13.f,13,over) && over==0.f,                       "the last sample is where it ends");
  require(TransEnd::past(13.5f,13,over) && std::abs(over-0.5f)<1e-5f,      "half a frame past the last sample");
  require(!TransEnd::past(5.f,0,over),                                     "a record without a way has no end");
  // the body keeps the speed it ended with: 10 cm a frame at the end stays 10 cm a frame
  require(std::abs(TransEnd::extend(70.f,60.f,0.f)-70.f)<1e-5f,  "at the last sample the way is the clip's");
  require(std::abs(TransEnd::extend(70.f,60.f,1.f)-80.f)<1e-5f,  "a frame later it has gone one more step");
  require(std::abs(TransEnd::extend(47.f,47.f,1.f)-47.f)<1e-5f,  "a stop that ended at rest stays at rest");

  std::puts("leanaccel contract: ok");
  return 0;
  }
