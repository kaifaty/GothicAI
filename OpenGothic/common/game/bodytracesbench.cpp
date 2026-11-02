#include "bodytracesbench.h"

#include <Tempest/MemWriter>
#include <Tempest/Pixmap>

#include <rapidjson/document.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

#include "game/constants.h"
#include "game/gamescript.h"
#include "game/gamesession.h"
#include "game/serialize.h"
#include "physics/dynamicworld.h"
#include "world/bodytraces.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"
#include "testbench.h"

using namespace Tempest;

namespace {

// Distinct instances so a fresh process finds each role again after loading a save.
struct Role {
  const char* name;
  const char* instance;
  bool        monster;
  };

const Role Roles[] = {
  {"victim",          "BDT_1000_BANDIT_L", false}, // an ordinary person: a trace, leaves after 3 days unseen
  {"wolf",            "WOLF",              true }, // a monster: no trace, leaves after 3 days unseen
  {"mission",         "BDT_1001_BANDIT_L", false}, // carries an ITM_MISSION item: stays
  {"immortal",        "BDT_1002_BANDIT_L", false}, // IMMORTAL: stays
  {"story",           "BDT_1003_BANDIT_M", false}, // npctype NPCTYPE_MAIN: stays
  {"sleeper",         "BDT_1005_BANDIT_M", false}, // second batch: sleep near, then sleep far
  {"sleeper_mission", "BDT_1006_BANDIT_H", false}, // second batch with a mission item: stays where it fell
  };
constexpr const char* ObserverInstance = "BDT_1004_BANDIT_M";
// Gothic II keys flagged ITEM_MISSION; the first one that really is a mission item is used.
const char* MissionItems[] = {"ITKE_BANDIT","ITKE_DEXTER","ITKE_BUERGER","ITKE_BROMOR","ITKE_THORUS"};

struct Actor {
  const Role* role = nullptr;
  Npc*        npc  = nullptr;
  Vec3        fell;       // where it lay before the clock leaps
  bool        fellKnown = false;
  bool        sleepBatch = false;
  };

struct State {
  World*                       world = nullptr;
  std::map<std::string,Actor>  actors;
  Npc*                         observer = nullptr;
  bool                         sleepBatch = false; // the batch the hero moves around
  int64_t                      firstAge = -1;
  int64_t                      loadMinute = -1;
  } st;

std::string jsonStr(std::string_view s) {
  std::string out = "\"";
  for(char c:s) {
    if(c=='"' || c=='\\') { out+='\\'; out+=c; }
    else if(uint8_t(c)<0x20) { char buf[8]; std::snprintf(buf,sizeof(buf),"\\u%04x",unsigned(uint8_t(c))); out+=buf; }
    else out+=c;
    }
  return out+"\"";
  }

std::string num(float v) {
  char buf[32];
  std::snprintf(buf,sizeof(buf),"%.1f",double(v));
  return buf;
  }

std::string vec(const Vec3& v) {
  return "["+num(v.x)+","+num(v.y)+","+num(v.z)+"]";
  }

[[noreturn]] void fail(const std::string& why) {
  throw std::runtime_error("body traces: "+why);
  }

std::string symbolOf(World& world, const Npc& npc) {
  if(npc.isPlayer())
    return "PLAYER";
  auto* s = world.script().findSymbol(npc.instanceSymbol());
  return s!=nullptr ? s->name() : std::string();
  }

bool inWorld(World& world, const Npc* npc) {
  return npc!=nullptr && world.npcId(npc)!=uint32_t(-1);
  }

Vec3 forwardOf(const Npc& npc) {
  const float a = npc.rotationRad();
  return Vec3(std::cos(a),0,std::sin(a));
  }

// A standable floor below `point`, clear of walls at the hips (the trace fixtures' rule).
bool floorAt(World& world, Vec3& point, float above = 2000.f) {
  auto* phys = world.physic();
  const auto floor = phys->landRay(point+Vec3(0,above,0),above+2000.f);
  if(!floor.hasCol || floor.n.y<.75f)
    return false;
  point.y = floor.v.y;
  if(phys->ray(point+Vec3(0,30,0),point+Vec3(0,200,0)).hasCol)
    return false;
  for(const auto offset:{Vec3(40,0,0),Vec3(-40,0,0),Vec3(0,0,40),Vec3(0,0,-40)})
    if(phys->ray(point+Vec3(0,85,0),point+offset+Vec3(0,85,0)).hasCol)
      return false;
  return true;
  }

bool raySees(World& world, const Vec3& eye, const Npc& body) {
  for(const auto& p:world.bodySightPoints(body))
    if(!world.physic()->ray(eye,p).hasCol)
      return true;
  return false;
  }

void park(Npc& npc, const Vec3& at, const Vec3& look) {
  npc.clearAiQueue();
  npc.clearState(false);
  npc.stopWalking();
  npc.setInteraction(nullptr,true);
  npc.closeWeapon(true);
  npc.setPosition(at);
  npc.setDirection(look-at);
  npc.updateTransform();
  npc.aiPush(AiQueue::aiWait(10ull*60*60*1000));
  }

// Ground for a body near `at`: the first spot of a small spiral with a floor below.
bool groundNear(World& world, Vec3& at) {
  for(float r:{0.f,100.f,200.f,300.f,450.f}) {
    for(int i=0;i<(r==0 ? 1 : 12);++i) {
      const float a = float(i)*float(M_PI)/6.f;
      auto p = at+Vec3(std::cos(a)*r,0,std::sin(a)*r);
      const auto floor = world.physic()->landRay(p+Vec3(0,400,0),1200.f);
      if(!floor.hasCol || floor.n.y<.5f || world.physic()->ray(floor.v+Vec3(0,30,0),floor.v+Vec3(0,180,0)).hasCol)
        continue;
      at = floor.v;
      return true;
      }
    }
  return false;
  }

Npc* spawn(World& world, const char* instance, Vec3 at) {
  const auto id = world.script().findSymbolIndex(instance);
  if(id==size_t(-1))
    fail(std::string("no instance ")+instance);
  if(!groundNear(world,at))
    fail(std::string("no floor for ")+instance);
  auto* npc = world.addNpc(id,at);
  if(npc==nullptr)
    fail(std::string("cannot insert ")+instance);
  return npc;
  }

int32_t scriptInt(World& world, const char* name, int32_t fallback) {
  auto* s = world.script().getVm().find_symbol_by_name(name);
  return s!=nullptr && s->type()==zenkit::DaedalusDataType::INT ? s->get_int(0) : fallback;
  }

void giveMissionItem(World& world, Npc& npc) {
  for(const char* name:MissionItems) {
    const auto id = world.script().findSymbolIndex(name);
    if(id==size_t(-1))
      continue;
    if(auto* it = npc.addItem(id,1); it!=nullptr && it->isMission())
      return;
    }
  fail("no ITM_MISSION key among the candidates");
  }

void kill(Npc& npc, Npc& hero) {
  npc.setLastHitBy(&hero);
  npc.setRagdollDown(true);
  npc.prepareRagdoll();
  if(!npc.isDead())
    fail("fixture kill failed for "+std::to_string(npc.bodyTrace().id));
  }

Actor& actor(const std::string& role) {
  auto it = st.actors.find(role);
  if(it==st.actors.end() || it->second.npc==nullptr)
    fail("no fixture actor "+role+" (setup or reload first)");
  return it->second;
  }

std::string actorJson(World& world, const Npc& hero, const Actor& a) {
  const auto& npc   = *a.npc;
  const auto& trace = npc.bodyTrace();
  const bool present = inWorld(world,&npc);
  auto rule = world.bodyRule(npc,&hero);
  const bool sees = world.heroSeesBody(hero,npc);
  rule.heroSees = sees;
  std::string r = "{\"role\":"+jsonStr(a.role->name)+",\"npc\":"+jsonStr(symbolOf(world,npc))+
    ",\"present\":"+(present?"true":"false")+",\"dead\":"+(npc.isDead()?"true":"false")+
    ",\"id\":"+std::to_string(trace.id)+",\"trace_id\":"+jsonStr(world.bodyTraceId(npc))+
    ",\"died_at\":"+std::to_string(trace.diedAt)+",\"clock_from\":"+std::to_string(trace.clockFrom)+
    ",\"culprit_truth\":"+jsonStr(trace.culprit)+",\"position_cm\":"+vec(npc.position())+
    ",\"hero_cm\":"+num((npc.position()-hero.position()).length())+",\"hero_sees\":"+(sees?"true":"false")+
    ",\"mission_items\":"+(rule.missionItems?"true":"false")+",\"immortal\":"+(rule.immortal?"true":"false")+
    ",\"story_type\":"+(rule.storyType?"true":"false")+",\"npctype\":"+std::to_string(int(npc.handlePtr()->type))+
    ",\"rule_removable\":"+(BodyTraces::removable(rule)?"true":"false");
  if(a.fellKnown)
    r += ",\"moved_cm\":"+num((npc.position()-a.fell).length());
  return r+"}";
  }

std::string actorsJson(World& world, const Npc& hero) {
  std::string r = "[";
  for(auto& [name,a]:st.actors) {
    if(a.npc==nullptr)
      continue;
    if(r.size()>1)
      r += ",";
    r += actorJson(world,hero,a);
    }
  return r+"]";
  }

std::string common(World& world, const Npc& hero) {
  return "\"world\":"+jsonStr(world.name())+",\"body_traces\":"+(Gothic::physicsFlags().bodyTraces?"true":"false")+
         ",\"game_minute\":"+std::to_string(world.gameMinute())+",\"hero_cm\":"+vec(hero.position());
  }

void setup(World& world, Npc& hero, bool sleepBatch) {
  if(st.world!=&world) {
    st = {};
    st.world = &world;
    }
  const auto fwd   = forwardOf(hero);
  const auto right = Vec3(-fwd.z,0,fwd.x);
  const auto anchor = hero.position()+fwd*700.f;
  const int32_t ambient = scriptInt(world,"NPCTYPE_AMBIENT",0);
  const int32_t main    = scriptInt(world,"NPCTYPE_MAIN",BodyTraces::StoryTypes[0]);
  struct Place { const char* role; float side; float ahead; };
  const Place main_[]  = {{"victim",-100,0},{"wolf",100,0},{"mission",-350,150},{"immortal",-550,150},{"story",350,150}};
  const Place sleep_[] = {{"sleeper",-100,0},{"sleeper_mission",150,0}};
  const Place* places = sleepBatch ? sleep_ : main_;
  const size_t count  = sleepBatch ? std::size(sleep_) : std::size(main_);
  for(size_t i=0;i<count;++i) {
    const Role* role = nullptr;
    for(auto& r:Roles)
      if(std::string_view(r.name)==places[i].role)
        role = &r;
    auto* npc = spawn(world,role->instance,anchor+right*places[i].side+fwd*places[i].ahead);
    if(!role->monster)
      npc->handle().type = zenkit::NpcType(std::string_view(role->name)=="story" ? main : ambient);
    if(std::string_view(role->name)=="mission" || std::string_view(role->name)=="sleeper_mission")
      giveMissionItem(world,*npc);
    kill(*npc,hero);
    // IMMORTAL people cannot be killed by damage: the flag is set on the body itself.
    if(std::string_view(role->name)=="immortal")
      npc->handle().flags = npc->handle().flags | zenkit::NpcFlag::IMMORTAL;
    st.actors[role->name] = Actor{role,npc,{},false,sleepBatch};
    }
  st.sleepBatch = sleepBatch;
  if(!sleepBatch && st.observer==nullptr) {
    auto* obs = spawn(world,ObserverInstance,hero.position()-fwd*300.f);
    for(size_t i=0;i<PERC_Count;++i)
      obs->setPerceptionDisable(PercType(i)); // no ransacking of the fixture's bodies
    park(*obs,obs->position(),anchor);
    st.observer = obs;
    }
  }

Npc* findDead(World& world, Npc& hero, const char* instance) {
  const auto id = world.script().findSymbolIndex(instance);
  Npc*  best = nullptr;
  float dist = 0;
  for(uint32_t i=0;i<world.npcCount();++i) {
    auto* n = world.npcById(i);
    if(n==nullptr || n->instanceSymbol()!=id || !n->isDead())
      continue;
    const float d = (n->position()-hero.position()).length();
    if(best==nullptr || d<dist) {
      best = n;
      dist = d;
      }
    }
  return best;
  }

Npc* findObserver(World& world, Npc& hero, bool create) {
  if(st.observer!=nullptr && inWorld(world,st.observer) && !st.observer->isDead())
    return st.observer;
  const auto id = world.script().findSymbolIndex(ObserverInstance);
  for(uint32_t i=0;i<world.npcCount();++i)
    if(auto* n = world.npcById(i); n!=nullptr && n->instanceSymbol()==id && !n->isDead())
      st.observer = n;
  if(st.observer==nullptr && create)
    st.observer = spawn(world,ObserverInstance,hero.position()-forwardOf(hero)*300.f);
  if(st.observer==nullptr)
    fail("no living observer");
  for(size_t i=0;i<PERC_Count;++i)
    st.observer->setPerceptionDisable(PercType(i));
  return st.observer;
  }

// Re-find each role in a loaded world: the nearest dead NPC of its instance.
void rebind(World& world, Npc& hero) {
  st = {};
  st.world = &world;
  for(auto& r:Roles)
    if(auto* npc = findDead(world,hero,r.instance))
      st.actors[r.name] = Actor{&r,npc,npc->position(),true,std::string_view(r.name).starts_with("sleeper")};
  }

// Place the observer 5–12 m from the target body, facing it, with a native ray to a sight point.
void placeObserver(World& world, Npc& obs, const Npc& body) {
  const auto center = body.position();
  for(float d:{600.f,800.f,1000.f,1200.f,500.f}) {
    for(int i=0;i<72;++i) {
      const float a = float(i)*float(M_PI)/36.f;
      auto p = center+Vec3(std::cos(a)*d,0,std::sin(a)*d);
      if(!floorAt(world,p,300.f) || std::abs(p.y-center.y)>250.f)
        continue;
      park(obs,p,center);
      bool visible = false;
      for(const auto& s:world.bodySightPoints(body))
        visible |= obs.canRayHitPoint(s,false);
      if(visible)
        return;
      }
    }
  fail("no observer place seeing the body");
  }

std::string view(World& world, Npc& hero, const std::string& label) {
  if(!TestBench::npcView)
    fail("the AI bridge did not install TestBench::npcView (GOTHIC_AI=0?)");
  const bool on = Gothic::physicsFlags().bodyTraces;
  auto& obs = *findObserver(world,hero,label=="legacy");
  // The body this view is about: the victim, or the plain body of a legacy save.
  Actor* subject = nullptr;
  for(const char* r:{"victim","sleeper"})
    if(auto it = st.actors.find(r); it!=st.actors.end() && it->second.npc!=nullptr && inWorld(world,it->second.npc)) {
      subject = &it->second;
      break;
      }
  if(subject==nullptr)
    fail("no body to look at");
  placeObserver(world,obs,*subject->npc);
  const std::string raw = TestBench::npcView(world,symbolOf(world,obs));
  rapidjson::Document doc;
  doc.Parse(raw.c_str(),raw.size());
  if(doc.HasParseError() || !doc.IsArray() || doc.Size()!=1 || !doc[rapidjson::SizeType(0)].HasMember("traces") ||
     !doc[rapidjson::SizeType(0)]["traces"].IsArray())
    fail("npc_view is not one view with traces: "+raw.substr(0,200));
  const auto& traces = doc[rapidjson::SizeType(0)]["traces"];
  const std::set<std::string> allowed = {"id","kind","name","npc","waypoint","distance","age","hero_cm","from_hero"};
  const auto& body = *subject->npc;
  const auto  expectedId = world.bodyTraceId(body);
  const auto  now = world.gameMinute();
  const rapidjson::Value* row = nullptr;
  size_t bodies = 0;
  std::string wolfId;
  if(auto it = st.actors.find("wolf"); it!=st.actors.end() && it->second.npc!=nullptr)
    wolfId = world.bodyTraceId(*it->second.npc);
  for(const auto& t:traces.GetArray()) {
    for(auto m=t.MemberBegin();m!=t.MemberEnd();++m)
      if(!allowed.contains(m->name.GetString()))
        fail(std::string("private or unknown trace field ")+m->name.GetString());
    const std::string kind = t["kind"].GetString();
    if(kind!="body") {
      if(t.HasMember("npc"))
        fail("a non-body trace carries npc");
      continue;
      }
    ++bodies;
    const std::string id = t["id"].GetString();
    if(id==wolfId)
      fail("the wolf's body became a trace");
    if(id==expectedId)
      row = &t;
    }
  int64_t age = -2; // -1: null
  if(on) {
    if(row==nullptr)
      fail("ON: no body trace "+expectedId+" in the observer's npc_view: "+raw.substr(0,400));
    const auto& t = *row;
    if(!t.HasMember("npc") || std::string(t["npc"].GetString())!=symbolOf(world,body))
      fail("body trace npc is not the dead one's symbol");
    if(std::string(t["name"].GetString())!="тело")
      fail("body trace name");
    if(!t.HasMember("distance") || t["distance"].GetDouble()>1500.)
      fail("body trace beyond 15 m");
    if(!t.HasMember("age"))
      fail("body trace without age");
    age = t["age"].IsNull() ? -1 : t["age"].GetInt64();
    const auto diedAt = body.bodyTrace().diedAt;
    if(diedAt<0) {
      if(age!=-1)
        fail("a body of an older save must have age null");
      }
    else {
      if(age<0 || std::llabs(age-std::max<int64_t>(0,now-diedAt))>1)
        fail("body trace age "+std::to_string(age)+" differs from the clock: "+std::to_string(now-diedAt));
      if(label=="first" && age>2)
        fail("a fresh body is older than 2 minutes");
      if(label=="first")
        st.firstAge = age;
      if(label=="aged" && (st.firstAge<0 || age-st.firstAge<16 || age-st.firstAge>18))
        fail("age did not grow by 17 game minutes: "+std::to_string(st.firstAge)+" -> "+std::to_string(age));
      }
    }
  else if(bodies>0) {
    fail("OFF: npc_view has body traces");
    }
  // Engine truth beside the wire: the culprit never reaches npc_view.
  std::string truth = "[";
  for(const auto& t:world.physicsTraces()) {
    if(t.kind!="body")
      continue;
    if(truth.size()>1)
      truth += ",";
    truth += "{\"id\":"+jsonStr(t.id)+",\"npc\":"+jsonStr(t.npc)+",\"appeared_at\":"+std::to_string(t.appearedAt)+
             ",\"actor_truth\":"+jsonStr(t.culprit)+",\"distance_cm\":"+num((t.position-obs.position()).length())+"}";
    if(t.id==wolfId)
      fail("physicsTraces: the wolf's body became a trace");
    }
  if(!on && truth.size()>1)
    fail("OFF: physicsTraces has body traces");
  return "{"+common(world,hero)+",\"label\":"+jsonStr(label)+",\"observer\":"+jsonStr(symbolOf(world,obs))+
         ",\"observer_cm\":"+vec(obs.position())+",\"expected_id\":"+jsonStr(expectedId)+
         ",\"age\":"+(age==-1 ? std::string("null") : std::to_string(age))+",\"body_rows\":"+std::to_string(bodies)+
         ",\"truth\":"+truth+"],\"view\":"+raw+",\"pass\":true}";
  }

void advance(World& world, int64_t minutes) {
  auto* game = Gothic::inst().gameSession();
  if(game==nullptr || minutes<=0)
    fail("advance needs a session and positive minutes");
  auto t = game->time();
  t.addMilis(uint64_t(minutes)*60ull*1000ull);
  game->setTime(t); // the clock alone: no World::setDayTime, no routine reset
  (void)world;
  }

void sleepHours(World& world, int64_t hours) {
  // Sleep as the bed does it (Wld_SetTime → World::setDayTime → resetPositionToTA),
  // in steps below a day: setDayTime only goes up to 24 hours ahead.
  int64_t left = hours*60;
  while(left>0) {
    const int64_t step = std::min<int64_t>(left,23*60);
    const int64_t target = world.gameMinute()+step;
    world.setDayTime(int32_t((target/60)%24),int32_t(target%60));
    left -= step;
    }
  }

void moveHero(World& world, Npc& hero, const std::string& where) {
  std::vector<const Npc*> bodies;
  Vec3 center = {};
  for(auto& [name,a]:st.actors)
    if(a.npc!=nullptr && a.sleepBatch==st.sleepBatch && inWorld(world,a.npc)) {
      bodies.push_back(a.npc);
      center += a.npc->position();
      }
  if(bodies.empty()) {
    if(!Gothic::physicsFlags().bodyTraces)
      return; // OFF: vanilla clock leaps may have retired them all
    fail("no bodies to stand by");
    }
  center /= float(bodies.size());
  std::vector<const Npc*> watched; // the bodies the rule would remove: they decide visibility
  for(const char* r:{"victim","wolf","sleeper"})
    if(auto it = st.actors.find(r); it!=st.actors.end() && it->second.npc!=nullptr &&
       it->second.sleepBatch==st.sleepBatch && inWorld(world,it->second.npc))
      watched.push_back(it->second.npc);
  const bool near = where=="near";
  const bool visible = where=="far-visible";
  if(!near && !visible && where!="far-hidden")
    fail("hero near|far-visible|far-hidden");
  std::vector<float> distances;
  if(near)
    distances = {800,1000,1200,600};
  else
    for(float d=3700;d<=9000;d+=400)
      distances.push_back(d);
  for(float d:distances) {
    for(int i=0;i<72;++i) {
      const float a = float(i)*float(M_PI)/36.f;
      auto p = center+Vec3(std::cos(a)*d,0,std::sin(a)*d);
      if(!floorAt(world,p))
        continue;
      const auto eye = p+Vec3(0,170,0);
      bool ok = true;
      for(const auto* b:bodies) {
        const float cm = (b->position()-p).length();
        ok &= near ? cm<2500.f : cm>3300.f;
        }
      for(const auto* b:watched) {
        if(near)
          continue;
        const bool sees = raySees(world,eye,*b);
        ok &= visible ? sees : !sees;
        }
      if(!ok)
        continue;
      hero.clearAiQueue();
      hero.setPosition(p);
      hero.setDirection(center-p);
      hero.updateTransform();
      return;
      }
    }
  fail("no supported hero place "+where);
  }

std::string check(World& world, Npc& hero, const std::string& label) {
  const bool on = Gothic::physicsFlags().bodyTraces;
  const auto has = [&](const char* r) { auto it = st.actors.find(r); return it!=st.actors.end() && it->second.npc!=nullptr; };
  const auto present = [&](const char* r) { return inWorld(world,actor(r).npc); };
  const auto moved = [&](const char* r) {
    auto& a = actor(r);
    return a.fellKnown ? (a.npc->position()-a.fell).length() : 0.f;
    };
  const auto aged = [&](const char* r) {
    return world.gameMinute()-actor(r).npc->bodyTrace().clockFrom>=BodyTraces::LifetimeMinutes;
    };
  const auto require = [&](bool ok, const std::string& why) { if(!ok) fail(label+": "+why); };
  // The first batch: bodies the rule may remove, and bodies it must keep. A legacy save has only the first two.
  std::vector<const char*> removable, kept;
  for(const char* r:{"victim","wolf"})
    if(has(r))
      removable.push_back(r);
  for(const char* r:{"mission","immortal","story"})
    if(has(r))
      kept.push_back(r);
  std::string vanilla;
  if(label=="stay-near" || label=="stay-visible" || label=="gone-hidden") {
    require(has("victim"),"no person's body in the fixture");
    }
  if(label=="stay-near") {
    for(auto* list:{&removable,&kept})
      for(const char* r:*list)
        require(present(r),std::string(r)+" left with the hero near");
    if(on)
      for(const char* r:removable) {
        require(aged(r),std::string(r)+" is not three days old yet");
        require((actor(r).npc->position()-hero.position()).length()<BodyTraces::HeroAwayCm,std::string(r)+": hero not within 30 m");
        }
    }
  else if(label=="stay-visible") {
    for(auto* list:{&removable,&kept})
      for(const char* r:*list)
        require(present(r),std::string(r)+" left while the hero sees it");
    if(on)
      for(const char* r:removable) {
        auto& n = *actor(r).npc;
        require(aged(r),std::string(r)+" is not three days old");
        require((n.position()-hero.position()).length()>BodyTraces::HeroAwayCm,std::string(r)+": hero within 30 m");
        require(world.heroSeesBody(hero,n),std::string(r)+": the hero does not see it");
        }
    }
  else if(label=="gone-hidden") {
    for(const char* r:removable)
      require(present(r)!=on,std::string(on ? "" : "OFF: ")+r+(on ? " stayed though unseen and three days old" : " left"));
    for(const char* r:kept)
      require(present(r),std::string(r)+" body left");
    }
  else if(label=="sleep-near") {
    if(on) {
      for(const char* r:{"sleeper","sleeper_mission"}) {
        require(present(r),std::string(r)+" left after an hour's sleep");
        require(moved(r)<=10.f,std::string(r)+" moved "+num(moved(r))+" cm in the sleep");
        }
      }
    else {
      // Vanilla OpenGothic: the clock leap retires bodies without mission items.
      require(!present("sleeper"),"OFF: the plain body survived the sleep (vanilla retires it)");
      vanilla = ",\"vanilla_mission_present\":"+std::string(present("sleeper_mission")?"true":"false")+
                ",\"vanilla_mission_moved_cm\":"+num(moved("sleeper_mission"));
      }
    }
  else if(label=="sleep-far") {
    if(on) {
      require(!present("sleeper"),"the plain body stayed after three days' sleep far away");
      require(present("sleeper_mission"),"the mission body left");
      require(moved("sleeper_mission")<=10.f,"the mission body moved "+num(moved("sleeper_mission"))+" cm (vanilla: to its routine point)");
      }
    }
  else if(label=="legacy") {
    auto& a = actor("victim");
    const auto& t = a.npc->bodyTrace();
    require(t.id!=0,"an old body got no identity");
    require(t.diedAt==-1,"an old body got a death time");
    require(t.clockFrom>=0 && st.loadMinute>=0 && std::llabs(t.clockFrom-st.loadMinute)<=2,
            "the lifetime of an old body does not start at loading");
    }
  else {
    fail("unknown check "+label);
    }
  return "{"+common(world,hero)+",\"label\":"+jsonStr(label)+vanilla+",\"actors\":"+actorsJson(world,hero)+",\"pass\":true}";
  }

void markFell() {
  for(auto& [name,a]:st.actors)
    if(a.npc!=nullptr) {
      a.fell = a.npc->position();
      a.fellKnown = true;
      }
  }

std::string save(World& world, Npc& hero, const std::string& label, const std::string& out) {
  if(label.empty() || label.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos)
    fail("invalid save label");
  std::vector<uint8_t> bytes;
  {
  MemWriter writer(bytes);
  Serialize fout(writer);
  Pixmap preview(1,1,TextureFormat::RGBA8);
  std::memset(preview.data(),0,4);
  Gothic::inst().gameSession()->save(fout,"body traces "+label,preview);
  }
  std::ofstream file(out+"/session-"+label+".sav",std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!file)
    fail("cannot write the session archive");
  return "{"+common(world,hero)+",\"archive\":"+jsonStr("session-"+label+".sav")+",\"bytes\":"+std::to_string(bytes.size())+
         ",\"actors\":"+actorsJson(world,hero)+",\"pass\":true}";
  }

}

