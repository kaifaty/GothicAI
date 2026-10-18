#include "navdemos.h"
#include "navbench.h"
#include "navkeyteacher.h"
#include "navdemowriter.h"
#include "navpolicy.h"
#include "navtransitions.h"
#include "game/knownroadsbench.h"
#include "world/world.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <sstream>
#include <unordered_set>

namespace {
using Tempest::Vec3;
constexpr float Pi=3.14159265358979323846f;
constexpr uint64_t PolicyWarmupDecisions=30;
struct RecordedKeys { uint64_t time=0; NpcKeys::KeyState keys; };
struct MotionSample { uint64_t time=0; Vec3 position; bool commanded=false; };
struct Episode {
  Npc* npc=nullptr;
  Vec3 goal,previousPosition,velocity;
  std::string output,from,to,failure,initialState;
  bool active=false,replay=false,drive=false,success=false;
  bool population=false,record=true,sampled=false,pending=false,fell=false;
  bool cvMode=false,heroNear=true,killInjected=false,resetAfterDeath=false;
  uint64_t killAt=0;
  Vec3 expectedStart;
  size_t slot=0,pairIndex=0,sequence=0;
  uint64_t actionAt=0;
  int32_t health=0;
  double startPathCm=0,lastPathCm=0,progressCm=0;
  double minHeroDistanceCm=std::numeric_limits<double>::max(),maxHeroDistanceCm=0;
  bool rewardPathValid=true;
  uint64_t invalidRewardPaths=0;
  NavPolicy::Result selected;
  std::array<size_t,6> selectedIndices={};
  std::array<double,6> selectedLogProb={};
  // Eye, memory, roads/teacher, encoding, policy, reward, recording.
  std::array<double,7> phaseUs={};
  uint32_t teacherVersion=1;
  double teacherBeta=0;
  uint64_t mixingSeed=7,teacherControlDecisions=0;
  std::mt19937_64 mixingRng;
  KnownRoads::Guidance guidance;
  uint64_t began=0,limit=240000,next=0,decisions=0,frames=0,cvFrames=0;
  uint64_t progressAt=0;
  float best=std::numeric_limits<float>::max();
  size_t replayIndex=0;
  uint64_t replans=0,fallbacks=0;
  double writeUs=0,maxWriteUs=0;
  std::shared_ptr<const NavPolicy> policy;
  std::vector<NavPolicy::State> policyStates;
  std::vector<double> policyUs,encodeUs,populationUs;
  std::array<uint64_t,6> disagreements={};
  std::deque<MotionSample> motion;
  uint64_t blockedSpans=0,blockedMs=0,maxBlockedMs=0,blockedSince=0,blockedUntil=0;
  bool blocked=false;
  std::chrono::steady_clock::time_point wallStart;
  NpcKeys::KeyState previous;
  NavMesh::Path path;
  NavigationEye eye{[] { NavigationEye::Config c;c.layout=250;return c; }()};
  NavigationMemory memory;
  NavigationMemory::View view;
  std::unique_ptr<NavDemoWriter> writer;
  std::ofstream actions,trajectory,knownPaths,samples;
  std::vector<RecordedKeys> replayActions;
};
Episode legacy;
struct Pair {std::string from,to;};
struct Population {
  bool active=false,record=true,sampled=false,near=false;
  size_t instance=0,targetEpisodes=0,started=0,completed=0,initialDecisions=0;
  uint64_t seed=0,limit=0,began=0,runMs=0,decisions=0;
  Vec3 home;
  std::string output;
  std::shared_ptr<const NavPolicy> policy;
  std::vector<Pair> pairs;
  std::vector<std::unique_ptr<Episode>> slots;
  std::vector<std::mt19937_64> rngs;
  std::unordered_set<const Npc*> members;
  std::ofstream episodes;
  std::array<double,7> phaseUs={};
  std::chrono::steady_clock::time_point wallStart;
  double startupSeconds=0;
} population;
struct Settings {
  bool cv=false,heroNear=true,killArmed=false,killFired=false;
  size_t killSlot=0;
  uint64_t killAfterMs=0;
} settings;
Npc* preparingLegacy=nullptr;
struct PrepareLegacy {
  explicit PrepareLegacy(Npc* npc) {preparingLegacy=npc;}
  ~PrepareLegacy() {preparingLegacy=nullptr;}
};
using Clock=std::chrono::steady_clock;
double micros(Clock::time_point start) {return std::chrono::duration<double,std::micro>(Clock::now()-start).count();}

NavMesh::Point meters(const Vec3& p) {return {p.x*.01f,p.y*.01f,p.z*.01f};}
Vec3 cm(const NavMesh::Point& p) {return {p[0]*100,p[1]*100,p[2]*100};}
std::array<float,3> array(const Vec3& p) {return {p.x,p.y,p.z};}
void vector(std::ostream& out,const Vec3& p) {out<<'['<<p.x<<','<<p.y<<','<<p.z<<']';}
double hiddenL2(const Episode& ep) {
  double square=0;
  for(const auto& state:ep.policyStates)for(float value:state.hidden)square+=double(value)*double(value);
  return std::sqrt(square);
}
std::string initialState(const Episode& ep) {
  const auto& npc=*ep.npc;const auto held=NpcKeys::status(npc).keys;
  size_t hidden=0,observed=0;
  for(const auto& state:ep.policyStates)hidden+=state.hidden.size();
  for(const auto& ray:ep.eye.samples())observed+=ray.complete;
  std::ostringstream out;out<<std::setprecision(9)<<std::boolalpha;
  out<<"{\"hp\":"<<npc.attribute(ATR_HITPOINTS)<<",\"hp_max\":"<<npc.attribute(ATR_HITPOINTSMAX)
     <<",\"down\":"<<npc.isDown()<<",\"dead\":"<<npc.isDead()<<",\"ragdoll\":"<<npc.hasRagdoll()
     <<",\"body_state\":"<<uint32_t(npc.bodyStateMasked())<<",\"in_air\":"<<npc.isInAir()
     <<",\"climbing\":"<<npc.navigationClimbing()<<",\"gru_hidden_size\":"<<hidden<<",\"gru_l2\":"<<hiddenL2(ep)
     <<",\"memory_traveled_cm\":"<<ep.memory.traveledCm()<<",\"guidance_points\":"<<ep.guidance.polyline.size()
     <<",\"eye_observed\":"<<observed<<",\"held_keys\":["<<held.forward<<','<<held.strafe<<','<<held.turn
     <<','<<int(held.walk)<<','<<int(held.jump)<<','<<int(held.climb)<<"],\"position_cm\":";
  vector(out,NpcKeys::physicalPosition(npc));out<<",\"body_yaw_deg\":"<<npc.rotation()<<'}';
  return out.str();
}
std::array<float,3> local(const Vec3& p,float yaw) {
  const float c=std::cos(yaw),s=std::sin(yaw);
  return {-p.x*s+p.z*c,p.y,p.x*c+p.z*s};
}
std::array<float,3> direction(Vec3 delta,float yaw) {
  const float len=delta.length();if(len>0)delta*=1.f/len;return local(delta,yaw);
}
std::ofstream file(Episode& ep,const char* name) {
  std::ofstream out(std::filesystem::path(ep.output)/name);
  if(!out)throw std::runtime_error(std::string("navdemos cannot write ")+name);
  out<<std::setprecision(9)<<std::boolalpha;return out;
}
void trajectory(Episode& ep,uint64_t elapsed) {
  if(!ep.record)return;
  const auto p=NpcKeys::physicalPosition(*ep.npc);
  ep.trajectory<<elapsed<<' '<<p.x<<' '<<p.y<<' '<<p.z<<' '<<ep.npc->rotation()<<'\n';
  if(!ep.trajectory)throw std::runtime_error("navdemos trajectory write failed");
}
void action(Episode& ep,uint64_t elapsed,const NpcKeys::KeyState& k) {
  if(!ep.record)return;
  ep.actions<<elapsed<<' '<<k.forward<<' '<<k.strafe<<' '<<k.turn<<' '
            <<int(k.walk)<<' '<<int(k.jump)<<' '<<int(k.climb)<<' '<<k.headYaw<<' '<<k.headPitch<<'\n';
  if(!ep.actions)throw std::runtime_error("navdemos action write failed");
}
double percentile(std::vector<double> values,double quantile) {
  if(values.empty())return 0;
  std::sort(values.begin(),values.end());
  const double index=quantile*double(values.size()-1);
  const auto lower=size_t(index),upper=std::min(lower+1,values.size()-1);
  return values[lower]+(values[upper]-values[lower])*(index-double(lower));
}
void closeBlocked(Episode& ep,uint64_t elapsed) {
  if(!ep.blocked)return;
  const auto duration=elapsed-ep.blockedSince;
  ep.blockedMs+=duration;ep.maxBlockedMs=std::max(ep.maxBlockedMs,duration);
  ep.blockedUntil=elapsed;ep.blocked=false;
}
void measureBlocked(Episode& ep,uint64_t elapsed) {
  const auto& k=ep.previous;
  ep.motion.push_back({elapsed,NpcKeys::physicalPosition(*ep.npc),
                       k.forward!=0 || k.strafe!=0 || k.jump || k.climb});
  const auto cutoff=elapsed>3000?elapsed-3000:0;
  while(ep.motion.size()>1 && ep.motion[1].time<=cutoff)ep.motion.pop_front();
  uint64_t commanded=0;
  for(size_t i=1;i<ep.motion.size();++i)
    if(ep.motion[i].commanded)commanded+=ep.motion[i].time-ep.motion[i-1].time;
  const auto displacement=ep.motion.back().position-ep.motion.front().position;
  const bool stalled=elapsed-ep.motion.front().time>=3000 && commanded>=1500 &&
                     std::hypot(displacement.x,displacement.z)<50.f;
  if(stalled && !ep.blocked) {
    ep.blocked=true;ep.blockedSince=std::max(ep.motion.front().time,ep.blockedUntil);++ep.blockedSpans;
  }
  else if(!stalled)closeBlocked(ep,elapsed);
}
double rewardPath(Episode& ep,World& world) {
  const auto route=NavBench::navigationMesh(world).findPath(meters(NpcKeys::physicalPosition(*ep.npc)),meters(ep.goal));
  ep.rewardPathValid=route.complete;
  if(!route.complete) {++ep.invalidRewardPaths;return ep.lastPathCm;}
  return double(route.length)*100.;
}
void reward(Episode& ep,World& world,bool terminated=false,bool truncated=false) {
  if(!ep.population || !ep.pending)return;
  const auto start=Clock::now();
  const bool beforeValid=ep.rewardPathValid;
  const double after=rewardPath(ep,world),progress=ep.lastPathCm-after;
  ep.progressCm+=progress;
  const auto now=world.tickCount();
  const auto health=ep.npc->attribute(ATR_HITPOINTS);
  ep.phaseUs[5]+=micros(start);
  if(ep.record) {
    const auto recordStart=Clock::now();
    auto& out=ep.samples;
    out<<"{\"time_ms\":"<<ep.actionAt<<",\"next_time_ms\":"<<now<<",\"elapsed_ms\":"<<ep.actionAt-ep.began
       <<",\"action_indices\":[";
    for(size_t i=0;i<6;++i) {if(i)out<<',';out<<ep.selectedIndices[i];}
    out<<"],\"logits\":[";bool first=true;
    for(const auto& head:ep.selected.logits)for(float value:head) {if(!first)out<<',';first=false;out<<value;}
    out<<"],\"log_probs\":[";double joint=0;
    for(size_t i=0;i<6;++i) {if(i)out<<',';out<<ep.selectedLogProb[i];joint+=ep.selectedLogProb[i];}
    out<<"],\"log_prob\":"<<joint<<",\"path_before_cm\":"<<ep.lastPathCm<<",\"path_after_cm\":"<<after
       <<",\"path_valid\":"<<(beforeValid && ep.rewardPathValid)<<",\"terminated\":"<<terminated<<",\"truncated\":"<<truncated
       <<",\"rewards\":{\"progress_cm\":"<<progress<<",\"arrived\":"<<int(ep.success)
       <<",\"blocked\":"<<int(ep.blocked)<<",\"fall_damage\":"<<(ep.fell?std::max(0,ep.health-health):0)
       <<",\"death\":"<<int(ep.npc->isDead())<<",\"jump\":"<<int(ep.previous.jump)
       <<",\"climb\":"<<int(ep.previous.climb)<<",\"time_ms\":"<<now-ep.actionAt<<"}}\n";
    if(!out)throw std::runtime_error("navdemos rewards write failed");
    ep.phaseUs[6]+=micros(recordStart);
  }
  ep.health=health;ep.lastPathCm=after;ep.fell=false;ep.pending=false;
}
void finish(Episode& ep,World& world) {
  const auto elapsed=world.tickCount()-ep.began;
  closeBlocked(ep,elapsed);
  trajectory(ep,elapsed);
  if(!ep.replay) {NpcKeys::KeyState end;end.walk=true;action(ep,elapsed,end);if(ep.writer)ep.writer->finish();}
  ep.trajectory.close();ep.actions.close();ep.knownPaths.close();ep.samples.close();
  auto out=file(ep,"episode.json");
  const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-ep.wallStart).count();
  out<<"{\"success\":"<<ep.success<<",\"replay\":"<<ep.replay<<",\"drive\":"<<ep.drive<<",\"start\":"<<std::quoted(ep.from)
     <<",\"goal\":"<<std::quoted(ep.to)<<",\"elapsed_ms\":"<<elapsed<<",\"limit_ms\":"<<ep.limit
     <<",\"distance_cm\":"<<(ep.npc->position()-ep.goal).length()<<",\"failure\":"<<std::quoted(ep.failure)
     <<",\"teacher_version\":"<<ep.teacherVersion<<",\"demo_version\":"<<ep.teacherVersion
     <<",\"decisions\":"<<ep.decisions<<",\"record_us_mean\":"<<(ep.decisions?ep.writeUs/double(ep.decisions):0)
     <<",\"record_us_max\":"<<ep.maxWriteUs<<",\"wall_seconds\":"<<seconds
     <<",\"decisions_per_second\":"<<double(ep.decisions)/seconds<<",\"frames\":"<<ep.frames
     <<",\"cv_frames\":"<<ep.cvFrames<<",\"replans\":"<<ep.replans<<",\"fallbacks\":"<<ep.fallbacks
     <<",\"final_position\":";vector(out,NpcKeys::physicalPosition(*ep.npc));
  out<<",\"target_position\":";vector(out,ep.goal);
  out<<",\"next_corner\":";vector(out,ep.path.corners.empty()?ep.goal:cm(ep.path.corners.back()));
  if(ep.drive) {
    out<<",\"teacher_beta\":"<<ep.teacherBeta<<",\"mixing_seed\":"<<ep.mixingSeed
       <<",\"teacher_control_decisions\":"<<ep.teacherControlDecisions
       <<",\"policy_population\":"<<ep.policyStates.size()
       <<",\"policy_warmup_decisions\":"<<std::min(ep.decisions,PolicyWarmupDecisions)
       <<",\"policy_measured_calls\":"<<ep.policyUs.size()
       <<",\"population_measured_ticks\":"<<ep.populationUs.size()
       <<",\"policy_us_median\":"<<percentile(ep.policyUs,.5)
       <<",\"policy_us_p95\":"<<percentile(ep.policyUs,.95)
       <<",\"encode_us_median\":"<<percentile(ep.encodeUs,.5)
       <<",\"encode_us_p95\":"<<percentile(ep.encodeUs,.95)
       <<",\"population_us_median\":"<<percentile(ep.populationUs,.5)
       <<",\"population_us_p95\":"<<percentile(ep.populationUs,.95)
       <<",\"blocked_spans\":"<<ep.blockedSpans<<",\"blocked_ms\":"<<ep.blockedMs
       <<",\"max_blocked_ms\":"<<ep.maxBlockedMs<<",\"disagreement_counts\":{";
    constexpr const char* heads[]={"forward","strafe","turn","walk","jump","climb"};
    for(size_t i=0;i<ep.disagreements.size();++i) {
      if(i)out<<',';
      out<<std::quoted(heads[i])<<':'<<ep.disagreements[i];
    }
    out<<"},\"disagreement_rates\":{";
    for(size_t i=0;i<ep.disagreements.size();++i) {
      if(i)out<<',';
      out<<std::quoted(heads[i])<<':'<<(ep.decisions?double(ep.disagreements[i])/double(ep.decisions):0);
    }
    out<<'}';
  }
  out<<",\"eye_layout\":[";
  for(size_t i=0;i<ep.eye.samples().size();++i) {
    if(i)out<<',';
    const auto& ray=ep.eye.samples()[i];out<<'['<<ray.yawDeg<<','<<ray.pitchDeg<<','<<ray.maxRangeCm<<']';
  }
  out<<']';
  if(ep.population)
    out<<",\"population_index\":"<<ep.slot<<",\"pair_index\":"<<ep.pairIndex
       <<",\"episode_index\":"<<ep.sequence<<",\"sampling\":"<<ep.sampled
       <<",\"start_path_cm\":"<<ep.startPathCm<<",\"remaining_path_cm\":"<<ep.lastPathCm
       <<",\"progress_cm\":"<<ep.progressCm<<",\"invalid_reward_paths\":"<<ep.invalidRewardPaths;
  out<<",\"min_hero_distance_cm\":"<<ep.minHeroDistanceCm<<",\"max_hero_distance_cm\":"<<ep.maxHeroDistanceCm
     <<",\"physics_mode\":"<<std::quoted(ep.cvMode?"cv":"native")
     <<",\"hero_mode\":"<<std::quoted(ep.heroNear?"near":"keep")
     <<",\"initial_state\":"<<(ep.initialState.empty()?"{}":ep.initialState)
     <<",\"expected_start_position_cm\":";vector(out,ep.expectedStart);
  out<<",\"final_hp\":"<<ep.npc->attribute(ATR_HITPOINTS)<<",\"final_dead\":"<<ep.npc->isDead()
     <<",\"final_down\":"<<ep.npc->isDown()<<",\"final_ragdoll\":"<<ep.npc->hasRagdoll()
     <<",\"final_state_started\":"<<ep.npc->isStateStarted()<<",\"final_gru_l2\":"<<hiddenL2(ep)
     <<",\"reset_after_death\":"<<ep.resetAfterDeath<<",\"kill_injected\":"<<ep.killInjected
     <<",\"kill_at_ms\":"<<ep.killAt;
  if(ep.population)out<<",\"directory\":"<<std::quoted(ep.output);
  out<<"}\n";
  NpcKeys::enable(*ep.npc,false);ep.npc->stopWalking();ep.active=false;
}

