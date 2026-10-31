#include "navigationmemorybench.h"
#include "navigationmemory.h"
#include "navigationeye.h"
#include "navigationeyebench.h"
#include "npckeysbench.h"

#include "game/gamescript.h"
#include "physics/dynamicworld.h"
#include "world/world.h"
#include "world/objects/npc.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace {
using Tempest::Vec3;
using Clock=std::chrono::steady_clock;
constexpr uint64_t DecisionPeriodMs=100, WarmupMs=2000;

NavigationEye::Config eyeConfig() {
  NavigationEye::Config cfg;
  cfg.layout=250;
  return cfg;
}

struct Actor {
  Npc* npc=nullptr;
  int64_t handleId=0;
  std::string selector;
  NavigationEye eye{eyeConfig()};
  NavigationMemory memory;
  NavigationMemory::View view;
  uint64_t nextDecision=0;
  double poseUs=0;
  bool enabled=false;
};
struct Cost {
  uint64_t time=0;
  uint32_t actor=0;
  double updateUs=0,viewUs=0;
};
struct State {
  std::string directory,tag;
  std::vector<std::unique_ptr<Actor>> actors;
  std::vector<Cost> costs;
  uint64_t warmupUntil=0,beginMs=0;
  bool measuring=false;
} state;

std::string quote(const std::string& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.String(value.data(),rapidjson::SizeType(value.size()));
  return {buffer.GetString(),buffer.GetSize()};
}
std::string number(double value) {
  if(!std::isfinite(value)) return "null";
  std::ostringstream out;
  out << std::setprecision(10) << value;
  return out.str();
}
std::string vector(const Vec3& value) {
  return '['+number(value.x)+','+number(value.y)+','+number(value.z)+']';
}
void append(const char* name,const std::string& row) {
  std::ofstream out(state.directory+"/"+name,std::ios::app);
  if(!out) throw std::runtime_error(std::string("cannot open memory journal ")+name);
  out << row << '\n';
  if(!out) throw std::runtime_error(std::string("cannot write memory journal ")+name);
}
bool digits(const std::string& text) {
  return !text.empty() && std::all_of(text.begin(),text.end(),[](char c){return c>='0' && c<='9';});
}
Npc& resolve(World& world,const std::string& selector) {
  Npc* npc=nullptr;
  if(digits(selector)) {
    npc=NavigationEyeBench::actor(std::stoull(selector));
  } else if(selector.starts_with("id:") && digits(selector.substr(3))) {
    const auto id=std::stoull(selector.substr(3));
    if(id<=std::numeric_limits<uint32_t>::max()) npc=world.npcById(uint32_t(id));
  } else {
    const auto symbol=world.script().findSymbolIndex(selector);
    if(symbol!=size_t(-1)) npc=world.findNpcByInstance(symbol);
  }
  if(!npc) throw std::runtime_error("memory actor unavailable: "+selector);
  return *npc;
}
bool live(const Actor& actor,World& world) {
  // World NPC indices change when the active set is reordered. Verify the
  // pointer first, then its lifetime handle, before dereferencing it.
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc==actor.npc) return npc->handle().id==actor.handleId;
  return false;
}
NavigationMemory::Pose pose(const Npc& npc,uint64_t now) {
  NavigationMemory::Pose result;
  result.position=npc.position();
  result.forward=Vec3(std::cos(npc.rotationRad()),0,std::sin(npc.rotationRad()));
  result.gameMs=now;
  const auto& guild=npc.world().script().guildVal();
  const float step=float(guild.step_height[npc.guild()]);
  result.stepHeightCm=step>0.f?step:50.f;
  constexpr float Pi=3.14159265358979323846f;
  result.maxSlopeDegrees=std::acos(std::clamp(std::cos(float(guild.slide_angle[npc.guild()])*Pi/180.f),0.f,1.f))*180.f/Pi;
  return result;
}
Actor& tracked(World& world,const std::string& selector,bool create) {
  auto& npc=resolve(world,selector);
  for(const auto& actor:state.actors)
    if(actor->npc==&npc && live(*actor,world)) return *actor;
  if(!create) throw std::runtime_error("memory on is required before snapshot: "+selector);
  auto actor=std::make_unique<Actor>();
  actor->npc=&npc;actor->handleId=npc.handle().id;actor->selector=selector;
  state.actors.push_back(std::move(actor));
  return *state.actors.back();
}
void enable(World& world,const std::string& selector,bool enabled) {
  auto& actor=tracked(world,selector,enabled);
  if(enabled && !actor.enabled) {
    // Re-enabling starts a fresh observation session: motion while disabled
    // was not tracked and must not be reconstructed as a straight-line path.
    actor.memory.reset();
    actor.eye.reset();
    actor.view=NavigationMemory::View{};
    actor.poseUs=0;
    actor.nextDecision=0;
  }
  actor.enabled=enabled;
}