void BodyTracesBench::reset() {
  st = {};
  TestBench::allowBodySweep(false);
  }

std::string BodyTracesBench::command(const std::vector<std::string>& a, World& world, Npc& player,
                                     const std::string& out) {
  if(a.size()<2)
    fail("body_traces <command>");
  TestBench::allowBodySweep(true); // this fixture is about the body lifetime
  if(st.world!=&world && a[1]!="setup") {
    // A fresh process after loading a save: find the fixture's bodies again.
    rebind(world,player);
    st.loadMinute = world.gameMinute();
    }
  const auto& c = a[1];
  if(c=="setup") {
    setup(world,player,a.size()>2 && a[2]=="sleep");
    return "{"+common(world,player)+",\"batch\":"+jsonStr(a.size()>2 ? a[2] : "main")+
           ",\"actors\":"+actorsJson(world,player)+",\"pass\":true}";
    }
  if(c=="view")
    return view(world,player,a.size()>2 ? a[2] : "view");
  if(c=="advance" && a.size()==3) {
    const auto before = world.gameMinute();
    advance(world,std::stoll(a[2]));
    return "{"+common(world,player)+",\"from_minute\":"+std::to_string(before)+",\"pass\":true}";
    }
  if(c=="sleep" && a.size()==3) {
    markFell();
    const auto before = world.gameMinute();
    sleepHours(world,std::stoll(a[2]));
    return "{"+common(world,player)+",\"from_minute\":"+std::to_string(before)+",\"pass\":true}";
    }
  if(c=="hero" && a.size()==3) {
    moveHero(world,player,a[2]);
    return "{"+common(world,player)+",\"where\":"+jsonStr(a[2])+",\"actors\":"+actorsJson(world,player)+",\"pass\":true}";
    }
  if(c=="check" && a.size()==3)
    return check(world,player,a[2]);
  if(c=="save" && a.size()==3)
    return save(world,player,a[2],out);
  if(c=="sample")
    return "{"+common(world,player)+",\"load_minute\":"+std::to_string(st.loadMinute)+
           ",\"actors\":"+actorsJson(world,player)+",\"pass\":true}";
  fail("unknown command "+c);
  }
