#include "archerai.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "game/archery.h"
#include "game/damagecalculator.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"
#include "testbench.h"

using namespace Tempest;

namespace {

constexpr float    Deg         = float(M_PI)/180.f;
constexpr float    ChestRaise  = 15.f;    // cm over the body's middle (a man 160 cm to the head): the chest,
                                          // where head and limb hits even out (x2, x0.6)
constexpr float    HeadHeight  = 160.f;
constexpr float    HumanHeight = 180.f;   // a man as big as the spread below is for
constexpr float    MinHold     = 0.2f;    // seconds held at its draw limit at least (drawn, held)...
constexpr float    Settled     = 0.25f;   // ...then let go once the aim has settled (Archer's disturb)...
constexpr float    ReleaseTrack= 0.5f;    // ...and the arrow is on the point, degrees...
constexpr float    ReleaseWait = 1.f;     // ...or after that long anyway
constexpr float    EaseTime    = 0.3f;    // seconds: the next arrow's error comes in during the reload
constexpr float    Resteady    = 150.f;   // ms after the reload before the bow is steady again
constexpr float    PhysBow     = 1.15f;   // the bow's own spread per axis, of its sway (with the release's cone):
constexpr float    PhysCbow    = 1.35f;   // as the bench measured it (npc-mech, the aim's error off)
constexpr float    RhoMax      = 2.f;     // per second: an arrow at most that much surer than the dice's...
constexpr float    PMax        = 0.9f;    // ...and not surer than this
constexpr uint64_t RaiseStuck  = 3000;    // ms raised and never steady: the dice for this weapon
constexpr uint64_t GiveUp      = 5000;    // ms asked for an arrow that does not come: the fight AI goes on
constexpr uint64_t RequestLapse= 1500;    // ms not asked any more (the fight AI's queue cleared, a strafe,
                                          // hit): the arrow is not wanted
constexpr float    LeadTau     = 0.15f;   // seconds: the target's speed, smoothed...
constexpr float    LeadMax     = 0.8f;    // ...not over (cm/ms)...
constexpr float    LeadJump    = 200.f;   // ...and forgotten when it jumps that far (a teleport)
constexpr uint64_t CycleGap    = 5000;    // ms between two arrows at most to count as its cadence...
constexpr float    CycleRate   = 0.3f;    // ...smoothed that much an arrow

// A man seen from the front, hit anywhere, aimed at the chest: how often by the spread round it (cm per
// axis, a gaussian; the fitted hit zones of the bench)
const struct { float s, p; } Silhouette[] = {
  {0,1.f},{5,1.f},{10,0.99f},{15,0.92f},{20,0.81f},{30,0.59f},{40,0.43f},{60,0.26f},{80,0.17f},{100,0.12f},{150,0.06f}
  };

// the spread (cm per axis) that hits that often
float spreadFor(float p) {
  if(p>=0.995f)
    return 0;
  for(size_t i=1; i<std::size(Silhouette); ++i) {
    auto& a = Silhouette[i-1];
    auto& b = Silhouette[i];
    if(p>=b.p)
      return a.s + (b.s-a.s)*(a.p-p)/std::max(a.p-b.p,1e-4f);
    }
  // rarer than the last row: wider as much as it is rarer (far past it: a spread of 15 m)
  const auto& b = Silhouette[std::size(Silhouette)-1];
  return b.s*std::min(b.p/std::max(p,0.001f),10.f);
  }

float smooth(float x) {
  x = std::clamp(x,0.f,1.f);
  return x*x*(3.f-2.f*x);
  }

uint32_t hash(uint32_t x) {
  x ^= x>>16; x *= 0x7feb352d;
  x ^= x>>15; x *= 0x846ca68b;
  x ^= x>>16;
  return x;
  }

int enabled = -1; // -1: as Gothic.ini says

}

ArcherAi::Mode     ArcherAi::mode        = ArcherAi::PerSecond;
float              ArcherAi::forcedSigma = -1;
bool               ArcherAi::leadOn      = true;
uint32_t           ArcherAi::seedBase    = 0;
uint32_t           ArcherAi::epoch       = 0;
ArcherAi::Probe    ArcherAi::subjectProbe;
const void*        ArcherAi::probeNpc    = nullptr;

