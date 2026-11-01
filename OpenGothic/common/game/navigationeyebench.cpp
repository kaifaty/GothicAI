#include "navigationeyebench.h"
#include "navigationeye.h"

#include "game/gamescript.h"
#include "physics/dynamicworld.h"
#include "world/world.h"
#include "world/objects/npc.h"
#include "gothic.h"
#include "camera.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using Tempest::Vec3;
using Clock = std::chrono::steady_clock;
constexpr float Pi = 3.14159265358979323846f;

std::string jsonQuote(const std::string& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.String(value.data(),rapidjson::SizeType(value.size()));
  return {buffer.GetString(),buffer.GetSize()};
}

std::string vector(const Vec3& value) {
  std::ostringstream out;
  out << std::setprecision(9) << '[' << value.x << ',' << value.y << ',' << value.z << ']';
  return out.str();
}

uint64_t cpuNs() {
#ifdef _WIN32
  FILETIME created{},exited{},kernel{},user{};
  if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) return 0;
  const auto ticks=[](const FILETIME& time) { return (uint64_t(time.dwHighDateTime)<<32)|uint64_t(time.dwLowDateTime); };
  return (ticks(kernel)+ticks(user))*100;
#else
  timespec now{};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&now);
  return uint64_t(now.tv_sec)*1000000000ULL+uint64_t(now.tv_nsec);
#endif
}

struct Cost {
  uint64_t time=0;
  size_t actor=0;
  double microseconds=0;
  size_t casts=0,nearCasts=0,farCasts=0;
  bool farUpdated=false;
};

struct State {
  std::string directory,scene,anchor,tag;
  std::vector<Npc*> actors;
  std::vector<std::unique_ptr<NavigationEye>> eyes;
  NavigationEye::Config config;
  bool active=false,enabled=false,measuring=false;
  uint64_t warmupUntil=0,startSim=0,steps=0,startCpu=0;
  Clock::time_point startWall{};
  std::vector<Cost> costs;
  Vec3 anchorPosition,anchorDirection;
} state;

void append(const char* name,const std::string& json) {
  std::ofstream out(state.directory+"/"+name,std::ios::app);
  if(!out) throw std::runtime_error(std::string("cannot open eye journal ")+name);
  out << json << '\n';
  if(!out) throw std::runtime_error(std::string("cannot write eye journal ")+name);
}

bool floorAt(World& world,const Vec3& point,Vec3& floor) {
  const auto hit=world.physic()->ray(point+Vec3(0,220,0),point-Vec3(0,450,0));
  if(!hit.hasCol || (hit.n.quadLength()>.01f && hit.n.y<.65f)) return false;
  floor=hit.v;
  // A supported root alone could be inside a house wall or under a low prop.
  const auto clearance=world.physic()->ray(floor+Vec3(0,25,0),floor+Vec3(0,180,0));
  return !clearance.hasCol;
}

void freeze(Npc& npc) {
  npc.setInteraction(nullptr,true);
  npc.clearAiQueue();
  npc.clearState(false);
  npc.clearGoTo();
  npc.setExternalControl(true);
}

void makeEyes() {
  state.eyes.clear();
  for(size_t i=0;i<state.actors.size();++i)
    state.eyes.push_back(std::make_unique<NavigationEye>(state.config));
}

