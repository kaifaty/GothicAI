#include "graphics/pfx/particlefx.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
  if(!value)
    throw std::runtime_error(message);
  }
}

void particleKeySpan() {
  asset::VirtualObject object;
  object.visual=std::make_shared<asset::VisualParticleEffect>();
  ParticleFx effect(Material{},object);
  effect.ppsValue=100.f;
  effect.ppsScaleKeys={1.f,2.f,4.f};
  effect.ppsFPS=7.f;
  effect.ppsIsLooping=true;
  effect.ppsIsSmooth=true;
  effect.prefferedTime=429;

  constexpr uint64_t born=91,hour=3'600'000,dt=16;
  const auto span=effect.keySpan(true,born,born+hour,dt,0);
  require(span.scale==1.f && span.scale==effect.ppsScale(hour),
          "three keys at 7 fps must return to the first key after an hour, even with a new block");
  require(std::abs(span.fromMs-2888.0/7.0)<1e-9 && std::abs(span.toMs-3000.0/7.0)<1e-9,
          "an interval crossing a fractional cycle counts both ends from the same cycle start");

  const auto after=effect.keySpan(true,born,born+hour+1,dt,400);
  require(after.scale==effect.ppsScale(hour+1) && std::abs(after.scale-1.007f)<1e-6f,
          "the first millisecond of the next cycle keeps its smooth phase");
  effect.ppsIsSmooth=false;
  const auto beforeHalfKey=effect.keySpan(true,born,born+hour+499,dt,0);
  const auto atHalfKey=effect.keySpan(true,born,born+hour+500,dt,0);
  require(beforeHalfKey.scale==1.f && atHalfKey.scale==2.f,
          "a non-smooth loop switches keys at the half-key boundary");

  const auto newborn=effect.keySpan(true,born,born+5,dt,100);
  require(newborn.fromMs==0.0 && newborn.toMs==5.0 && newborn.scale==effect.ppsScale(5),
          "an emitter born during a step counts time only since its birth");
  const auto burst=effect.keySpan(false,born,born+hour,dt,425);
  require(burst.fromMs==425.0 && burst.toMs==429.0 && burst.scale==effect.ppsScale(429),
          "a non-looped emitter counts its block time capped at the preferred duration");

  effect.ppsIsLooping=false;
  const auto block=effect.keySpan(true,born,born+hour,dt,425);
  require(block.fromMs==425.0 && block.toMs==441.0 && block.scale==4.f,
          "a non-looping curve follows the block even if the emitter itself loops");
  std::puts("Particle key span contracts passed");
  }
