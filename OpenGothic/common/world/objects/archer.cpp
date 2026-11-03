#include "archer.h"

#include <algorithm>
#include <cmath>

using namespace Tempest;

namespace {

constexpr float Pi2           = 2.f*float(M_PI);
constexpr float Deg           = float(M_PI)/180.f;
constexpr float SettleTurn    = 10.f*Deg; // the aim turned that far: shaken fully (disturb 1)...
constexpr float MaxDisturb    = 2.f;
constexpr float DisturbGain   = 1.5f;     // ...the sway grows by this much per disturb
constexpr float ShotDisturb   = 1.f;      // after a shot
constexpr float NockDisturb   = 0.6f;     // right after the arrow is on the string
constexpr float HoldBreath    = 0.4f;     // the sway with the breath held...
constexpr float ShortBreath   = 1.5f;     // ...and out of breath
constexpr float CalmLag       = 0.3f;     // seconds: the breath held, let go calms, shakes the aim this smooth
constexpr float BreathRefill  = 3.f;      // seconds to breathe all the breath back
constexpr float BreathRecover = 3.f;      // seconds out of breath when it ran out
constexpr float BreathRate    = 0.25f;    // Hz: breathing moves the chest up and down...
constexpr float BreathAmp     = 0.8f;     // ...that much of the sway
constexpr float TremorMax     = 1.5f;     // of the sway: trembling at GiveUp past the hold limit
constexpr float TremorTired   = 0.5f;
constexpr float TremorWeak    = 0.6f;     // too weak for the bow: trembles drawing
constexpr float TremorBurst   = 1.5f;     // of the sway at the burst draw limit
constexpr float FatigueRest   = 4.f;      // seconds to rest from full fatigue
constexpr float RaiseMin      = 0.25f;    // seconds raising before the string can be drawn
constexpr float ReloadMax     = 3.f;      // seconds: reloading takes no longer, steady or not
constexpr float CrouchSteady  = 0.7f;     // the sway crouched (a knee bent, low)...
constexpr float PeekShaky     = 1.25f;    // ...leaning out round a corner...
constexpr float WalkShaky     = 1.6f;     // ...and stepping

uint32_t hash(uint32_t x) {
  x ^= x>>16; x *= 0x7feb352d;
  x ^= x>>15; x *= 0x846ca68b;
  x ^= x>>16;
  return x;
  }

float phase(uint32_t seed, uint32_t i) {
  return float(hash(seed*31u+i)%6283u)/1000.f;
  }

}

Archer::Probe Archer::playerProbe;

void Archer::reset(uint32_t s) {
  *this = Archer();
  seed  = s;
  }

void Archer::setStage(Stage s) {
  stg       = s;
  stageTime = 0;
  unready   = false;
  }

// incommensurate frequencies: a smooth wander that does not repeat soon, RMS 1
float Archer::wander(float t, uint32_t axis) const {
  static const float f[2][4] = {{0.21f,0.37f,0.61f,1.13f},{0.17f,0.43f,0.71f,1.27f}};
  static const float a[4]    = {1.f,0.8f,0.5f,0.3f};
  float s = 0;
  for(uint32_t i=0; i<4; ++i)
    s += a[i]*std::sin(Pi2*f[axis][i]*t + phase(seed,axis*8u+i));
  return s/std::sqrt((1.f+0.64f+0.25f+0.09f)*0.5f);
  }

// the arms trembling: fast, RMS 1
float Archer::tremor(float t, uint32_t axis) const {
  static const float f[2][2] = {{7.3f,9.1f},{8.2f,6.7f}};
  float s = std::sin(Pi2*f[axis][0]*t + phase(seed,axis*8u+4u)) + 0.7f*std::sin(Pi2*f[axis][1]*t + phase(seed,axis*8u+5u));
  return s/std::sqrt((1.f+0.49f)*0.5f);
  }

