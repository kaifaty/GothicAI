#include "npcobserver.h"
#include "game/bodycontroller.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <queue>
#include <unordered_map>
#include <utility>

#include "game/gamescript.h"
#include "game/herodeed.h"
#include "game/inventory.h"
#include "gothic.h"
#include "physics/dynamicworld.h"
#include "physics/physicsnoise.h"
#include "testbench.h"
#include "world/aiqueue.h"
#include "world/movablevobs.h"
#include "world/waypoint.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include "jsonread.h"
#include "jsonwriter.h"
#include "npcfind.h"
#include "textcodec.h"

namespace {

constexpr uint64_t RosterPeriodMs = 500;
constexpr float    RosterRadius   = 6000;  // cm: the engine simplifies AI beyond this
constexpr float    EventRadius    = 3000;  // cm: the engine perceives within this
constexpr float    ViewRadius     = 3000;
constexpr float    ViewHalfAngle  = 60;    // degrees around where the player faces
constexpr float    HeardRadius    = 6000;  // cm: the player's voice is followed round rock and walls to those this near
constexpr float    AroundMax      = 10000; // cm: and this far round, at most
constexpr float    NotHeard       = 2*AroundMax; // "heard" of one his voice does not reach
constexpr float    WalledSide     = 40;    // cm: the lines either side of the one between the heads
constexpr float    WaySight       = 2000;  // cm: a voice goes round by the waynet in sight this near
constexpr float    WayEye         = 100;   // cm: above a point of the waynet, where it is looked at
constexpr float    NearEnter      = 500;   // cm: "player_near" when the player comes this close
constexpr float    NearLeave      = 700;   // and forgets it once they are this far again
constexpr uint64_t EventPeriodMs  = 1000;  // per npc, kind and who did it
constexpr uint64_t MonsterNearPeriodMs = 10000;
constexpr float    ArmedThreatRange = 1200;
constexpr uint64_t ArmedThreatPeriodMs = 5000;
constexpr uint64_t NearPeriodMs   = 30000;
constexpr uint64_t NoisePeriodMs  = 10000; // per listener, regardless of body or culprit
constexpr float    NoiseMinVolume = .35f; // source gain, 0..1, after the engine's hearing gate
constexpr uint64_t ActMaxMs       = 30000; // npc_idle at the latest after this
constexpr uint64_t ActGraceMs     = 200;   // let the npc take the action from its queue
constexpr uint64_t StateMaxMs     = 120000; // fight/flee: npc_idle at the latest after this
constexpr uint64_t StateStartMs   = 3000;
constexpr uint64_t ActivityMaxMs  = 10*60*1000;
constexpr uint64_t HoldMs         = 600;    // a held threat goes to the scripts if the server has not decided
constexpr uint64_t VerdictMs      = 5000;   // a verdict covers repeats of the same pair this long

std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

std::string npcId(World& world, Npc& npc) {
  if(npc.isPlayer())
    return "PLAYER";
  if(auto sym = world.script().findSymbol(npc.instanceSymbol()))
    return sym->name();
  return {};
  }

Npc* npcBySymbol(World& world, std::string_view symbol) {
  if(symbol=="PLAYER")
    return world.player();
  const size_t id = world.script().findSymbolIndex(upper(symbol));
  return id==size_t(-1) ? nullptr : world.findNpcByInstance(id);
  }

bool inView(Npc& player, Npc& npc, float dist) {
  if(dist>ViewRadius)
    return false;
  const auto  d   = npc.position()-player.position();
  const float len = std::sqrt(d.x*d.x+d.z*d.z);
  if(len<1)
    return true;
  const float rot = player.rotationRad();
  const float cos = (d.x*std::cos(rot)+d.z*std::sin(rot))/len;
  return cos>=std::cos(ViewHalfAngle*float(M_PI)/180.f);
  }

// rock or a wall between two heads: no straight line from one to the other, nor a hand's breadth to either
// side of it (a post or a tree trunk in between is no wall)
bool walled(const DynamicWorld& physic, const Tempest::Vec3& a, const Tempest::Vec3& b) {
  const float dx = b.x-a.x, dz = b.z-a.z;
  const float len = std::sqrt(dx*dx+dz*dz);
  if(len<1)
    return false;
  const Tempest::Vec3 side = Tempest::Vec3(-dz/len,0,dx/len)*WalledSide;
  return physic.ray(a,b).hasCol && physic.ray(a+side,b+side).hasCol && physic.ray(a-side,b-side).hasCol;
  }

// the nearest point of the waynet in sight of `eye`, not farther than WaySight
const WayPoint* wayInSight(World& world, const Tempest::Vec3& eye) {
  const DynamicWorld& physic = *world.physic();
  return world.findWayPoint(eye,[&](const WayPoint& w) {
    if(!w.isConnected() || (w.position()-eye).quadLength()>WaySight*WaySight)
      return false;
    return !physic.ray(eye,w.position()+Tempest::Vec3(0,WayEye,0)).hasCol;
    });
  }

// how far a voice goes from `a` to `b` round rock or a wall between them: to the waynet in sight, along it,
// and from it (it goes nowhere through the rock); farther than `limit`: NotHeard
float around(World& world, const WayPoint* from, const Tempest::Vec3& a, const Tempest::Vec3& b, float limit) {
  const WayPoint* to = wayInSight(world,b);
  if(from==nullptr || to==nullptr)
    return NotHeard;
  const float start = (from->position()-a).length(), end = (to->position()-b).length();
  using Step = std::pair<float,const WayPoint*>;
  std::priority_queue<Step,std::vector<Step>,std::greater<Step>> open;
  std::unordered_map<const WayPoint*,float> best;
  open.push({start,from});
  best[from] = start;
  while(!open.empty()) {
    auto [len,w] = open.top();
    open.pop();
    if(len>best[w])
      continue;
    if(len+end>limit)
      break;
    if(w==to)
      return len+end;
    for(auto& c:w->connections()) {
      const float next = len+float(c.len);
      auto it = best.find(c.point);
      if(it!=best.end() && it->second<=next)
        continue;
      best[c.point] = next;
      open.push({next,c.point});
      }
    }
  return NotHeard;
  }

std::string_view activity(Npc& npc, std::string_view owner) {
  if(!owner.empty())
    return owner;
  if(npc.isDead() || npc.isDown())
    return "down";
  if(npc.isTalk())
    return "dialog";  // an original dialog with the player
  if(npc.weaponState()!=WeaponState::NoWeapon)
    return "fight";
  return "routine";
  }

// what a perception of `self` is about: the player's doings, or a threat from someone else
// (a monster, an armed npc). `about` is the victim for attacks and murders.
struct Seen final {
  std::string_view kind;
  Npc*             about    = nullptr;
  uint64_t         periodMs = EventPeriodMs;
  };

Seen classify(World& world, Npc& self, Npc& other, Npc* victim, PercType perc, bool bandits = false) {
  if(bandits && (perc==PERC_ASSESSENEMY ||
                (perc==PERC_ASSESSPLAYER && other.isPlayer() &&
                 HeroDeed::aivar(self,"AIV_EnemyOverride")==0)) &&
     !other.isDead() && !other.isDown() &&
     world.script().personAttitude(self,other)==ATT_HOSTILE) {
    // Without EnemyOverride, B_AssessPlayer can attack through B_AssessEnemy.
    if(self.guild()==GIL_BDT && (other.isPlayer() || HeroDeed::aivar(other,"AIV_PARTYMEMBER")!=0))
      return Seen{"bandit_enemy"};
    if(perc==PERC_ASSESSENEMY && HeroDeed::aivar(self,"AIV_PARTYMEMBER")!=0 && other.guild()==GIL_BDT)
      return Seen{"party_enemy"};
    }
  if(victim!=nullptr && victim->isMonster() && world.script().guildAttitude(*victim,self)==ATT_HOSTILE) {
    // a dangerous monster killed by a person (the hero, someone else, the npc itself): the danger is over.
    // Hits on it are no news (the hero fighting a wolf is not an attack on someone), nor is a kill by another
    // monster. Animals the npc does not fear (a sheep) are not dangers: their attackers are
    return perc==PERC_ASSESSMURDER && !other.isMonster() ? Seen{"monster_killed",victim} : Seen{};
    }
  if(other.isPlayer()) {
    switch(perc) {
      case PERC_DRAWWEAPON:      return Seen{"weapon_drawn"};
      case PERC_ASSESSTHREAT:    return Seen{"threatened"};
      case PERC_ASSESSTHEFT:     return Seen{"theft"};
      case PERC_ASSESSENTERROOM: return Seen{"enter_room"};
      case PERC_ASSESSGIVENITEM: return Seen{"item_given"};
      case PERC_ASSESSMAGIC:
      case PERC_ASSESSCASTER:    return Seen{"magic"};
      case PERC_ASSESSDAMAGE:    return Seen{"attack",&self};
      case PERC_ASSESSOTHERSDAMAGE:
        return victim!=nullptr ? Seen{"attack",victim} : Seen{};
      case PERC_ASSESSMURDER:
        return victim!=nullptr ? Seen{"murder",victim} : Seen{};
      default:
        return Seen{};
      }
    }
  if(other.isDead() || other.isDown())
    return Seen{};
  switch(perc) {
    case PERC_ASSESSENEMY:
    case PERC_ASSESSWARN:
      // an enemy the npc senses, or a monster growling at it; repeats every perception tick: reported seldom
      return other.isMonster() ? Seen{"monster_near",nullptr,MonsterNearPeriodMs} : Seen{};
    case PERC_ASSESSDAMAGE:
      return Seen{other.isMonster() ? "monster_attack" : "npc_attack",&self};
    case PERC_ASSESSOTHERSDAMAGE:
      if(victim==nullptr)
        return Seen{};
      if(victim->isPlayer())
        return Seen{"hero_attacked",victim};
      return other.isMonster() ? Seen{"monster_attack",victim} : Seen{};
    default:
      return Seen{};
    }
  }

std::string noiseSource(const PhysicsNoise& noise) {
  const auto what = upper(noise.what);
  auto has = [&](std::string_view token) { return what.find(token)!=std::string::npos; };
  if(has("RAGDOLL"))
    return "body";
  if(const auto* rule = MovableVobs::classifyVisual(noise.what))
    return rule->className;
  if(has("CUP") || has("MUG") || has("BECHER") || has("BEER") || noise.material==MAT_CLAY)
    return "cup";
  return noise.what.empty() ? "object" : noise.what;
  }

}

