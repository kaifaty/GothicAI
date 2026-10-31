#include "beasts.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include <Tempest/Log>

#include "game/gamescript.h"
#include "graphics/mesh/animation.h"
#include "world/aiqueue.h"
#include "world/waypoint.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include "jsonread.h"
#include "jsonwriter.h"
#include "npcfind.h"
#include "textcodec.h"

namespace {

constexpr float    ScanRadius   = 5000;   // cm: wolves this near the hero are watched
constexpr float    ReportRadius = 2500;   // and listed for the server
constexpr uint64_t ReportMs     = 500;
constexpr float    ListenBiteCm = 500;    // listening, it goes for him if he comes this close (the scripts: 7 m)
constexpr float    WarnCm       = 400;    // calm: it gets up and growls at him this close
constexpr float    BiteCm       = 200;    // and goes for him closer still
constexpr float    BackOffCm    = 350;    // calm this close to him (it was biting him): it steps back first
constexpr float    BackedCm     = 450;    // to this far
constexpr uint64_t BackOffMs    = 4000;   // at most this long
constexpr float    BackOffFarCm = 900;    // where it backs off to: a point this far from him at most
constexpr float    LeaveCm      = 1500;   // he is gone (PERC_DIST_MONSTER_ACTIVE_MAX)
constexpr float    PackCm       = 1500;   // the hero hitting a wolf this near it breaks its calm too
constexpr uint64_t HitFreshMs   = 1500;
constexpr uint64_t ListenMs     = 6000;
constexpr uint64_t ListenMaxMs  = 15000;
constexpr uint64_t CalmMaxMs    = 120000;
constexpr uint64_t TolerateMs   = 120000; // after the calm: it does not take him for prey this long
constexpr uint64_t SitAfterMs   = 2500;   // calm: it sits down once it has watched him this long
constexpr uint64_t WarnEveryMs  = 2500;
constexpr float    FaceCos      = 0.9f;   // ~25 degrees: it turns to keep facing him

std::string_view weaponOf(const Npc& npc) {
  switch(npc.weaponState()) {
    case WeaponState::NoWeapon: return "none";
    case WeaponState::Fist:     return "fists";
    case WeaponState::W1H:
    case WeaponState::W2H:      return "melee";
    case WeaponState::Bow:
    case WeaponState::CBow:     return "ranged";
    case WeaponState::Mage:     return "magic";
    }
  return "none";
  }

// `npc` faces `other`: its body turned towards it
bool faces(const Npc& npc, const Npc& other) {
  const auto  d   = other.position()-npc.position();
  const float len = std::sqrt(d.x*d.x+d.z*d.z);
  if(len<1)
    return true;
  const float rot = npc.rotationRad();
  return (d.x*std::cos(rot)+d.z*std::sin(rot))/len>=FaceCos;
  }

std::string_view modeName(uint8_t m) {
  switch(m) {
    case 1:  return "listen";
    case 2:  return "calm";
    default: return "free";
    }
  }

}

BeastWatch::BeastWatch(Emit emit):emit(std::move(emit)) {
  }

