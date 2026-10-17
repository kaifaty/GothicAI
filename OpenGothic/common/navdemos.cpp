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
#include <stdexcept>

namespace {
using Tempest::Vec3;
constexpr float Pi=3.14159265358979323846f;
constexpr uint64_t PolicyWarmupDecisions=30;
struct RecordedKeys { uint64_t time=0; NpcKeys::KeyState keys; };
struct MotionSample { uint64_t time=0; Vec3 position; bool commanded=false; };
struct Episode {
  Npc* npc=nullptr;
  Vec3 goal,previousPosition,velocity;
  std::string output,from,to,failure;
  bool active=false,replay=false,drive=false,success=false;
  uint64_t began=0,limit=240000,next=0,decisions=0,frames=0,cvFrames=0;
  uint64_t progressAt=0;
  float best=std::numeric_limits<float>::max();
  size_t replayIndex=0;
  uint64_t replans=0,fallbacks=0;
  double writeUs=0,maxWriteUs=0;
  std::unique_ptr<NavPolicy> policy;
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
  std::ofstream actions,trajectory;
  std::vector<RecordedKeys> replayActions;
} ep;

NavMesh::Point meters(const Vec3& p) {return {p.x*.01f,p.y*.01f,p.z*.01f};}
Vec3 cm(const NavMesh::Point& p) {return {p[0]*100,p[1]*100,p[2]*100};}
std::array<float,3> array(const Vec3& p) {return {p.x,p.y,p.z};}
void vector(std::ostream& out,const Vec3& p) {out<<'['<<p.x<<','<<p.y<<','<<p.z<<']';}
std::array<float,3> local(const Vec3& p,float yaw) {
  const float c=std::cos(yaw),s=std::sin(yaw);
  return {-p.x*s+p.z*c,p.y,p.x*c+p.z*s};
}
std::array<float,3> direction(Vec3 delta,float yaw) {
  const float len=delta.length();if(len>0)delta*=1.f/len;return local(delta,yaw);
}
std::ofstream file(const char* name) {
  std::ofstream out(std::filesystem::path(ep.output)/name);
  if(!out)throw std::runtime_error(std::string("navdemos cannot write ")+name);
  out<<std::setprecision(9)<<std::boolalpha;return out;
}
void trajectory(uint64_t elapsed) {
  const auto p=NpcKeys::physicalPosition(*ep.npc);
  ep.trajectory<<elapsed<<' '<<p.x<<' '<<p.y<<' '<<p.z<<' '<<ep.npc->rotation()<<'\n';
  if(!ep.trajectory)throw std::runtime_error("navdemos trajectory write failed");
}
void action(uint64_t elapsed,const NpcKeys::KeyState& k) {
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
void closeBlocked(uint64_t elapsed) {
  if(!ep.blocked)return;
  const auto duration=elapsed-ep.blockedSince;
  ep.blockedMs+=duration;ep.maxBlockedMs=std::max(ep.maxBlockedMs,duration);
  ep.blockedUntil=elapsed;ep.blocked=false;
}
void measureBlocked(uint64_t elapsed) {
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
  else if(!stalled)closeBlocked(elapsed);
}
void finish(World& world) {
  const auto elapsed=world.tickCount()-ep.began;
  closeBlocked(elapsed);
  trajectory(elapsed);
  if(!ep.replay) {NpcKeys::KeyState end;end.walk=true;action(elapsed,end);ep.writer->finish();}
  ep.trajectory.close();ep.actions.close();
  auto out=file("episode.json");
  const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-ep.wallStart).count();
  out<<"{\"success\":"<<ep.success<<",\"replay\":"<<ep.replay<<",\"drive\":"<<ep.drive<<",\"start\":"<<std::quoted(ep.from)
     <<",\"goal\":"<<std::quoted(ep.to)<<",\"elapsed_ms\":"<<elapsed<<",\"limit_ms\":"<<ep.limit
     <<",\"distance_cm\":"<<(ep.npc->position()-ep.goal).length()<<",\"failure\":"<<std::quoted(ep.failure)
     <<",\"decisions\":"<<ep.decisions<<",\"record_us_mean\":"<<(ep.decisions?ep.writeUs/double(ep.decisions):0)
     <<",\"record_us_max\":"<<ep.maxWriteUs<<",\"wall_seconds\":"<<seconds
     <<",\"decisions_per_second\":"<<double(ep.decisions)/seconds<<",\"frames\":"<<ep.frames
     <<",\"cv_frames\":"<<ep.cvFrames<<",\"replans\":"<<ep.replans<<",\"fallbacks\":"<<ep.fallbacks
     <<",\"final_position\":";vector(out,NpcKeys::physicalPosition(*ep.npc));
  out<<",\"target_position\":";vector(out,ep.goal);
  out<<",\"next_corner\":";vector(out,ep.path.corners.empty()?ep.goal:cm(ep.path.corners.back()));
  if(ep.drive) {
    out<<",\"policy_population\":"<<ep.policyStates.size()
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
  out<<"}\n";
  NpcKeys::enable(*ep.npc,false);ep.npc->stopWalking();ep.active=false;
}

// This route is the complete teacher state. No remembered successful action or recovery
// phase is needed to query it from a student's pose. Waynet is privileged teacher data.
NavMesh::Path plan(World& world,bool blocked) {
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
void observe(World& w) {
  auto& n=*ep.npc;
  ep.eye.update(n,w.tickCount());
  NavigationMemory::Pose pose;
  pose.position=n.position();pose.forward={std::cos(n.rotationRad()),0,std::sin(n.rotationRad())};pose.gameMs=w.tickCount();
  ep.memory.update(ep.eye.samples(),pose);
}
void decide(World& w) {
  auto& n=*ep.npc;const auto elapsed=w.tickCount()-ep.began;
  trajectory(elapsed);
  if(ep.replay) {
    if(ep.replayIndex>=ep.replayActions.size())throw std::runtime_error("navdemos missing replay action");
    const auto& a=ep.replayActions[ep.replayIndex++];
    if(a.time!=elapsed)throw std::runtime_error("navdemos replay clock mismatch");
    NpcKeys::set(n,a.keys,1000);ep.previous=a.keys;++ep.decisions;return;
  }
  if(ep.drive)measureBlocked(elapsed);
  NavKeyTeacher::State body;
  body.position=meters(n.position());body.velocity=meters(ep.velocity);body.yawDegrees=n.rotation();
  body.inAir=n.isInAir();body.falling=n.isFalling();body.sliding=n.isSlide();
  body.climbing=n.navigationClimbing();body.swimming=n.isSwim();body.jumping=n.isJump()||n.isJumpUp();
  body.standing=n.isStanding();
  body.supported=NavTransitions::supported(w,n);
  body.turnSpeedDegrees=n.weaponState()==WeaponState::NoWeapon?90.f:180.f;
  const auto present=NavKeyTeacher::decide(body,ep.path,meters(ep.goal));
  const bool followingRoad=ep.path.reason=="waynet_fallback" && present.pathValid && !present.replan;
  if((body.supported && !followingRoad) || ep.path.corners.empty()) {
    // A commanded step with no body velocity chooses a road alternative directly from
    // the present state. Recovery has no timeout or hidden action phase to reconstruct.
    const bool blocked=ep.previous.forward>0 && ep.previous.turn==0 &&
                       std::hypot(ep.velocity.x,ep.velocity.z)<5.f;
    ep.path=plan(w,blocked);++ep.replans;
    if(ep.path.reason=="waynet_fallback")++ep.fallbacks;
  }
  const auto d=NavKeyTeacher::decide(body,ep.path,meters(ep.goal));
  ep.memory.view(ep.view);
  NavDemoWriter::Snapshot sample;
  sample.gameMs=w.tickCount();sample.position=array(NpcKeys::physicalPosition(n));sample.bodyYawDeg=n.rotation();
  sample.body.velocityCmS=local(ep.velocity,n.rotationRad());
  sample.body.flags={uint8_t(body.inAir),uint8_t(body.falling),uint8_t(body.sliding),uint8_t(body.climbing),uint8_t(body.swimming)};
  const auto normal=n.navigationGroundNormal();sample.body.groundNormal=local(normal,n.rotationRad());
  sample.body.groundSlopeDeg=std::acos(std::clamp(normal.y,-1.f,1.f))*180.f/Pi;
  const auto head=n.navigationEyePose().forward;
  sample.body.headYawDeg=std::remainder(std::atan2(head.z,head.x)*180.f/Pi-n.rotation(),360.f);
  sample.body.headPitchDeg=std::atan2(head.y,std::hypot(head.x,head.z))*180.f/Pi;
  Vec3 next;const auto roads=KnownRoadsBench::readPath(w,n,ep.goal,next);
  sample.roads.nextDirection=direction(next-n.position(),n.rotationRad());sample.roads.nextDistanceCm=(next-n.position()).length();
  sample.roads.goalDirection=direction(ep.goal-n.position(),n.rotationRad());sample.roads.goalDistanceCm=(ep.goal-n.position()).length();
  sample.roads.remainingVisualCm=float(roads.remainingCm);sample.roads.goalInKnown=roads.goalInKnown;
  sample.previous=ep.previous;sample.keys=d.keys;sample.eye=&ep.eye.samples();sample.memory=&ep.view;
  auto hint=d.hint;
  if(ep.path.reason=="waynet_fallback") {
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
    if(measure)ep.encodeUs.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-populationStart).count());
    for(size_t i=0;i<ep.policyStates.size();++i) {
      const auto began=std::chrono::steady_clock::now();
      const auto result=ep.policy->step(features,ep.policyStates[i]);
      if(measure)ep.policyUs.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-began).count());
      if(i==0)executed=result.keys;
    }
    if(measure)ep.populationUs.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-populationStart).count());
    executed.headYaw=0;executed.headPitch=0;
    const std::array<bool,6> disagree={executed.forward!=d.keys.forward,executed.strafe!=d.keys.strafe,
      executed.turn!=d.keys.turn,executed.walk!=d.keys.walk,executed.jump!=d.keys.jump,executed.climb!=d.keys.climb};
    for(size_t i=0;i<disagree.size();++i)ep.disagreements[i]+=disagree[i];
  }
  const double cost=ep.writer->append(sample);ep.writeUs+=cost;ep.maxWriteUs=std::max(ep.maxWriteUs,cost);
  action(elapsed,executed);NpcKeys::set(n,executed,1000);ep.previous=executed;++ep.decisions;
  if(d.hint.remainingLength*100+20<ep.best) {ep.best=d.hint.remainingLength*100;ep.progressAt=w.tickCount();}
  if(!d.pathValid)ep.failure="no_path:"+ep.path.reason;
  else if(ep.drive?ep.blocked:w.tickCount()-ep.progressAt>=3000)ep.failure="body_blocked";
  else ep.failure.clear();
}
}