NpcObserver::NpcObserver(Emit emit, Owner owner):emit(std::move(emit)), owner(std::move(owner)) {
  }

void NpcObserver::clear() {
  close.clear();
  lastEvent.clear();
  previousPositions.clear();
  running.clear();
  held.clear();
  verdicts.clear();
  nextRoster = 0;
  }

bool NpcObserver::isActing(std::string_view symbol) const {
  const auto key = upper(symbol);
  return std::any_of(running.begin(),running.end(),[&](const Running& r){ return r.symbol==key; });
  }

void NpcObserver::tick(World& world) {
  const uint64_t now = world.tickCount();
  checkRunning(world,now);
  for(size_t i=0; i<held.size();) {
    if(now<held[i].at+HoldMs) {
      ++i;
      continue;
      }
    if(held[i].bandit) {
      // A missed renewal means the server can no longer promise a quiet parley.
      for(auto it=verdicts.begin(); it!=verdicts.end();) {
        if(it->second.to==HoldTo::Parley)
          it = verdicts.erase(it); else
          ++it;
        }
      }
    releaseHeld(world,held[i]);  // no verdict in time: the scripts react as they would have
    held.erase(held.begin()+ptrdiff_t(i));
    }
  if(now<nextRoster && nextRoster<=now+RosterPeriodMs)
    return;
  nextRoster = now+RosterPeriodMs;
  // Deed keys are unique for every action, unlike the bounded actor-pair cache.
  for(auto it=verdicts.begin(); it!=verdicts.end();) {
    if(now>=it->second.until)
      it = verdicts.erase(it); else
      ++it;
    }
  emitRoster(world);
  }

