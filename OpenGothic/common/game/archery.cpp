#include "archery.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "physics/dynamicworld.h"
#include "gothic.h"

using namespace Tempest;

namespace {

constexpr float Deg = float(M_PI)/180.f;

// the table to balance: a novice (skill 0) and a master (skill 1)
constexpr float SwayNovice     = 0.9f;  // degrees, RMS at full draw (each way)
constexpr float SwayMaster     = 0.25f;
constexpr float SettleNovice   = 0.6f;  // seconds
constexpr float SettleMaster   = 0.2f;
constexpr float BreathNovice   = 2.f;   // seconds
constexpr float BreathMaster   = 5.f;
constexpr float DrawSlow       = 1.2f;  // seconds: dexterity 0..DexFull
constexpr float DrawFast       = 0.5f;
constexpr float DexFull        = 150.f;
constexpr float ReloadSlow     = 0.85f; // the reload's tempo: dexterity 0..DexFull
constexpr float ReloadFast     = 1.6f;
constexpr float HoldWeak       = 1.5f;  // seconds: strength just half of what the bow needs...
constexpr float HoldStrong     = 6.f;   // ...and twice
constexpr float SpeedLight     = 5.0f;  // cm/ms: a bow of DamageLight...
constexpr float SpeedHeavy     = 7.5f;  // ...and of DamageHeavy
constexpr float DamageLight    = 30.f;
constexpr float DamageHeavy    = 150.f;
constexpr float ConeNovice     = 0.6f;  // degrees
constexpr float ConeMaster     = 0.1f;
constexpr float NeedPerDamage  = 0.6f;  // legacy bows and crossbows: strength per point of damage
constexpr float NeedMin        = 10.f;
constexpr float SlowSpeed      = 0.4f;  // of the speed at no draw (MinDraw shoots a bit faster)
constexpr float CbowSteady     = 0.8f;  // a crossbow braced at the shoulder sways less than a drawn bow...
constexpr float CbowLight      = 0.9f;  // ...a light one less, a heavy one more (by the strength it needs,
constexpr float CbowHeavy      = 1.3f;  // NeedLight..NeedHeavy)
constexpr float NeedLight      = 20.f;
constexpr float NeedHeavy      = 100.f;
constexpr float CbowSpeed      = 1.3f;  // a bolt flies faster than an arrow of a bow as heavy
constexpr float CReloadSlow    = 0.75f; // the crossbow's reload (cocking): strength half of what it needs...
constexpr float CReloadFast    = 1.4f;  // ...and twice
constexpr float CantDraw       = 0.6f;  // the bow canted (a quick shot): drawn that much faster...
constexpr float CantSettle     = 0.6f;  // ...settles after a turn that much faster...
constexpr float CantSway       = 1.3f;  // ...but sways more (no anchor, no sight over the arrow)...
constexpr float CantCone       = 0.3f;  // ...and the release throws it off by that many degrees more
constexpr float HeadDamage     = 2.f;
constexpr float LimbDamage     = 0.6f;

int bowStrength = -1;

float lerp(float a, float b, float t) {
  return a + (b-a)*std::clamp(t,0.f,1.f);
  }

}

bool Archery::isBowStrengthEnabled() {
  if(bowStrength<0) {
    const char* v = std::getenv("OPENGOTHIC_BOWSTRENGTH");
    if(v!=nullptr && v[0]!='\0')
      bowStrength = v[0]!='0' ? 1 : 0; else
      bowStrength = Gothic::settingsGetI("GAME","bowDrawByStrength")!=0 ? 1 : 0;
    }
  return bowStrength!=0;
  }

void Archery::setBowStrengthEnabled(bool e) {
  bowStrength = e ? 1 : 0;
  Log::i("bow draw by strength: ",e ? "on" : "off");
  }

float Archery::bowDrawWeight(float damage, float strengthRequirement) {
  return std::max({10.f+0.5f*(damage-30.f),strengthRequirement,NeedMin});
  }

float Archery::steadyDraw(float r) {
  return std::clamp(r,0.f,1.f);
  }

float Archery::burstDraw(float r) {
  return std::clamp(Burst*r,0.f,1.f);
  }