// This route is the complete teacher state. No remembered successful action or recovery
// phase is needed to query it from a student's pose. Waynet is privileged teacher data.
NavMesh::Path plan(Episode& ep,World& world,bool blocked) {
  auto& n=*ep.npc;const auto& mesh=NavBench::navigationMesh(world);
  auto result=mesh.findPath(meters(n.position()),meters(ep.goal));
  if(result.complete && result.length<=200.f && !blocked)return result;
  const auto* endpoint=world.findWayPoint(ep.goal,[](const WayPoint& p){return p.isConnected();});
  if(!endpoint)return result;
  auto route=world.wayTo(n,*endpoint);
  std::vector<Vec3> points{n.position()};
  float length=0;
  while(const auto* point=route.pop()) {
    length+=(point->groundPos-points.back()).length();points.push_back(point->groundPos);
  }
  const auto tail=mesh.findPath(meters(endpoint->groundPos),meters(ep.goal));
  length+=tail.length*100;
  // As in 2b, prefer the native-road shortcut when the walking mesh exceeds the budget.
  if(result.complete && !blocked && !(result.length>200.f && length<result.length*80.f))return result;
  if(points.size()<2 || !tail.complete)return result;
  NavMesh::Path fallback;fallback.complete=true;fallback.reason="waynet_fallback";
  fallback.corners.push_back(meters(n.position()));
  for(size_t i=1;i<points.size();++i) {
    auto segment=mesh.findPath(meters(points[i-1]),meters(points[i]));
    if(!segment.complete) {
      // A verified authored road may connect mesh components; still execute it with keys.
      fallback.corners.push_back(meters(points[i]));continue;
    }
    const size_t offset=fallback.corners.size()-1;
    fallback.corners.insert(fallback.corners.end(),segment.corners.begin()+1,segment.corners.end());
    for(auto link:segment.transitions) {link.cornerIndex+=offset;fallback.transitions.push_back(link);}
  }
  const size_t offset=fallback.corners.size()-1;
  fallback.corners.insert(fallback.corners.end(),tail.corners.begin()+1,tail.corners.end());
  for(auto link:tail.transitions) {link.cornerIndex+=offset;fallback.transitions.push_back(link);}
  for(size_t i=1;i<fallback.corners.size();++i)fallback.length+=(cm(fallback.corners[i])-cm(fallback.corners[i-1])).length()*.01f;
  return fallback;
}
void observe(Episode& ep,World& w) {
  auto& n=*ep.npc;
  const auto eyeStart=Clock::now();
  ep.eye.update(n,w.tickCount());
  if(ep.population)ep.phaseUs[0]+=micros(eyeStart);
  const auto memoryStart=Clock::now();
  NavigationMemory::Pose pose;
  pose.position=n.position();pose.forward={std::cos(n.rotationRad()),0,std::sin(n.rotationRad())};pose.gameMs=w.tickCount();
  ep.memory.update(ep.eye.samples(),pose);
  if(ep.population)ep.phaseUs[1]+=micros(memoryStart);
}
NpcKeys::KeyState select(Episode& ep,NavPolicy::Result result) {
  ep.selected=std::move(result);
  for(size_t i=0;i<6;++i) {
    const auto& head=ep.selected.logits[i];
    const double maximum=*std::max_element(head.begin(),head.end());
    std::vector<double> weights;double total=0;
    for(float value:head) {weights.push_back(std::exp(double(value)-maximum));total+=weights.back();}
    size_t index=size_t(std::max_element(head.begin(),head.end())-head.begin());
    if(ep.sampled) {
      const double draw=std::uniform_real_distribution<double>(0.,total)(population.rngs[ep.slot]);
      double cumulative=0;index=weights.size()-1;
      for(size_t j=0;j<weights.size();++j) {cumulative+=weights[j];if(draw<cumulative) {index=j;break;}}
    }
    ep.selectedIndices[i]=index;ep.selectedLogProb[i]=double(head[index])-maximum-std::log(total);
  }
  NpcKeys::KeyState keys;
  keys.forward=float(int(ep.selectedIndices[0])-1);keys.strafe=float(int(ep.selectedIndices[1])-1);
  keys.turn=float(int(ep.selectedIndices[2])-1);keys.walk=ep.selectedIndices[3]!=0;
  keys.jump=ep.selectedIndices[4]!=0;keys.climb=ep.selectedIndices[5]!=0;
  return keys;
}
void decide(Episode& ep,World& w) {
  auto& n=*ep.npc;const auto elapsed=w.tickCount()-ep.began;
  trajectory(ep,elapsed);
  if(ep.replay) {
    if(ep.replayIndex>=ep.replayActions.size())throw std::runtime_error("navdemos missing replay action");
    const auto& a=ep.replayActions[ep.replayIndex++];
    if(a.time!=elapsed)throw std::runtime_error("navdemos replay clock mismatch");
    NpcKeys::set(n,a.keys,1000);ep.previous=a.keys;++ep.decisions;return;
  }
  if(ep.drive || ep.teacherVersion==2)measureBlocked(ep,elapsed);
  reward(ep,w);
  const auto teacherStart=Clock::now();
  NavKeyTeacher::State body;
  const auto position=ep.teacherVersion==2 ? NpcKeys::physicalPosition(n) : n.position();
  body.position=meters(position);body.velocity=meters(ep.velocity);body.yawDegrees=n.rotation();
  body.inAir=n.isInAir();body.falling=n.isFalling();body.sliding=n.isSlide();
  body.climbing=n.navigationClimbing();body.swimming=n.isSwim();body.jumping=n.isJump()||n.isJumpUp();
  body.standing=n.isStanding();
  body.supported=NavTransitions::supported(w,n);
  body.turnSpeedDegrees=n.weaponState()==WeaponState::NoWeapon?90.f:180.f;
  NavKeyTeacher::Decision d;
  if(ep.teacherVersion==2) {
    KnownRoadsBench::readGuidance(w,n,position,ep.goal,ep.guidance);
    if(ep.guidance.rebuilt && ep.record) {
      ep.knownPaths<<"{\"elapsed_ms\":"<<elapsed<<",\"points_cm\":[";
      for(size_t i=0;i<ep.guidance.polyline.size();++i) {
        if(i)ep.knownPaths<<',';
        vector(ep.knownPaths,ep.guidance.polyline[i]);
      }
      ep.knownPaths<<"]}\n";
      if(!ep.knownPaths)throw std::runtime_error("navdemos known path write failed");
    }
    const auto carrot=meters(ep.guidance.carrot);
    const auto& mesh=NavBench::navigationMesh(w);
    const bool airborne=body.inAir || body.falling || body.jumping || body.climbing;
    // The only privileged query is a local obstacle route to the shared carrot.
    // In flight retain the takeoff route, just as in version 1.
    if(!airborne || ep.path.corners.empty()) {
      ep.path=mesh.findPath(body.position,carrot);++ep.replans;
    }
    const bool direct=!airborne && !ep.guidance.beyondKnown &&
                      mesh.straightWalkable(body.position,carrot);
    d=NavKeyTeacher::decideV2(body,ep.path,meters(ep.goal),carrot,direct);
  } else {
    const auto present=NavKeyTeacher::decide(body,ep.path,meters(ep.goal));
    const bool followingRoad=ep.path.reason=="waynet_fallback" && present.pathValid && !present.replan;
    if((body.supported && !followingRoad) || ep.path.corners.empty()) {
      // A commanded step with no body velocity chooses a road alternative directly from
      // the present state. Recovery has no timeout or hidden action phase to reconstruct.
      const bool blocked=ep.previous.forward>0 && ep.previous.turn==0 &&
                         std::hypot(ep.velocity.x,ep.velocity.z)<5.f;
      ep.path=plan(ep,w,blocked);++ep.replans;
      if(ep.path.reason=="waynet_fallback")++ep.fallbacks;
    }
    d=NavKeyTeacher::decide(body,ep.path,meters(ep.goal));
  }
  if(ep.population)ep.phaseUs[2]+=micros(teacherStart);
  const auto viewStart=Clock::now();
  ep.memory.view(ep.view);
  if(ep.population)ep.phaseUs[1]+=micros(viewStart);
  NavDemoWriter::Snapshot sample;
  sample.gameMs=w.tickCount();sample.position=array(NpcKeys::physicalPosition(n));sample.bodyYawDeg=n.rotation();
  sample.body.velocityCmS=local(ep.velocity,n.rotationRad());
  sample.body.flags={uint8_t(body.inAir),uint8_t(body.falling),uint8_t(body.sliding),uint8_t(body.climbing),uint8_t(body.swimming)};
  const auto normal=n.navigationGroundNormal();sample.body.groundNormal=local(normal,n.rotationRad());
  sample.body.groundSlopeDeg=std::acos(std::clamp(normal.y,-1.f,1.f))*180.f/Pi;
  const auto head=n.navigationEyePose().forward;
  sample.body.headYawDeg=std::remainder(std::atan2(head.z,head.x)*180.f/Pi-n.rotation(),360.f);
  sample.body.headPitchDeg=std::atan2(head.y,std::hypot(head.x,head.z))*180.f/Pi;
  if(ep.teacherVersion==2) {
    sample.roads.carrotDirection=direction(ep.guidance.carrot-position,n.rotationRad());
    sample.roads.carrotDistanceCm=(ep.guidance.carrot-position).length();
    sample.roads.remainingKnownCm=float(ep.guidance.remainingKnownCm);
    sample.roads.beyondKnown=ep.guidance.beyondKnown;
  } else {
    Vec3 next;const auto roads=KnownRoadsBench::readPath(w,n,ep.goal,next);
    sample.roads.nextDirection=direction(next-n.position(),n.rotationRad());sample.roads.nextDistanceCm=(next-n.position()).length();
    sample.roads.remainingVisualCm=float(roads.remainingCm);sample.roads.goalInKnown=roads.goalInKnown;
  }
  sample.roads.goalDirection=direction(ep.goal-position,n.rotationRad());sample.roads.goalDistanceCm=(ep.goal-position).length();
  sample.previous=ep.previous;sample.keys=d.keys;sample.eye=&ep.eye.samples();sample.memory=&ep.view;
  auto hint=d.hint;
  if(ep.teacherVersion==1 && ep.path.reason=="waynet_fallback") {
    const auto meshPath=NavBench::navigationMesh(w).findPath(body.position,meters(ep.goal));
    hint=NavKeyTeacher::decide(body,meshPath,meters(ep.goal)).hint;
  }
  sample.teacherCorner=array(cm(hint.nextCorner));sample.teacherPathCm=hint.remainingLength*100;
  auto executed=d.keys;
  if(ep.drive) {
    // Encoding reads only learner fields. The very same snapshot is written below with
    // the teacher's label, while previous keeps the action that the student actually held.
    const bool measure=ep.decisions>=PolicyWarmupDecisions;
    const auto populationStart=std::chrono::steady_clock::now();
    const auto features=ep.policy->encode(sample);
    const auto encodeCost=micros(populationStart);
    if(measure)ep.encodeUs.push_back(encodeCost);
    if(ep.population)ep.phaseUs[3]+=encodeCost;
    for(size_t i=0;i<ep.policyStates.size();++i) {
      const auto began=std::chrono::steady_clock::now();
      auto result=ep.policy->step(features,ep.policyStates[i]);
      const auto policyCost=micros(began);
      if(measure)ep.policyUs.push_back(policyCost);
      if(ep.population)ep.phaseUs[4]+=policyCost;
      if(i==0)executed=ep.population?select(ep,std::move(result)):result.keys;
    }
    if(measure)ep.populationUs.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-populationStart).count());
    executed.headYaw=0;executed.headPitch=0;
    const std::array<bool,6> disagree={executed.forward!=d.keys.forward,executed.strafe!=d.keys.strafe,
      executed.turn!=d.keys.turn,executed.walk!=d.keys.walk,executed.jump!=d.keys.jump,executed.climb!=d.keys.climb};
    for(size_t i=0;i<disagree.size();++i)ep.disagreements[i]+=disagree[i];
    // The recurrent student observes every visited state. The same state's
    // teacher label is always recorded, regardless of who presses the keys.
    if(std::bernoulli_distribution(ep.teacherBeta)(ep.mixingRng)) {
      executed=d.keys;++ep.teacherControlDecisions;
    }
  }
  const auto recordStart=Clock::now();
  if(ep.writer) {const double cost=ep.writer->append(sample);ep.writeUs+=cost;ep.maxWriteUs=std::max(ep.maxWriteUs,cost);}
  action(ep,elapsed,executed);NpcKeys::set(n,executed,1000);ep.previous=executed;++ep.decisions;
  if(ep.population) {ep.phaseUs[6]+=micros(recordStart);ep.pending=true;ep.actionAt=w.tickCount();}
  if(d.hint.remainingLength*100+20<ep.best) {ep.best=d.hint.remainingLength*100;ep.progressAt=w.tickCount();}
  if(!d.pathValid)ep.failure="no_path:"+ep.path.reason;
  else if((ep.drive || ep.teacherVersion==2)?ep.blocked:w.tickCount()-ep.progressAt>=3000)ep.failure="body_blocked";
  else ep.failure.clear();
}
std::vector<Pair> readPairs(const std::string& path) {
  std::ifstream in(path);
  if(!in)throw std::runtime_error("navdemos cannot read pairs "+path);
  std::vector<Pair> result;std::string line;
  while(std::getline(in,line)) {
    if(line.empty() || line[0]=='#')continue;
    std::istringstream row(line);Pair pair;std::string extra;
    if(!(row>>pair.from>>pair.to) || row>>extra)throw std::runtime_error("navdemos pairs require FROM TO per line");
    result.push_back(std::move(pair));
  }
  if(result.empty())throw std::runtime_error("navdemos empty pairs");
  return result;
}
void reachable(World& w,const std::string& path,const std::string& output) {
  const auto pairs=readPairs(path);
  std::ofstream out(std::filesystem::path(output)/"reachable.jsonl");
  if(!out)throw std::runtime_error("navdemos cannot write reachable pairs");
  out<<std::setprecision(10)<<std::boolalpha;
  const auto& mesh=NavBench::navigationMesh(w);
  for(size_t i=0;i<pairs.size();++i) {
    const auto& p=pairs[i];const auto* from=w.findPoint(p.from,false);const auto* to=w.findPoint(p.to,false);
    NavMesh::Path route;
    if(from && to)route=mesh.findPath(meters(from->groundPos),meters(to->groundPos));
    out<<"{\"pair_index\":"<<i<<",\"start\":"<<std::quoted(p.from)<<",\"goal\":"<<std::quoted(p.to)
       <<",\"complete\":"<<route.complete<<",\"length_cm\":"<<double(route.length)*100.<<"}\n";
  }
  if(!out)throw std::runtime_error("navdemos reachable pairs write failed");
}
void startPopulationEpisode(World& w,Npc& player,size_t slot,Npc* original=nullptr) {
  auto& pop=population;
  if(pop.started>=pop.targetEpisodes)return;
  auto& ep=*pop.slots[slot];
  auto* actor=original?original:ep.npc;
  const bool afterDeath=actor && actor->isDead();
  if(actor && !original)actor->resetNavigationEpisode();
  // Slots retain the same physical NPC across episodes. Besides avoiding a world
  // load, this bounds memory: World::removeNpc keeps removed NPCs for script refs.
  ep=Episode{};ep.npc=actor?actor:w.addNpc(pop.instance,pop.home);
  if(!ep.npc)throw std::runtime_error("navdemos cannot spawn population actor");
  pop.members.insert(ep.npc);
  ep.population=true;ep.drive=true;ep.record=pop.record;ep.sampled=pop.sampled;
  ep.cvMode=settings.cv;ep.heroNear=pop.near;ep.resetAfterDeath=afterDeath;
  ep.slot=slot;ep.sequence=pop.started++;ep.pairIndex=ep.sequence%pop.pairs.size();
  const auto& pair=pop.pairs[ep.pairIndex];ep.from=pair.from;ep.to=pair.to;
  ep.output=(std::filesystem::path(pop.output)/"episodes"/std::to_string(ep.sequence)).string();
  std::filesystem::create_directories(ep.output);
  ep.policy=pop.policy;ep.policyStates.resize(1);ep.teacherVersion=2;ep.limit=pop.limit;
  const auto* from=w.findPoint(ep.from,false);const auto* to=w.findPoint(ep.to,false);
  if(!from || !to)throw std::runtime_error("navdemos population missing waypoint");
  ep.goal=to->groundPos;
  ep.expectedStart=from->groundPos;
  auto& npc=*ep.npc;
  npc.setProcessPolicy(NpcProcessPolicy::AiNormal);
  npc.setInteraction(nullptr,true);npc.clearAiQueue();npc.clearState(false);npc.clearSpeed();
  npc.setExternalControl(true);npc.clearGoTo();npc.attachToPoint(nullptr);
  // Drive initializes road familiarity at the saved actor's home before teleporting.
  // Every slot uses that same home and its native routines; no route is granted here.
  npc.setPosition(pop.home);npc.updateTransform();
  KnownRoadsBench::command({"roads","on","id:"+std::to_string(w.npcId(&npc))},w,pop.output);
  npc.setPosition(from->groundPos);npc.updateTransform();npc.setDirection(ep.goal-npc.position());
  npc.setWalkMode(WalkBit::WM_Walk);
  if(pop.near) {player.setPosition(npc.position()+Vec3(0,0,500));player.updateTransform();}
  NpcKeys::enable(npc,true);ep.previousPosition=NpcKeys::physicalPosition(npc);
  ep.health=npc.attribute(ATR_HITPOINTS);
  ep.began=ep.next=ep.progressAt=w.tickCount();ep.active=true;ep.wallStart=Clock::now();
  ep.minHeroDistanceCm=ep.maxHeroDistanceCm=(npc.position()-player.position()).length();
  ep.startPathCm=ep.lastPathCm=rewardPath(ep,w);
  if(!ep.rewardPathValid)throw std::runtime_error("navdemos population start has no Detour path");
  if(ep.record) {
    ep.trajectory=file(ep,"trajectory.txt");ep.actions=file(ep,"actions.txt");
    ep.knownPaths=file(ep,"known-paths.jsonl");ep.samples=file(ep,"samples.jsonl");
    ep.writer=std::make_unique<NavDemoWriter>((std::filesystem::path(ep.output)/"samples.bin.gz").string(),2);
  }
  ep.initialState=initialState(ep);
  observe(ep,w);decide(ep,w);ep.next=ep.began+100;
}
void populationSummary(World& w) {
  auto& pop=population;
  const double wallSeconds=std::chrono::duration<double>(Clock::now()-pop.wallStart).count();
  const double collectSeconds=std::max(1e-9,wallSeconds-pop.startupSeconds);
  double accountedUs=0;for(double cost:pop.phaseUs)accountedUs+=cost;
  std::ofstream out(std::filesystem::path(pop.output)/"population.json");
  if(!out)throw std::runtime_error("navdemos cannot write population summary");
  out<<std::setprecision(10)<<std::boolalpha
     <<"{\"population\":"<<pop.slots.size()<<",\"seed\":"<<pop.seed<<",\"sampling\":"<<pop.sampled
     <<",\"record\":"<<pop.record<<",\"near\":"<<pop.near<<",\"episodes\":"<<pop.completed
     <<",\"physics_mode\":"<<std::quoted(settings.cv?"cv":"native")
     <<",\"kill_fixture_fired\":"<<settings.killFired
     <<",\"decisions\":"<<pop.decisions<<",\"measured_decisions\":"<<pop.decisions-pop.initialDecisions
     <<",\"elapsed_ms\":"<<w.tickCount()-pop.began<<",\"wall_seconds\":"<<wallSeconds
     <<",\"startup_seconds\":"<<pop.startupSeconds<<",\"collect_seconds\":"<<collectSeconds
     <<",\"decisions_per_second\":"<<double(pop.decisions-pop.initialDecisions)/collectSeconds
     <<",\"profile_us\":{";
  constexpr const char* names[]={"eye","memory","roads_teacher","encode","policy","reward","record"};
  for(size_t i=0;i<7;++i) {if(i)out<<',';out<<std::quoted(names[i])<<':'<<pop.phaseUs[i];}
  // Includes world physics/animation/AI, runner scheduling and bookkeeping. Initial
  // observations and policy calls are excluded together with startup wall time.
  out<<",\"world_and_other\":"<<std::max(0.,collectSeconds*1e6-accountedUs)<<"}}\n";
  if(!out)throw std::runtime_error("navdemos population summary write failed");
  pop.episodes.close();pop.active=false;pop.members.clear();
}
void beginPopulation(const std::vector<std::string>& a,World& w,Npc& player,const std::string& output) {
  if(a.size()<11 || a.size()>13)
    throw std::runtime_error("navdemos population NPC PAIRS LIMIT_MS WEIGHTS N SEED sample|argmax EPISODES RECORD [near|far [RUN_MS]]");
  if(legacy.active || population.active)throw std::runtime_error("navdemos already active");
  population=Population{};auto& pop=population;pop.output=output;
  pop.instance=w.script().findSymbolIndex(a[2]);auto* original=w.findNpcByInstance(pop.instance);
  if(!original)throw std::runtime_error("navdemos population missing NPC");
  pop.home=original->position();pop.pairs=readPairs(a[3]);pop.limit=std::stoull(a[4]);
  pop.policy=std::make_shared<NavPolicy>(a[5]);
  if(pop.policy->navDemoVersion()!=2)throw std::runtime_error("navdemos population requires a NAVDEMO v2 policy");
  const auto count=std::stoull(a[6]);pop.seed=std::stoull(a[7]);
  if(a[8]!="sample" && a[8]!="argmax")throw std::runtime_error("navdemos population action mode must be sample or argmax");
  pop.sampled=a[8]=="sample";pop.targetEpisodes=std::stoull(a[9]);
  if(a[10]!="0" && a[10]!="1")throw std::runtime_error("navdemos population RECORD must be 0 or 1");
  pop.record=a[10]=="1";
  if(a.size()>11 && a[11]!="near" && a[11]!="far")throw std::runtime_error("navdemos population hero mode must be near or far");
  pop.near=a.size()>11 && a[11]=="near";pop.runMs=a.size()>12?std::stoull(a[12]):0;
  if(!count || count>1000 || pop.targetEpisodes<count || !pop.limit || (pop.near && count!=1))
    throw std::runtime_error("navdemos invalid population/episode count, limit, or near mode");
  if(settings.killArmed && settings.killSlot>=count)throw std::runtime_error("navdemos kill slot outside population");
  // Validate the entire file once, including pairs reached only after an episode reset.
  const auto& mesh=NavBench::navigationMesh(w);
  for(const auto& pair:pop.pairs) {
    const auto* from=w.findPoint(pair.from,false);const auto* to=w.findPoint(pair.to,false);
    if(!from || !to || !mesh.findPath(meters(from->groundPos),meters(to->groundPos)).complete)
      throw std::runtime_error("navdemos population pair has no complete Detour path: "+pair.from+" "+pair.to);
  }
  pop.slots.reserve(count);pop.rngs.reserve(count);
  for(size_t i=0;i<count;++i) {
    pop.slots.push_back(std::make_unique<Episode>());
    // Separate streams persist across this slot's episodes and never share draws.
    std::seed_seq seed{uint32_t(pop.seed),uint32_t(pop.seed>>32),uint32_t(i),uint32_t(i>>32)};
    pop.rngs.emplace_back(seed);
  }
  pop.episodes.open(std::filesystem::path(output)/"episodes.jsonl");
  if(!pop.episodes)throw std::runtime_error("navdemos cannot write population episodes");
  pop.wallStart=Clock::now();pop.began=w.tickCount();pop.active=true;
  for(size_t i=0;i<count;++i)startPopulationEpisode(w,player,i,i==0?original:nullptr);
  pop.initialDecisions=count;pop.startupSeconds=std::chrono::duration<double>(Clock::now()-pop.wallStart).count();
  for(auto& slot:pop.slots)slot->phaseUs={};
}
void tickEpisode(Episode& ep,World& w,uint64_t dt,bool truncate=false) {
  if(!ep.active)return;
  auto& n=*ep.npc;++ep.frames;if(n.characterVirtualActive())++ep.cvFrames;
  const auto p=NpcKeys::physicalPosition(n);if(dt)ep.velocity=(p-ep.previousPosition)*(1000.f/float(dt));ep.previousPosition=p;
  if(ep.population)
    ep.fell=ep.fell || n.isFalling() || n.isInAir();
  if(const auto* player=w.player()) {
    const double distance=(n.position()-player->position()).length();
    ep.minHeroDistanceCm=std::min(ep.minHeroDistanceCm,distance);ep.maxHeroDistanceCm=std::max(ep.maxHeroDistanceCm,distance);
  }
  const auto elapsed=w.tickCount()-ep.began;
  const bool arrived=(n.position()-ep.goal).length()<100.f && !n.isInAir() && !n.navigationClimbing();
  if((!ep.replay && arrived) || elapsed>=ep.limit || n.isDown() || truncate) {
    ep.success=arrived;
    if(!ep.success && ep.failure.empty())ep.failure=n.isDown()?"actor_down":truncate?"collection_limit":"path_timeout";
    if(ep.population) {measureBlocked(ep,elapsed);reward(ep,w,arrived || n.isDown(),!arrived && !n.isDown());}
    finish(ep,w);return;
  }
  observe(ep,w);
  if(w.tickCount()>=ep.next) {decide(ep,w);ep.next+=100;}
}
void tickPopulation(World& w,uint64_t dt) {
  auto& pop=population;const bool stop=pop.runMs && w.tickCount()-pop.began>=pop.runMs;
  for(size_t i=0;i<pop.slots.size();++i) {
    auto& ep=*pop.slots[i];if(!ep.active)continue;
    const auto previousDecisions=ep.decisions;
    tickEpisode(ep,w,dt,stop);
    // Inject after a decision boundary so the final transition still fits one
    // decision period. Leave the episode alive for a real world tick: native
    // ZS_Dead initialization, ragdoll animation and physics run before closure.
    if(ep.active && settings.killArmed && !settings.killFired && i==settings.killSlot &&
       w.tickCount()-pop.began>=settings.killAfterMs && ep.decisions>previousDecisions) {
      NpcKeys::enable(*ep.npc,false);ep.npc->setExternalControl(false);
      ep.npc->setRagdollDown(true);ep.npc->prepareRagdoll();
      ep.killInjected=true;ep.killAt=w.tickCount()-pop.began;ep.failure="actor_down";settings.killFired=true;
    }
    if(ep.active)continue;
    ++pop.completed;pop.decisions+=ep.decisions;
    for(size_t j=0;j<7;++j)pop.phaseUs[j]+=ep.phaseUs[j];
    std::ifstream input(std::filesystem::path(ep.output)/"episode.json");pop.episodes<<input.rdbuf();
    if(!pop.episodes)throw std::runtime_error("navdemos population episodes write failed");
    if(!stop && pop.started<pop.targetEpisodes)startPopulationEpisode(w,*w.player(),i);
    else {
      // Keep the actor's suspended routines and release all movement inputs.
      NpcKeys::enable(*ep.npc,true);NpcKeys::set(*ep.npc,{},1000);
      pop.members.erase(ep.npc);
    }
  }
  if(std::none_of(pop.slots.begin(),pop.slots.end(),[](const auto& slot){return slot->active;}))populationSummary(w);
}

}