void BeastWatch::tick(World& world) {
  Npc* hero = world.player();
  if(hero==nullptr)
    return;
  const uint64_t now = world.tickCount();

  std::vector<std::pair<Npc*,Wolf*>> near;
  world.detectNpc(hero->position(),ScanRadius,[&](Npc& n) {
    if(&n==hero || n.guild()!=GIL_WOLF)
      return;
    auto it = wolves.find(&n);
    if(it!=wolves.end() && it->second.instance!=n.instanceSymbol()) {
      wolves.erase(it);  // the memory of a gone wolf, now another npc's
      it = wolves.end();
      }
    if(it==wolves.end()) {
      Wolf w;
      w.serial   = ++serials;
      w.instance = n.instanceSymbol();
      it = wolves.emplace(&n,w).first;
      }
    Wolf&         w  = it->second;
    const int32_t hp = n.attribute(ATR_HITPOINTS);
    if(w.hp>=0 && hp<w.hp && n.lastHitBy()==hero)
      w.heroHitAt = now;
    w.hp     = hp;
    w.seenAt = now;
    near.emplace_back(&n,&w);
    });

  for(auto& [npc,w]:near) {
    if(npc->isDead()) {
      if(w->mode!=Mode::Free) {
        npc->setExternalControl(false);
        w->mode = Mode::Free;
        event(world,*npc,*w,"calm_broken","dead");
        }
      continue;
      }
    // the hero hit it, or a wolf near it: whatever it gave him is over
    bool hit = w->heroHitAt!=0 && now<w->heroHitAt+HitFreshMs;
    for(auto& [other,o]:near)
      if(!hit && o!=w && o->heroHitAt!=0 && now<o->heroHitAt+HitFreshMs && other->qDistTo(*npc)<PackCm*PackCm)
        hit = true;
    if(hit && w->mode==Mode::Free)
      w->tolerateUntil = 0;  // its scripts take it from here
    if(hit && w->mode!=Mode::Free) {
      release(world,*npc,*w,"attack",now);
      event(world,*npc,*w,"calm_broken","hit");
      continue;
      }
    if(w->mode!=Mode::Free)
      drive(world,*npc,*w,*hero,now);
    }

  // the ones no longer near him: he is far (a teleport, a loaded save) or they are gone
  for(auto it=wolves.begin(); it!=wolves.end();) {
    Wolf& w = it->second;
    if(w.seenAt==now) {
      ++it;
      continue;
      }
    if(w.mode!=Mode::Free) {
      if(Npc* npc = locate(world,it->first,w))
        release(world,*npc,w,"routine",now);
      w.mode = Mode::Free;
      }
    if(now>w.seenAt+TolerateMs && now>=w.tolerateUntil)
      it = wolves.erase(it); else
      ++it;
    }

  if(now>=nextReport || nextReport>now+ReportMs) {
    nextReport = now+ReportMs;
    report(world,*hero,now);
    }
  }

void BeastWatch::drive(World& world, Npc& npc, Wolf& w, Npc& hero, uint64_t now) {
  const float d = std::sqrt(npc.qDistTo(hero));
  if(w.mode==Mode::Listen) {
    if(d<ListenBiteCm) {
      release(world,npc,w,"attack",now);
      event(world,npc,w,"listen_over","close");
      return;
      }
    if(d>LeaveCm || now>=w.until) {
      release(world,npc,w,"routine",now);
      event(world,npc,w,"listen_over",d>LeaveCm ? "left" : "time");
      return;
      }
    } else if(w.backing) {
    // calmed at his throat: it backs off a few steps before it watches him, and does not bite meanwhile
    if(d>=BackedCm || now>=w.since+BackOffMs) {
      npc.clearAiQueue();  // the walk away too
      npc.setTarget(nullptr);
      w.backing = false;
      w.nextAni = now;
      Tempest::Log::i("[beast] ",idOf(world,npc,w)," backed off to ",int(d)," cm");
      } else if(npc.isAiQueueEmpty()) {
      // at the point, or no point: away from him (AI_Flee steers a step and leaves the queue at once)
      npc.setTarget(&hero);
      npc.aiPush(AiQueue::aiFlee());
      }
    return;
    } else {
    if(d<BiteCm) {
      release(world,npc,w,"attack",now);
      event(world,npc,w,"calm_broken","close");
      return;
      }
    if(d>LeaveCm || now>=w.until) {
      w.tolerateUntil = now+TolerateMs;
      release(world,npc,w,"routine",now);
      event(world,npc,w,"calm_over",d>LeaveCm ? "left" : "time");
      return;
      }
    }

  // its body: faces him, a look or a growl now and then; calm, it sits down once sure of him. The
  // animations are played here, not queued: one that cannot start now would hold the queue for good
  if(!npc.isAiQueueEmpty() || now<w.busyUntil)
    return;
  if(w.mode==Mode::Calm) {
    if(d<WarnCm) {
      if(w.sitting) {
        if(play(world,npc,w,"T_SIT_2_STAND",BS_STAND,now))
          w.sitting = false;
        w.nextAni = now;
        return;
        }
      if(!faces(npc,hero)) {
        npc.aiPush(AiQueue::aiTurnToNpc(&hero));
        return;
        }
      if(!w.warned || now>=w.nextAni) {
        play(world,npc,w,"T_WARN",BS_NONE,now);
        w.warned  = true;
        w.nextAni = now+WarnEveryMs;
        }
      return;
      }
    w.warned = false;
    if(w.sitting)
      return;  // it watches him sitting, without turning
    if(w.sit && now>=w.since+SitAfterMs && faces(npc,hero)) {
      if(play(world,npc,w,"T_STAND_2_SIT",BS_SIT,now)) {
        w.sitting = true;
        Tempest::Log::i("[beast] ",idOf(world,npc,w)," sits down");
        }
      return;
      }
    }
  if(!faces(npc,hero)) {
    npc.aiPush(AiQueue::aiTurnToNpc(&hero));
    return;
    }
  if(now>=w.nextAni) {
    const bool first = w.nextAni<=w.since;
    // listening it perks up first, then a low growl now and then; calm, a low growl now and then
    play(world,npc,w,w.mode==Mode::Listen && first ? "T_PERCEPTION" : "R_GROWL",BS_NONE,now);
    w.nextAni = now + (w.mode==Mode::Listen ? 3500 : 6000) + (w.serial*977+now)%2000;
    }
  }