float Archery::baseDrawSpeed() {
  return (Burst/DrawT1)*(-4.f/Burst-5.f*std::log(1.f-1.f/Burst));
  }

float Archery::drawSpeed(float r, float draw, float v0) {
  if(r<=0.f)
    return 0.f;
  const float phi = std::clamp(draw/(Burst*r),0.f,1.f);
  return v0*std::max((1.f-phi)/(1.f+4.f*phi),DrawEpsilon);
  }

float Archery::drawTime(float r, float draw, float v0) {
  if(draw<=0.f)
    return 0.f;
  if(r<=0.f || v0<=0.f || draw>burstDraw(r))
    return std::numeric_limits<float>::infinity();
  // Integral of (1+4 phi)/(1-phi); after the epsilon crossover the speed is constant.
  const double phi   = double(draw)/(double(Burst)*double(r));
  const double floor = (1.-double(DrawEpsilon))/(1.+4.*double(DrawEpsilon));
  const double p     = std::min(phi,floor);
  const double time  = -4.*p-5.*std::log1p(-p)+(phi-p)/double(DrawEpsilon);
  return float(double(Burst)*double(r)/double(v0)*time);
  }

float Archery::advanceDraw(float r, float draw, float dt, float v0) {
  const float target = burstDraw(r);
  if(r<=0.f)
    return 0.f;
  draw = std::clamp(draw,0.f,target);
  if(dt<=0.f || v0<=0.f)
    return draw;
  const float time = drawTime(r,draw,v0)+dt;
  if(time>=drawTime(r,target,v0))
    return target;
  float lo = draw, hi = target;
  // Invert the same integral each tick: the result does not depend on the frame duration.
  for(int i=0; i<24; ++i) {
    const float mid = (lo+hi)*0.5f;
    if(drawTime(r,mid,v0)<time)
      lo = mid; else
      hi = mid;
    }
  return (lo+hi)*0.5f;
  }

float Archery::rohmert(float phi) {
  if(phi<0.4f)
    // Match the high-load branch at 0.4 instead of the approximate 20.5/0.9 multiplier.
    return 1.5625f*std::exp(6.8f*(0.4f-phi));
  return -1.5f + 2.1f/phi - 0.6f/(phi*phi) + 0.1f/(phi*phi*phi);
  }

float Archery::holdLimit(float r, float draw) {
  if(draw<=0.f)
    return 8.f;
  if(r<=0.f)
    return 0.f;
  const float phi = std::clamp(draw/(Burst*r),0.f,1.f);
  return std::min(8.f,3.f*rohmert(phi)/rohmert(1.f/Burst));
  }

Archery::Stats Archery::stats(Npc& npc, const Item* w) {
  const bool crossbow = w!=nullptr && w->isCrossbow();
  const float dex = float(npc.attribute(ATR_DEXTERITY));
  const float str = float(npc.attribute(ATR_STRENGTH));
  const float skill = float(npc.hitChance(crossbow ? TALENT_CROSSBOW : TALENT_BOW))/100.f;
  const float dmg  = w!=nullptr ? float(w->handle().damage_total) : DamageLight;
  float requirement = 0;
  if(w!=nullptr) {
    auto& h = w->handle();
    for(size_t i=0; i<std::size(h.cond_atr); ++i)
      if(h.cond_atr[i]==ATR_STRENGTH)
        requirement = std::max(requirement,float(h.cond_value[i]));
    }
  return stats(str,dex,skill,dmg,requirement,crossbow,isBowStrengthEnabled());
  }

