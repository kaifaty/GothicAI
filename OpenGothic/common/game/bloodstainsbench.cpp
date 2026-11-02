#include "bloodstainsbench.h"

#include <Tempest/MemWriter>
#include <Tempest/Pixmap>

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

#include "game/constants.h"
#include "game/gamescript.h"
#include "game/gamesession.h"
#include "game/serialize.h"
#include "physics/dynamicworld.h"
#include "world/bloodstains.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"

using namespace Tempest;

namespace {

struct State {
  World*                          world  = nullptr;
  Npc*                            victim = nullptr;
  std::map<std::string,uint32_t>  ids;   // role -> stain id (this process only)
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
  std::snprintf(buf,sizeof(buf),"%.2f",double(v));
  return buf;
  }

std::string vec(const Vec3& v) {
  return "["+num(v.x)+","+num(v.y)+","+num(v.z)+"]";
  }

[[noreturn]] void fail(const std::string& why) {
  throw std::runtime_error("blood stains: "+why);
  }

const char* kindName(BloodStains::Kind k) {
  switch(k) {
    case BloodStains::Kind::Splash: return "splash";
    case BloodStains::Kind::Pool:   return "pool";
    case BloodStains::Kind::Smear:  return "smear";
    }
  return "?";
  }

const char* bloodName(BloodStains::Blood b) {
  switch(b) {
    case BloodStains::Blood::Red:    return "red";
    case BloodStains::Blood::Yellow: return "yellow";
    case BloodStains::Blood::Gore:   return "gore";
    case BloodStains::Blood::Stone:  return "stone";
    }
  return "?";
  }

int64_t nowMs(World& world) {
  return world.time().toInt();
  }

const BloodStains::Stain* stainById(World& world, uint32_t id) {
  for(const auto& s:world.bloodStains())
    if(s.id==id)
      return &s;
  return nullptr;
  }

std::vector<World::PhysicsTrace> bloodTraces(World& world) {
  std::vector<World::PhysicsTrace> out;
  for(auto& t:world.physicsTraces())
    if(t.kind=="blood")
      out.push_back(std::move(t));
  return out;
  }

std::string common(World& world) {
  return "\"world\":"+jsonStr(world.name())+",\"blood\":"+(Gothic::physicsFlags().blood ? "true" : "false")+
         ",\"game_minute\":"+std::to_string(world.gameMinute());
  }

std::string stainsJson(World& world) {
  const auto now = nowMs(world);
  std::string r = "[";
  for(const auto& s:world.bloodStains()) {
    if(r.size()>1)
      r += ",";
    std::string role;
    for(auto& [name,id]:st.ids)
      if(id==s.id)
        role = name;
    r += "{\"id\":"+std::to_string(s.id)+",\"role\":"+jsonStr(role)+",\"kind\":"+jsonStr(kindName(s.kind))+
         ",\"blood\":"+jsonStr(bloodName(s.blood))+",\"indoor\":"+(s.indoor ? "true" : "false")+
         ",\"position_cm\":"+vec(s.pos)+",\"normal\":"+vec(s.normal)+",\"radius_cm\":"+num(s.radiusCm)+
         ",\"born_at_ms\":"+std::to_string(s.bornAt)+",\"age_min\":"+std::to_string((now-s.bornAt)/BloodStains::MinuteMs)+
         ",\"body_id\":"+std::to_string(s.bodyId)+",\"evidence\":"+(BloodStains::isEvidence(s) ? "true" : "false")+"}";
    }
  return r+"]";
  }

std::string tracesJson(World& world) {
  const auto minute = world.gameMinute();
  std::string r = "[";
  for(const auto& t:bloodTraces(world)) {
    if(r.size()>1)
      r += ",";
    r += "{\"id\":"+jsonStr(t.id)+",\"name\":"+jsonStr(t.name)+",\"position_cm\":"+vec(t.position)+
         ",\"age\":"+std::to_string(minute-t.appearedAt)+",\"culprit_truth\":"+jsonStr(t.culprit)+"}";
    }
  return r+"]";
  }

std::string sample(World& world) {
  return common(world)+",\"stains\":"+stainsJson(world)+",\"traces\":"+tracesJson(world);
  }

// Gentle ground near `at`: the first spot of a small spiral with a floor below (the valley has
// cliffs). The stain lands there as placeBloodStain lays it.
Vec3 ground(World& world, Vec3 at, float maxR = 200.f) {
  for(float r:{0.f,15.f,30.f,60.f,120.f,200.f}) {
    if(r>maxR)
      break;
    for(int i=0;i<(r==0 ? 1 : 12);++i) {
      const float a = float(i)*float(M_PI)/6.f;
      const auto  p = at+Vec3(std::cos(a)*r,0,std::sin(a)*r);
      const auto floor = world.physic()->landRay(p+Vec3(0,600,0),3000.f);
      if(floor.hasCol && floor.n.y>=.5f)
        return floor.v;
      }
    }
  fail("no ground near "+vec(at));
  }

void place(World& world, const std::string& role, const Vec3& at, BloodStains::Kind kind, float radius, uint32_t body = 0) {
  const auto id = world.placeBloodStain(ground(world,at,30.f),kind,radius,body);
  if(Gothic::physicsFlags().blood && id==0)
    fail("placeBloodStain failed for "+role);
  if(!Gothic::physicsFlags().blood && id!=0)
    fail("blood OFF: placeBloodStain added "+role);
  if(id!=0)
    st.ids[role] = id;
  }

void add(World& world, const std::string& role, BloodStains::Stain s) {
  const auto id = world.addBloodStain(s);
  if(Gothic::physicsFlags().blood!=(id!=0))
    fail(std::string("addBloodStain ")+(id==0 ? "refused " : "added with blood OFF: ")+role);
  if(id!=0)
    st.ids[role] = id;
  }

// Gentle open ground around the hero (the valley has cliffs and ravines): a ring of probes,
// then anchors at least 6 m from one another, so the groups stay apart.
std::vector<Vec3> anchors(World& world, const Vec3& h, size_t count) {
  std::vector<Vec3> found;
  for(float dist:{300.f,500.f,700.f,900.f,1100.f,1300.f}) {
    for(int i=0;i<24;++i) {
      const float a = float(i)*float(M_PI)/12.f;
      const auto  p = h+Vec3(std::cos(a)*dist,0,std::sin(a)*dist);
      const auto floor = world.physic()->landRay(p+Vec3(0,800,0),2400.f);
      if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-h.y)>400.f || world.isIndoor(floor.v+Vec3(0,50,0)))
        continue; // outdoors only: placeBloodStain records World::isIndoor, and the lifetime follows it
      bool apart = true;
      for(const auto& q:found)
        apart &= (q-floor.v).length()>=600.f;
      if(apart)
        found.push_back(floor.v);
      if(found.size()==count)
        return found;
      }
    }
  fail("only "+std::to_string(found.size())+" of "+std::to_string(count)+" ground anchors near the hero");
  }