std::string setup(World& world,Npc& player,const std::string& scene,size_t count) {
  if(state.active) throw std::runtime_error("eye end before changing fixture");
  const bool fence=scene=="lobart-fence";
  if(scene!="market" && scene!="lobart" && !fence) throw std::runtime_error("eye scene must be market, lobart or lobart-fence");
  if(count!=1 && count!=10 && count!=20 && count!=50)
    throw std::runtime_error("eye actors must be 1, 10, 20 or 50");
  Gothic::settingsSetI("GAME","living_bodies",1);
  for(auto* actor:state.actors) world.removeNpc(*actor);
  state.actors.clear();state.eyes.clear();
  state.scene=fence?"lobart":scene;
  state.anchor=scene=="market" ? "NW_CITY_MERCHANT_PATH_01" : fence?"NW_FARM1_ENTRANCE_03":"NW_FARM1_PATH_01";
  const auto* point=world.findWayPoint(state.anchor);
  if(!point) throw std::runtime_error("eye fixture stock waypoint unavailable: "+state.anchor);
  Vec3 origin;
  // Outside the fence where Maleth stopped at (14136,1790,-15993), 2.5 m back
  // toward the barn. PATH_01 itself is already inside the pasture.
  const Vec3 standing=fence?Vec3(14385,1790,-15968):point->pos;
  if(!floorAt(world,standing,origin)) throw std::runtime_error("eye anchor has no supported standing floor");
  Vec3 forward=point->dir;
  forward.y=0;
  if(forward.quadLength()<.01f) forward=Vec3(1,0,0);
  forward*=1.f/forward.length();
  // Looking from the authored path toward the sheep's stock pasture point
  // crosses the actual pasture fence rather than aiming at a made-up prop.
  size_t sheep=0;
  Vec3 pasture;
  for(uint32_t i=0;i<world.npcCount();++i)
    if(auto* npc=world.npcById(i);npc && npc->guild()==GIL_SHEEP &&
       (npc->position()-origin).quadLength()<6000.f*6000.f) {
      pasture+=npc->position();++sheep;
    }
  const auto* pasturePoint=world.findPoint("FP_ROAM_FARM1_SHEEP_09",false);
  if((scene=="lobart" || fence) && (pasturePoint || sheep)) {
    forward=(pasturePoint?pasturePoint->pos:pasture*(1.f/float(sheep)))-origin;forward.y=0;
    if(forward.quadLength()>.01f) forward*=1.f/forward.length();
  }
  const Vec3 side(-forward.z,0,forward.x);
  const auto instance=world.script().findSymbolIndex("BDT_1003_BANDIT_M");
  if(instance==size_t(-1)) throw std::runtime_error("eye native human instance unavailable");
  // Freeze native residents too: baseline and enabled spans run the same
  // colliders, without routine movement changing visibility between spans.
  size_t residents=0;
  for(uint32_t i=0;i<world.npcCount();++i)
    if(auto* npc=world.npcById(i);npc && npc!=&player &&
       (npc->position()-origin).quadLength()<6000.f*6000.f) {
      freeze(*npc);++residents;
    }
  freeze(player);
  Vec3 playerFloor;
  bool playerPlaced=floorAt(world,origin-forward*180.f+side*130.f,playerFloor);
  size_t heroAttempts=1;
  // A blocked preferred offset must never leave the hero in the loaded save:
  // the rig-radius guard below is measured from the hero's real destination.
  for(float radius=150.f;!playerPlaced && radius<=600.f;radius+=75.f)
    for(unsigned angle=0;!playerPlaced && angle<24;++angle) {
      const float yaw=Pi+float(angle)*Pi/12.f;
      const auto candidate=origin+(forward*std::cos(yaw)+side*std::sin(yaw))*radius;
      ++heroAttempts;
      playerPlaced=floorAt(world,candidate,playerFloor) && std::abs(playerFloor.y-origin.y)<180.f;
    }
  if(!playerPlaced) throw std::runtime_error("eye hero has no supported offset after "+std::to_string(heroAttempts)+" candidates at "+state.anchor);
  player.setPosition(playerFloor);
  player.setDirection(forward);player.updateTransform();
  Gothic::inst().camera()->reset(&player);
  std::vector<Vec3> positions{origin};
  size_t candidates=0,rejectedFloor=0,rejectedHeight=0,rejectedRange=0,rejectedCrowded=0;
  const auto tryRoot=[&](const Vec3& candidate) {
    ++candidates;
    Vec3 floor;
    if(!floorAt(world,candidate,floor)) {++rejectedFloor;return;}
    if(std::abs(floor.y-origin.y)>350.f) {++rejectedHeight;return;}
    if((floor-player.position()).quadLength()>1850.f*1850.f) {++rejectedRange;return;}
    bool crowded=(floor-player.position()).quadLength()<100.f*100.f;
    for(const auto& existing:positions) crowded=crowded || (floor-existing).quadLength()<130.f*130.f;
    if(crowded) {++rejectedCrowded;return;}
    positions.push_back(floor);
  };
  // Distinct supported roots behind actor 0 keep the calibration view open.
  // Stay inside the 20 m living-rig birth radius, measured from the hero.
  for(size_t row=0;positions.size()<count && row<14;++row)
    for(int column=-6;column<=6 && positions.size()<count;++column) {
      const auto candidate=origin-forward*(250.f+float(row)*180.f)+side*(float(column)*180.f);
      tryRoot(candidate);
    }
  // Streets need not extend behind the waypoint. Search the rest of the real
  // surrounding floor too, retaining an open central 120-degree view for actor 0.
  for(float radius=250.f;positions.size()<count && radius<=1900.f;radius+=140.f) {
    const unsigned sectors=unsigned(std::ceil(2.f*Pi*radius/145.f));
    for(unsigned angle=0;positions.size()<count && angle<sectors;++angle) {
      const float yaw=Pi+float(angle)*2.f*Pi/float(sectors);
      if(std::cos(yaw)>.5f) continue;
      tryRoot(origin+(forward*std::cos(yaw)+side*std::sin(yaw))*radius);
    }
  }
  if(positions.size()!=count)
    throw std::runtime_error("eye fixture roots "+std::to_string(positions.size())+"/"+std::to_string(count)+
      " at "+state.anchor+"; candidates="+std::to_string(candidates)+" floor="+std::to_string(rejectedFloor)+
      " height="+std::to_string(rejectedHeight)+" rig_range="+std::to_string(rejectedRange)+
      " crowded="+std::to_string(rejectedCrowded)+" hero="+vector(player.position())+" anchor="+vector(origin));
  std::ostringstream actors;
  actors << '[';
  for(size_t i=0;i<count;++i) {
    auto* npc=world.addNpc(instance,positions[i]);
    if(!npc) throw std::runtime_error("eye NPC creation failed");
    state.actors.push_back(npc);freeze(*npc);
    const float yaw=i==0 ? 0.f : float(i)*137.50776f*Pi/180.f;
    const Vec3 facing=forward*std::cos(yaw)+side*std::sin(yaw);
    npc->setPosition(positions[i]);npc->setDirection(facing);npc->updateTransform();
    if(i) actors << ',';
    actors << "{\"actor\":" << i << ",\"npc_id\":" << world.npcId(npc)
           << ",\"root_cm\":" << vector(npc->position()) << ",\"body_yaw_deg\":" << npc->rotation() << '}';
  }
  actors << ']';
  state.anchorPosition=origin;state.anchorDirection=forward;makeEyes();
  const std::string result="{\"kind\":\"eye_setup\",\"scene\":"+jsonQuote(scene)+",\"anchor\":"+jsonQuote(state.anchor)+
      ",\"root_cm\":"+vector(origin)+",\"forward\":"+vector(forward)+",\"pasture_sheep\":"+std::to_string(sheep)+
      ",\"frozen_residents\":"+std::to_string(residents)+",\"actors\":"+actors.str()+"}";
  append("eye-events.jsonl",result);
  return result;
}

