#include "stopfoot.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>

bool StopFoot::isRight(float phase) {
  return phase>=0.25f && phase<0.75f;
  }

float StopFoot::phase(uint64_t elapsedMs, float fps, uint32_t frames) {
  if(frames==0 || !(fps>0.f))
    return 0.f;
  const double at = double(elapsedMs)*double(fps)/1000.0/double(frames);
  return float(at-std::floor(at));
  }

std::string StopFoot::rightName(std::string_view leftLoop) {
  if(leftLoop.size()<2 || leftLoop.back()!='L')
    return std::string();
  std::string ret(leftLoop);
  ret.back() = 'R';
  return ret;
  }

uint32_t StopFoot::restFrame(const std::vector<std::array<float,2>>& way, float endX, float endZ, float fps) {
  static const float still = 35.f; // cm/s
  const float step = fps>0.f ? still/fps : 0.f;
  uint32_t ret = uint32_t(way.size()+1);
  float    nx  = endX, nz = endZ;  // the root's place one frame later
  for(size_t i=way.size(); i>0; --i) {
    const float dx = nx-way[i-1][0], dz = nz-way[i-1][1];
    if(dx*dx+dz*dz>step*step)
      break;
    ret = uint32_t(i-1);
    nx  = way[i-1][0];
    nz  = way[i-1][1];
    }
  return ret;
  }

bool StopFoot::enabled() {
  static const bool on = []() {
    const char* v = std::getenv("OPENGOTHIC_STOPFOOT");
    return v==nullptr || v[0]!='0';
    }();
  return on;
  }