// A spot `dist` cm from `at` on gentle ground, the first of 12 directions that has one.
Vec3 beside(World& world, const Vec3& at, float dist) {
  for(int i=0;i<12;++i) {
    const float a = float(i)*float(M_PI)/6.f;
    const auto  p = at+Vec3(std::cos(a)*dist,0,std::sin(a)*dist);
    const auto floor = world.physic()->landRay(p+Vec3(0,200,0),600.f);
    if(floor.hasCol && floor.n.y>=.5f && std::abs(floor.v.y-at.y)<100.f && !world.isIndoor(floor.v+Vec3(0,50,0)))
      return floor.v;
    }
  fail("no ground "+num(dist)+" cm beside "+vec(at));
  }

void setup(World& world, Npc& hero) {
  st = {};
  st.world = &world;
  const auto spots = anchors(world,hero.position(),5);

  // A person dies at the first anchor: his pool and a splash beside it are his body's evidence.
  const auto instance = world.script().findSymbolIndex("BDT_1000_BANDIT_L");
  if(instance==size_t(-1))
    fail("no instance BDT_1000_BANDIT_L");
  st.victim = world.addNpc(instance,spots[0]);
  if(st.victim==nullptr)
    fail("cannot insert the victim");
  st.victim->setLastHitBy(&hero);
  st.victim->changeAttribute(ATR_HITPOINTS,-st.victim->attribute(ATR_HITPOINTSMAX)-1,false);
  if(!st.victim->isDead() || st.victim->bodyTrace().id==0)
    fail("the victim did not die with a body trace");
  const auto body = st.victim->bodyTrace().id;
  const auto at   = st.victim->position();
  place(world,"pool",at,BloodStains::Kind::Pool,45.f,body);
  place(world,"near_pool",beside(world,at,80.f),BloodStains::Kind::Splash,15.f);

  // Two splashes 1.2 m apart (one trace, named by the older, placed at the larger) and one far away.
  place(world,"a1",spots[1],BloodStains::Kind::Splash,15.f);
  place(world,"a2",beside(world,spots[1],120.f),BloodStains::Kind::Splash,25.f);
  place(world,"b", spots[2],BloodStains::Kind::Splash,20.f);

  // A meatbug's yellow blood is evidence; a golem's stone chips 1 m from it are not, and do not join it.
  BloodStains::Stain yellow;
  yellow.pos      = spots[3];
  yellow.blood    = BloodStains::Blood::Yellow;
  yellow.radiusCm = 12.f;
  add(world,"yellow",yellow);
  BloodStains::Stain stone = yellow;
  stone.pos       = beside(world,yellow.pos,100.f);
  stone.blood     = BloodStains::Blood::Stone;
  add(world,"stone",stone);

  // A splash recorded indoors (the lifetime is the record's): three days, not one.
  BloodStains::Stain indoor;
  indoor.pos      = spots[4];
  indoor.indoor   = true;
  indoor.radiusCm = 18.f;
  add(world,"in",indoor);

  // The layout the checks rely on: pairs within 2 m, groups apart.
  if(Gothic::physicsFlags().blood) {
    const auto pos = [&](const char* role) { return stainById(world,st.ids.at(role))->pos; };
    const auto near = [&](const char* x, const char* y) { return (pos(x)-pos(y)).length(); };
    if(near("a1","a2")>BloodStains::GroupLinkCm || near("pool","near_pool")>BloodStains::GroupLinkCm)
      fail("setup: a pair is farther than 2 m apart");
    const char* groups[] = {"a1","b","pool","yellow","in"};
    for(auto* x:groups)
      for(auto* y:groups)
        if(x!=y && near(x,y)<BloodStains::GroupLinkCm*2.f)
          fail(std::string("setup: ")+x+" and "+y+" are closer than 4 m");
    }
  }