void layer(std::ostream& out,const NavigationMemory::Layer& map,const NavigationMemory::Pose& body) {
  const Vec3 right(-body.forward.z,0,body.forward.x);
  out << "{\"size\":" << map.size << ",\"cell_cm\":" << map.cellCm
      << ",\"columns\":[\"x\",\"z\",\"world_x_cm\",\"world_z_cm\",\"confidence\",\"free\",\"wall\",\"ledge\",\"drop\",\"water\",\"movable\",\"ground_height_cm\"],\"cells\":[";
  bool first=true;
  for(size_t z=0;z<map.size;++z) for(size_t x=0;x<map.size;++x) {
    const auto& cell=map.cells[z*map.size+x];
    if(cell.confidence<=0) continue;
    if(!first) out << ',';
    first=false;
    const float sideways=(float(x)+.5f-float(map.size)*.5f)*map.cellCm;
    const float ahead=(float(z)+.5f-float(map.size)*.5f)*map.cellCm;
    const Vec3 center=body.position+right*sideways+body.forward*ahead;
    out << '[' << x << ',' << z << ',' << center.x << ',' << center.z << ',' << cell.confidence
        << ',' << cell.free << ',' << cell.wall << ',' << cell.ledge << ',' << cell.drop
        << ',' << cell.water << ',' << cell.movable << ',' << number(cell.groundHeightCm) << ']';
  }
  out << "]}";
}
std::string snapshot(World& world,const std::string& tag,const std::string& selector) {
  if(state.measuring) throw std::runtime_error("memory snapshot must be outside a measured span");
  auto& actor=tracked(world,selector,false);
  actor.memory.update({},pose(*actor.npc,world.tickCount()));
  actor.memory.view(actor.view);
  const auto head=actor.npc->navigationEyePose();
  const auto& body=actor.view.pose;
  std::ostringstream out;
  out << std::setprecision(9) << "{\"kind\":\"memory_snapshot\",\"tag\":" << quote(tag)
      << ",\"actor\":" << quote(selector) << ",\"npc_id\":" << world.npcId(actor.npc)
      << ",\"game_ms\":" << world.tickCount() << ",\"root_cm\":" << vector(body.position)
      << ",\"body_forward\":" << vector(body.forward) << ",\"body_yaw_deg\":" << actor.npc->rotation()
      << ",\"traveled_cm\":" << actor.view.traveledCm
      << ",\"head_origin_cm\":" << vector(head.origin) << ",\"head_forward\":" << vector(head.forward)
      << ",\"head_right\":" << vector(head.right) << ",\"head_up\":" << vector(head.up)
      << ",\"animated_head\":" << (head.animatedHead?"true":"false")
      << ",\"requested_layout\":250,\"ray_count\":" << actor.eye.rayCount()
      << ",\"current_rays\":[";
  size_t index=0;
  for(const auto& ray:actor.eye.samples()) {
    if(index) out << ',';
    out << "{\"index\":" << index++ << ",\"yaw_deg\":" << ray.yawDeg << ",\"pitch_deg\":" << ray.pitchDeg
        << ",\"origin_cm\":" << vector(ray.origin) << ",\"direction\":" << vector(ray.direction)
        << ",\"point_cm\":" << vector(ray.point) << ",\"distance_cm\":" << ray.distanceCm
        << ",\"max_range_cm\":" << ray.maxRangeCm << ",\"normal_y\":" << ray.normalY
        << ",\"hit_kind\":" << quote(NavigationEye::hitKindName(ray.kind))
        << ",\"hit\":" << (ray.hit?"true":"false") << ",\"complete\":" << (ray.complete?"true":"false")
        << ",\"sampled_at_ms\":" << ray.sampledAtMs << ",\"near_sampled_at_ms\":" << ray.nearSampledAtMs << '}';
  }
  out << "],\"near\":";layer(out,actor.view.near,body);
  out << ",\"far\":";layer(out,actor.view.far,body);
  out << '}';append("memory-snapshots.jsonl",out.str());
  return "{\"kind\":\"memory_snapshot_written\",\"tag\":"+quote(tag)+",\"actor\":"+quote(selector)+"}";
}