std::string snapshot(World& world,const std::string& tag,size_t actor) {
  if(state.active) throw std::runtime_error("eye snapshots must be outside measured spans");
  if(actor>=state.actors.size()) throw std::runtime_error("eye snapshot actor out of range");
  NavigationEye fresh(state.config);
  fresh.update(*state.actors[actor],world.tickCount());
  const auto pose=state.actors[actor]->navigationEyePose();
  std::ostringstream out;
  out << std::setprecision(10) << "{\"kind\":\"eye_snapshot\",\"tag\":" << jsonQuote(tag)
      << ",\"scene\":" << jsonQuote(state.scene) << ",\"actor\":" << actor
      << ",\"game_ms\":" << world.tickCount() << ",\"requested_layout\":" << state.config.layout
      << ",\"ray_count\":" << fresh.rayCount() << ",\"origin_cm\":" << vector(pose.origin)
      << ",\"forward\":" << vector(pose.forward) << ",\"right\":" << vector(pose.right)
      << ",\"up\":" << vector(pose.up) << ",\"animated_head\":" << (pose.animatedHead?"true":"false")
      << ",\"center_range_cm\":" << state.config.centerRangeCm << ",\"near_range_cm\":" << state.config.nearRangeCm
      << ",\"range_multiplier\":" << state.config.rangeMultiplier << ",\"rays\":[";
  size_t index=0;
  for(const auto& ray:fresh.samples()) {
    if(index) out << ',';
    out << "{\"index\":" << index++ << ",\"yaw_deg\":" << ray.yawDeg << ",\"pitch_deg\":" << ray.pitchDeg
        << ",\"max_range_cm\":" << ray.maxRangeCm << ",\"distance_cm\":" << ray.distanceCm
        << ",\"origin_cm\":" << vector(ray.origin) << ",\"direction\":" << vector(ray.direction)
        << ",\"point_cm\":" << vector(ray.point) << ",\"hit\":" << (ray.hit?"true":"false")
        << ",\"hit_kind\":" << jsonQuote(NavigationEye::hitKindName(ray.kind)) << ",\"normal_y\":" << ray.normalY
        << ",\"sampled_at_ms\":" << ray.sampledAtMs << ",\"near_sampled_at_ms\":" << ray.nearSampledAtMs
        << ",\"complete\":" << (ray.complete?"true":"false") << '}';
  }
  out << "]}";
  append("eye-snapshots.jsonl",out.str());
  return "{\"kind\":\"eye_snapshot_written\",\"tag\":"+jsonQuote(tag)+",\"rays\":"+std::to_string(fresh.rayCount())+"}";
}