void NpcObserver::emitRoster(World& world) {
  Npc* pl = world.player();
  if(pl==nullptr)
    return;
  const auto t = world.time();
  JsonWriter j("npc_roster");
  j.integer("t",int64_t(world.tickCount()))
   .integer("hour",t.hour())
   .integer("minute",t.minute())
   .boolean("full",true)
   .array("npcs");
  struct Near { std::string symbol; float dist; };
  std::vector<Near> cameClose;  // reported after the roster, so the server already knows these npc's
  std::vector<std::pair<Npc*,std::string>> people;
  std::unordered_map<const Npc*,Tempest::Vec3> positions;
  const Tempest::Vec3 head = pl->mapHeadBone();
  const WayPoint*     way  = nullptr;  // the player's, once someone is behind a wall
  bool                wayFound = false;
  // how far the player's voice goes to the npc (v30): straight on, or round what is between them
  auto heard = [&](Npc& npc, float dist) {
    if(dist>HeardRadius)
      return NotHeard;
    const Tempest::Vec3 at = npc.mapHeadBone();
    if(!walled(*world.physic(),head,at))
      return dist;
    if(!wayFound) {
      way      = wayInSight(world,head);
      wayFound = true;
      }
    return around(world,way,head,at,AroundMax);
    };
  world.detectNpc(pl->position(),RosterRadius,[&](Npc& npc) {
    if(npc.isPlayer() || npc.guild()>=GIL_SEPERATOR_HUM)
      return;  // people only: monsters keep their own AI
    const auto symbol = npcId(world,npc);
    if(symbol.empty())
      return;
    const float dist = std::sqrt(npc.qDistTo(*pl));
    positions[&npc] = npc.position();
    if(dist<=EventRadius)
      people.emplace_back(&npc,symbol);
    if(trackProximity(world,npc,symbol,dist))
      cameClose.push_back({symbol,dist});
    j.object()
       .str("id",symbol)
       .str("name",TextCodec::toUtf8(npc.displayName()))
       .integer("guild",int64_t(npc.guild()))
       .num("distance",double(dist))
       .boolean("in_view",inView(*pl,npc,dist))
       .num("heard",double(heard(npc,dist)))
       .str("activity",activity(npc,owner(symbol)))
       .str("state",AiNpc::stateName(world,npc.currentState()))
       .boolean("moving",AiNpc::isMoving(npc))
     .end();
    });
  j.endArray();
  emit(j.finish());
  for(auto& n:cameClose)
    emitEvent(n.symbol,"player_near","PLAYER",double(n.dist),"","");
  for(auto& n:people)
    armedApproach(world,*n.first,n.second);
  previousPositions = std::move(positions);
  }

void NpcObserver::armedApproach(World& world, Npc& self, std::string_view symbol) {
  const auto who = owner(symbol);
  if((!who.empty() && who!="attached") || self.isDown() || self.isTalk() || self.isAttack())
    return;
  world.detectNpc(self.position(),ArmedThreatRange,[&](Npc& other) {
    if(&other==&self || other.isPlayer() || other.isMonster() || other.isDown() ||
       other.weaponState()==WeaponState::NoWeapon || other.weaponState()==WeaponState::Fist ||
       !AiNpc::isMoving(other))
      return;
    if(!(other.isAttack() && other.target()==&self) && world.script().personAttitude(other,self)!=ATT_HOSTILE)
      return;
    const auto previous = previousPositions.find(&other);
    const float dist = std::sqrt(self.qDistTo(other));
    // Compare the attacker's actual movement to the victim's current position: the victim walking
    // towards a stationary armed npc is not an approaching attacker.
    if(previous==previousPositions.end() || (previous->second-self.position()).length()<dist+15 ||
       !self.canSeeNpc(other,true))
      return;
    const auto otherId = npcId(world,other);
    if(otherId.empty() || throttled(symbol,"npc_threat/"+otherId,world.tickCount(),ArmedThreatPeriodMs))
      return;
    emitEvent(symbol,"npc_threat",otherId,dist,"you",TextCodec::toUtf8(other.displayName()));
    });
  }

bool NpcObserver::trackProximity(World& world, Npc& npc, std::string_view symbol, float dist) {
  auto& near = close[std::string(symbol)];
  if(near && dist>NearLeave)
    near = false;
  if(near || dist>NearEnter)
    return false;
  near = true;
  if(!owner(symbol).empty() || npc.isDead() || npc.isDown())
    return false;
  return !throttled(symbol,"player_near",world.tickCount(),NearPeriodMs);
  }

void NpcObserver::perception(World& world, Npc& self, Npc& other, Npc* victim, PercType perc) {
  if(self.isPlayer() || self.isDead() || self.isDown() || self.guild()>=GIL_SEPERATOR_HUM)
    return;
  const Seen seen = classify(world,self,other,victim,perc);
  if(seen.kind.empty() || (&other==&self && seen.kind!="monster_killed"))
    return;
  Npc* pl = world.player();
  if(pl==nullptr)
    return;
  if(std::sqrt(self.qDistTo(*pl))>EventRadius)
    return;
  const auto symbol = npcId(world,self);
  if(symbol.empty() || !owner(symbol).empty())
    return;
  const auto otherId = npcId(world,other);
  if(otherId.empty() || throttled(symbol,std::string(seen.kind)+"/"+otherId,world.tickCount(),seen.periodMs))
    return;
  std::string detail;
  if(seen.about==&self)
    detail = "you"; else
  if(seen.about!=nullptr)
    detail = seen.about->isPlayer() ? "PLAYER" : TextCodec::toUtf8(seen.about->displayName());
  const double dist = std::sqrt(double(self.qDistTo(other)));
  // monster_killed: `victim` is the monster's symbol, the threats from it are over
  const auto victimId = seen.kind=="monster_killed" ? npcId(world,*seen.about) : std::string();
  emitEvent(symbol,seen.kind,otherId,dist,detail,other.isPlayer() ? "" : TextCodec::toUtf8(other.displayName()),
            victimId);
  }

void NpcObserver::physicsNoise(World& world, Npc& self, const PhysicsNoise& noise) {
  if(noise.volume<NoiseMinVolume || self.isPlayer() || self.isDead() || self.isDown() ||
     self.guild()>=GIL_SEPERATOR_HUM || self.weaponState()!=WeaponState::NoWeapon ||
     !self.canHearPhysicsNoise() || self.isPhysicsInvestigating())
    return;
  Npc* pl = world.player();
  if(pl==nullptr || self.qDistTo(*pl)>EventRadius*EventRadius)
    return;
  const auto symbol = npcId(world,self);
  if(symbol.empty() || !owner(symbol).empty() || throttled(symbol,"noise",world.tickCount(),NoisePeriodMs))
    return;
  JsonWriter j("npc_event");
  const auto now = world.time();
  j.str("npc",symbol).str("kind","noise")
   .integer("time",now.day()*24*60+now.hour()*60+now.minute())
   .array("at").item(double(noise.position.x)).item(double(noise.position.y)).item(double(noise.position.z)).endArray()
   .num("distance",double((self.position()-noise.position).length()))
   .num("loudness",double(noise.volume)).str("source",noiseSource(noise));
  if(const auto* point = world.findWayPoint(noise.position))
    j.str("place",point->name); // the server's PlaceBook supplies words for this point
  if(noise.culprit!=nullptr && self.canSeeNpc(*noise.culprit,false)) {
    const auto other = npcId(world,*noise.culprit);
    if(!other.empty()) {
      j.str("other",other);
      if(!noise.culprit->isPlayer())
        j.str("other_name",TextCodec::toUtf8(noise.culprit->displayName()));
      }
    }
  TestBench::physicsNoiseBridgeEvent(self,world.tickCount());
  emit(j.finish());
  }