std::string distribution(const std::vector<Cost>& costs,int part) {
  std::vector<double> values;
  values.reserve(costs.size());
  double sum=0;
  for(const auto& cost:costs) {
    const double value=part==1?cost.updateUs:part==2?cost.viewUs:cost.updateUs+cost.viewUs;
    values.push_back(value);sum+=value;
  }
  std::sort(values.begin(),values.end());
  const double median=values.empty()?0:values[values.size()/2];
  const double p95=values.empty()?0:values[size_t(std::ceil(double(values.size())*.95))-1];
  return "{\"samples\":"+std::to_string(values.size())+",\"median_us\":"+number(median)+
      ",\"p95_us\":"+number(p95)+",\"mean_us\":"+number(values.empty()?0:sum/double(values.size()))+"}";
}
std::string end(World& world) {
  if(!state.measuring) throw std::runtime_error("memory end needs an active span");
  state.measuring=false;
  if(state.costs.empty()) throw std::runtime_error("memory span must exceed its 2 s warmup");
  size_t actors=0;
  for(const auto& actor:state.actors) actors+=actor->enabled && live(*actor,world);
  std::string row="{\"kind\":\"memory_span\",\"tag\":"+quote(state.tag)+",\"actors\":"+std::to_string(actors)+
      ",\"ray_count\":238,\"decision_period_ms\":"+std::to_string(DecisionPeriodMs)+
      ",\"warmup_ms\":"+std::to_string(WarmupMs)+",\"start_game_ms\":"+std::to_string(state.beginMs+WarmupMs)+
      ",\"end_game_ms\":"+std::to_string(world.tickCount())+",\"eye_cost_included\":false,\"pose_updates_amortized\":true,\"update\":"+
      distribution(state.costs,1)+",\"view\":"+distribution(state.costs,2)+",\"update_and_view\":"+distribution(state.costs,0)+"}";
  append("memory-spans.jsonl",row);
  std::ofstream timing(state.directory+"/memory-timing.jsonl",std::ios::app);
  if(!timing) throw std::runtime_error("cannot open memory timing journal");
  for(const auto& cost:state.costs)
    timing << "{\"kind\":\"memory_decision\",\"tag\":" << quote(state.tag) << ",\"actors\":" << actors
           << ",\"ray_count\":238,\"game_ms\":" << cost.time << ",\"npc_id\":" << cost.actor
           << ",\"update_us\":" << number(cost.updateUs) << ",\"view_us\":" << number(cost.viewUs)
           << ",\"update_and_view_us\":" << number(cost.updateUs+cost.viewUs) << "}\n";
  if(!timing) throw std::runtime_error("cannot write memory timing journal");
  state.costs.clear();return row;
}

std::string spawnBehind(World& world,const std::string& selector,float distance,float sideways) {
  auto& npc=resolve(world,selector);
  const auto body=pose(npc,world.tickCount());
  const Vec3 right(-body.forward.z,0,body.forward.x);
  const auto desired=body.position-body.forward*distance+right*sideways;
  const auto floor=world.physic()->landRay(desired+Vec3(0,180,0),600);
  if(!floor.hasCol || floor.n.y<.65f) throw std::runtime_error("memory hidden NPC has no supported root");
  const auto instance=world.script().findSymbolIndex("BDT_1003_BANDIT_M");
  if(instance==size_t(-1)) throw std::runtime_error("memory human fixture unavailable");
  auto* hidden=world.addNpc(instance,floor.v);
  if(!hidden) throw std::runtime_error("memory hidden NPC creation failed");
  hidden->setInteraction(nullptr,true);hidden->clearAiQueue();hidden->clearState(false);hidden->clearGoTo();
  hidden->setExternalControl(true);hidden->setPosition(floor.v);hidden->setDirection(body.forward);hidden->updateTransform();
  hidden->handle().attribute[ATR_HITPOINTS]=hidden->handle().attribute[ATR_HITPOINTSMAX]=50000;
  return "{\"kind\":\"memory_hidden_npc\",\"actor\":"+quote(selector)+",\"npc_id\":"+std::to_string(world.npcId(hidden))+
      ",\"root_cm\":"+vector(hidden->position())+",\"observer_root_cm\":"+vector(body.position)+
      ",\"observer_forward\":"+vector(body.forward)+",\"game_ms\":"+std::to_string(world.tickCount())+"}";
}
}

void NavigationMemoryBench::reset(bool keepJournal) {
  const auto directory=keepJournal?state.directory:std::string{};
  state=State{};
  state.directory=directory;
}