bool NavDemos::running() {return legacy.active || population.active;}
bool NavDemos::forceCharacterVirtual(const Npc& npc) {
  return settings.cv && ((legacy.active && legacy.npc==&npc) || preparingLegacy==&npc ||
                          (population.active && population.members.contains(&npc)));
}
bool NavDemos::fullSimulation(const Npc& npc) {
  return (population.active && population.members.contains(&npc)) || forceCharacterVirtual(npc);
}
void NavDemos::reset() {legacy=Episode{};population=Population{};settings=Settings{};preparingLegacy=nullptr;}
void NavDemos::command(const std::vector<std::string>& a,World& w,Npc& player,const std::string& output) {
  auto& ep=legacy;
  if(a.size()>=2 && (a[1]=="physics" || a[1]=="hero" || a[1]=="kill-slot")) {
    if(running())throw std::runtime_error("navdemos cannot change settings during an episode");
    if(a[1]=="physics" && a.size()==3 && (a[2]=="cv" || a[2]=="native"))settings.cv=a[2]=="cv";
    else if(a[1]=="hero" && a.size()==3 && (a[2]=="near" || a[2]=="keep"))settings.heroNear=a[2]=="near";
    else if(a[1]=="kill-slot" && a.size()==4) {
      settings.killSlot=std::stoull(a[2]);settings.killAfterMs=std::stoull(a[3]);
      settings.killArmed=true;settings.killFired=false;
    } else throw std::runtime_error("navdemos physics cv|native | hero near|keep | kill-slot SLOT AFTER_MS");
    return;
  }
  if(a.size()>=2 && a[1]=="population") {beginPopulation(a,w,player,output);return;}
  if(a.size()==3 && a[1]=="reachable") {reachable(w,a[2],output);return;}
  if(a.size()==2 && a[1]=="wait")return;
  const bool replay=a.size()==6 && a[1]=="replay";
  const bool drive=a.size()>=7 && a.size()<=9 && a[1]=="drive";
  const bool current=a.size()>=6 && a.size()<=8 && a[1]=="drive-current";
  const bool speed=a.size()==8 && a[1]=="speed";
  const bool learner=drive || current || speed;
  const bool carrot=a.size()>=2 && a[1]=="start-v2";
  if(!replay && !learner && !((a.size()==6 || a.size()==8 || a.size()==9) && (a[1]=="start" || carrot)))
    throw std::runtime_error("navdemos start|start-v2 NPC FROM TO LIMIT [lateral_cm yaw_deg fraction] | replay NPC FROM TO ACTIONS | drive NPC FROM TO LIMIT WEIGHTS [BETA SEED] | drive-current NPC GOAL LIMIT WEIGHTS [BETA SEED] | speed NPC FROM TO LIMIT WEIGHTS POPULATION");
  if(ep.active || population.active)throw std::runtime_error("navdemos already active");
  const auto* from=current?nullptr:w.findPoint(a[3],false);const auto* to=w.findPoint(a[current?3:4],false);
  auto* npc=w.findNpcByInstance(w.script().findSymbolIndex(a[2]));
  if((!from && !current) || !to || !npc)throw std::runtime_error("navdemos missing NPC/waypoint");
  ep=Episode{};ep.npc=npc;ep.from=current?"intentional":a[3];ep.to=a[current?3:4];ep.output=output;
  ep.cvMode=settings.cv;ep.heroNear=!current && settings.heroNear;
  ep.teacherVersion=carrot?2:1;
  ep.goal=to->groundPos;ep.replay=replay;ep.drive=learner;
  // Optional start arguments begin at 6.
  ep.limit=replay?0:std::stoull(a[current?4:5]);
  if(learner) {
    const auto population=speed?std::stoul(a[7]):1;
    if(population==0 || population>1000)throw std::runtime_error("navdemos invalid policy population");
    ep.policy=std::make_unique<NavPolicy>(a[current?5:6]);
    ep.teacherVersion=ep.policy->navDemoVersion();
    const size_t betaIndex=current?6:7;
    if(!speed && a.size()>betaIndex)ep.teacherBeta=std::stod(a[betaIndex]);
    if(!std::isfinite(ep.teacherBeta) || ep.teacherBeta<0 || ep.teacherBeta>1)
      throw std::runtime_error("navdemos teacher beta must be in [0,1]");
    if(!speed && a.size()>betaIndex+1)ep.mixingSeed=std::stoull(a[betaIndex+1]);
    ep.mixingRng.seed(ep.mixingSeed);
    ep.policyStates.resize(population);
    ep.policyUs.reserve(size_t((ep.limit+99)/100)*population);
    ep.encodeUs.reserve(size_t((ep.limit+99)/100));
    ep.populationUs.reserve(size_t((ep.limit+99)/100));
  }
  const PrepareLegacy preparing(npc);
  if(settings.cv)npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->clearSpeed();
  npc->setExternalControl(true);npc->clearGoTo();
  if(!current) {npc->setPosition(from->groundPos);npc->updateTransform();}
  // Keyboard episodes must not inherit a native route's waypoint from the save.
  npc->attachToPoint(nullptr);
  if(!current)npc->setDirection(ep.goal-npc->position());
  npc->setWalkMode(WalkBit::WM_Walk);
  if(!replay && !learner && a.size()>6) {
    const auto path=NavBench::navigationMesh(w).findPath(meters(from->groundPos),meters(ep.goal));
    if(!path.complete || path.corners.size()<2) {
      auto out=file(ep,"episode.json");
      out<<"{\"success\":false,\"failure\":\"dagger_start_not_supported\",\"elapsed_ms\":0,\"decisions\":0}\n";
      return;
    }
    float left=path.length*(a.size()>8?std::stof(a[8]):.5f);
    Vec3 point=from->groundPos,tangent=ep.goal-point;
    for(size_t i=1;i<path.corners.size();++i) {
      const auto begin=cm(path.corners[i-1]),end=cm(path.corners[i]);const float length=(end-begin).length()*.01f;
      if(left<=length || i+1==path.corners.size()) {point=begin+(end-begin)*std::clamp(left/std::max(length,.001f),0.f,1.f);tangent=end-begin;break;}
      left-=length;
    }
    tangent.y=0;const float length=tangent.length();
    if(length>0)point+=Vec3(-tangent.z,0,tangent.x)*(std::stof(a[6])/length);
    const auto floor=w.physic()->landRay(point+Vec3(0,200,0),500.f);
    if(!floor.hasCol || floor.n.y<.65f) {
      auto out=file(ep,"episode.json");
      out<<"{\"success\":false,\"failure\":\"dagger_start_not_supported\",\"elapsed_ms\":0,\"decisions\":0}\n";
      return;
    }
    point=floor.v;
    npc->setPosition(point);npc->updateTransform();npc->setDirection(std::stof(a[7]));
  }
  if(!current && settings.heroNear) {player.setPosition(npc->position()+Vec3(0,0,500));player.updateTransform();}
  NpcKeys::enable(*npc,true);ep.previousPosition=NpcKeys::physicalPosition(*npc);
  ep.began=ep.next=ep.progressAt=w.tickCount();ep.active=true;ep.wallStart=std::chrono::steady_clock::now();
  ep.expectedStart=NpcKeys::physicalPosition(*npc);
  ep.minHeroDistanceCm=ep.maxHeroDistanceCm=(npc->position()-player.position()).length();
  ep.trajectory=file(ep,"trajectory.txt");
  if(replay) {
    std::ifstream input(a[5]);RecordedKeys row;int walk=0,jump=0,climb=0;
    while(input>>row.time>>row.keys.forward>>row.keys.strafe>>row.keys.turn>>walk>>jump>>climb>>row.keys.headYaw>>row.keys.headPitch) {
      row.keys.walk=walk!=0;row.keys.jump=jump!=0;row.keys.climb=climb!=0;ep.replayActions.push_back(row);
    }
    if(!input.eof() || ep.replayActions.size()<2)throw std::runtime_error("invalid actions replay");
    ep.limit=ep.replayActions.back().time;
  } else {
    ep.actions=file(ep,"actions.txt");ep.writer=std::make_unique<NavDemoWriter>((std::filesystem::path(output)/"samples.bin.gz").string(),ep.teacherVersion);
    if(ep.teacherVersion==2)ep.knownPaths=file(ep,"known-paths.jsonl");
  }
  ep.initialState=initialState(ep);
  observe(ep,w);decide(ep,w);ep.next=ep.began+100;
}
void NavDemos::tick(World& w,uint64_t dt) {
  if(population.active)tickPopulation(w,dt);
  else tickEpisode(legacy,w,dt);
}