bool BeastWatch::play(World& world, Npc& npc, Wolf& w, std::string_view ani, BodyState bs, uint64_t now) {
  if(auto sq = npc.playAnimByName(ani,bs)) {
    w.busyUntil = now+uint64_t(sq->totalTime());
    return true;
    }
  w.busyUntil = now+500;  // it tries again soon
  if(now>=w.loggedAt+5000) {
    w.loggedAt = now;
    Tempest::Log::i("[beast] ",idOf(world,npc,w)," cannot play ",ani," now");
    }
  return false;
  }

bool BeastWatch::command(World& world, const rapidjson::Value& msg, std::string& err) {
  const auto id   = JsonRead::str(msg,"npc");
  const auto what = JsonRead::str(msg,"do");
  const auto ms   = JsonRead::integer(msg,"ms",0);
  Npc*  ptr = nullptr;
  Wolf* w   = find(id,ptr);
  Npc*  npc = w!=nullptr ? locate(world,ptr,*w) : nullptr;
  if(npc==nullptr) {
    err = "no such wolf: " + std::string(id);
    return false;
    }
  if(npc->isDead()) {
    err = "the wolf is dead";
    return false;
    }
  return apply(world,*npc,*w,what,ms,JsonRead::boolean(msg,"sit",false),err);
  }

bool BeastWatch::apply(World& world, Npc& npc, Wolf& w, std::string_view what, int64_t ms, bool sit,
                       std::string& err) {
  const uint64_t now = world.tickCount();
  if(what=="listen") {
    if(w.mode==Mode::Calm)
      return true;  // it lets him pass already
    take(world,npc,w,Mode::Listen,now,ms>0 ? std::min(uint64_t(ms),ListenMaxMs) : ListenMs);
    return true;
    }
  if(what=="calm") {
    take(world,npc,w,Mode::Calm,now,ms>0 ? std::min(uint64_t(ms),CalmMaxMs) : CalmMaxMs);
    w.sit = sit;
    return true;
    }
  if(what=="attack" || what=="flee") {
    release(world,npc,w,what,now);
    return true;
    }
  if(what=="resume") {
    if(w.mode!=Mode::Free)
      release(world,npc,w,"routine",now);
    return true;
    }
  err = "unknown beast command: " + std::string(what);
  return false;
  }

std::string BeastWatch::console(World& world, std::string_view what, int64_t ms, bool sit) {
  Npc* hero = world.player();
  if(hero==nullptr)
    return "no hero";
  Npc*  best  = nullptr;
  float bestD = 0;
  world.detectNpc(hero->position(),ScanRadius,[&](Npc& n) {
    if(&n==hero || n.guild()!=GIL_WOLF || n.isDead())
      return;
    const float d = n.qDistTo(*hero);
    if(best==nullptr || d<bestD) {
      best  = &n;
      bestD = d;
      }
    });
  if(best==nullptr)
    return "no wolf near the hero";
  auto it = wolves.find(best);
  if(it==wolves.end() || it->second.instance!=best->instanceSymbol()) {
    Wolf w;
    w.serial   = ++serials;
    w.instance = best->instanceSymbol();
    w.seenAt   = world.tickCount();
    it = wolves.insert_or_assign(best,w).first;
    }
  std::string err;
  if(!apply(world,*best,it->second,what,ms,sit,err))
    return err;
  return idOf(world,*best,it->second) + " " + std::string(what) + (sit ? " sit" : "");
  }