std::string NavigationMemoryBench::command(const std::vector<std::string>& args,World& world,Npc& player,
                                          PlayerControl& control,const std::string& outputDirectory) {
  if(state.directory.empty()) {
    state.directory=outputDirectory;
    for(const char* file:{"memory-events.jsonl","memory-snapshots.jsonl","memory-spans.jsonl","memory-timing.jsonl"}) {
      std::ofstream truncate(state.directory+"/"+file,std::ios::trunc);
      if(!truncate) throw std::runtime_error(std::string("cannot initialize memory journal ")+file);
    }
  }
  if(args.size()>=3 && args[1]=="measure") {
    auto forwarded=args;
    forwarded.erase(forwarded.begin()+1);
    return command(forwarded,world,player,control,outputDirectory);
  }
  std::string result;
  if(args.size()==3 && (args[1]=="on" || args[1]=="off")) {
    const bool enabled=args[1]=="on";
    if(state.measuring) throw std::runtime_error("memory end before changing enabled actors");
    if(args[2]=="all") {
      if(enabled) {
        size_t count=0;
        for(;NavigationEyeBench::actor(count);++count) enable(world,std::to_string(count),true);
        if(!count) throw std::runtime_error("memory on all needs eye setup");
      } else for(auto& actor:state.actors) actor->enabled=false;
    } else enable(world,args[2],enabled);
    result="{\"kind\":\"memory_enable\",\"actor\":"+quote(args[2])+",\"enabled\":"+(enabled?"true":"false")+"}";
  } else if(args.size()==4 && args[1]=="snapshot") {
    return snapshot(world,args[2],args[3]);
  } else if(args.size()>=4 && args[1]=="keys") {
    auto forwarded=args;
    forwarded.erase(forwarded.begin());forwarded[0]="keys";
    forwarded[1]="id:"+std::to_string(world.npcId(&resolve(world,args[2])));
    result=NpcKeysBench::command(forwarded,world,player,control,outputDirectory);
  } else if(args.size()==3 && args[1]=="begin") {
    if(state.measuring) throw std::runtime_error("memory end before begin");
    size_t actors=0;
    for(const auto& actor:state.actors) actors+=actor->enabled && live(*actor,world);
    if(!actors) throw std::runtime_error("memory begin needs enabled actors");
    state.tag=args[2];state.costs.clear();state.costs.reserve(actors*120);
    state.beginMs=world.tickCount();state.warmupUntil=state.beginMs+WarmupMs;state.measuring=true;
    result="{\"kind\":\"memory_begin\",\"tag\":"+quote(args[2])+",\"actors\":"+std::to_string(actors)+
        ",\"ray_count\":238,\"warmup_ms\":"+std::to_string(WarmupMs)+",\"decision_period_ms\":"+std::to_string(DecisionPeriodMs)+"}";
  } else if(args.size()==2 && args[1]=="end") {
    return end(world);
  } else if((args.size()==4 || args.size()==5) && args[1]=="spawn-behind") {
    result=spawnBehind(world,args[2],std::stof(args[3]),args.size()==5?std::stof(args[4]):0);
  } else throw std::runtime_error("memory: on|off <actor|all> | snapshot <tag> <actor> | keys <actor> <keys arguments> | begin <tag> | end | spawn-behind <actor> <cm> [side cm]");
  append("memory-events.jsonl",result);return result;
}

void NavigationMemoryBench::tick(World& world) {
  const auto now=world.tickCount();
  for(const auto& actor:state.actors) {
    if(!actor->enabled || !live(*actor,world)) continue;
    const auto body=pose(*actor->npc,now);
    // The empty update follows the body's full path, including turns between decisions.
    // It cannot observe anything. Rays and the view are processed at the decision rate.
    const auto poseBegin=Clock::now();
    actor->memory.update({},body);
    actor->poseUs+=std::chrono::duration<double,std::micro>(Clock::now()-poseBegin).count();
    if(now<actor->nextDecision) continue;
    actor->nextDecision=now+DecisionPeriodMs;
    actor->eye.update(*actor->npc,now);
    const auto begin=Clock::now();
    actor->memory.update(actor->eye.memorySamples(),body);
    const auto sampled=Clock::now();
    actor->memory.view(actor->view);
    const auto viewed=Clock::now();
    if(state.measuring && now>=state.warmupUntil)
      state.costs.push_back({now,world.npcId(actor->npc),actor->poseUs+std::chrono::duration<double,std::micro>(sampled-begin).count(),
                            std::chrono::duration<double,std::micro>(viewed-sampled).count()});
    actor->poseUs=0;
  }
}