std::string begin(World& world,size_t layout,float range,bool enabled,const std::string& tag) {
  if(state.active || state.actors.empty()) throw std::runtime_error("eye begin needs setup and no active span");
  if(layout!=250 && layout!=500 && layout!=1000) throw std::runtime_error("eye layout must be 250, 500 or 1000");
  if(range!=20 && range!=50 && range!=100) throw std::runtime_error("eye center range must be 20, 50 or 100 metres");
  state.config.layout=layout;state.config.centerRangeCm=range*100.f;
  state.config.peripheralRangeCm=std::min(3000.f,state.config.centerRangeCm);
  makeEyes();state.tag=tag;state.enabled=enabled;state.active=true;state.measuring=false;
  state.warmupUntil=world.tickCount()+2000;state.steps=0;state.costs.clear();
  state.costs.reserve(state.actors.size()*200);
  return "{\"kind\":\"eye_begin\",\"tag\":"+jsonQuote(tag)+",\"enabled\":"+(enabled?"true":"false")+
      ",\"warmup_ms\":2000,\"requested_layout\":"+std::to_string(layout)+",\"ray_count\":"+
      std::to_string(state.eyes.front()->rayCount())+",\"center_range_cm\":"+std::to_string(state.config.centerRangeCm)+"}";
}

std::string end(World& world) {
  if(!state.active || !state.measuring) throw std::runtime_error("eye end needs a span longer than its 2 s warmup");
  const double wallUs=std::chrono::duration<double,std::micro>(Clock::now()-state.startWall).count();
  const double processUs=double(cpuNs()-state.startCpu)/1000.;
  state.active=false;
  double eyeUs=0;
  for(const auto& sample:state.costs) eyeUs+=sample.microseconds;
  size_t livingRigs=0;
  for(const auto* actor:state.actors) livingRigs+=actor->livingBody()!=nullptr;
  const auto distribution=[&](int subset) {
    std::vector<double> values;
    double sum=0;
    uint64_t casts=0,nearCasts=0,farCasts=0;
    for(const auto& sample:state.costs) {
      if(subset==1 && sample.farUpdated) continue;
      if(subset==2 && !sample.farUpdated) continue;
      values.push_back(sample.microseconds);sum+=sample.microseconds;
      casts+=sample.casts;nearCasts+=sample.nearCasts;farCasts+=sample.farCasts;
    }
    std::sort(values.begin(),values.end());
    const double median=values.empty()?0:values[values.size()/2];
    const double p95=values.empty()?0:values[size_t(std::ceil(double(values.size())*.95))-1];
    std::ostringstream json;
    json << std::setprecision(12) << "{\"samples\":" << values.size() << ",\"sum_us\":" << sum
         << ",\"mean_us\":" << (values.empty()?0:sum/double(values.size()))
         << ",\"median_us\":" << median << ",\"p95_us\":" << p95 << ",\"casts\":" << casts
         << ",\"near_casts\":" << nearCasts << ",\"far_casts\":" << farCasts << '}';
    return json.str();
  };
  std::ostringstream out;
  out << std::setprecision(12) << "{\"kind\":\"eye_span\",\"tag\":" << jsonQuote(state.tag)
      << ",\"scene\":" << jsonQuote(state.scene) << ",\"actors\":" << state.actors.size()
      << ",\"living_rigs\":" << livingRigs
      << ",\"enabled\":" << (state.enabled?"true":"false") << ",\"requested_layout\":" << state.config.layout
      << ",\"ray_count\":" << state.eyes.front()->rayCount() << ",\"center_range_cm\":" << state.config.centerRangeCm
      << ",\"near_range_cm\":" << state.config.nearRangeCm << ",\"near_period_ms\":" << state.config.nearPeriodMs
      << ",\"far_period_ms\":" << state.config.farPeriodMs << ",\"warmup_ms\":2000,\"start_game_ms\":" << state.startSim
      << ",\"end_game_ms\":" << world.tickCount() << ",\"simulated_ms\":" << world.tickCount()-state.startSim
      << ",\"steps\":" << state.steps << ",\"wall_us\":" << wallUs << ",\"process_cpu_us\":" << processUs
      << ",\"eye_updates\":" << state.costs.size() << ",\"total_eye_us\":" << eyeUs
      << ",\"achieved_hz\":" << (double(state.costs.size())*1000./double(state.actors.size())/double(world.tickCount()-state.startSim))
      << ",\"all_updates\":" << distribution(0) << ",\"near_only_updates\":" << distribution(1)
      << ",\"full_updates\":" << distribution(2) << '}';
  append("eye-spans.jsonl",out.str());
  std::ofstream timing(state.directory+"/eye-timing.jsonl",std::ios::app);
  if(!timing) throw std::runtime_error("cannot create eye timing journal");
  timing << std::setprecision(12);
  for(const auto& sample:state.costs)
    timing << "{\"kind\":\"eye_update\",\"tag\":" << jsonQuote(state.tag) << ",\"scene\":" << jsonQuote(state.scene)
           << ",\"actors\":" << state.actors.size() << ",\"requested_layout\":" << state.config.layout
           << ",\"ray_count\":" << state.eyes.front()->rayCount() << ",\"center_range_cm\":" << state.config.centerRangeCm
           << ",\"game_ms\":" << sample.time << ",\"actor\":" << sample.actor << ",\"wall_us\":" << sample.microseconds
           << ",\"casts\":" << sample.casts << ",\"near_casts\":" << sample.nearCasts << ",\"far_casts\":" << sample.farCasts
           << ",\"far_updated\":" << (sample.farUpdated?"true":"false") << "}\n";
  if(!timing) throw std::runtime_error("eye timing journal write failed");
  state.costs.clear();return out.str();
}