void BeastWatch::take(World& world, Npc& npc, Wolf& w, Mode mode, uint64_t now, uint64_t ms) {
  Npc* hero = world.player();
  if(w.mode==Mode::Free) {
    // from its scripts (it threatened or attacked him): its state's end may queue something, cleared too
    npc.clearAiQueue();
    npc.clearState(false);
    npc.clearAiQueue();
    npc.setTarget(nullptr);
    npc.setExternalControl(true);
    // out of its fighting stance at once: AI_RemoveWeapon has nothing to put away for a wolf and would
    // hold the queue (and so its back-off, its growls, its sitting down) for good
    npc.closeWeapon(true);
    w.sitting = false;
    }
  if(w.mode!=mode) {
    w.since     = now;
    w.nextAni   = now;
    w.busyUntil = 0;
    w.warned    = false;
    w.backing   = false;
    Tempest::Log::i("[beast] ",idOf(world,npc,w)," ",modeName(uint8_t(mode)),", the hero ",
                    hero!=nullptr ? int(std::sqrt(npc.qDistTo(*hero))) : -1," cm off");
    }
  if(mode==Mode::Calm && hero!=nullptr && npc.qDistTo(*hero)<BackOffCm*BackOffCm) {
    // it was at his throat: a few steps back first, to a point away from him (or just away from him)
    const auto at = npc.position(), from = hero->position();
    const WayPoint* wp = world.findWayPoint(at,[&](const WayPoint& p) {
      const float dh = (p.position()-from).quadLength(), dn = (p.position()-at).quadLength();
      return dh>=BackedCm*BackedCm && dh<=BackOffFarCm*BackOffFarCm && dh>(at-from).quadLength() &&
             dn<=BackOffFarCm*BackOffFarCm;
      });
    npc.setWalkMode(WalkBit::WM_Run);  // a trot back, not a crawl: it was at his throat
    if(wp!=nullptr) {
      npc.aiPush(AiQueue::aiGoToPoint(*wp));
      } else {
      npc.setTarget(hero);
      npc.aiPush(AiQueue::aiFlee());
      }
    w.backing = true;
    }
  w.mode  = mode;
  w.until = now+ms;
  }

void BeastWatch::release(World& world, Npc& npc, Wolf& w, std::string_view to, uint64_t now) {
  const bool sitting = w.sitting;
  if(w.mode!=Mode::Free)
    Tempest::Log::i("[beast] ",idOf(world,npc,w)," ",to);
  w.mode    = Mode::Free;
  w.sitting = false;
  w.backing = false;
  npc.setExternalControl(false);
  npc.clearAiQueue();
  if(sitting)
    npc.playAnimByName("T_SIT_2_STAND",BS_STAND);
  Npc* hero = world.player();
  if((to=="attack" || to=="flee") && hero!=nullptr) {
    const auto fn = AiNpc::scriptState(world,to=="attack" ? "ZS_MM_ATTACK" : "ZS_MM_FLEE");
    if(fn.isValid()) {
      w.tolerateUntil = to=="attack" ? 0 : now+TolerateMs;
      npc.setTarget(hero);
      npc.aiPush(AiQueue::aiStartState(fn,0,hero,nullptr,""));
      return;
      }
    }
  npc.resumeAiRoutine();
  }

bool BeastWatch::intercept(World& world, Npc& self, Npc& other, Npc* victim, PercType perc) {
  if(self.guild()!=GIL_WOLF)
    return false;
  auto it = wolves.find(&self);
  Npc* hero = world.player();
  if(it==wolves.end() || hero==nullptr || it->second.instance!=self.instanceSymbol())
    return false;
  Wolf& w = it->second;
  if(world.tickCount()>=w.tolerateUntil)
    return false;
  if(perc==PERC_ASSESSDAMAGE) {
    w.tolerateUntil = 0;  // he hit it: the scripts answer
    return false;
    }
  if((perc==PERC_ASSESSENEMY || perc==PERC_ASSESSPLAYER) && &other==hero)
    return true;
  // a wolf of its pack growls at him, or goes for him
  return perc==PERC_ASSESSWARN && (victim==hero || &other==hero);
  }