bool NpcObserver::intercept(World& world, Npc& self, Npc& other, Npc* victim, PercType perc,
                            const HeroDeed* deed, std::string_view deedId) {
  if(&other==&self || self.isPlayer() || self.isDead() || self.isDown() ||
     self.guild()>=GIL_SEPERATOR_HUM || self.isTalk() || self.isAttack())
    return false;
  const bool hero = other.isPlayer();
  const bool room = hero && perc==PERC_ASSESSENTERROOM;
  Seen seen = classify(world,self,other,victim,perc,holdBandits);
  const bool bandit = seen.kind=="bandit_enemy" || seen.kind=="party_enemy";
  if(bandit && BodyController::owns(self))
    return false;
  // ASSESSENEMY ignores AIV_EnemyOverride: even a script-silenced bandit
  // must tell the server that its gang sees the hero.
  if(!bandit && hero) {
    seen = Seen{};
    if(!holdDeeds)
      return false;
    if(room) {
      seen = Seen{"enter_room"};
      } else if(deed==nullptr || deedId.empty()) {
      return false;
      } else if(perc==PERC_ASSESSTHEFT && deed->kind=="take_item" && !deed->item.empty()) {
      seen = Seen{"theft"};
      } else if(perc==PERC_ASSESSOTHERSDAMAGE && deed->kind=="attack" && victim!=nullptr && !deed->victimIsMonster) {
      seen = Seen{"attack",victim};
      } else if(perc==PERC_ASSESSMURDER && deed->kind=="murder" && victim!=nullptr) {
      if(deed->victimIsSheep && !deed->victimToughGuy)
        seen = Seen{"sheep",victim}; else
      if(!deed->victimIsMonster)
        seen = Seen{"murder",victim};
      }
    // Self-defence, weapon warnings and all uncertain events stay vanilla.
    if(seen.kind.empty())
      return false;
    } else if(!bandit) {
    if(!holdThreats)
      return false;
    if(perc!=PERC_ASSESSENEMY && perc!=PERC_ASSESSWARN && perc!=PERC_ASSESSDAMAGE && perc!=PERC_ASSESSOTHERSDAMAGE)
      return false;
    seen = classify(world,self,other,victim,perc);
    }
  if(seen.kind.empty())
    return false;
  Npc* pl = world.player();
  if(pl==nullptr || std::sqrt(self.qDistTo(*pl))>EventRadius)
    return false;
  const auto symbol  = npcId(world,self);
  const auto otherId = npcId(world,other);
  if(symbol.empty() || otherId.empty() || !owner(symbol).empty())
    return false;

  const uint64_t now = world.tickCount();
  // Separate actions of the hero must never inherit a verdict about an earlier
  // theft or strike. Threats from other actors keep their existing pair cache.
  const auto     key = symbol + "/" + otherId + (bandit ? "/bandit" : (room ? "/room" : (hero ? "/"+std::string(deedId) : "")));
  if(auto v = verdicts.find(key); v!=verdicts.end() && now<v->second.until)
    return v->second.to!=HoldTo::Scripts;  // pending, leased, or already assigned to the scripts
  verdicts[key] = Verdict{HoldTo::Server,now+HoldMs,bandit};

  Held h;
  h.id     = "h" + std::to_string(nextHold++);
  h.key    = key;
  if(hero && !room && !bandit) {
    h.item = deed->item;
    h.deed = deedId;
    }
  h.room   = room;
  h.bandit = bandit;
  h.self   = &self;
  h.other  = &other;
  h.victim = victim;
  h.perc   = perc;
  h.at     = now;
  JsonWriter j("perception_held");
  j.str("hold",h.id).str("npc",symbol).str("kind",seen.kind).str("other",otherId)
   .str("other_name",TextCodec::toUtf8(other.displayName()))
   .num("distance",std::sqrt(double(self.qDistTo(other))));
  if(!h.deed.empty())
    j.str("deed",h.deed);
  if(seen.about==&self)
    j.str("detail","you"); else
  if(seen.about!=nullptr)
    j.str("detail",seen.about->isPlayer() ? "PLAYER" : TextCodec::toUtf8(seen.about->displayName()));
  emit(j.finish());
  held.push_back(std::move(h));
  return true;
  }

void NpcObserver::verdict(World& world, const rapidjson::Value& msg) {
  const auto id = JsonRead::str(msg,"hold");
  auto it = std::find_if(held.begin(),held.end(),[&](const Held& h){ return h.id==id; });
  if(it==held.end())
    return;  // released already (too late)
  if(it->bandit && world.tickCount()>=it->at+HoldMs)
    return;  // tick releases the overdue signal and all leases after the inbound queue
  const auto to = JsonRead::str(msg,"to");
  const auto ms = msg.FindMember("ms");
  if(to=="parley" && it->bandit && ms!=msg.MemberEnd() && ms->value.IsInt64() &&
     ms->value.GetInt64()>=1000 && ms->value.GetInt64()<=15000) {
    verdicts[it->key] = Verdict{HoldTo::Parley,world.tickCount()+uint64_t(ms->value.GetInt64()),true};
    held.erase(it);
    return;
    }
  bool server = to=="server";
  // Deed and room rules may deliberately end in silence, a warning or time to
  // leave. Threats still require an actual body action before they are swallowed.
  if(server && it->deed.empty() && !it->room) {
    // A decision is not ownership: a busy attachment or a rejected command may leave no body acting
    // on the verdict. Accept an actual combat provider, or the requested native fight/flee.
    Npc* self = nullptr;
    Npc* other = nullptr;
    if(auto* pl = world.player())
      world.detectNpc(pl->position(),RosterRadius,[&](Npc& npc) {
        if(&npc==it->self)  self = &npc;
        if(&npc==it->other) other = &npc;
        });
    const auto flee = AiNpc::scriptState(world,"ZS_FLEE");
    server = self!=nullptr && other!=nullptr && !self->isDown() && self->target()==other &&
             (BodyController::owns(*self) ||
              (!self->isExternalControl() && (self->isAttack() || (flee.isValid() && self->isInState(flee)))));
    }
  // A new entry must be reconsidered against current knowledge, including after
  // the hero left or made amends. Only in-flight room repeats share the hold.
  verdicts[it->key] = Verdict{server ? HoldTo::Server : HoldTo::Scripts,
                             world.tickCount()+(it->room ? 0 : VerdictMs),it->bandit};
  if(!server)
    releaseHeld(world,*it);
  held.erase(it);
  }