bool NavDemos::running() {return ep.active;}
void NavDemos::reset() {ep=Episode{};}
void NavDemos::command(const std::vector<std::string>& a,World& w,Npc& player,const std::string& output) {
  if(a.size()==2 && a[1]=="wait")return;
  const bool replay=a.size()==6 && a[1]=="replay";
  const bool drive=a.size()==7 && a[1]=="drive";
  const bool current=a.size()==6 && a[1]=="drive-current";
  const bool speed=a.size()==8 && a[1]=="speed";
  const bool learner=drive || current || speed;
  if(!replay && !learner && !((a.size()==6 || a.size()==8 || a.size()==9) && a[1]=="start"))
    throw std::runtime_error("navdemos start NPC FROM TO LIMIT [lateral_cm yaw_deg fraction] | replay NPC FROM TO ACTIONS | drive NPC FROM TO LIMIT WEIGHTS | drive-current NPC GOAL LIMIT WEIGHTS | speed NPC FROM TO LIMIT WEIGHTS POPULATION");
  if(ep.active)throw std::runtime_error("navdemos already active");
  const auto* from=current?nullptr:w.findPoint(a[3],false);const auto* to=w.findPoint(a[current?3:4],false);
  auto* npc=w.findNpcByInstance(w.script().findSymbolIndex(a[2]));
  if((!from && !current) || !to || !npc)throw std::runtime_error("navdemos missing NPC/waypoint");
  ep=Episode{};ep.npc=npc;ep.from=current?"intentional":a[3];ep.to=a[current?3:4];ep.output=output;
  ep.goal=to->groundPos;ep.replay=replay;ep.drive=learner;
  // Optional start arguments begin at 6.
  ep.limit=replay?0:std::stoull(a[current?4:5]);
  if(learner) {
    const auto population=speed?std::stoul(a[7]):1;
    if(population==0 || population>1000)throw std::runtime_error("navdemos invalid policy population");
    ep.policy=std::make_unique<NavPolicy>(a[current?5:6]);
    ep.policyStates.resize(population);
    ep.policyUs.reserve(size_t((ep.limit+99)/100)*population);
    ep.encodeUs.reserve(size_t((ep.limit+99)/100));
    ep.populationUs.reserve(size_t((ep.limit+99)/100));
  }
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
      auto out=file("episode.json");
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
      auto out=file("episode.json");
      out<<"{\"success\":false,\"failure\":\"dagger_start_not_supported\",\"elapsed_ms\":0,\"decisions\":0}\n";
      return;
    }
    point=floor.v;
    npc->setPosition(point);npc->updateTransform();npc->setDirection(std::stof(a[7]));
  }
  if(!current) {player.setPosition(npc->position()+Vec3(0,0,500));player.updateTransform();}
  NpcKeys::enable(*npc,true);ep.previousPosition=NpcKeys::physicalPosition(*npc);
  ep.began=ep.next=ep.progressAt=w.tickCount();ep.active=true;ep.wallStart=std::chrono::steady_clock::now();
  ep.trajectory=file("trajectory.txt");
  if(replay) {
    std::ifstream input(a[5]);RecordedKeys row;int walk=0,jump=0,climb=0;
    while(input>>row.time>>row.keys.forward>>row.keys.strafe>>row.keys.turn>>walk>>jump>>climb>>row.keys.headYaw>>row.keys.headPitch) {
      row.keys.walk=walk!=0;row.keys.jump=jump!=0;row.keys.climb=climb!=0;ep.replayActions.push_back(row);
    }
    if(!input.eof() || ep.replayActions.size()<2)throw std::runtime_error("invalid actions replay");
    ep.limit=ep.replayActions.back().time;
  } else {
    ep.actions=file("actions.txt");ep.writer=std::make_unique<NavDemoWriter>((std::filesystem::path(output)/"samples.bin.gz").string());
  }
  observe(w);decide(w);ep.next=ep.began+100;
}
void NavDemos::tick(World& w,uint64_t dt) {
  if(!ep.active)return;
  auto& n=*ep.npc;++ep.frames;if(n.characterVirtualActive())++ep.cvFrames;
  const auto p=NpcKeys::physicalPosition(n);if(dt)ep.velocity=(p-ep.previousPosition)*(1000.f/float(dt));ep.previousPosition=p;
  const auto elapsed=w.tickCount()-ep.began;
  const bool arrived=(n.position()-ep.goal).length()<100.f && !n.isInAir() && !n.navigationClimbing();
  if((!ep.replay && arrived) || elapsed>=ep.limit || n.isDown()) {
    ep.success=arrived;
    if(!ep.success && ep.failure.empty())ep.failure=n.isDown()?"actor_down":"path_timeout";
    finish(w);return;
  }
  observe(w);
  if(w.tickCount()>=ep.next) {decide(w);ep.next+=100;}
}