float dot(const Vec3& a,const Vec3& b) { return a.x*b.x+a.y*b.y+a.z*b.z; }

std::string wall(World& world) {
  if(state.active || state.actors.empty()) throw std::runtime_error("eye wall needs setup and no measured span");
  Npc& npc=*state.actors.front();
  const auto savedPosition=npc.position();const auto savedRotation=npc.rotation();
  NavigationEye::Config config=state.config;config.layout=1000;config.centerRangeCm=10000;
  config.peripheralRangeCm=3000;
  size_t attempts=0;
  // Bounded search around actual authored city/farm waypoints. Each candidate
  // is a real Jolt vertical surface; no synthetic collision is inserted.
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    if(!point.underWater && (point.pos-state.anchorPosition).quadLength()<2200.f*2200.f) points.push_back(&point);
  });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  for(const auto* point:points) for(unsigned angle=0;angle<24;++angle) {
    if(++attempts>1200) break;
    Vec3 floor;
    if(!floorAt(world,point->pos,floor)) continue;
    const float yaw=float(angle)*Pi/12.f;
    const Vec3 direction(std::cos(yaw),0,std::sin(yaw));
    const Vec3 source=floor+Vec3(0,165,0);
    const auto hit=world.physic()->navigationRay(source,source+direction*1500.f,&npc);
    if(!hit.hasCol || std::abs(hit.normal.y)>.12f ||
       (hit.kind!=DynamicWorld::NavigationHit::World && hit.kind!=DynamicWorld::NavigationHit::StaticVob)) continue;
    Vec3 normal=hit.normal;normal.y=0;normal*=1.f/normal.length();
    if(dot(normal,source-hit.point)<0) normal*=-1.f;
    Vec3 root;
    if(!floorAt(world,hit.point+normal*100.f-Vec3(0,165,0),root)) continue;
    if(std::abs(root.y-floor.y)>80.f) continue;
    npc.setPosition(root);npc.setDirection(normal);npc.updateTransform();
    const auto backPose=npc.navigationEyePose();
    const float distance=dot(backPose.origin-hit.point,normal);
    if(distance<90 || distance>110 || dot(backPose.forward,normal)<.98f) continue;
    const auto sameWall=[&](const NavigationEye::Ray& ray) {
      return ray.hit && (ray.kind==NavigationEye::HitKind::World || ray.kind==NavigationEye::HitKind::StaticVob) &&
             std::abs(dot(ray.point-hit.point,normal))<8.f && std::abs(ray.normalY)<.2f;
    };
    NavigationEye back(config);back.update(npc,world.tickCount());
    const auto backHits=std::count_if(back.samples().begin(),back.samples().end(),sameWall);
    if(backHits) continue;
    npc.setDirection(normal*-1.f);npc.updateTransform();
    NavigationEye front(config);front.update(npc,world.tickCount());
    const auto frontHits=std::count_if(front.samples().begin(),front.samples().end(),sameWall);
    if(frontHits<3) continue;
    const auto frontPose=npc.navigationEyePose();
    const float frontDistance=dot(frontPose.origin-hit.point,normal);
    if(frontDistance<90 || frontDistance>110 || dot(frontPose.forward,normal)>-.98f) continue;
    const std::string result="{\"kind\":\"eye_wall\",\"pass\":true,\"scene\":"+jsonQuote(state.scene)+
      ",\"attempts\":"+std::to_string(attempts)+",\"waypoint\":"+jsonQuote(point->name)+",\"root_cm\":"+vector(root)+
      ",\"plane_point_cm\":"+vector(hit.point)+",\"plane_normal\":"+vector(normal)+",\"wall_distance_cm\":"+
      std::to_string(distance)+",\"back_origin_cm\":"+vector(backPose.origin)+",\"back_forward\":"+vector(backPose.forward)+
      ",\"front_origin_cm\":"+vector(frontPose.origin)+",\"front_forward\":"+vector(frontPose.forward)+
      ",\"front_wall_distance_cm\":"+std::to_string(frontDistance)+
      ",\"back_wall_hits\":"+std::to_string(backHits)+",\"front_wall_hits\":"+std::to_string(frontHits)+
      ",\"ray_count\":"+std::to_string(back.rayCount())+",\"plane_tolerance_cm\":8,\"finite_wall\":true}";
    append("eye-events.jsonl",result);
    const auto savedConfig=state.config;state.config=config;
    npc.setDirection(normal);npc.updateTransform();snapshot(world,"wall-back",0);
    npc.setDirection(normal*-1.f);npc.updateTransform();snapshot(world,"wall-front",0);
    state.config=savedConfig;
    npc.setPosition(savedPosition);npc.setDirection(savedRotation);npc.updateTransform();
    return result;
  }
  npc.setPosition(savedPosition);npc.setDirection(savedRotation);npc.updateTransform();
  throw std::runtime_error("bounded real finite-wall fixture unavailable after "+std::to_string(attempts)+" candidates");
}
}