void NpcObserver::setHoldBandits(World* world, bool on) {
  holdBandits = on;
  if(on)
    return;
  for(size_t i=0; i<held.size();) {
    if(!held[i].bandit) {
      ++i;
      continue;
      }
    if(world!=nullptr)
      releaseHeld(*world,held[i]);
    held.erase(held.begin()+ptrdiff_t(i));
    }
  for(auto it=verdicts.begin(); it!=verdicts.end();) {
    if(it->second.bandit)
      it = verdicts.erase(it); else
      ++it;
    }
  }

void NpcObserver::releaseAll(World& world) {
  cancelDeeds(world);
  for(auto& h:held)
    releaseHeld(world,h);
  held.clear();
  verdicts.clear();
  for(auto& r:running)
    if(auto* npc = world.findNpcByInstance(r.instance)) {
      if(r.investigate)
        npc->stopPhysicsInvestigation(owner(r.symbol).empty() && npc->canHearPhysicsNoise());
      }
  running.erase(std::remove_if(running.begin(),running.end(),[](const Running& r){ return r.investigate; }),running.end());
  }

void NpcObserver::cancelDeeds(World& world) {
  for(auto& r:running)
    if(auto* npc = world.findNpcByInstance(r.instance)) {
      if(r.theft.active)
        NpcSteal::finish(*npc,r.theft);
      if(r.deed)
        npc->clearServerDeed();
      }
  running.erase(std::remove_if(running.begin(),running.end(),[](const Running& r){
    return r.theft.target.item!=nullptr || r.deed;
    }),running.end());
  }

void NpcObserver::releaseHeld(World& world, Held& h) {
  // the npc's pointers are only trusted if they still belong to the world
  Npc *self = nullptr, *other = nullptr, *victim = nullptr;
  if(Npc* pl = world.player())
    world.detectNpc(pl->position(),RosterRadius,[&](Npc& n) {
      if(&n==h.self)   self   = &n;
      if(&n==h.other)  other  = &n;
      if(&n==h.victim) victim = &n;
      });
  if(self==nullptr || other==nullptr || self->isDead() || self->isDown() || other->isDead())
    return;
  if(auto v = verdicts.find(h.key); v!=verdicts.end())
    v->second = Verdict{HoldTo::Scripts,world.tickCount()+(h.room ? 0 : VerdictMs),h.bandit};
  if(h.bandit && (self->isAttack() || self->isTalk() || !owner(npcId(world,*self)).empty() ||
                 BodyController::owns(*self)))
    return;  // an explicit fight or a newer owner takes precedence over a late sight signal
  if(!h.item.empty()) {
    const auto item = world.script().findSymbolIndex(h.item);
    if(item!=size_t(-1))
      world.script().setInstanceItem(*other,item);
    }
  self->passPerception(*other,h.victim!=nullptr ? victim : nullptr,h.perc);
  }

