#pragma once

#include <Tempest/Matrix4x4>
#include "asset/worldtypes.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// One continuous world wind, sampled by the native cloth solver and the
// original vegetation transform. No random state or saved simulation state.
namespace WorldWind {
inline Tempest::Vec3 sample(uint64_t timeMs, uint64_t periodMs, bool enabled, float scale = 1.f) {
  if(!enabled || periodMs==0 || scale<=0.f)
    return {};
  const double phase = double(timeMs)*6.283185307179586/double(periodMs);
  const float angle = float(.24*std::sin(phase*.071)+.1*std::sin(phase*.137));
  const float gust = float(.5+.5*std::sin(phase*.31));
  const float speed = (2.2f+.65f*float(std::sin(phase*.113))+2.8f*gust*gust*gust*gust)*scale;
  return {std::sin(angle)*speed,0,std::cos(angle)*speed};
  }

inline Tempest::Matrix4x4 shaderMatrix(const Tempest::Matrix4x4& object,
                                       uint64_t timeMs, uint64_t periodMs, bool enabled,
                                       asset::AnimationType mode, float intensity,
                                       float scale = 1.f, bool legacy = false) {
  auto out = object;
  if(!enabled || periodMs==0)
    return out;
  float magnitude = 0;
  if(mode==asset::AnimationType::WIND && intensity>0.f)
    magnitude = .03f;
  else if(mode==asset::AnimationType::WIND_ALT && intensity>0.f && intensity<=1.f)
    magnitude = intensity*.1f;
  const auto velocity = legacy ? Tempest::Vec3(0,0,4) : sample(timeMs,periodMs,true,scale);
  const Tempest::Vec2 direction(velocity.x*.25f,velocity.z*.25f);
  const float ax = float(timeMs%periodMs)/float(periodMs)*2.f-1.f;
  const float shift = object[3][0]*direction.x+object[3][2]*direction.y;
  const float amount = magnitude*std::cos(float(ax*3.141592653589793)+shift*.0001f);
  out[1][0] += direction.x*amount;
  out[1][2] += direction.y*amount;
  return out;
  }
}
