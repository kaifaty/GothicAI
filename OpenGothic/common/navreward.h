#pragma once

#include <Tempest/Vec>

#include <cstdint>
#include <deque>

namespace NavReward {

struct Keys {
  float forward=0,strafe=0;
  bool jump=false,climb=false;
};

// Episode-relative milliseconds and centimetres, matching population's body poses.
struct Blocked {
  void measure(uint64_t elapsed,const Tempest::Vec3& position,const Keys& keys);
  void close(uint64_t elapsed);

  uint64_t spans=0,totalMs=0,maxMs=0,since=0,until=0;
  bool active=false;

  private:
    struct Sample { uint64_t time=0;Tempest::Vec3 position;bool commanded=false; };
    std::deque<Sample> motion;
};

struct Path {
  double lengthCm=0;
  bool complete=false;
};

double pathLength(double previousCm,const Path& path);

struct Input {
  double previousPathCm=0;
  Path path;
  uint64_t actionAt=0,now=0;
  Keys held;
  int32_t previousHealth=0,health=0;
  bool fell=false,arrived=false,blocked=false,dead=false;
};

struct Components {
  double pathAfterCm=0,progressCm=0;
  bool arrived=false,blocked=false,death=false,jump=false,climb=false;
  int32_t fallDamage=0;
  uint64_t timeMs=0;
};

Components transition(const Input& input);

}