Archery::Stats Archery::stats(float str, float dex, float skill, float dmg, float requirement,
                            bool crossbow, bool strengthModel) {
  Stats s;
  s.crossbow    = crossbow;
  s.bowStrength = !crossbow && strengthModel;
  s.skill      = std::clamp(skill,0.f,1.f);
  const float need = s.bowStrength ? bowDrawWeight(dmg,requirement) : std::max({dmg*NeedPerDamage,requirement,NeedMin});
  s.need     = need;
  s.strength = str/need;

  const float weak = std::max(0.f,1.f-s.strength);
  const float dexK = dex/DexFull;
  s.sway      = lerp(SwayNovice,SwayMaster,s.skill)*(s.bowStrength ? 1.f : 1.f+weak)*Deg;
  if(s.crossbow)
    s.sway *= CbowSteady*lerp(CbowLight,CbowHeavy,(need-NeedLight)/(NeedHeavy-NeedLight));
  s.settle    = lerp(SettleNovice,SettleMaster,s.skill)*lerp(1.2f,0.8f,dexK);
  s.breath    = lerp(BreathNovice,BreathMaster,s.skill);
  s.drawTime  = s.crossbow ? 0.f : lerp(DrawSlow,DrawFast,dexK)*std::clamp(1.f/std::sqrt(std::max(s.strength,0.01f)),0.8f,2.f);
  s.holdLimit = s.crossbow ? 1e6f : lerp(HoldWeak,HoldStrong,(s.strength-0.5f)/1.5f);
  s.speed     = lerp(SpeedLight,SpeedHeavy,(dmg-DamageLight)/(DamageHeavy-DamageLight))*(s.crossbow ? CbowSpeed : 1.f);
  s.cone      = lerp(ConeNovice,ConeMaster,s.skill)*Deg;
  s.reload    = s.crossbow ? lerp(CReloadSlow,CReloadFast,(s.strength-0.5f)/1.5f) : lerp(ReloadSlow,ReloadFast,dexK);
  if(s.bowStrength) {
    s.steadyDraw = steadyDraw(s.strength);
    s.burstDraw  = burstDraw(s.strength);
    s.drawSpeed  = baseDrawSpeed();
    s.drawTime   = drawTime(s.strength,s.steadyDraw,s.drawSpeed);
    s.holdLimit  = holdLimit(s.strength,s.burstDraw);
    }
  return s;
  }

Archery::Stats Archery::canted(const Stats& s, float k) {
  Stats c = s;
  k = std::clamp(k,0.f,1.f);
  if(s.crossbow || k<=0.f)
    return c;
  c.drawTime *= lerp(1.f,CantDraw,k);
  c.drawSpeed /= lerp(1.f,CantDraw,k);
  c.settle   *= lerp(1.f,CantSettle,k);
  c.sway     *= lerp(1.f,CantSway,k);
  c.cone     += CantCone*Deg*k;
  return c;
  }

Tempest::Vec3 Archery::launch(const Vec3& from, const Vec3& to, float speed) {
  // the low one of the two arcs: tan = (v^2 - sqrt(v^4 - g(g x^2 + 2 y v^2)))/(g x)
  const Vec3  d   = to-from;
  const float x   = std::sqrt(d.x*d.x + d.z*d.z);
  const float len = d.length();
  if(x<1.f || speed<=0.f)
    return to;
  const float g    = DynamicWorld::gravity;
  const float v2   = speed*speed;
  const float disc = v2*v2 - g*(g*x*x + 2.f*d.y*v2);
  const float tan  = disc>=0.f ? (v2-std::sqrt(disc))/(g*x) : 1.f;
  const float c    = 1.f/std::sqrt(1.f+tan*tan);
  return from + Vec3(d.x/x*c, tan*c, d.z/x*c)*len;
  }

float Archery::speedAt(const Stats& s, float draw) {
  if(s.bowStrength)
    return s.speed*std::clamp(draw,0.f,1.f);
  return s.speed*lerp(SlowSpeed,1.f,draw);
  }

float Archery::releaseKick(float draw, float drawWeight) {
  const float f = std::clamp(draw,0.f,1.f);
  return std::clamp(f*f*std::max(drawWeight,0.f)/KickWeight,KickMin,1.f);
  }

float Archery::releaseSettle(float r) {
  return std::clamp(1.f/std::sqrt(std::max(r,1e-3f)),0.7f,1.3f);
  }

float Archery::damageAt(float draw) {
  return std::clamp(draw,0.f,1.f);
  }

float Archery::zoneDamage(HitZones::Zone z) {
  switch(z) {
    case HitZones::Head: return HeadDamage;
    case HitZones::Limb: return LimbDamage;
    case HitZones::Body:
    case HitZones::None: return 1.f;
    }
  return 1.f;
  }