bool Archer::tick(const Input& in, const Archery::Stats& st) {
  const float dt = float(in.dt)/1000.f;
  const bool strengthDraw = st.bowStrength;
  const float targetDraw = strengthDraw ? (in.burst ? st.burstDraw : st.steadyDraw) : 1.f;
  clock     += dt;
  stageTime += dt;
  strength   = st.strength;
  skill      = st.skill;
  if(breath<0.f)
    breath = st.breath;

  bool release = false;
  if(!in.raised) {
    if(stg!=Lowered)
      setStage(Lowered);
    drawAmt = 0;
    hold    = 0;
    repress = false;
    } else {
    if(stg==Lowered)
      setStage(Raising);
    if(!in.draw)
      repress = false;

    switch(stg) {
      case Lowered:
        break;
      case Raising:
        // raised with no arrow (bolt) put in (lowered before the reload put it in): it is put in first
        if(in.ready && stageTime>=RaiseMin && in.loaded && !in.reload) {
          setStage(Nocked);
          disturb = std::max(disturb,NockDisturb);
          }
        break;
      case Reloading:
        // the next arrow: the bow moves, then holds still again
        if(!in.ready)
          unready = true;
        if((unready && in.ready && !in.reload) || stageTime>=ReloadMax) {
          setStage(Nocked);
          disturb = std::max(disturb,NockDisturb);
          }
        break;
      case Nocked:
        if(in.draw && !repress && in.ready)
          setStage(Drawing);
        break;
      case Drawing:
        if(!in.draw) {
          if(drawAmt>=Archery::MinDraw) {
            release = true;
            } else {
            drawAmt = 0;
            setStage(Nocked);
            }
          break;
          }
        if(strengthDraw)
          drawAmt = std::min(targetDraw,Archery::advanceDraw(st.strength,drawAmt,dt,st.drawSpeed)); else
          drawAmt = st.drawTime>0.f ? std::min(1.f,drawAmt+dt/st.drawTime) : 1.f;
        if(drawAmt>=targetDraw) {
          setStage(Held);
          hold = 0;
          }
        break;
      case Held:
        if(!in.draw) {
          if(drawAmt>=Archery::MinDraw) {
            release = true;
            } else {
            drawAmt = 0;
            hold = 0;
            setStage(Nocked);
            }
          break;
          }
        drawAmt = std::min(drawAmt,targetDraw);
        if(drawAmt<targetDraw) {
          setStage(Drawing);
          hold = 0;
          break;
          }
        hold += dt;
        if(hold>(strengthDraw ? Archery::holdLimit(st.strength,drawAmt) : st.holdLimit)+Archery::GiveUp) {
          // the arms give up: the string goes back, press again to draw
          drawAmt = 0;
          hold    = 0;
          fatigue = 1;
          repress = true;
          setStage(Nocked);
          }
        break;
      }
    }

  if(stg!=Held)
    fatigue = std::max(0.f,fatigue-dt/FatigueRest);

  // the breath is held drawing or holding; runs out
  const bool drawn = (stg==Drawing || stg==Held);
  holding = in.breath && drawn && recover<=0.f && breath>0.f;
  if(holding) {
    breath -= dt;
    if(breath<=0.f) {
      breath  = 0;
      recover = BreathRecover;
      holding = false;
      }
    }
  else if(recover>0.f) {
    recover = std::max(0.f,recover-dt);
    }
  else {
    breath = std::min(st.breath,breath+dt*st.breath/BreathRefill);
    }
  const float calmGoal = holding ? HoldBreath : (recover>0.f ? ShortBreath : 1.f);
  calm += (calmGoal-calm)*std::min(1.f,dt/CalmLag);

  // a turn of the aim shakes it, it settles
  disturb  = std::min(MaxDisturb,disturb+std::abs(in.turn)/SettleTurn);
  disturb *= std::exp(-dt/std::max(st.settle,0.05f));

  if(stg==Lowered) {
    amp = 0;
    spr = 0;
    sw  = Vec2();
    trm = Vec2();
    return false;
    }

  const float stance = (1.f + (CrouchSteady-1.f)*std::clamp(in.crouch,0.f,1.f)) *
                       (1.f + (PeekShaky-1.f)*std::clamp(std::abs(in.peek),0.f,1.f)) *
                       (1.f + (WalkShaky-1.f)*std::clamp(in.walk,0.f,1.f));
  amp = st.sway*(0.6f+0.4f*drawAmt)*calm*(1.f+DisturbGain*disturb)*stance;
  float trem = TremorTired*fatigue;
  if(stg==Held)
    trem += TremorMax*std::clamp((hold-(strengthDraw ? Archery::holdLimit(st.strength,drawAmt) : st.holdLimit))/Archery::GiveUp,0.f,1.f);
  if(drawn) {
    if(strengthDraw) {
      const float x = st.strength>0.f ? std::clamp((drawAmt/st.strength-1.f)/(Archery::Burst-1.f),0.f,1.f) : 0.f;
      trem += TremorBurst*x;
      } else {
      trem += TremorWeak*std::max(0.f,1.f-st.strength);
      }
    }
  trem *= st.sway;
  const float br = st.sway*BreathAmp*std::clamp(calm,0.f,1.f);
  const float t  = float(clock);
  sw.x  = amp*wander(t,0);
  sw.y  = amp*wander(t,1) + br*std::sin(Pi2*BreathRate*t);
  trm.x = trem*tremor(t,0);
  trm.y = trem*tremor(t,1);
  // how far off the arrow may go: the sway's reach (twice its RMS), the trembling, the breathing, the
  // release
  spr = 2.f*(amp+trem) + br + st.cone;
  return release;
  }

void Archer::onShot(bool shot) {
  drawAmt = 0;
  hold    = 0;
  if(shot) {
    setStage(Reloading);
    disturb = std::max(disturb,ShotDisturb);
    } else {
    setStage(Nocked);
    }
  }

void Archer::probe(Probe& p) const {
  p.valid    = true;
  p.stage    = stg;
  p.draw     = drawAmt;
  p.hold     = hold;
  p.fatigue  = fatigue;
  p.breath   = std::max(breath,0.f);
  p.holding  = holding;
  p.short_   = recover>0.f;
  p.disturb  = disturb;
  p.amp      = amp/Deg;
  p.spread   = spr/Deg;
  p.yaw      = (sw.x+trm.x)/Deg;
  p.pitch    = (sw.y+trm.y)/Deg;
  p.strength = strength;
  p.skill    = skill;
  }