bool ArcherAi::isEnabled() {
  if(enabled<0) {
    // OPENGOTHIC_NPCAIM=0|1 over Gothic.ini (a before/after short films the dice)
    const char* v = std::getenv("OPENGOTHIC_NPCAIM");
    if(v!=nullptr && v[0]!='\0')
      enabled = v[0]!='0' ? 1 : 0; else
      enabled = Gothic::settingsGetI("GAME","npcManualAim")!=0 ? 1 : 0;
    }
  return enabled!=0;
  }

void ArcherAi::setEnabled(bool e) {
  enabled = e ? 1 : 0;
  Log::i("npc manual aim: ", e ? "on" : "off");
  }

bool ArcherAi::isDriving(Npc& npc) const {
  if(!isEnabled() || npc.isPlayer() || !npc.canAimByHand())
    return false;
  return !(diceOnly && weapon==npc.currentRangedWeapon() && (!diceWeak || Archery::isBowStrengthEnabled()));
  }

float ArcherAi::gauss() {
  // Box-Muller on its own xorshift: the scripts' random numbers stay as they were
  auto next = [&]() {
    rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5;
    return (float(rng>>8)+0.5f)/float(1u<<24);
    };
  const float u = next(), v = next();
  return std::sqrt(-2.f*std::log(u))*std::cos(2.f*float(M_PI)*v);
  }

bool ArcherAi::attack(Npc& npc) {
  const uint64_t now = npc.world().tickCount();
  askedAt = now;
  if(!npc.hasAmmunition()) {
    // out of arrows: the fight AI's loop puts more in (B_CreateAmmo), or takes the sword
    want = false;
    return true;
    }
  if(done) {
    done = false;
    want = false;
    return true;
    }
  if(!want) {
    want   = true;
    wantAt = now;
    }
  else if(now-wantAt>GiveUp) {
    // never mute: the fight AI decides again (melee, flight)
    want = false;
    return true;
    }
  return false;
  }

void ArcherAi::cancel(Npc& npc) {
  want = false;
  done = false;
  if(active)
    end(npc);
  }

void ArcherAi::begin(Npc& npc) {
  if(!seeded) {
    // its own sway and errors: not in step with every other archer, the same on every run
    const uint32_t s = hash(uint32_t(npc.instanceSymbol())*31u + npc.world().npcId(&npc)) ^ seedBase;
    npc.archer().reset(s);
    rng    = (s ^ 0x9e3779b9u) | 1u;
    seeded = true;
    }
  active  = true;
  raiseAt = npc.world().tickCount();
  heldAt  = 0;
  tgKnown = false;
  sigma   = -1;
  }

void ArcherAi::end(Npc& npc) {
  // the string let down, the stance as the fight AI plays it
  npc.endManualAim(false);
  active = false;
  heldAt = 0;
  if(&npc==probeNpc)
    probe(subjectProbe);
  }

void ArcherAi::nextError(Npc& npc, const Npc& tg) {
  spread(npc,tg);
  // the next arrow's error: in from the last one's while the next arrow is put on
  errFrom = errFrom + (err-errFrom)*smooth(easeT);
  err     = Vec2(gauss(),gauss());
  easeT   = 0;
  }

void ArcherAi::spread(Npc& npc, const Npc& tg) {
  // the dice's chance at this distance, as sure (perArrow), or as many hits a second (perSecond: the
  // hand is slower than the dice's cadence, the reload and 2 frames)
  const auto     st     = Archery::stats(npc,npc.currentRangedWeapon());
  const int32_t  talent = npc.hitChance(st.crossbow ? TALENT_CROSSBOW : TALENT_BOW);
  const float    d      = std::max((tg.position()-npc.position()).length(),100.f);
  pv = float(DamageCalculator::vanillaChance(npc.world().script(),talent,d))/100.f;
  // its cadence as it shoots (the fight AI's waits, the steadying, the draw), till it has one: the reload,
  // the draw, the hold
  const float reload = npc.animTime(Archery::reloadAnim(st.crossbow));
  const float drawTime = st.bowStrength && st.steadyDraw<Archery::MinDraw ?
                         Archery::drawTime(st.strength,st.burstDraw,st.drawSpeed) : st.drawTime;
  const float tNpc   = cycle>0.f ? cycle : reload/std::max(st.reload,0.1f) + Resteady + (drawTime+MinHold)*1000.f;
  const float tDice  = reload + 67.f;
  rho = std::clamp(tNpc/std::max(tDice,1.f),1.f,RhoMax);
  ps  = mode==PerArrow ? pv : std::max(pv,std::min(PMax,pv*rho));

  // the spread round the chest that hits that often, as big as the target is; less what the bow throws
  // off by itself (the sway as it is let go, the breathing, the release's cone)
  const float size  = std::clamp(2.f*(tg.collosionCenter().y-tg.position().y)/HumanHeight,0.3f,3.f);
  const float need  = std::atan2(spreadFor(ps)*size,d);
  const float k     = st.crossbow ? PhysCbow : PhysBow;
  const float phys2 = std::pow(st.sway*k,2.f) + std::pow(st.cone*0.5f,2.f);
  sigma = forcedSigma>=0.f ? forcedSigma*Deg : std::sqrt(std::max(0.f,need*need-phys2));
  epochSeen = epoch;
  }