void NpcObserver::act(World& world, const rapidjson::Value& msg) {
  const auto id     = JsonRead::str(msg,"id");
  const auto symbol = upper(JsonRead::str(msg,"npc"));
  const auto action = JsonRead::str(msg,"action");
  auto result = [&](std::string_view status, std::string_view detail) { reply(id,symbol,status,detail,action); };
  Npc*       npc    = npcBySymbol(world,symbol);
  Npc*       pl     = world.player();
  if(npc==nullptr || npc->isPlayer() || pl==nullptr) {
    result("rejected","npc not found");
    return;
    }
  if(BodyController::owns(*npc)) {
    BodyController::legacyRejected(*npc,"legacy_npc_act_rejected");
    reply(id,symbol,"rejected","npc body is owned by a combat provider");
    return;
    }
  if(!owner(symbol).empty() || npc->isTalk()) {
    result("rejected","npc is busy (attached or in a dialog)");
    return;
    }
  if(npc->isDead() || npc->isDown()) {
    result("rejected","npc is down");
    return;
    }

  static const rapidjson::Value empty(rapidjson::kObjectType);
  const auto  argsIt = msg.FindMember("args");
  const auto& args   = (argsIt!=msg.MemberEnd() && argsIt->value.IsObject()) ? argsIt->value : empty;
  Npc*        target = pl;
  if(auto t = JsonRead::str(args,"npc"); !t.empty()) {
    target = AiNpc::findNpc(world,t,npc);
    if(target==nullptr || target==npc) {
      result("rejected","target not found");
      return;
      }
    }
  ScriptFn state;
  Running  run;  // what is tracked until npc_idle

  for(auto& previous:running)
    if(previous.symbol==symbol) {
      if(previous.theft.active)
        NpcSteal::finish(*npc,previous.theft);
      if(previous.deed)
        npc->clearServerDeed();
      }

  if(npc->isPhysicsInvestigating() && action!="fight" && action!="flee" && action!="draw_weapon" && action!="steal") {
    result("rejected","npc is investigating a noise");
    return;
    }

  if(action=="steal") {
    std::string error;
    if(!NpcSteal::begin(world,*npc,args,run.theft,error)) {
      result("rejected",error);
      return;
      }
    }
  else if(action=="say_svm") {
    std::string svm = upper(JsonRead::str(args,"svm"));
    if(svm.empty()) {
      result("rejected","say_svm needs svm");
      return;
      }
    if(svm[0]!='$')
      svm.insert(svm.begin(),'$');
    if(JsonRead::boolean(args,"overlay",false)) {
      // shouted on the move (AI_OutputSVM_Overlay): the body goes on with what it does
      npc->aiPush(AiQueue::aiOutputSvmOverlay(*target,svm,0));
      } else {
      npc->aiPush(AiQueue::aiTurnToNpc(target));
      npc->aiPush(AiQueue::aiOutputSvm(*target,svm,0));
      }
    }
  else if(action=="turn_to") {
    npc->aiPush(AiQueue::aiTurnToNpc(target));
    }
  else if(action=="look_at") {
    const auto ms = JsonRead::integer(args,"ms",3000);
    npc->aiPush(AiQueue::aiLookAtNpc(target));
    npc->aiPush(AiQueue::aiWait(uint64_t(std::clamp<int64_t>(ms,500,10000))));
    npc->aiPush(AiQueue::aiStopLookAt());
    }
  else if(action=="play_ani") {
    const auto name = JsonRead::str(args,"name");
    if(name.empty()) {
      result("rejected","play_ani needs name");
      return;
      }
    npc->aiPush(AiQueue::aiTurnToNpc(target));
    npc->aiPush(AiQueue::aiPlayAnim(name));
    }
  else if(action=="go_to_npc") {
    npc->aiPush(AiQueue::aiGoToNpc(target));
    npc->aiPush(AiQueue::aiTurnToNpc(target));
    }
  else if(action=="investigate") {
    const auto at = args.FindMember("at");
    if(at==args.MemberEnd() || !at->value.IsArray() || at->value.Size()!=3) {
      result("rejected","investigate needs at: [x,y,z]");
      return;
      }
    for(auto& value:at->value.GetArray())
      if(!value.IsNumber() || !std::isfinite(value.GetDouble())) {
        result("rejected","invalid investigation position");
        return;
        }
    const Tempest::Vec3 point(float(at->value[0].GetDouble()),float(at->value[1].GetDouble()),float(at->value[2].GetDouble()));
    if((point-npc->position()).quadLength()>EventRadius*EventRadius ||
       !npc->startPhysicsInvestigation(point,uint64_t(std::clamp<int64_t>(JsonRead::integer(args,"ms",5000),2000,8000)))) {
      result("rejected","npc cannot investigate this position now");
      return;
      }
    run.investigate = true;
    }
  else if(action=="turn_away") {
    npc->aiPush(AiQueue::aiTurnAway(target));
    }
  else if(action=="fight" || action=="flee") {
    // the game's own behaviour, the way B_Attack / B_Flee start it; the routine resumes afterwards
    state = AiNpc::scriptState(world,action=="fight" ? "ZS_ATTACK" : "ZS_FLEE");
    if(!state.isValid() || target->isDead()) {
      result("rejected",state.isValid() ? "target is dead" : "the scripts have no such state");
      return;
      }
    if(action=="fight") {
      run.deed = JsonRead::boolean(args,"deed",false);
      run.nonlethal = run.deed || JsonRead::boolean(args,"nonlethal",false);
      if(run.nonlethal && (target->isPlayer() || target->isMonster() || target->isDown())) {
        result("rejected","NPC nonlethal fight needs a standing human target");
        return;
        }
      const auto reasonName = run.nonlethal ? std::string("AR_NONE") : upper(JsonRead::str(args,"reason"));
      if(!reasonName.empty()) {
        auto* reason = world.script().findSymbol(reasonName);
        const auto attack = HeroDeed::constant(*npc,"AIV_ATTACKREASON");
        const auto last = HeroDeed::constant(*npc,"AIV_LASTPLAYERAR");
        const auto fight = HeroDeed::constant(*npc,"AIV_LASTFIGHTAGAINSTPLAYER");
        const auto comment = HeroDeed::constant(*npc,"AIV_LASTFIGHTCOMMENT");
        const auto valid = [](int32_t index) { return index>=0 && uint32_t(index)<zenkit::INpc::aivar_count; };
        if(!reasonName.starts_with("AR_") || reason==nullptr || !reason->is_const() ||
           reason->type()!=zenkit::DaedalusDataType::INT || !valid(attack) ||
           (target->isPlayer() && !valid(last)) || !valid(fight) || !valid(comment)) {
          result("rejected","fight needs a vanilla AR_* reason and its attack variables");
          return;
          }
        // Match B_Attack's combat bookkeeping without B_MemorizePlayerCrime:
        // the server already owns the witness memory and community counters.
        auto& aiv = npc->handlePtr()->aivar;
        aiv[size_t(attack)] = reason->get_int(0);
        if(target->isPlayer())
          aiv[size_t(last)] = reason->get_int(0);
        aiv[size_t(fight)] = HeroDeed::constant(*npc,"FIGHT_CANCEL",0);
        aiv[size_t(comment)] = 0;
        }
      }
    // An urgent verdict owns the body now, rather than waiting behind a routine walk, listening or
    // queued speech. Install the same state as AI_StartState(behavior=0), then add its fresh readiness
    // and shout: startState clears both native queues, including the overlay queue.
    npc->stopListening();
    npc->thinkOfAnswer(0);
    npc->clearAiQueue();
    if(!npc->startState(state,"",gtime::endOfTime(),true)) {
      result("rejected","npc could not start the state");
      return;
      }
    npc->setOther(target);
    npc->setVictim(nullptr);
    npc->setTarget(target);
    if(run.nonlethal) {
      npc->setNonlethalFight(*target);
      run.fightTarget = target->instanceSymbol();
      }
    if(run.deed)
      npc->setServerDeed("attack",target);
    if(action=="fight" && npc->weaponState()==WeaponState::NoWeapon)
      npc->aiPush(AiQueue::aiDrawWeapon());
    if(auto svm = upper(JsonRead::str(args,"svm")); !svm.empty()) {
      if(svm[0]!='$')
        svm.insert(svm.begin(),'$');
      // Explicit combat shouts belong to each defender. Ambient SVM overlays share one global
      // barrier and silently discard the second npc's shout; play this npc's own resolved voice
      // through the same sound/lip-sync path, without that ambient barrier or a movement wait.
      const auto line = world.script().messageFromSvm(svm,npc->handle().voice);
      if(!line.empty()) {
        uint64_t audioMs = 0;
        std::shared_ptr<LipSync::Track> voice;
        auto& fn = Gothic::inst().interceptSvm;
        const bool replaced = fn && fn(*npc,svm,line,target,true);
        if(replaced)
          audioMs = world.script().messageTime(line); else
          world.addDlgSound(std::string(line)+".WAV",npc->mapHeadBone(),WorldSound::talkRange,audioMs,&voice);
        if(replaced || audioMs>0) {
          if(!replaced)
            npc->startSpeech(std::move(voice));
          npc->setAiOutputBarrier(world.script().messageTime(line),true);
          TestBench::npcOutput(*npc,svm,target,audioMs);
          Tempest::Log::i("[ai] ",symbol," combat shout ",svm," audio_ms=",audioMs,
                         " replaced=",replaced," game_ms=",world.tickCount());
          }
        }
      }
    }
  else if(action=="draw_weapon") {
    const auto kind = JsonRead::str(args,"kind");
    npc->aiPush(kind=="ranged" ? AiQueue::aiReadyRangedWeapon()
                               : (kind=="melee" ? AiQueue::aiReadyMeleeWeapon() : AiQueue::aiDrawWeapon()));
    }
  else if(action=="remove_weapon") {
    npc->aiPush(AiQueue::aiRemoveWeapon());
    }
  else if(action=="start_activity") {
    // what a routine entry does (TA_Sit_Bench: ZS_SIT_BENCH at a waypoint), now, for `minutes` of game
    // time; afterwards the routine goes on by itself
    const auto name = upper(JsonRead::str(args,"state"));
    const auto wp   = world.findPoint(JsonRead::str(args,"waypoint"),false);
    state = name.starts_with("ZS_") ? AiNpc::scriptState(world,name) : ScriptFn();
    if(!state.isValid() || wp==nullptr) {
      result("rejected",!state.isValid() ? "unknown state: " + name : "unknown waypoint");
      return;
      }
    auto until = world.time();
    until.addMilis(uint64_t(std::clamp<int64_t>(JsonRead::integer(args,"minutes",60),1,24*60))*60*1000);
    // `place` (v38): it is there at once, the way the engine puts everyone at their routine's spot when the
    // clock leaps (World::setDayTime: the hero slept), as Npc::resetPositionToTA does: the walk went by meanwhile
    if(JsonRead::boolean(args,"place",false)) {
      npc->clearAiQueue();
      npc->setPosition (wp->position());
      npc->setDirection(wp->direction());
      world.script().fixNpcPosition(*npc,0,100);
      }
    npc->startState(state,wp->name,until,false);
    // `arrive` (v15): npc_idle "arrived" once it is there doing it, not only when it leaves it
    run.arrival = JsonRead::boolean(args,"arrive",false);
    // Native waypoint walks end at groundPos; authored points can float above the floor.
    run.at      = wp->groundPos;
    run.radius  = float(std::clamp<int64_t>(JsonRead::integer(args,"radius",500),50,5000));
    }
  else if(action=="set_routine") {
    // Npc_ExchangeRoutine: another set of routine entries (Rtn_<name>_<id>), effective at once
    const auto   name = upper(JsonRead::str(args,"name"));
    const size_t fn   = world.script().findSymbolIndex("RTN_" + name + "_" + std::to_string(npc->handle().id));
    if(name.empty() || fn==size_t(-1)) {
      result("rejected","unknown routine: " + name);
      return;
      }
    npc->excRoutine(fn);
    npc->resumeAiRoutine();
    }
  else if(action=="give") {
    // to another npc (v15), as give_item does for an attached one: walks up if needed, turns, the item
    // changes hands once its queue is done (checkRunning). The hero gets things only through deals
    if(target==pl) {
      result("rejected","give: to an npc only");
      return;
      }
    const auto   name = upper(JsonRead::str(args,"item"));
    const size_t cls  = world.script().findSymbolIndex(name);
    const size_t have = cls==size_t(-1) ? 0 : npc->inventory().itemCount(cls);
    if(have==0) {
      result("rejected","npc has no item " + name);
      return;
      }
    if(auto* it = const_cast<Inventory&>(npc->inventory()).getItem(cls); it!=nullptr && it->isMission()) {
      result("rejected","a mission item: " + name);  // v39: never between npc's
      return;
      }
    run.item   = cls;
    run.count  = std::min(have,size_t(std::max<int64_t>(1,JsonRead::integer(args,"count",1))));
    run.giveTo = target->instanceSymbol();
    if(npc->qDistTo(*target)>AiNpc::GiveRange*AiNpc::GiveRange)
      npc->aiPush(AiQueue::aiGoToNpc(target));
    npc->aiPush(AiQueue::aiTurnToNpc(target));
    }
  else {
    result("rejected","unknown action: " + std::string(action));
    return;
    }

  // Only combat actions pass the busy guard. Preserve their newly queued work.
  if(!run.investigate) {
    npc->stopPhysicsInvestigation(false);
    for(const auto& previous:running)
      if(previous.symbol==symbol && previous.investigate)
        emit(JsonWriter("npc_idle").str("npc",symbol).str("act",previous.id).str("reason","interrupted").finish());
    }

  // a newer action for the same npc replaces the tracking of the older one
  running.erase(std::remove_if(running.begin(),running.end(),[&](const Running& r){ return r.symbol==symbol; }),
                running.end());
  run.id       = std::string(id);
  run.symbol   = symbol;
  run.instance = world.script().findSymbolIndex(symbol);
  run.start    = world.tickCount();
  run.state    = state;
  run.maxMs    = run.theft.active ? StateMaxMs : (run.investigate ? 46000 : (action=="start_activity" ? ActivityMaxMs : (state.isValid() ? StateMaxMs : ActMaxMs)));
  running.push_back(std::move(run));
  result("queued","");
  }