void advance(World& world, int64_t minutes) {
  auto* game = Gothic::inst().gameSession();
  if(game==nullptr || minutes<=0)
    fail("advance needs a session and positive minutes");
  auto t = game->time();
  t.addMilis(uint64_t(minutes)*60ull*1000ull);
  game->setTime(t); // the clock alone: the world's minute sweep does the rest
  (void)world;
  }

std::string check(World& world, const std::string& label) {
  const auto require = [&](bool ok, const std::string& why) { if(!ok) fail(label+": "+why); };
  const auto traces  = bloodTraces(world);
  const bool on      = Gothic::physicsFlags().blood;
  if(!on) {
    require(world.bloodStains().empty(),"blood OFF keeps stains");
    require(traces.empty(),"blood OFF has blood traces");
    return "{"+common(world)+",\"label\":"+jsonStr(label)+",\"pass\":true}";
    }
  const auto has   = [&](const char* role) { auto it=st.ids.find(role); return it!=st.ids.end() && stainById(world,it->second)!=nullptr; };
  const auto trace = [&](const char* role) -> const World::PhysicsTrace* {
    auto it = st.ids.find(role);
    if(it==st.ids.end())
      return nullptr;
    const auto id = world.bloodTraceId(it->second);
    for(const auto& t:traces)
      if(t.id==id)
        return &t;
    return nullptr;
    };
  const auto ageOf = [&](const char* role) {
    const auto* s = stainById(world,st.ids.at(role));
    return world.gameMinute()-s->bornAt/BloodStains::MinuteMs;
    };
  std::vector<const char*> expectStains, expectTraces;
  if(label=="fresh" || label=="aged") {
    expectStains = {"pool","near_pool","a1","a2","b","yellow","stone","in"};
    expectTraces = {"a1","b","yellow","in"};
    }
  else if(label=="body-gone") {
    expectStains = {"near_pool","a1","a2","b","yellow","stone","in"};
    expectTraces = {"near_pool","a1","b","yellow","in"};
    }
  else if(label=="day") {
    expectStains = {"in"};
    expectTraces = {"in"};
    }
  else if(label=="gone") {
    }
  else {
    fail("unknown check "+label);
    }
  require(world.bloodStains().size()==expectStains.size(),
          std::to_string(world.bloodStains().size())+" stains, expected "+std::to_string(expectStains.size()));
  for(auto* role:expectStains)
    require(has(role),std::string("stain ")+role+" is missing");
  require(traces.size()==expectTraces.size(),
          std::to_string(traces.size())+" blood traces, expected "+std::to_string(expectTraces.size()));
  for(auto* role:expectTraces) {
    const auto* t = trace(role);
    require(t!=nullptr,std::string("no blood trace named by ")+role);
    require(t->culprit.empty(),"a blood trace carries a culprit");
    }
  if(label=="fresh" || label=="aged") {
    // The pair: placed at the larger stain, aged by the fresher one.
    const auto* pair = trace("a1");
    require((pair->position-stainById(world,st.ids.at("a2"))->pos).length()<0.5f,"the pair is not placed at its larger stain");
    require(std::abs((world.gameMinute()-pair->appearedAt)-ageOf("a2"))<=1,"the pair's age is not its freshest stain's");
    require(trace("pool")==nullptr && trace("near_pool")==nullptr,"the body's blood became a blood trace");
    require(trace("stone")==nullptr,"stone chips became a blood trace");
    }
  if(label=="aged")
    require(ageOf("a2")>=17,"the clock did not move 17 minutes");
  if(label=="body-gone")
    require(!has("pool"),"the pool stayed after its body left");
  return "{"+common(world)+",\"label\":"+jsonStr(label)+","+"\"stains\":"+stainsJson(world)+
         ",\"traces\":"+tracesJson(world)+",\"pass\":true}";
  }