void NavigationEyeBench::reset() { state=State{}; }

Npc* NavigationEyeBench::actor(size_t index) {
  return index<state.actors.size() ? state.actors[index] : nullptr;
}

std::string NavigationEyeBench::command(const std::vector<std::string>& args,World& world,Npc& player,
                                       const std::string& outputDirectory) {
  if(state.directory.empty()) {
    state.directory=outputDirectory;
    for(const char* file:{"eye-events.jsonl","eye-spans.jsonl","eye-timing.jsonl","eye-snapshots.jsonl"}) {
      std::ofstream truncate(state.directory+"/"+file,std::ios::trunc);
      if(!truncate) throw std::runtime_error(std::string("cannot initialize eye journal ")+file);
    }
  }
  if(args.size()==4 && args[1]=="setup") return setup(world,player,args[2],std::stoul(args[3]));
  if(args.size()==6 && args[1]=="begin" && (args[4]=="on" || args[4]=="off"))
    return begin(world,std::stoul(args[2]),std::stof(args[3]),args[4]=="on",args[5]);
  if(args.size()==2 && args[1]=="end") return end(world);
  if((args.size()==3 || args.size()==4) && args[1]=="snapshot")
    return snapshot(world,args[2],args.size()==4?std::stoul(args[3]):0);
  if(args.size()==2 && args[1]=="wall") return wall(world);
  throw std::runtime_error("eye: setup <market|lobart|lobart-fence> <count> | begin <250|500|1000> <20|50|100> <on|off> <tag> | end | snapshot <tag> [actor] | wall");
}

void NavigationEyeBench::tick(uint64_t gameMs) {
  if(!state.active) return;
  if(!state.measuring && gameMs>=state.warmupUntil) {
    state.measuring=true;state.startSim=gameMs;state.startWall=Clock::now();state.startCpu=cpuNs();
  }
  if(state.measuring) ++state.steps;
  if(!state.enabled) return;
  for(size_t i=0;i<state.actors.size();++i) {
    const auto update=state.eyes[i]->update(*state.actors[i],gameMs);
    if(state.measuring && update.updated)
      state.costs.push_back({gameMs,i,update.costUs,update.casts,update.nearCasts,update.farCasts,update.farUpdated});
  }
}