void NpcObserver::routine(World& world, const rapidjson::Value& msg) {
  const auto symbol = upper(JsonRead::str(msg,"npc"));
  Npc*       npc    = npcBySymbol(world,symbol);
  JsonWriter j("routine");
  j.str("req",JsonRead::str(msg,"req")).str("npc",symbol);
  if(npc==nullptr || npc->isPlayer()) {
    emit(j.str("error","npc not found").finish());
    return;
    }
  auto stateName = [&](ScriptFn fn) { return AiNpc::stateName(world,fn); };
  auto hhmm = [](gtime t) {
    char buf[8];
    std::snprintf(buf,sizeof(buf),"%02d:%02d",int(t.hour()),int(t.minute()));
    return std::string(buf);
    };
  j.str("current",stateName(npc->currentState())).array("entries");
  npc->forEachRoutine([&](const Npc::RoutineInfo& r) {
    j.object()
       .str("start",hhmm(r.start))
       .str("end",hhmm(r.end))
       .str("state",stateName(r.state))
       .str("waypoint",r.point)
     .end();
    });
  j.endArray().array("routines");
  // the npc's routine variants: Rtn_<name>_<id> in the scripts
  const std::string suffix = "_" + std::to_string(npc->handle().id);
  for(size_t i=0; i<world.script().symbolsCount(); ++i) {
    auto sym = world.script().findSymbol(i);
    if(sym==nullptr)
      continue;
    const std::string& n = sym->name();
    if(n.size()>4+suffix.size() && n.starts_with("RTN_") && n.ends_with(suffix))
      j.item(n.substr(4,n.size()-4-suffix.size()));
    }
  emit(j.endArray().finish());
  }