void BeastWatch::releaseAll(World& world) {
  const uint64_t now = world.tickCount();
  for(auto& [ptr,w]:wolves) {
    w.tolerateUntil = 0;
    if(w.mode==Mode::Free)
      continue;
    if(Npc* npc = locate(world,ptr,w))
      release(world,*npc,w,"routine",now);
    w.mode = Mode::Free;
    }
  }

void BeastWatch::clear() {
  wolves.clear();
  nextReport = 0;
  listed = false;
  }

std::string BeastWatch::status() const {
  size_t held = 0, tolerant = 0;
  for(auto& [ptr,w]:wolves) {
    if(w.mode!=Mode::Free)
      ++held;
    if(w.tolerateUntil!=0)
      ++tolerant;
    }
  return std::to_string(wolves.size()) + " wolves watched, " + std::to_string(held) + " held, " +
         std::to_string(tolerant) + " leave the hero be";
  }

BeastWatch::Wolf* BeastWatch::find(std::string_view id, Npc*& npc) {
  const auto at = id.find('#');
  if(at==std::string_view::npos)
    return nullptr;
  const auto serial = uint32_t(std::strtoul(std::string(id.substr(at+1)).c_str(),nullptr,10));
  for(auto& [ptr,w]:wolves)
    if(w.serial==serial) {
      npc = ptr;
      return &w;
      }
  return nullptr;
  }

Npc* BeastWatch::locate(World& world, Npc* ptr, const Wolf& w) {
  const uint32_t n = world.npcCount();
  for(uint32_t i=0; i<n; ++i)
    if(world.npcById(i)==ptr)
      return ptr->instanceSymbol()==w.instance ? ptr : nullptr;
  return nullptr;
  }

std::string BeastWatch::idOf(World& world, const Npc& npc, const Wolf& w) const {
  std::string sym;
  if(auto s = world.script().findSymbol(npc.instanceSymbol()))
    sym = s->name();
  return sym + "#" + std::to_string(w.serial);
  }

std::string BeastWatch::stateOf(World& world, Npc& npc, const Wolf& w, Npc& hero) const {
  if(npc.isDead())
    return "dead";
  if(w.mode!=Mode::Free)
    return std::string(modeName(uint8_t(w.mode)));
  if(npc.isInState(AiNpc::scriptState(world,"ZS_MM_THREATENENEMY")))
    return npc.stateOther()==&hero ? "threaten" : "routine";
  if(npc.isInState(AiNpc::scriptState(world,"ZS_MM_ATTACK")))
    return npc.target()==&hero ? "attack" : "fight";
  if(npc.isInState(AiNpc::scriptState(world,"ZS_MM_FLEE")))
    return npc.stateOther()==&hero || npc.target()==&hero ? "flee" : "routine";
  return "routine";
  }

void BeastWatch::event(World& world, const Npc& npc, const Wolf& w, std::string_view kind, std::string_view detail) {
  emit(JsonWriter("beast_event")
         .str("npc",idOf(world,npc,w))
         .str("kind",kind)
         .str("detail",detail)
         .finish());
  }

void BeastWatch::report(World& world, Npc& hero, uint64_t now) {
  JsonWriter j("beasts");
  j.integer("t",int64_t(now));
  j.str("hero_weapon",weaponOf(hero));
  Npc* focus = hero.target();
  int  count = 0;
  j.array("wolves");
  for(auto& [ptr,w]:wolves) {
    if(w.seenAt!=now)
      continue;
    Npc& npc = *ptr;
    if(npc.isDead())
      continue;
    const float d = std::sqrt(npc.qDistTo(hero));
    if(d>ReportRadius)
      continue;
    j.object()
     .str("id",idOf(world,npc,w))
     .str("name",TextCodec::toUtf8(npc.displayName()))
     .num("distance",std::round(double(d)))
     .str("state",stateOf(world,npc,w,hero))
     .integer("hp",npc.attribute(ATR_HITPOINTS))
     .integer("hp_max",npc.attribute(ATR_HITPOINTSMAX))
     .boolean("focus",&npc==focus);
    if(w.sitting)
      j.boolean("sitting",true);
    if(w.heroHitAt!=0)
      j.integer("hit_ms",int64_t(now-w.heroHitAt));  // since the hero hit it last
    if(now<w.tolerateUntil)
      j.boolean("tolerant",true);
    j.end();
    ++count;
    }
  j.endArray();
  if(count>0 || listed)
    emit(j.finish());
  listed = count>0;
  }
