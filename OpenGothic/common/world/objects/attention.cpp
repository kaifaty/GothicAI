#include "attention.h"

#include <algorithm>
#include <cmath>

#include "game/rpgvocabulary.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace {

constexpr uint64_t CheckPeriod = 250;    // ms between choices of whom to look at
constexpr float    NearHero    = 2500;   // further from the hero nobody sees the npc: no attention
constexpr float    SeeRange    = 500;    // the hero is noticed that close...
constexpr float    ArmedRange  = 1000;   // ...one with a weapon drawn much further
constexpr float    HearRange   = 200;    // behind the back only steps heard that close
constexpr float    MaxHeight   = 300;    // another floor of a house
constexpr float    FieldOfView = 100.f*float(M_PI)/180.f;
constexpr float    MaxTurn     = 140.f*float(M_PI)/180.f; // the gaze gives up at 150
constexpr float    Keep        = 0.25f;  // the one it looks at stays interesting a bit longer
constexpr float    Moved       = 300;    // the one it got bored of moved that far: interesting again
constexpr float    BusyRange   = 0.6f;   // at a bench, an anvil: notices only closer...
constexpr uint64_t LookMin     = 2000, LookMax     = 5000;  // looks that long, ms
constexpr uint64_t RestMin     = 4000, RestMax     = 10000; // then looks away that long
constexpr uint64_t BusyLookMin = 1500, BusyLookMax = 3000;  // ...and looks shorter, less often
constexpr uint64_t BusyRestMin = 6000, BusyRestMax = 12000;

uint32_t hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
  }

bool isArmed(const Npc& n) {
  return n.weaponState()!=WeaponState::NoWeapon;
  }

bool isSneak(const Npc& n) {
  return (n.bodyStateMasked() & BS_MAX)==(BS_SNEAK & BS_MAX);
  }

}

void Attention::clear() {
  current   = nullptr;
  tired     = nullptr;
  lookUntil = 0;
  restUntil = 0;
  }

bool Attention::canLook(const Npc& self) const {
  if(self.isPlayer() || !self.isHuman() || self.isDown())
    return false;
  if(self.isInAir() || self.isSwim() || self.isDive())
    return false;
  if(isArmed(self))
    return false; // in a fight the body is busy with the enemy
  return (self.bodyStateMasked() & BS_MAX)!=BS_LIE; // asleep
  }

float Attention::interest(const Npc& self, const Npc& o, uint64_t now) const {
  if(&o==&self || o.isDown())
    return 0;
  const bool armed = isArmed(o);
  const auto bs    = o.bodyStateMasked() & BS_MAX;
  float      range = armed ? ArmedRange : SeeRange;
  if(isSneak(o))
    range *= 0.5f;
  else if(bs==BS_RUN || bs==BS_SPRINT)
    range *= 1.3f;
  if(self.interactive()!=nullptr)
    range *= BusyRange;

  const Vec3  d    = o.position()-self.position();
  const float dist = std::sqrt(d.x*d.x+d.z*d.z);
  if(dist>range || std::abs(d.y)>MaxHeight)
    return 0;
  const float a   = self.rotationRad();
  const float c   = dist>1.f ? (std::cos(a)*d.x+std::sin(a)*d.z)/dist : 1.f;
  const float ang = std::acos(std::clamp(c,-1.f,1.f));
  if(ang>MaxTurn)
    return 0;
  const bool loud = bs==(BS_WALK & BS_MAX) || bs==BS_RUN || bs==BS_SPRINT; // steps, not sneaking
  if(ang>FieldOfView && (dist>HearRange || !loud))
    return 0;
  if(&o==tired && now<restUntil && !armed && (o.position()-tiredAt).quadLength()<Moved*Moved)
    return 0;

  float s = 1.1f - dist/range; // closer is more interesting
  if(armed)
    s += 1.f;
  if(&o==current)
    s += Keep;
  return s;
  }

uint64_t Attention::random(const Npc& self, uint64_t from, uint64_t to) {
  // the same run of the game picks the same times: the test bench repeats
  const uint32_t h = hash(self.instanceSymbol()*0x9e3779b9u + (++counter));
  return from + h%(to-from+1);
  }

void Attention::tick(Npc& self, World& world) {
  const uint64_t now = world.tickCount();
  if(now<nextCheck)
    return;
  // the npcs choose at different frames
  nextCheck = now + CheckPeriod - (nextCheck==0 ? hash(self.instanceSymbol())%CheckPeriod : 0);

  Npc* pl = world.player();
  if(pl==nullptr || !canLook(self) || self.qDistTo(*pl)>NearHero*NearHero) {
    current = nullptr;
    return;
    }

  const bool busy = self.interactive()!=nullptr;
  if(self.isTalk()) {
    // a dialog with the hero: looks at him between the lines too, a bit after the end
    current   = pl;
    lookUntil = now + random(self,1000,3000);
    return;
    }

  Npc*  best  = nullptr;
  float score = 0;
  auto  consider = [&](Npc& o) {
    const float s = interest(self,o,now);
    if(s>score) {
      best  = &o;
      score = s;
      }
    };
  consider(*pl);
  world.detectNpc(self.position(),ArmedRange,[&](Npc& o) {
    if(&o!=pl && isArmed(o))
      consider(o);
    });
  if(best!=nullptr && !self.canSeeNpc(*best,true))
    best = nullptr;

  if(best!=nullptr && best==current && now>=lookUntil && !isArmed(*best)) {
    // seen enough: looks away, again later
    tired     = current;
    tiredAt   = current->position();
    restUntil = now + (busy ? random(self,BusyRestMin,BusyRestMax) : random(self,RestMin,RestMax));
    current   = nullptr;
    return;
    }
  if(best!=current) {
    current = best;
    if(best!=nullptr)
      lookUntil = now + (busy ? random(self,BusyLookMin,BusyLookMax) : random(self,LookMin,LookMax));
    }
  }
