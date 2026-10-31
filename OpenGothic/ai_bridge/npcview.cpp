#include "npcview.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/bodycontroller.h"
#include "game/constants.h"
#include "game/gamescript.h"
#include "camera.h"
#include "gothic.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include "jsonwriter.h"
#include "npcfind.h"
#include "textcodec.h"

namespace {

constexpr uint64_t PushPeriodMs = 1000;
constexpr float    PushRadius   = 2000;  // cm from the player: npc's whose view is pushed
constexpr size_t   PushLimit    = 12;
constexpr size_t   SeenLimit    = 12;    // people and creatures in one view
constexpr float    SightMax     = 3000;  // cm: never further, whatever the senses range says
constexpr float    MonsterSight = 4000;  // cm: companions spot hostile creatures further than people
constexpr float    FacingCos    = 0.7f;  // within ~45 degrees: it faces you
constexpr float    NoticeRange  = 1500;  // cm: this close it notices someone out of its field of view too
constexpr size_t   ItemLimit    = 6;     // things lying about in one view (v22)
constexpr float    ItemRange    = 1500;  // cm: things lying further off are not told of
constexpr float    UnderFoot    = 300;   // cm: this close it notices a thing out of its field of view too
constexpr size_t   TraceLimit   = 6;
constexpr float    TraceRange   = 1500;  // cm: visible physical evidence

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

void key(Writer& w, std::string_view k) {
  w.Key(k.data(),rapidjson::SizeType(k.size()));
  }

void str(Writer& w, std::string_view k, std::string_view v) {
  key(w,k);
  w.String(v.data(),rapidjson::SizeType(v.size()));
  }

void utf8(Writer& w, std::string_view k, std::string_view v) {
  str(w,k,TextCodec::toUtf8(v));
  }

std::string symbolOf(World& world, size_t cls) {
  auto sym = world.script().findSymbol(cls);
  return sym!=nullptr ? sym->name() : std::string();
  }

std::string npcId(World& world, Npc& npc) {
  if(npc.isPlayer())
    return "PLAYER";
  return symbolOf(world,npc.instanceSymbol());
  }

// what a mob is called in the game: its focus name is a script string (MOBNAME_BOOKSBOARD), shown by value
std::string mobName(World& world, std::string_view focus) {
  auto sym = world.script().findSymbol(focus);
  if(sym!=nullptr && sym->type()==zenkit::DaedalusDataType::STRING)
    return sym->get_string();
  return std::string(focus);
  }

std::string_view weapon(const Npc& npc) {
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

std::string_view body(const Npc& npc) {
  if(npc.isDead())
    return "dead";
  if(npc.isUnconscious())
    return "down";
  if(npc.interactive()!=nullptr)
    return "mob";
  switch(npc.bodyStateMasked()) {
    case BS_WALK:
    case BS_SNEAK:          return "walk";
    case BS_RUN:
    case BS_SPRINT:         return "run";
    case BS_SWIM:
    case BS_DIVE:           return "swim";
    case BS_SIT:            return "sit";
    case BS_LIE:            return "lie";
    case BS_ITEMINTERACT:   return "item";
    default:                return "stand";
    }
  }

// `other` faces `self`: its body turned towards it
bool faces(const Npc& other, const Npc& self) {
  const auto  d   = self.position()-other.position();
  const float len = std::sqrt(d.x*d.x+d.z*d.z);
  if(len<1)
    return true;
  const float rot = other.rotationRad();
  return (d.x*std::cos(rot)+d.z*std::sin(rot))/len>=FacingCos;
  }

std::string_view attitude(Attitude a) {
  switch(a) {
    case ATT_HOSTILE:  return "hostile";
    case ATT_ANGRY:    return "angry";
    case ATT_FRIENDLY: return "friendly";
    case ATT_NEUTRAL:
    case ATT_NULL:     break;
    }
  return "neutral";
  }

bool partyMember(World& world, const Npc& npc) {
  auto* s = world.script().getVm().find_symbol_by_name("AIV_PARTYMEMBER");
  if(s==nullptr || s->type()!=zenkit::DaedalusDataType::INT)
    return false;
  const int32_t aiv = s->get_int(0);
  return aiv>=0 && uint32_t(aiv)<zenkit::INpc::aivar_count && npc.handlePtr()->aivar[size_t(aiv)]!=0;
  }

bool distantMonster(World& world, const Npc& self, const Npc& other) {
  const auto* pl = world.player();
  return !other.isDown() && other.isMonster() && partyMember(world,self) &&
         (world.script().personAttitude(other,self)==ATT_HOSTILE ||
          (pl!=nullptr && world.script().personAttitude(other,*pl)==ATT_HOSTILE));
  }

bool monsterInSight(const Npc& self, const Npc& other) {
  const float extra = std::max(0.f,MonsterSight-float(self.handlePtr()->senses_range));
  return self.canRayHitPoint(other.centerPosition(),false,extra) ||
         self.canRayHitPoint(other.mapHeadBone(),false,extra);
  }

// where `other` is from the player, as he faces (v18): ahead, left, right or behind him. Left and right
// are as the player sees them on the screen: the side is checked against the camera when there is one
std::string_view fromHero(const Npc& pl, const Tempest::Vec3& at) {
  const auto  d   = at-pl.position();
  const float len = std::sqrt(d.x*d.x+d.z*d.z);
  if(len<1)
    return "ahead";
  const auto  ax    = heroAxes(pl);
  const float ahead = (d.x*ax.aheadX+d.z*ax.aheadZ)/len;
  const float right = (d.x*ax.rightX+d.z*ax.rightZ)/len;
  if(ahead>=0.7f)
    return "ahead";
  if(ahead<=-0.7f)
    return "behind";
  return right>0 ? "right" : "left";
  }

std::string_view fromHero(const Npc& pl, const Npc& other) {
  return fromHero(pl,other.position());
  }

void writeItem(Writer& w, World& world, std::string_view k, const Item* it) {
  if(it==nullptr)
    return;
  key(w,k);
  w.StartObject();
  str(w,"item",symbolOf(world,it->clsId()));
  utf8(w,"name",it->displayName());
  w.EndObject();
  }

// what one can tell about a person or creature by looking at it
void writeLook(Writer& w, World& world, Npc& self, Npc& other, float dist) {
  str(w,"id",npcId(world,other));
  utf8(w,"name",other.displayName());
  str(w,"kind",other.isDead() ? "body" : (other.guild()>=GIL_SEPERATOR_HUM ? "monster" : "human"));
  w.Key("guild");    w.Int(int(other.guild()));
  w.Key("distance"); w.Double(std::round(double(dist)));
  str(w,"state",AiNpc::stateName(world,other.currentState()));
  str(w,"body",body(other));
  w.Key("moving");   w.Bool(AiNpc::isMoving(other));
  str(w,"weapon",weapon(other));
  w.Key("faces_you"); w.Bool(faces(other,self));
  const int32_t hp = other.attribute(ATR_HITPOINTS), hpMax = other.attribute(ATR_HITPOINTSMAX);
  w.Key("hurt");     w.Bool(hpMax>0 && hp*2<hpMax);
  if(other.guild()<GIL_SEPERATOR_HUM) {
    auto& inv = const_cast<Inventory&>(other.inventory());
    writeItem(w,world,"armor",inv.currentArmor());
    writeItem(w,world,"in_hands",inv.stateSlotItem());
    }
  if(auto partner = other.talkPartner())
    str(w,"talking_to",npcId(world,*partner));
  if(other.guild()<GIL_SEPERATOR_HUM) {
    // where a person is: what the server remembers it was seen at (v12)
    if(auto wp = world.findWayPoint(other.position()))
      str(w,"waypoint",wp->name);
    }
  if(other.isPlayer())
    return;
  w.Key("fighting"); w.Bool(other.isAttack());
  if(auto* target = other.target(); target!=nullptr && target!=&other)
    str(w,"combat_target",npcId(world,*target));
  // v18: how it treats the viewer and the hero, and where it is from the hero
  auto& vm = world.script();
  str(w,"att",attitude(vm.personAttitude(other,self)));
  if(Npc* pl = world.player()) {
    str(w,"att_hero",attitude(vm.personAttitude(other,*pl)));
    w.Key("hero_cm"); w.Double(std::round(double(std::sqrt(other.qDistTo(*pl)))));
    str(w,"from_hero",fromHero(*pl,other));
    }
  }

// things lying about that it sees, the nearest first (v22): what the hero threw down, a body's loot, herbs
void writeItems(Writer& w, World& world, Npc& npc, float range) {
  struct Thing { Item* it; float dist; };
  std::vector<Thing> things;
  world.detectItem(npc.position(),std::min(range,ItemRange),[&](Item& it) {
    const float dist = std::sqrt((it.position()-npc.position()).quadLength());
    if(npc.canSeeItem(it,false) || (dist<=UnderFoot && npc.canSeeItem(it,true)))
      things.push_back({&it,dist});
    });
  std::sort(things.begin(),things.end(),[](const Thing& a, const Thing& b){ return a.dist<b.dist; });
  if(things.size()>ItemLimit)
    things.resize(ItemLimit);
  Npc* pl = world.player();
  key(w,"items");
  w.StartArray();
  for(auto& t:things) {
    w.StartObject();
    str(w,"item",symbolOf(world,t.it->clsId()));
    utf8(w,"name",t.it->displayName());
    w.Key("count");    w.Uint64(uint64_t(t.it->count()));
    w.Key("distance"); w.Double(std::round(double(t.dist)));
    if(pl!=nullptr) {
      w.Key("hero_cm"); w.Double(std::round(double(std::sqrt((t.it->position()-pl->position()).quadLength()))));
      str(w,"from_hero",fromHero(*pl,t.it->position()));
      }
    w.EndObject();
    }
  w.EndArray();
  }

void writeTraces(Writer& w, World& world, Npc& npc, float range) {
  struct SeenTrace { const World::PhysicsTrace* trace; float dist; };
  const auto traces = world.physicsTraces();
  std::vector<SeenTrace> seen;
  const auto maxRange = std::min(range,TraceRange);
  for(const auto& trace:traces) {
    const auto dist = (trace.position-npc.position()).length();
    if(dist>maxRange)
      continue;
    const bool visible = std::any_of(trace.sightPoints.begin(),trace.sightPoints.end(),[&](const Tempest::Vec3& p) {
      return npc.canRayHitPoint(p,false);
      });
    if(visible)
      seen.push_back({&trace,dist});
    }
  std::sort(seen.begin(),seen.end(),[](const SeenTrace& a,const SeenTrace& b) { return a.dist<b.dist; });
  if(seen.size()>TraceLimit)
    seen.resize(TraceLimit);
  const auto now = world.time();
  const int64_t minute = now.day()*24*60+now.hour()*60+now.minute();
  const auto* pl = world.player();
  key(w,"traces");
  w.StartArray();
  for(const auto& seenTrace:seen) {
    const auto& trace = *seenTrace.trace;
    w.StartObject();
    // Deliberate field list: the engine's culprit is never part of perception.
    str(w,"id",trace.id);
    str(w,"kind",trace.kind);
    str(w,"name",trace.name); // engine trace labels are UTF-8
    if(const auto* waypoint = world.findWayPoint(trace.position))
      str(w,"waypoint",waypoint->name);
    w.Key("distance"); w.Double(std::round(double(seenTrace.dist)));
    w.Key("age");
    if(trace.appearedAt<0)
      w.Null(); // records written before trace metadata do not reveal their age
    else
      w.Int64(std::max(int64_t(0),minute-trace.appearedAt));
    if(pl!=nullptr) {
      w.Key("hero_cm"); w.Double(std::round(double((trace.position-pl->position()).length())));
      str(w,"from_hero",fromHero(*pl,trace.position));
      }
    w.EndObject();
    }
  w.EndArray();
  }

void writeView(Writer& w, World& world, Npc& npc) {
  Npc* pl = world.player();
  w.StartObject();
  str(w,"npc",npcId(world,npc));
  utf8(w,"name",npc.displayName());
  if(auto wp = npc.currentWayPoint())
    str(w,"waypoint",wp->name);
  if(auto wp = world.findWayPoint(npc.position()))
    str(w,"nearest_waypoint",wp->name);
  const auto portal = npc.portalName();
  w.Key("indoors"); w.Bool(!portal.empty());
  if(!portal.empty()) {
    w.Key("room_guild"); w.Int(world.guildOfRoom(portal));
    }
  str(w,"state",AiNpc::stateName(world,npc.currentState()));
  // Scene preparation can wait for the routine initializer to install the
  // native callback. Observing this never enables or dispatches a perception.
  const auto perceptionReady = [&](PercType perception) {
    return npc.isStateStarted() && npc.hasPerc(perception) &&
           npc.processPolicy()==NpcProcessPolicy::AiNormal && npc.guild()<GIL_SEPERATOR_HUM &&
           !npc.isPlayer() && !npc.isDead() && !npc.isDown() &&
           !npc.isExternalControl() && !BodyController::owns(npc) && !npc.isTalk() && !npc.isAttack() &&
           !Gothic::inst().isNpcInDialog(npc);
    };
  w.Key("theft_ready"); w.Bool(perceptionReady(PERC_ASSESSTHEFT));
  w.Key("senses_range"); w.Int(npc.handle().senses_range);
  w.Key("theft_range"); w.Double(npc.perceptionRange(PERC_ASSESSTHEFT));
  w.Key("others_damage_ready"); w.Bool(perceptionReady(PERC_ASSESSOTHERSDAMAGE));
  w.Key("others_damage_range"); w.Double(npc.perceptionRange(PERC_ASSESSOTHERSDAMAGE));
  str(w,"body",body(npc));
  if(auto mob = npc.interactive())
    utf8(w,"mob",mobName(world,mob->focusName()));
  str(w,"weapon",weapon(npc));
  writeItem(w,world,"in_hands",npc.inventory().stateSlotItem());
  w.Key("hp");     w.Int(npc.attribute(ATR_HITPOINTS));
  w.Key("hp_max"); w.Int(npc.attribute(ATR_HITPOINTSMAX));
  const auto now = world.time();  // the game's clock, in minutes since the first day began (v12)
  w.Key("time");   w.Int64(int64_t(now.day())*24*60+now.hour()*60+now.minute());

  // the player: always there (it hears him if he speaks), seen or not
  if(pl!=nullptr && pl!=&npc) {
    const float dist = std::sqrt(npc.qDistTo(*pl));
    key(w,"hero");
    w.StartObject();
    writeLook(w,world,npc,*pl,dist);
    w.Key("seen"); w.Bool(npcSees(npc,*pl));
    if(Npc* focus = pl->target(); focus!=nullptr && focus!=pl)
      str(w,"focus",npcId(world,*focus));  // whom the hero looks at (v18)
    w.EndObject();
    }

  // whom it sees: the nearest first
  // and close by, out of its field of view: it notices them all the same, if nothing is in between (v18)
  struct Seen { Npc* npc; float dist; bool noticed; };
  std::vector<Seen> seen;
  const float range = std::min(float(npc.handle().senses_range),SightMax);
  const bool companion = partyMember(world,npc);
  world.detectNpc(npc.position(),companion ? std::max(range,MonsterSight) : range,[&](Npc& other) {
    if(&other==&npc || other.isPlayer())
      return;
    const float dist = std::sqrt(npc.qDistTo(other));
    const bool extended = companion && dist<=MonsterSight && distantMonster(world,npc,other);
    if((dist<=range && npc.canSeeNpc(other,false)) || (extended && monsterInSight(npc,other)))
      seen.push_back({&other,dist,false});
    else if(dist<=NoticeRange && npc.canSeeNpc(other,true))
      seen.push_back({&other,dist,true});
    });
  std::sort(seen.begin(),seen.end(),[](const Seen& a, const Seen& b){ return a.dist<b.dist; });
  if(seen.size()>SeenLimit)
    seen.resize(SeenLimit);
  key(w,"sees");
  w.StartArray();
  for(auto& s:seen) {
    w.StartObject();
    writeLook(w,world,npc,*s.npc,s.dist);
    if(s.noticed) {
      w.Key("noticed"); w.Bool(true);  // out of its field of view: behind it or to the side
      }
    w.EndObject();
    }
  w.EndArray();
  writeItems(w,world,npc,range);
  writeTraces(w,world,npc,range);
  w.EndObject();
  }

}

bool npcSees(const Npc& self, const Npc& other) {
  return self.qDistTo(other)<=SightMax*SightMax && self.canSeeNpc(other,false);
  }

NpcView::NpcView(Emit emit):emit(std::move(emit)) {
  }

void NpcView::clear() {
  next = 0;
  }

void NpcView::tick(World& world) {
  const uint64_t now = world.tickCount();
  if(now<next && next<=now+PushPeriodMs)
    return;
  next = now+PushPeriodMs;
  Npc* pl = world.player();
  if(pl==nullptr)
    return;
  struct Near { Npc* npc; float dist; };
  std::vector<Near> near;
  world.detectNpc(pl->position(),PushRadius,[&](Npc& npc) {
    if(npc.isPlayer() || npc.isDead() || npc.guild()>=GIL_SEPERATOR_HUM)
      return;  // people: monsters keep their own AI
    near.push_back({&npc,std::sqrt(npc.qDistTo(*pl))});
    });
  if(near.empty())
    return;
  std::sort(near.begin(),near.end(),[](const Near& a, const Near& b){ return a.dist<b.dist; });
  if(near.size()>PushLimit)
    near.resize(PushLimit);

  rapidjson::StringBuffer buf;
  Writer                  w(buf);
  w.StartArray();
  for(auto& n:near)
    writeView(w,world,*n.npc);
  w.EndArray();
  JsonWriter j("npc_views");
  j.integer("t",int64_t(now))
   .raw("views",std::string_view(buf.GetString(),buf.GetSize()));
  emit(j.finish());
  }

std::string NpcView::views(World& world, const rapidjson::Value& npcs) {
  rapidjson::StringBuffer buf;
  Writer                  w(buf);
  w.StartArray();
  if(npcs.IsArray()) {
    for(auto& v:npcs.GetArray()) {
      if(!v.IsString())
        continue;
      if(Npc* npc = AiNpc::findNpc(world,v.GetString()); npc!=nullptr && !npc->isPlayer())
        writeView(w,world,*npc);
      }
    }
  w.EndArray();
  return std::string(buf.GetString(),buf.GetSize());
  }

HeroAxes heroAxes(const Npc& pl) {
  const float a  = pl.rotationRad();
  HeroAxes    ax = {std::cos(a),std::sin(a),std::sin(a),-std::cos(a)};  // left-handed, y up: the world's own
  if(auto cam = Gothic::inst().camera()) {
    // a point to that side of the player must come out to the right of him on the screen
    auto  vp   = cam->viewProj();
    auto  at   = pl.position()+Tempest::Vec3(0,100,0);
    auto  side = at+Tempest::Vec3(ax.rightX,0,ax.rightZ)*100.f;
    float w0 = 1, w1 = 1;
    vp.project(at.x,at.y,at.z,w0);
    vp.project(side.x,side.y,side.z,w1);
    if(w0>0 && w1>0 && side.x/w1<at.x/w0) {
      ax.rightX = -ax.rightX;
      ax.rightZ = -ax.rightZ;
      }
    }
  return ax;
  }