void NpcObserver::checkRunning(World& world, uint64_t now) {
  for(size_t i=0; i<running.size();) {
    auto& r   = running[i];
    Npc*  npc = world.findNpcByInstance(r.instance);
    const bool  gone   = npc==nullptr || npc->isDead() || npc->isDown() || !owner(r.symbol).empty();
    bool        done   = false;
    std::string reason = "done";
    std::string detail;
    Npc* fightTarget = r.nonlethal ? world.findNpcByInstance(r.fightTarget) : nullptr;
    const auto stopScopedFight = [&] {
      // A terminal receipt belongs to this command only. Native retargeting or
      // a later owner must keep its state and queues; unconscious bodies keep
      // their own native recovery state too.
      if(gone || !r.nonlethal || fightTarget==nullptr || !npc->isInState(r.state) ||
         npc->target()!=fightTarget)
        return false;
      npc->clearAiQueue();
      npc->clearState(true);
      npc->setTarget(nullptr);
      npc->resumeAiRoutine();
      return true;
      };
    if(!gone && r.theft.active && now>r.start+ActGraceMs) {
      const auto progress = NpcSteal::tick(world,*npc,r.theft,detail);
      done = progress!=NpcSteal::Progress::Running;
      if(progress==NpcSteal::Progress::Failed) reason = "failed";
      }
    else if(!gone && fightTarget!=nullptr && fightTarget->isDown()) {
      // AR_NONE never requests finishing. Stop the command at the actual
      // knockout as well, before ZS_ATTACK can queue any post-combat action.
      stopScopedFight();
      detail = fightTarget->isDead() ? "target_dead" : "target_down";
      done = true;
      }
    else if(!gone && r.investigate) {
      done = !npc->isPhysicsInvestigating();
      if(done && !npc->physicsInvestigationResult().empty())
        reason = npc->physicsInvestigationResult();
      }
    else if(!gone && r.state.isValid()) {
      // a script state: done once the npc has entered it and left it again
      const bool in = npc->isInState(r.state);
      r.entered = r.entered || in;
      done      = (r.entered && !in) || (!r.entered && now>r.start+StateStartMs);
      // there and doing it (v15): the state has begun (its start pushes the walk to the spot) and the walk
      // is over (queue, go-to and waits empty). Measure from the feet to the grounded waypoint,
      // just as MoveAlgo does; qDistTo measures from the body's centre instead.
      if(r.arrival && in && now>r.start+ActGraceMs && npc->isAiQueueEmpty() &&
         (npc->position()-r.at).quadLength()<=r.radius*r.radius) {
        done   = true;
        reason = "arrived";
        }
      } else {
      done = !gone && now>r.start+ActGraceMs && npc->isAiQueueEmpty();
      }
    const bool late = now>r.start+r.maxMs;
    if(!gone && !done && !late) {
      ++i;
      continue;
      }
    if(!done && late && stopScopedFight()) {
      // ZS_ATTACK_LOOP keeps counterattacking a nearby standing target even
      // after that target leaves combat. End only this pair's native response;
      // another server action, owner or state has priority over our old run.
      const bool otherRun = std::any_of(running.begin(),running.end(),[&](const Running& other) {
        return other.instance==r.fightTarget;
        });
      if(!otherRun && !fightTarget->isDown() && !fightTarget->isLie() &&
         !BodyController::owns(*fightTarget) && !fightTarget->isExternalControl() &&
         owner(npcId(world,*fightTarget)).empty() && !fightTarget->isTalk() &&
         !Gothic::inst().isNpcInDialog(*fightTarget) && fightTarget->isInState(r.state) &&
         fightTarget->target()==npc) {
        fightTarget->clearAiQueue();
        fightTarget->clearState(true);
        fightTarget->setTarget(nullptr);
        fightTarget->resumeAiRoutine();
        }
      }
    if(npc!=nullptr) {
      NpcSteal::finish(*npc,r.theft);
      if(r.deed) npc->clearServerDeed();
      }
    if(gone) {
      reason = "gone";
      if(r.nonlethal && npc!=nullptr && npc->isDown())
        detail = npc->isDead() ? "defender_dead" : "defender_down";
      } else
    if(!done)
      reason = "timeout"; else
    if(r.giveTo!=size_t(-1)) {
      // a give (v15): walked up and turned, the item changes hands now
      Npc* to = world.findNpcByInstance(r.giveTo);
      bool ok = false;
      if(to==nullptr || to->isDead())
        detail = "target npc is gone"; else
        ok = AiNpc::giveItem(*npc,*to,r.item,r.count,detail);
      if(!ok)
        reason = "failed";
      }
    JsonWriter j("npc_idle");
    j.str("npc",r.symbol).str("act",r.id).str("reason",reason);
    if(!detail.empty())
      j.str("detail",detail);
    emit(j.finish());
    running.erase(running.begin()+ptrdiff_t(i));
    }
  }

void NpcObserver::emitEvent(std::string_view npc, std::string_view kind, std::string_view other,
                            double distance, std::string_view detail, std::string_view otherName,
                            std::string_view victim) {
  JsonWriter j("npc_event");
  j.str("npc",npc).str("kind",kind).str("other",other).num("distance",distance);
  if(!otherName.empty())
    j.str("other_name",otherName);
  if(!victim.empty())
    j.str("victim",victim);
  if(!detail.empty())
    j.str("detail",detail);
  emit(j.finish());
  }

bool NpcObserver::throttled(std::string_view npc, std::string_view kind, uint64_t now, uint64_t periodMs) {
  std::string key(npc);
  key += '/';
  key += kind;
  auto it = lastEvent.find(key);
  if(it!=lastEvent.end() && it->second<=now && now<it->second+periodMs)
    return true;
  lastEvent[key] = now;
  return false;
  }

void NpcObserver::reply(std::string_view id, std::string_view npc, std::string_view status, std::string_view detail,
                        std::string_view action) {
  JsonWriter j("npc_act_result");
  j.str("id",id).str("npc",npc).str("status",status);
  if(!action.empty())
    j.str("action",action);
  if(!detail.empty())
    j.str("detail",detail);
  emit(j.finish());
  }