std::string save(World& world, const std::string& label, const std::string& out) {
  if(label.empty() || label.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos)
    fail("invalid save label");
  std::vector<uint8_t> bytes;
  {
  MemWriter writer(bytes);
  Serialize fout(writer);
  Pixmap preview(1,1,TextureFormat::RGBA8);
  std::memset(preview.data(),0,4);
  Gothic::inst().gameSession()->save(fout,"blood stains "+label,preview);
  }
  std::ofstream file(out+"/session-"+label+".sav",std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!file)
    fail("cannot write the session archive");
  return "{"+sample(world)+",\"archive\":"+jsonStr("session-"+label+".sav")+",\"bytes\":"+std::to_string(bytes.size())+
         ",\"pass\":true}";
  }

}

void BloodStainsBench::reset() {
  st = {};
  }

std::string BloodStainsBench::command(const std::vector<std::string>& a, World& world, Npc& player,
                                      const std::string& out) {
  if(a.size()<2)
    fail("blood_stains <command>");
  const auto& c = a[1];
  if(c=="setup") {
    setup(world,player);
    return "{"+sample(world)+",\"pass\":true}";
    }
  if(st.world!=&world)
    st = State{&world,nullptr,{}}; // a fresh process after loading a save: the stains carry their own ids
  if(c=="advance" && a.size()==3) {
    const auto before = world.gameMinute();
    advance(world,std::stoll(a[2]));
    return "{"+common(world)+",\"from_minute\":"+std::to_string(before)+",\"pass\":true}";
    }
  if(c=="body" && a.size()==3 && a[2]=="gone") {
    if(st.victim==nullptr)
      fail("no victim (setup first)");
    world.removeNpc(*st.victim); // as World::sweepBodies takes a body three days old
    st.victim = nullptr;
    return "{"+common(world)+",\"pass\":true}";
    }
  if(c=="check" && a.size()==3)
    return check(world,a[2]);
  if(c=="save" && a.size()==3)
    return save(world,a[2],out);
  if(c=="sample")
    return "{"+sample(world)+",\"pass\":true}";
  if(c=="fill" && (a.size()==3 || a.size()==4)) {
    // fill <count> [spacing cm]: the cost bench's load, a square grid round the hero, pools and
    // splashes in turn (blood off: none, the same scene)
    const int   count = std::max(1,std::stoi(a[2]));
    const float step  = a.size()==4 ? std::stof(a[3]) : 125.f;
    const int   side  = int(std::ceil(std::sqrt(float(count))));
    const auto  at    = player.position();
    int laid = 0;
    for(int i=0; i<count; ++i) {
      const float x = (float(i%side) - float(side-1)*0.5f)*step;
      const float z = (float(i/side) - float(side-1)*0.5f)*step;
      const bool  pool = (i%2)==0;
      if(world.placeBloodStain(at+Tempest::Vec3(x,150,z),pool ? BloodStains::Kind::Pool : BloodStains::Kind::Splash,
                               pool ? 40.f : 20.f)!=0)
        ++laid;
      }
    return "{"+common(world)+",\"laid\":"+std::to_string(laid)+",\"pass\":true}";
    }
  fail("unknown command "+c);
  }