void ArcherAi::tick(Npc& npc, uint64_t dt) {
  const uint64_t now = npc.world().tickCount();
  if((want || done) && now-askedAt>RequestLapse) {
    // the fight AI dropped the attack (its queue cleared, a strafe, a hit): no arrow for it, none drawn
    want = false;
    done = false;
    if(active && (npc.archer().stage()==Archer::Drawing || npc.archer().stage()==Archer::Held))
      end(npc);
    }
  if(!active && !want)
    return;
  const auto     ws  = npc.weaponState();
  const auto     bs  = npc.bodyStateMasked() & BS_MAX;
  Npc*           tg  = npc.target();
  const bool ok = isDriving(npc) && (ws==WeaponState::Bow || ws==WeaponState::CBow) && tg!=nullptr && tg!=&npc &&
                  !tg->isDown() && !npc.isDown() && npc.interactive()==nullptr && !npc.isSwim() && !npc.isDive() &&
                  !npc.isInAir() && npc.hasAmmunition();
  // aiming, shooting, reloading; standing with an arrow asked for: raises. Running, strafing, stumbling:
  // not by hand (aimBow would stop a run)
  const bool aiming = bs==BS_AIMNEAR || bs==BS_AIMFAR || bs==(BS_HIT & BS_MAX) || (bs==(BS_STAND & BS_MAX) && want);
  if(!ok || !aiming) {
    if(active)
      end(npc);
    return;
    }
  if(!active)
    begin(npc);
  npc.aimBow();

  const float dtS = float(dt)/1000.f;
  auto&       ar  = npc.archer();
  const auto  st  = Archery::stats(npc,npc.currentRangedWeapon());
  const bool tooWeak = st.bowStrength && st.burstDraw<Archery::MinDraw;
  if(tooWeak || (ar.stage()==Archer::Raising && now-raiseAt>RaiseStuck)) {
    // Cannot shoot by hand (too weak, or an animation Aim cannot hold): the dice for this weapon.
    diceOnly = true;
    diceWeak = tooWeak;
    weapon   = npc.currentRangedWeapon();
    end(npc);
    if(TestBench::isActive()) {
      const auto by = TestBench::who(&npc);
      char buf[160];
      std::snprintf(buf,sizeof(buf),"archer %.*s: dice from now (%s)",int(by.size()),by.data(),tooWeak ? "draw below minimum" : "raise stuck");
      TestBench::note(buf);
      }
    return;
    }

  if(tg!=tgLast) {
    // another target: its speed is not known yet, its error is sized anew
    tgLast  = tg;
    tgKnown = false;
    sigma   = -1;
    }
  // the target's speed: the lead
  const Vec3 tp = tg->position();
  if(tgKnown && (tp-tgPrev).length()<LeadJump && dt>0) {
    const Vec3 v = (tp-tgPrev)*(1.f/float(dt));
    tgVel += (v-tgVel)*(1.f-std::exp(-dtS/LeadTau));
    if(tgVel.length()>LeadMax)
      tgVel = Vec3::normalize(tgVel)*LeadMax;
    } else {
    tgVel = Vec3();
    }
  tgPrev  = tp;
  tgKnown = true;
  if(sigma<0.f)
    nextError(npc,*tg);
  else if(epochSeen!=epoch)
    spread(npc,*tg);
  easeT = std::min(1.f,easeT+dtS/EaseTime);

  // where it aims: the chest (a man's: by its head; else the middle of the body), ahead of a runner by
  // the flight, over it by the drop; off by this arrow's error
  Vec3 nock, dir;
  if(!npc.arrowRay(nock,dir))
    nock = npc.position() + Vec3(0,145,0);
  Vec3 c = tg->collosionCenter();
  tg->hitZoneCenter(HitZones::Body,c);
  Vec3 head;
  if(tg->hitZoneCenter(HitZones::Head,head) && head.y>c.y)
    c.y += ChestRaise*std::clamp((head.y-tp.y)/HeadHeight,0.5f,2.f);
  const bool burst = st.bowStrength && st.steadyDraw<Archery::MinDraw;
  const float releaseDraw = st.bowStrength ? (burst ? st.burstDraw : st.steadyDraw) : 1.f;
  const float speed = Archery::speedAt(st,ar.stage()==Archer::Held ? ar.draw() : releaseDraw);
  lead = 0;
  if(leadOn && speed>0.f) {
    Vec3 ahead = c;
    for(int i=0; i<2; ++i) {
      const float t = (ahead-nock).length()/speed;
      ahead = c + Vec3(tgVel.x,0,tgVel.z)*t;
      }
    lead = (ahead-c).length();
    c    = ahead;
    }
  Vec3 at = Archery::launch(nock,c,speed);
  {
  const Vec2  e = errFrom + (err-errFrom)*smooth(easeT);
  const Vec3  v = at-nock;
  const float l = v.length();
  if(l>1.f) {
    const Vec3 u  = v/l;
    const Vec3 r  = Vec3::normalize(Vec3(u.z,0,-u.x));
    const Vec3 up = Vec3::normalize(Vec3(0,1,0) - u*u.y);
    at = nock + Vec3::normalize(u + r*std::tan(e.x*sigma) + up*std::tan(e.y*sigma))*l;
    }
  }

  // drawn on an arrow asked for, held, let go once settled (or held long enough)
  bool letGo = false;
  if(ar.stage()==Archer::Held) {
    if(heldAt==0)
      heldAt = now;
    const float held    = float(now-heldAt)/1000.f;
    const float track   = npc.aimTrackError();
    const bool  settled = ar.disturbance()<=Settled && track>=0.f && track<ReleaseTrack;
    letGo = held>=MinHold && (settled || held>=ReleaseWait);
    if(letGo && TestBench::isActive()) {
      const auto  by = TestBench::who(&npc);
      const Vec2  e  = errFrom + (err-errFrom)*smooth(easeT);
      char buf[320];
      std::snprintf(buf,sizeof(buf),"arrow aim: at %.1f %.1f %.1f, true %.1f %.1f %.1f, err %.3f %.3f deg, sigma %.3f, pv %.0f p %.0f rho %.2f, lead %.0f, hold %.2f, disturb %.2f, %s, by %.*s",
                    at.x,at.y,at.z,c.x,c.y,c.z,double(e.x*sigma/Deg),double(e.y*sigma/Deg),double(sigma/Deg),double(pv*100.f),
                    double(ps*100.f),double(rho),double(lead),double(held),double(ar.disturbance()),settled ? "settled" : "waited",
                    int(by.size()),by.data());
      TestBench::note(buf);
      }
    } else {
    heldAt = 0;
    }
  Archer::Input in;
  in.draw = ar.stage()==Archer::Drawing || (want && (ar.stage()==Archer::Nocked || ar.stage()==Archer::Held) && !letGo);
  in.burst = burst;
  in.dt   = dt;
  if(npc.tickManualAim(at,in,st)) {
    if(shotAt>0 && now-shotAt<CycleGap)
      cycle = cycle>0.f ? cycle + (float(now-shotAt)-cycle)*CycleRate : float(now-shotAt);
    shotAt = now;
    // the arrow for the attack asked for (none if it lapsed meanwhile), drawn no more till the next one
    done   = want;
    want   = false;
    nextError(npc,*tg);
    }

  if(&npc==probeNpc) {
    auto& p  = subjectProbe;
    p.valid  = true;
    p.active = active;
    p.want   = want;
    p.sigma  = sigma/Deg;
    p.err    = (errFrom + (err-errFrom)*smooth(easeT))*(sigma/Deg);
    p.lead   = lead;
    p.pv     = pv;
    p.ps     = ps;
    }
  }

void ArcherAi::probe(Probe& p) const {
  p.valid  = true;
  p.active = active;
  p.want   = want;
  p.sigma  = sigma/Deg;
  p.lead   = lead;
  p.pv     = pv;
  p.ps     = ps;
  }
