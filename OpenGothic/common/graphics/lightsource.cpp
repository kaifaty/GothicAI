#include "lightsource.h"

#include <cstring>

#include <Tempest/Log>

using namespace Tempest;

static Vec3 toVec3(uint32_t v) {
  uint8_t cl[4];
  std::memcpy(cl,&v,4);
  return Vec3(cl[0]/255.f,cl[1]/255.f,cl[2]/255.f);
  }

LightSource::LightSource() {
  }

void LightSource::setDir(const Tempest::Vec3& d) {
  float l = d.length();
  if(l>0){
    ldir = d/l;
    } else {
    ldir = Vec3();
    }
  }

void LightSource::setDir(float x, float y, float z) {
  setDir({x,y,z});
  }

void LightSource::setColor(const Vec3& cl) {
  clr                = cl;
  curClr             = clr;
  colorAniListFpsInv = 0;
  }

void LightSource::setColor(const std::vector<uint32_t>& arr, float fps, bool smooth) {
  colorSmooth = smooth;
  if(arr.size()==1) {
    setColor(toVec3(arr[0]));
    colorAniListFpsInv = 0;
    return;
    }

  colorAniList.resize(arr.size());
  for(size_t i=0; i<arr.size(); ++i)
    colorAniList[i] = toVec3(arr[i]);
  colorAniListFpsInv = arr.size()>0 ? uint64_t(1000.0/fps) : 0;
  }

void LightSource::setColor(const std::vector<asset::Color>& arr, float fps, bool smooth) {
  colorSmooth = smooth;
  if(arr.size()==1) {
    setColor({static_cast<float>(arr[0].r)/255.f, static_cast<float>(arr[0].g)/255.f, static_cast<float>(arr[0].b)/255.f});
    colorAniListFpsInv = 0;
    return;
    }

  colorAniList.resize(arr.size());
  for(size_t i=0; i<arr.size(); ++i)
    colorAniList[i] = {static_cast<float>(arr[i].r)/255.f, static_cast<float>(arr[i].g)/255.f, static_cast<float>(arr[i].b)/255.f};
  colorAniListFpsInv = !arr.empty() ? uint64_t(1000.0/fps) : 0;
  }

void LightSource::setColor(const std::vector<Vec3>& arr, float fps, bool smooth) {
  colorSmooth = smooth;
  if(arr.size()==0) {
    setColor(Vec3());
    colorAniListFpsInv = 0;
    return;
    }
  if(arr.size()==1) {
    setColor(arr[0]);
    colorAniListFpsInv = 0;
    return;
    }
  colorAniList       = arr;
  colorAniListFpsInv = arr.size()>0 ? uint64_t(1000.0/fps) : 0;
  }

void LightSource::setRange(float r) {
  rgn            = r;
  curRgn         = rgn;
  rangeAniFPSInv = 0;
  }

void LightSource::setRange(const std::vector<float>& arr, float base, float fps, bool smooth) {
  rangeSmooth = smooth;
  if(arr.size()==1) {
    setRange(arr[0]*base);
    return;
    }
  rangeAniScale  = arr;
  for(auto& i:rangeAniScale)
    i*=base;

  rangeAniFPSInv = arr.size() > 0 ? uint64_t(1000.0/fps) : 0;
  if(rangeAniScale.size()>0) {
    rgn = rangeAniScale[0];
    for(auto i:rangeAniScale)
      rgn = std::max(rgn,i);
    }
  }

void LightSource::setEnabled(bool e) {
  enable = e;
  }

void LightSource::setFlicker(const PfxMotion::Flicker& f, float jitterCm) {
  flicker         = f;
  flickerJitterCm = jitterCm;
  curPos          = pos;
  }

void LightSource::update(uint64_t time) {
  const uint64_t worldTime = time;
  if(timeOff<time)
    time -= timeOff; else
    time  = 0;

  if(rangeAniFPSInv==0) {
    curRgn = rgn;
    } else {
    size_t   frame = size_t(time/rangeAniFPSInv), mod = size_t(time%rangeAniFPSInv);
    float    a     = float(mod)/float(rangeAniFPSInv);
    if(!rangeSmooth)
      a = 0;

    float    r0    = rangeAniScale[(frame  )%rangeAniScale.size()];
    float    r1    = rangeAniScale[(frame+1)%rangeAniScale.size()];
    curRgn = r0+(r1-r0)*a;
    }

  if(colorAniListFpsInv==0) {
    curClr = clr;
    } else {
    size_t   frame = size_t(time/colorAniListFpsInv), mod = size_t(time%colorAniListFpsInv);
    float    a     = float(mod)/float(colorAniListFpsInv);
    if(!colorSmooth)
      a = 0;

    Vec3 cl0 = colorAniList[(frame  )%colorAniList.size()];
    Vec3 cl1 = colorAniList[(frame+1)%colorAniList.size()];
    curClr = cl0+(cl1-cl0)*a;
    }

  if(flicker.active()) {
    // the world time, without the light's offset: the emitter's particles read the same tick (pfxbucket.cpp)
    const float s = PfxMotion::flicker(flicker, worldTime);
    curClr = clr*s;
    curPos = pos + PfxMotion::jitter(flicker, worldTime, flickerJitterCm);
    if(PfxMotion::logEnabled())
      Tempest::Log::i("flicker light seed=", flicker.seed, " t=", worldTime, " s=", s);
    }
  }

bool LightSource::isDynamic() const {
  return rangeAniFPSInv!=0 || colorAniListFpsInv!=0 || flicker.active();
  }

bool LightSource::isEnabled() const {
  return enable;
  }

void LightSource::setTimeOffset(uint64_t t) {
  timeOff = t;
  }

uint64_t LightSource::effectPrefferedTime() const {
  uint64_t t0 = colorAniList .size()*colorAniListFpsInv;
  uint64_t t1 = rangeAniScale.size()*rangeAniFPSInv;
  return std::max(t0,t1);
  }

void LightSource::setDebugName(std::string_view hint) {
#ifndef NDEBUG
  dbgHint = hint;
#else
  (void)hint;
#endif
  }

std::string_view LightSource::debugName() const {
#ifndef NDEBUG
  return dbgHint;
#else
  return "";
#endif
  }
