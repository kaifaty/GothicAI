#include "eyeblink.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

// at random about 14 blinks a minute at rest, 17 speaking; more with the pauses of the voice and the
// shifts of the eyes (the gaps are the shortest one plus a random wait, exponential: most come soon,
// some take long)
float MinGap     = 1200.f;  // ms, between two blinks at random
float RestMean   = 3000.f;  // ms, the mean random wait on top, at rest...
float SpeakMean  = 2400.f;  // ...and while it speaks
float MaxGap     = 9000.f;
float Hold       = 40.f;    // ms the eyes stay closed (+ up to the same at random), between 40 ms in and out
float PauseMs    = 180.f;   // the voice quiet this long while it speaks: a phrase ends...
float PauseProb  = 0.45f;   // ...and it blinks with this chance
float ShiftProb  = 0.45f;   // the eyes go elsewhere: it blinks with this chance
float SoonMs     = 600.f;   // no blink of a pause or a shift this soon after another one
float DoubleProb = 0.1f;    // a second blink...
float DoubleGap  = 220.f;   // ...this long (ms, +-40) after the first starts

struct Param final {
  std::string_view name;
  float*           value;
  };

const Param params[] = {
  {"MinGap", &MinGap}, {"RestMean", &RestMean}, {"SpeakMean", &SpeakMean}, {"MaxGap", &MaxGap},
  {"Hold", &Hold}, {"PauseMs", &PauseMs}, {"PauseProb", &PauseProb}, {"ShiftProb", &ShiftProb},
  {"SoonMs", &SoonMs}, {"DoubleProb", &DoubleProb}, {"DoubleGap", &DoubleGap},
  };

bool enabled = []() {
  const char* v = std::getenv("OPENGOTHIC_BLINK");
  return v==nullptr || v[0]!='0';
  }();

constexpr float VoiceQuiet = 0.06f;  // the voice level below which it counts as a pause

}

EyeBlink::Probe EyeBlink::subjectProbe;
const void*     EyeBlink::probeNpc = nullptr;

bool EyeBlink::isEnabled() {
  return enabled;
  }

void EyeBlink::setEnabled(bool e) {
  enabled = e;
  }

bool EyeBlink::setParam(std::string_view name, float value) {
  for(auto& p:params)
    if(p.name==name) {
      *p.value = value;
      return true;
      }
  return false;
  }

std::string EyeBlink::paramList() {
  std::string ret;
  for(auto& p:params) {
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s%.*s=%g",ret.empty() ? "" : " ",int(p.name.size()),p.name.data(),double(*p.value));
    ret += buf;
    }
  return ret;
  }

bool EyeBlink::update(const Input& in, uint64_t& hold) {
  if(!started) {
    // heads that appear together do not blink together
    static uint32_t seed = 0;
    rnd     = (++seed)*2654435761u | 1u;
    next    = in.now + uint64_t(float(random()%1000)/1000.f*(MinGap+RestMean));
    started = true;
    }
  if(!enabled || !in.allowed) {
    next    = std::max(next, in.now + uint64_t(MinGap));
    second  = 0;
    quiet   = 0;
    inPause = false;
    return false;
    }

  // a pause of the voice while it speaks: the end of a phrase
  bool pause = false;
  if(in.speaking && in.voice<VoiceQuiet) {
    quiet += in.dt;
    if(!inPause && float(quiet)>=PauseMs) {
      inPause = true;
      pause   = true;
      }
    } else {
    quiet   = 0;
    inPause = false;
    }

  // the caller's blink near: none of its own comes, the next at random waits until it is past
  reserved.erase(std::remove_if(reserved.begin(),reserved.end(),[&](uint64_t r) {
    return r+ReserveGapMs<=in.now;
    }),reserved.end());
  for(auto r:reserved)
    if(in.now+ReserveGapMs>r) {
      next   = std::max(next,r+ReserveGapMs);
      second = 0;
      return false;
      }

  const bool soon  = float(in.now-last)<SoonMs;
  bool       blink = false;
  if(second!=0 && in.now>=second) {
    second = 0;
    blink  = true;
    ++doubles;
    }
  else if(in.now>=next) {
    blink = true;
    }
  else if(pause && !soon && float(random()%1000)<PauseProb*1000.f) {
    blink = true;
    ++pauses;
    }
  else if(in.shift && !soon && float(random()%1000)<ShiftProb*1000.f) {
    blink = true;
    ++shifts;
    }
  if(!blink)
    return false;

  hold = uint64_t(Hold*(1.f + float(random()%1000)/1000.f));
  last = in.now;
  next = in.now + interval(in.speaking,in.rate);
  if(second==0 && float(random()%1000)<DoubleProb*1000.f)
    second = in.now + uint64_t(std::max(0.f, DoubleGap + float(int(random()%81)-40)));
  ++blinks;
  return true;
  }

void EyeBlink::reserve(uint64_t at) {
  reserved.push_back(at);
  }

uint64_t EyeBlink::interval(bool speaking, float rate) {
  // exponential: -mean ln(u)
  const float u    = (float(random()%10000)+1.f)/10001.f;
  const float mean = (speaking ? SpeakMean : RestMean)/std::max(rate,0.1f);
  return uint64_t(std::min(MaxGap, MinGap - mean*std::log(u)));
  }

uint32_t EyeBlink::random() {
  // xorshift: an LCG stepped a few times per pick gave runs of short gaps
  rnd ^= rnd<<13;
  rnd ^= rnd>>17;
  rnd ^= rnd<<5;
  return rnd;
  }

void EyeBlink::probe(Probe& p) const {
  p.valid   = started;
  p.enabled = enabled;
  p.blinks  = blinks;
  p.pauses  = pauses;
  p.shifts  = shifts;
  p.doubles = doubles;
  }
