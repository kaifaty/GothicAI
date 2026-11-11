#include "navdemos.h"
#include "game/navigationeyeworld.h"
#include "navdemos_internal.h"
#include "gothic.h"
#include "testbench.h"
#include "navbench.h"
#include "navkeyteacher.h"
#include "navreward.h"
#include "navmemoryteacher.h"
#include "navmemorycosttrace.h"
#include "navdemowriter.h"
#include "navsimpackage.h"
#include "navpolicy.h"
#include "navlivetrace.h"
#include "navliveprobes.h"
#include "navtransitions.h"
#include "physics/charactervirtualdiagnostics.h"
#include "game/followtrail.h"
#include "game/knownroadsbench.h"
#include "game/navigationpassage.h"
#include "game/navperceptionbench.h"
#include "game/navperceptionsafety.h"
#include "world/world.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {
using Tempest::Vec3;
using namespace NavDemosInternal;
constexpr uint64_t PolicyWarmupDecisions=30;
struct RecordedKeys { uint64_t time=0; NpcKeys::KeyState keys; };
struct MotionSample { uint64_t time=0; Vec3 position; bool commanded=false; };
struct DaggerMotionSample : MotionSample { std::vector<Vec3> teacherPath; };
struct DaggerEvidence {
  const char* trigger="";
  bool stalled=false,memoryDanger=false,followBand=false;
  float headingCosine=1,pathProgressCm=0,targetDistanceCm=0;
  uint64_t headingMs=0,progressWindowMs=0;
};
struct CriticState {
  double pathCm=0;
  std::array<float,3> firstDirection={};
  bool blocked=false,inAir=false,valid=false;
};
struct NonfiniteObservation final : std::runtime_error {
  using Field=std::pair<std::string,std::string>;
  NonfiniteObservation(std::string message,uint64_t time,std::vector<Field> badFields)
    :std::runtime_error(std::move(message)),timeMs(time),fields(std::move(badFields)) {}
  uint64_t timeMs=0;
  std::vector<Field> fields;
};
struct Episode {
  Npc* npc=nullptr;
  // Scenario state is owned by the stand, never passed into NavMemoryTeacher.
  Npc* target=nullptr;
  std::string goalKind="point";
  std::vector<const WayPoint*> targetRoute;
  size_t targetLeg=0;
  uint64_t targetResume=0,scenarioSeed=0,firstContactMs=0,followMs=0,inBandMs=0;
  // After first contact: time beyond the band (>5 m) and inside it (<2 m).
  uint64_t followFarMs=0,followNearMs=0;
  // Follow carrot: the leader's trail (default) or the straight line ("line").
  FollowTrail followTrail;
  bool followLine=false,followRun=true;
  // Teacher comparison switches and forced pace (0 own, 1 walk, 2 run; follow keeps followRun).
  bool teacherBodyExit=true,followLostAside=true;
  uint8_t teacherPace=0;
  // Where the driving teacher stands (no forward/strafe key) far from its goal, by rule.
  std::array<uint64_t,size_t(NavMemoryTeacher::Branch::Count)> teacherBranches={},teacherFarIdle={};
  uint64_t teacherFarDecisions=0,teacherStallTraceAt=0;
  bool teacherStallPrevious=false;
  std::array<uint64_t,size_t(FollowTrail::Source::Count)> followSources={};
  FollowTrail::Source followSource=FollowTrail::Source::Count;
  bool followVisibleNow=false;
  uint64_t followVisibleDecisions=0,followLoopLimitedDecisions=0,followLoopLineDecisions=0;
  uint64_t followDetourDecisions=0,followGuardedDecisions=0;
  // Rising edges of native CV blocked/steep contact since the previous decision.
  bool followContact=false,followContactPrevious=false;
  uint64_t followContactEntries=0,followContactDecisions=0;
  bool contacted=false,targetQueued=false;
  bool liveTargetPlayer=false,liveTargetWasExternal=false;
  bool liveTargetWalkOnly=false;
  bool liveTargetWalking=false,liveTargetRunning=false;
  Vec3 liveTargetPosition;
  NavMesh::Path liveTargetPath;
  size_t liveTargetCorner=0;
  uint64_t liveTargetTurnAt=0,liveTargetLegAt=0;
  Npc* liveGuidePlayer=nullptr;
  bool liveGuideWasExternal=false,liveGuideWalking=false;
  uint64_t liveGuideLastMotionAt=0;
  double liveGuideTraveledCm=0,liveGuideLagCm=300;
  Vec3 liveGuidePosition;
  std::deque<std::pair<double,Vec3>> liveGuideTrail;
  Vec3 previousTargetPosition;
  uint64_t targetObservedAt=0,targetRunningUntil=0;
  bool targetObserved=false;
  std::mt19937_64 targetRng;
  double detourLengthCm=0,travelCm=0;
  uint64_t backwardDecisions=0,strafeDecisions=0;
  uint64_t oracleUnsupportedDecisions=0,startNotFoundDecisions=0;
  uint64_t oracleSnappedSupportDecisions=0;
  float oracleSupportSnapMaxCm=0,oracleSupportHorizontalMaxCm=0,oracleSupportVerticalAbsMaxCm=0;
  NavMemoryTeacher memoryTeacher;
  Vec3 goal,previousPosition,velocity;
  std::string output,from,to,failure,initialState;
  std::string tracedPlan;
  bool active=false,replay=false,drive=false,success=false;
  bool oracle=false;
  bool perceptionMonitor=false;
  bool population=false,record=true,sampled=false,pending=false,fell=false;
  std::optional<bool> populationWalk;
  bool trainingValid=true,accounted=false;
  std::string nonfiniteStage;
  std::unique_ptr<NonfiniteObservation> nonfinite;
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
  CriticState critic;
  NavPolicy::Result selected;
  std::array<size_t,6> selectedIndices={};
  std::array<double,6> selectedLogProb={};
  // Eye, memory, roads/teacher, encoding, policy, reward, recording.
  std::array<double,7> phaseUs={};
  uint32_t teacherVersion=1;
  double teacherBeta=0;
  uint64_t mixingSeed=7,teacherControlDecisions=0;
  bool dagger=false,teacherControlled=false,maneuverMasked=false,learnerCommanded=false;
  bool dartEpisode=false,dartStarted=false,headingDisagrees=false;
  uint64_t teacherUntil=0,headingDisagreedAt=0,daggerInterventions=0,dartDecisions=0,maneuverMaskedDecisions=0;
  unsigned dartRemaining=0;
  std::string takeoverReason;
  std::deque<DaggerMotionSample> daggerMotion;
  std::mt19937_64 mixingRng,actionRng,dartRng;
  KnownRoads::Guidance guidance;
  // Point goals: road progress per decision, and how often memory let progress
  // take a later stretch or moved the start of a rebuilt route (roads-guidance.txt).
  std::ofstream roadsGuidance;
  // Complete ordered PassageCheck requests, one row per road decision.
  std::ofstream roadsPassages;
  uint64_t roadsCatchUps=0,roadsRebuilds=0,roadsStartsMoved=0,roadsStartFallbacks=0,roadsStartDepartures=0;
  uint64_t roadsProjectionsRejected=0;
  // Diagnosis only (OPENGOTHIC_NAV_ROADS_CLOSED_TRACE=N): at each rebuild every
  // passage verdict and the memory cell that closed it, at most N lines.
  std::ofstream roadsClosed;
  uint64_t roadsClosedLines=0,roadsClosedLimit=0;
  uint64_t began=0,limit=240000,next=0,decisions=0,frames=0,cvFrames=0;
  uint64_t progressAt=0;
  float best=std::numeric_limits<float>::max();
  size_t replayIndex=0;
  uint64_t replans=0,fallbacks=0;
  double writeUs=0,maxWriteUs=0;
  double memoryUpdateUs=0,memoryViewUs=0;
  uint64_t memoryUpdates=0,memoryViews=0;
  uint64_t familiarInputDecisions=0,familiarInputNonzeroDecisions=0;
  uint64_t familiarNearInputNonzeroDecisions=0,familiarFarInputNonzeroDecisions=0;
  std::shared_ptr<const NavPolicy> policy;
  std::vector<NavPolicy::State> policyStates;
  std::vector<double> policyUs,encodeUs,populationUs;
  std::array<uint64_t,6> disagreements={};
  NavReward::Blocked blockage;
  std::chrono::steady_clock::time_point wallStart;
  NpcKeys::KeyState previous;
  NavMesh::Path path;
  NavMesh::Path::Transition observedTransition;
  bool hasObservedTransition=false;
  NavigationEye eye{[] { NavigationEye::Config c;c.layout=250;return c; }()};
  NavigationMemory memory;
  NavigationMemory::View view;
  std::unique_ptr<NavDemoWriter> writer;
  std::unique_ptr<NavMemoryCostTrace> memoryCostTrace;
  std::unique_ptr<NavLiveTrace> live;
  bool liveContact=false,liveSafety=false,liveRequestClipped=false,liveResolvedClipped=false;
  float liveEdgeClearanceCm=500,liveWaterClearanceCm=500;
  bool liveEdgeCensored=true,liveWaterCensored=true;
  uint64_t liveClearanceNext=0;
  std::ofstream actions,trajectory,knownPaths,teacherPaths,transitionFrames,samples,targetTrajectory;
  std::ofstream perceptionSafetyFrames;
  std::ofstream perceptionWaterObservations;
  std::vector<RecordedKeys> replayActions;
  Episode() { memory.setJourneyMemory(false); }
};
Episode legacy;

uint32_t demoVersion(const Episode& ep) {
  if(ep.drive) {
    const auto input=ep.policy->navDemoVersion();
    return input==2 && ep.memory.representationVersion()==4 ? 3 : input;
  }
  return ep.teacherVersion>=2 ? (ep.memory.representationVersion()==4 ? 3 : 2) : ep.teacherVersion;
}
// Population v5 carries an explicit episode pace. Other drives use
// OPENGOTHIC_NAV_FORCE_PACE=walk|run (true = walk); companions always run.
std::optional<bool> forcedWalk(const Episode& ep) {
  if(ep.populationWalk)return ep.populationWalk;
  const auto* pace=std::getenv("OPENGOTHIC_NAV_FORCE_PACE");
  if(!pace || !*pace)return std::nullopt;
  const std::string_view value=pace;
  if(value!="walk" && value!="run")throw std::runtime_error("navdemos: OPENGOTHIC_NAV_FORCE_PACE must be walk or run");
  if(ep.goalKind=="follow")return false;
  return value=="walk";
}
void requirePace(const NavPolicy& policy) {
  const auto* pace=std::getenv("OPENGOTHIC_NAV_FORCE_PACE");
  if(policy.paceInput() && (!pace || !*pace))
    throw std::runtime_error("navdemos: pace-input weights (.navbc v5) need OPENGOTHIC_NAV_FORCE_PACE=walk|run "
                             "(nav_rl.py drive --force-pace); the stand never picks a pace by default");
}

struct Pair {std::string from,to;std::optional<bool> walk;};
struct Population {
  bool active=false,record=true,sampled=false,near=false;
  std::optional<bool> walk;
  size_t instance=0,targetEpisodes=0,started=0,completed=0,initialDecisions=0;
  size_t invalidObservationEpisodes=0;
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
struct DemoSettings {
  bool cv=false,heroNear=true,killArmed=false,killFired=false;
  bool oracle=false,recordDemos=true;
  bool perceptionMonitor=false;
  bool dagger=false;
  bool followLine=false,followRun=true;
  bool teacherBodyExit=true,followLostAside=true;
  uint8_t teacherPace=0;
  uint32_t driveTeacher=0;
  bool live=false,liveTargetPlayer=false,liveGuidePlayer=false,liveTargetWalkOnly=false;
  NavigationEye::FloorSampling floorSampling=NavigationEye::FloorSampling::WalkingGazeDown;
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
const char* perceptionMode(NavigationEye::FloorSampling mode) {
  switch(mode) {
    case NavigationEye::FloorSampling::Baseline: return "baseline";
    case NavigationEye::FloorSampling::WalkingGazeDown: return "gaze";
    case NavigationEye::FloorSampling::ForwardRows: return "rows";
  }
  return "invalid";
}

std::array<float,3> array(const Vec3& p) {return {p.x,p.y,p.z};}
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
std::array<float,3> direction(Vec3 delta,float yaw) {
  const float len=delta.length();if(len>0)delta*=1.f/len;return local(delta,yaw);
}
std::ofstream file(Episode& ep,const char* name) {
  std::ofstream out(std::filesystem::path(ep.output)/name);
  if(!out)throw std::runtime_error(std::string("navdemos cannot write ")+name);
  out<<std::setprecision(9)<<std::boolalpha;return out;
}
// The walker's own memory decides where familiar-road progress may cut a loop
// of the road and where a rebuilt route may start (game/navigationpassage.h).
KnownRoads::Passage roadsPassage(NavigationPassage::Verdict verdict) {
  switch(verdict) {
    case NavigationPassage::Verdict::Free: return KnownRoads::Passage::Free;
    case NavigationPassage::Verdict::Closed: return KnownRoads::Passage::Closed;
    case NavigationPassage::Verdict::Unknown: break;
  }
  return KnownRoads::Passage::Unknown;
}
KnownRoads::PassageCheck memoryPassage(const NavigationMemory::View& view) {
  return [&view](const Vec3& from,const Vec3& to) {
    return roadsPassage(NavigationPassage::check(view,from,to,FollowTrail::corridorCm,FollowTrail::footprintCm));
  };
}
// Diagnosis: the same verdicts, with the cell that closed each passage.
struct RoadsPassageCall {
  Vec3 from,to;
  KnownRoads::Passage verdict=KnownRoads::Passage::Unknown;
  NavigationPassage::Closing closing;
};
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
bool livePlayerStart(Episode& ep,World& world,Npc& player,Vec3& position,NavMesh::Path& approach) {
  const auto leader=NpcKeys::physicalPosition(*ep.npc);
  const auto& mesh=NavBench::navigationMesh(world);
  const auto anchor=mesh.groundLocation(meters(leader));
  const auto candidates=mesh.nearbyGroundLocations(meters(leader));
  const auto capsule=player.navigationCapsule();
  auto out=file(ep,"live-guide-start.json");
  out<<"{\"method\":\"nearest_ground_annulus\",\"minimum_cm\":200,\"maximum_cm\":600,\"leader_cm\":";
  vector(out,leader);out<<",\"role\":"<<std::quoted(ep.liveTargetPlayer?"target":settings.liveGuidePlayer?"guide":"observer")
     <<",\"candidate_count\":"<<candidates.size()<<",\"attempts\":[";
  bool accepted=false,first=true;
  for(const auto& candidate:candidates) {
    const auto requested=cm(candidate.snapped);
    const auto rayFrom=requested+Vec3(0,200,0);
    const auto floor=world.physic()->landRay(rayFrom,500.f);
    auto normal=floor.n;
    if(floor.hasCol && normal.length()<.5f) {
      const auto surface=navigationRay(*world.physic(),floor.v+Vec3(0,2,0),floor.v-Vec3(0,2,0),&player);
      if(surface.hasCol && (surface.point-floor.v).length()<3.f)normal=surface.normal;
    }
    const float normalLength=normal.length();
    if(normalLength>.5f)normal*=1.f/normalLength;
    const bool supported=floor.hasCol && normalLength>.5f && normal.y>=.65f && std::abs(floor.v.y-requested.y)<=100.f;
    bool clear=false,dry=false;
    if(supported) {
      clear=!world.physic()->sweep(floor.v+Vec3(0,capsule.x+2,0),
          floor.v+Vec3(0,std::max(capsule.x+2,capsule.y-capsule.x),0),capsule.x).hasCol;
      const auto water=NavLiveProbes::waterAbove(world,floor.v);
      dry=!water.hasCol || water.wdepth-floor.v.y<=10.f;
    }
    const bool connected=!anchor.known || anchor.component==candidate.component;
    const char* reason=!connected?"different_ground_component":!floor.hasCol?"no_floor":
                       !supported?"unsupported_floor":!clear?"capsule_blocked":!dry?"water":"accepted";
    auto path=NavMesh::Path{};
    if(connected && supported && clear && dry && settings.liveGuidePlayer) {
      path=mesh.findPath(meters(floor.v),meters(leader));
      if(!path.complete || path.corners.size()<2 || !path.transitions.empty())reason="no_ground_path_to_leader";
    }
    if(!first)out<<',';
    first=false;
    out<<"{\"navmesh_cm\":";vector(out,requested);out<<",\"ray_from_cm\":";vector(out,rayFrom);
    out<<",\"floor_found\":"<<floor.hasCol<<",\"floor_cm\":";
    if(floor.hasCol)vector(out,floor.v);else out<<"null";
    out<<",\"normal_y\":";jsonNumber(out,normal.y);
    out<<",\"capsule_clear\":"<<clear<<",\"dry\":"<<dry<<",\"reason\":"<<std::quoted(reason)<<'}';
    if(std::string_view(reason)!="accepted")continue;
    position=floor.v;approach=std::move(path);accepted=true;break;
  }
  out<<"],\"accepted\":"<<accepted<<",\"selected_cm\":";
  if(accepted)vector(out,position);else out<<"null";
  out<<",\"reason\":"<<std::quoted(accepted?"accepted":"no_safe_ground_in_2_6m_annulus")<<"}\n";
  return accepted;
}
double percentile(std::vector<double> values,double quantile) {
  if(values.empty())return 0;
  std::sort(values.begin(),values.end());
  const double index=quantile*double(values.size()-1);
  const auto lower=size_t(index),upper=std::min(lower+1,values.size()-1);
  return values[lower]+(values[upper]-values[lower])*(index-double(lower));
}
void closeBlocked(Episode& ep,uint64_t elapsed) {
  ep.blockage.close(elapsed);
}
void measureBlocked(Episode& ep,uint64_t elapsed) {
  const auto& k=ep.previous;
  ep.blockage.measure(elapsed,NpcKeys::physicalPosition(*ep.npc),{k.forward,k.strafe,k.jump,k.climb});
}
double rewardPath(Episode& ep,World& world) {
  const auto position=NpcKeys::physicalPosition(*ep.npc);
  const auto route=NavBench::navigationMesh(world).findPath(meters(position),meters(ep.goal));
  ep.rewardPathValid=route.complete;
  // Training-only state shares the reward query to the final goal. The actor's
  // observation and the teacher's local carrot route remain unchanged.
  ep.critic={NavReward::pathLength(ep.lastPathCm,{double(route.length)*100.,route.complete}),
             {},ep.blockage.active,ep.npc->isInAir(),route.complete};
  if(!route.complete) {++ep.invalidRewardPaths;return ep.critic.pathCm;}
  if(!route.corners.empty()) {
    // The first corner is the projected start; the next is the first target.
    const size_t first=route.corners.size()>1?1:0;
    ep.critic.firstDirection=direction(cm(route.corners[first])-position,ep.npc->rotationRad());
  }
  return ep.critic.pathCm;
}
void criticState(std::ostream& out,const CriticState& state) {
  out<<"{\"path_cm\":"<<state.pathCm<<",\"first_direction\":["
     <<state.firstDirection[0]<<','<<state.firstDirection[1]<<','<<state.firstDirection[2]
     <<"],\"blocked\":"<<state.blocked<<",\"in_air\":"<<state.inAir<<",\"valid\":"<<state.valid<<'}';
}
void reward(Episode& ep,World& world,bool terminated=false,bool truncated=false) {
  if(!ep.population || !ep.pending)return;
  const auto start=Clock::now();
  const bool beforeValid=ep.rewardPathValid;
  const auto beforeCritic=ep.critic;
  const double after=rewardPath(ep,world);
  const auto now=world.tickCount();
  const auto health=ep.npc->attribute(ATR_HITPOINTS);
  const auto& held=ep.previous;
  const auto components=NavReward::transition({ep.lastPathCm,{after,ep.rewardPathValid},ep.actionAt,now,
      {held.forward,held.strafe,held.jump,held.climb},ep.health,health,
      ep.fell,ep.success,ep.blockage.active,ep.npc->isDead()});
  ep.progressCm+=components.progressCm;
  ep.phaseUs[5]+=micros(start);
  if(ep.record) {
    const auto recordStart=Clock::now();
    auto& out=ep.samples;
    out<<"{\"time_ms\":"<<ep.actionAt<<",\"next_time_ms\":"<<now<<",\"elapsed_ms\":"<<ep.actionAt-ep.began
       <<",\"action_indices\":[";
    for(size_t i=0;i<ep.policy->actionHeadCount();++i) {if(i)out<<',';out<<ep.selectedIndices[i];}
    out<<"],\"logits\":[";bool first=true;
    for(const auto& head:ep.selected.logits)for(float value:head) {if(!first)out<<',';first=false;out<<value;}
    out<<"],\"log_probs\":[";double joint=0;
    for(size_t i=0;i<ep.policy->actionHeadCount();++i) {if(i)out<<',';out<<ep.selectedLogProb[i];joint+=ep.selectedLogProb[i];}
    out<<"],\"log_prob\":"<<joint<<",\"path_before_cm\":"<<ep.lastPathCm<<",\"path_after_cm\":"<<after
       <<",\"path_valid\":"<<(beforeValid && ep.rewardPathValid)<<",\"critic_before\":";
    criticState(out,beforeCritic);out<<",\"critic_after\":";criticState(out,ep.critic);
    out<<",\"terminated\":"<<terminated<<",\"truncated\":"<<truncated
       <<",\"rewards\":{\"progress_cm\":"<<components.progressCm<<",\"arrived\":"<<int(components.arrived)
       <<",\"blocked\":"<<int(components.blocked)<<",\"fall_damage\":"<<components.fallDamage
       <<",\"death\":"<<int(components.death)<<",\"jump\":"<<int(components.jump)
       <<",\"climb\":"<<int(components.climb)<<",\"time_ms\":"<<components.timeMs<<"}}\n";
    if(!out)throw std::runtime_error("navdemos rewards write failed");
    ep.phaseUs[6]+=micros(recordStart);
  }
  ep.health=health;ep.lastPathCm=after;ep.fell=false;ep.pending=false;
}
void finish(Episode& ep,World& world) {
  const auto elapsed=world.tickCount()-ep.began;
  closeBlocked(ep,elapsed);
  trajectory(ep,elapsed);
  if(ep.targetTrajectory.is_open() && ep.decisions && elapsed>(ep.decisions-1)*100) {
    const float distance=(NpcKeys::physicalPosition(*ep.npc)-ep.goal).length();
    ep.targetTrajectory<<elapsed<<' '<<ep.goal.x<<' '<<ep.goal.y<<' '<<ep.goal.z<<' '
                       <<distance<<' '<<int(distance>=200.f && distance<=500.f)<<'\n';
  }
  if(!ep.replay) {NpcKeys::KeyState end;end.walk=true;action(ep,elapsed,end);if(ep.writer)ep.writer->finish();}
  ep.trajectory.close();ep.actions.close();ep.knownPaths.close();ep.teacherPaths.close();ep.transitionFrames.close();ep.samples.close();ep.targetTrajectory.close();ep.perceptionSafetyFrames.close();ep.perceptionWaterObservations.close();
  ep.roadsGuidance.close();ep.roadsClosed.close();ep.roadsPassages.close();
  if(ep.live) ep.live->finish();
  if(ep.live || (ep.goalKind=="follow" && ep.target))ep.npc->setCharacterVirtualDiagnosticsEnabled(false);
  auto out=file(ep,"episode.json");
  const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-ep.wallStart).count();
  out<<"{\"success\":"<<ep.success<<",\"replay\":"<<ep.replay<<",\"drive\":"<<ep.drive<<",\"start\":"<<std::quoted(ep.from)
     <<",\"goal\":"<<std::quoted(ep.to)<<",\"elapsed_ms\":"<<elapsed<<",\"limit_ms\":"<<ep.limit
     <<",\"distance_cm\":";jsonNumber(out,(ep.npc->position()-ep.goal).length());
  out<<",\"failure\":"<<std::quoted(ep.failure)
     <<",\"teacher_version\":"<<ep.teacherVersion<<",\"demo_version\":"
     <<demoVersion(ep)
     <<",\"navigation_memory_version\":"<<ep.memory.representationVersion()
     <<",\"journey_memory\":"<<ep.memory.journeyMemoryEnabled()
     <<",\"familiar_map\":"<<ep.memory.familiarMapEnabled()
     <<",\"roads_rebuilds\":"<<ep.roadsRebuilds<<",\"roads_catch_ups\":"<<ep.roadsCatchUps
     <<",\"roads_starts_moved\":"<<ep.roadsStartsMoved<<",\"roads_start_fallbacks\":"<<ep.roadsStartFallbacks
     <<",\"roads_start_departures\":"<<ep.roadsStartDepartures
     <<",\"roads_projections_rejected\":"<<ep.roadsProjectionsRejected
     <<",\"memory_update_us\":"<<(ep.memoryUpdates?ep.memoryUpdateUs/double(ep.memoryUpdates):0.)
     <<",\"memory_view_us\":"<<(ep.memoryViews?ep.memoryViewUs/double(ep.memoryViews):0.)
     <<",\"memory_updates\":"<<ep.memoryUpdates<<",\"memory_views\":"<<ep.memoryViews
     <<",\"perception_mode\":"<<std::quoted(perceptionMode(ep.eye.config().floorSampling))
     <<",\"perception_monitor\":"<<ep.perceptionMonitor
     <<",\"live_trace\":"<<bool(ep.live)<<",\"live_target_player\":"<<ep.liveTargetPlayer
     <<",\"live_target_walk_only\":"<<ep.liveTargetWalkOnly
     <<",\"live_guide_player\":"<<bool(ep.liveGuidePlayer)
     <<",\"oracle\":"<<ep.oracle<<",\"record\":"<<bool(ep.writer)
     <<",\"demonstration_recorded\":"<<bool(ep.writer)<<",\"demo_recorded\":"<<bool(ep.writer)
     <<",\"solver\":"<<std::quoted(ep.teacherVersion==3?"memory_teacher_v3":ep.teacherVersion==2?"teacher_v2":"teacher_v1")
     <<",\"observation_source\":"
     <<std::quoted(ep.oracle?"detour_raster":ep.teacherVersion==3 || ep.dagger?"learner_memory":ep.teacherVersion==2?"detour_teacher_v2":"detour_teacher_v1")
     <<",\"oracle_unsupported_decisions\":"<<ep.oracleUnsupportedDecisions
     <<",\"oracle_snapped_support_decisions\":"<<ep.oracleSnappedSupportDecisions
     <<",\"oracle_support_snap_max_cm\":"<<ep.oracleSupportSnapMaxCm
     <<",\"oracle_support_horizontal_max_cm\":"<<ep.oracleSupportHorizontalMaxCm
     <<",\"oracle_support_vertical_abs_max_cm\":"<<ep.oracleSupportVerticalAbsMaxCm
     <<",\"start_not_found_decisions\":";
  if(ep.teacherVersion==3) {
    out<<ep.startNotFoundDecisions;
  }
  else {
    out<<"null";
  }
  if(ep.teacherVersion==3 && !ep.drive) {
    // The driving teacher's rules over all its decisions, and those standing far from the goal.
    const auto counts=[&](const char* key,const auto& values) {
      out<<",\""<<key<<"\":{";
      for(size_t i=0;i<values.size();++i)
        out<<(i?",":"")<<std::quoted(NavMemoryTeacher::name(NavMemoryTeacher::Branch(i)))<<':'<<values[i];
      out<<'}';
    };
    out<<",\"teacher_body_exit\":"<<std::quoted(ep.teacherBodyExit?"on":"off")
       <<",\"teacher_pace\":"<<std::quoted(ep.teacherPace==0?"own":ep.teacherPace==1?"walk":"run")
       <<",\"teacher_far_decisions\":"<<ep.teacherFarDecisions;
    counts("teacher_branches",ep.teacherBranches);
    counts("teacher_far_idle",ep.teacherFarIdle);
  }
  out<<",\"decisions\":"<<ep.decisions<<",\"record_us_mean\":"<<(ep.decisions?ep.writeUs/double(ep.decisions):0)
     <<",\"record_us_max\":"<<ep.maxWriteUs<<",\"wall_seconds\":"<<seconds
     <<",\"decisions_per_second\":"<<double(ep.decisions)/seconds<<",\"frames\":"<<ep.frames
     <<",\"cv_frames\":"<<ep.cvFrames<<",\"replans\":"<<ep.replans<<",\"fallbacks\":"<<ep.fallbacks
     <<",\"final_position\":";vector(out,NpcKeys::physicalPosition(*ep.npc));
  out<<",\"target_position\":";vector(out,ep.goal);
  if(ep.teacherVersion>=2)
    out<<",\"goal_kind\":"<<std::quoted(ep.goalKind)<<",\"seed\":"<<ep.scenarioSeed
       <<",\"detour_length_cm\":"<<ep.detourLengthCm<<",\"travel_cm\":"<<ep.travelCm
       <<",\"backward_decisions\":"<<ep.backwardDecisions<<",\"strafe_decisions\":"<<ep.strafeDecisions
       <<",\"blocked_spans\":"<<ep.blockage.spans<<",\"blocked_ms\":"<<ep.blockage.totalMs
       <<",\"max_blocked_ms\":"<<ep.blockage.maxMs<<",\"first_contact_ms\":"<<(ep.contacted?int64_t(ep.firstContactMs):-1)
       <<",\"follow_ms\":"<<ep.followMs<<",\"in_band_ms\":"<<ep.inBandMs
       <<",\"follow_fraction\":"<<(ep.followMs?double(ep.inBandMs)/double(ep.followMs):0)
       <<",\"follow_far_ms\":"<<ep.followFarMs<<",\"follow_near_ms\":"<<ep.followNearMs;
  if(ep.teacherVersion>=2 && ep.goalKind=="follow" && ep.target) {
    out<<",\"follow_guidance\":"<<std::quoted(ep.followLine?"line":"trail")<<",\"follow_run\":"<<std::quoted(ep.followRun?"on":"off")
       <<",\"follow_lost\":"<<std::quoted(ep.followLostAside?"aside":"last_seen")
       <<",\"follow_visible_decisions\":"<<ep.followVisibleDecisions
       <<",\"follow_contact_entries\":"<<ep.followContactEntries
       <<",\"follow_contact_decisions\":"<<ep.followContactDecisions
       <<",\"follow_loop_limited_decisions\":"<<ep.followLoopLimitedDecisions
       <<",\"follow_loop_line_decisions\":"<<ep.followLoopLineDecisions
       <<",\"follow_unseen_detour_decisions\":"<<ep.followDetourDecisions
       <<",\"follow_guarded_decisions\":"<<ep.followGuardedDecisions<<",\"follow_carrot_sources\":{";
    for(size_t i=0;i<ep.followSources.size();++i) {
      if(i)out<<',';
      out<<std::quoted(FollowTrail::name(FollowTrail::Source(i)))<<':'<<ep.followSources[i];
    }
    out<<'}';
  }
  if(ep.target)
    out<<",\"target_immortal\":"<<ep.target->isImmortal()
       <<",\"target_attitude\":"<<int(ep.target->attitude())
       <<",\"target_temp_attitude\":"<<int(ep.target->tempAttitude())
       <<",\"target_external_control\":"<<ep.target->isExternalControl()
       <<",\"target_nonhostile\":"<<(world.script().personAttitude(*ep.npc,*ep.target)>=ATT_NEUTRAL &&
                                      world.script().personAttitude(*ep.target,*ep.npc)>=ATT_NEUTRAL)
       <<",\"target_final_down\":"<<ep.target->isDown()
       <<",\"target_final_dead\":"<<ep.target->isDead();
  out<<",\"next_corner\":";vector(out,ep.path.corners.empty()?ep.goal:cm(ep.path.corners.back()));
  if(ep.dagger)out<<",\"dagger_control_version\":3";
  if(ep.drive) {
    out<<",\"teacher_beta\":"<<ep.teacherBeta<<",\"mixing_seed\":"<<ep.mixingSeed
       <<",\"dagger\":"<<ep.dagger<<",\"dagger_interventions\":"<<ep.daggerInterventions
       <<",\"dart_episode\":"<<ep.dartEpisode<<",\"dart_decisions\":"<<ep.dartDecisions
       <<",\"maneuver_masked_decisions\":"<<ep.maneuverMaskedDecisions
       <<",\"action_mode\":"<<std::quoted(ep.sampled?"sample":"argmax")
       <<",\"pace_input\":"<<ep.policy->paceInput()<<",\"pace\":";
    if(const auto walk=forcedWalk(ep))out<<std::quoted(*walk?"walk":"run");
    else out<<"null";
    out<<",\"teacher_control_decisions\":"<<ep.teacherControlDecisions
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
       <<",\"blocked_spans\":"<<ep.blockage.spans<<",\"blocked_ms\":"<<ep.blockage.totalMs
       <<",\"max_blocked_ms\":"<<ep.blockage.maxMs<<",\"disagreement_counts\":{";
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
    if(ep.policy->weightVersion()==4)
      out<<",\"familiar_input_decisions\":"<<ep.familiarInputDecisions
         <<",\"familiar_input_nonzero_decisions\":"<<ep.familiarInputNonzeroDecisions
         <<",\"familiar_near_input_nonzero_decisions\":"<<ep.familiarNearInputNonzeroDecisions
         <<",\"familiar_far_input_nonzero_decisions\":"<<ep.familiarFarInputNonzeroDecisions;
    if(ep.dagger)out<<",\"training_valid\":"<<(ep.trainingValid && !ep.nonfinite);
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
       <<",\"training_valid\":"<<ep.trainingValid
       <<",\"termination\":"<<std::quoted(ep.success?"arrived":ep.failure)
       <<",\"start_path_cm\":"<<ep.startPathCm<<",\"remaining_path_cm\":"<<ep.lastPathCm
       <<",\"progress_cm\":"<<ep.progressCm<<",\"invalid_reward_paths\":"<<ep.invalidRewardPaths;
  if(ep.nonfinite) {
    out<<",\"nonfinite_observation\":{\"stage\":"<<std::quoted(ep.nonfiniteStage)
       <<",\"time_ms\":"<<ep.nonfinite->timeMs<<",\"elapsed_ms\":"<<ep.nonfinite->timeMs-ep.began
       <<",\"diagnostic\":"<<std::quoted(ep.nonfinite->what())<<",\"fields\":[";
    for(size_t i=0;i<ep.nonfinite->fields.size();++i) {
      if(i)out<<',';
      const auto& field=ep.nonfinite->fields[i];
      out<<"{\"name\":"<<std::quoted(field.first)<<",\"value\":"<<std::quoted(field.second)<<'}';
    }
    out<<"]}";
  }
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
  if(ep.liveTargetPlayer && ep.target) {ep.target->setExternalControl(ep.liveTargetWasExternal);ep.target->clearSpeed();}
  if(ep.liveGuidePlayer) {
    ep.liveGuidePlayer->stopWalking();ep.liveGuidePlayer->clearSpeed();
    ep.liveGuidePlayer->setExternalControl(ep.liveGuideWasExternal);ep.liveGuidePlayer=nullptr;
  }
  NpcKeys::enable(*ep.npc,false);ep.npc->stopWalking();ep.active=false;
  ep.memoryCostTrace.reset();
  if(ep.target) {
    ep.target->clearAiQueue();ep.target->clearGoTo();ep.target->stopWalking();
    if(!ep.liveTargetPlayer)world.removeNpc(*ep.target);
    ep.target=nullptr;
  }
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
void traceV2Plan(Episode& ep,const Vec3& position,const NavKeyTeacher::Decision& decision,
                 bool direct,uint64_t elapsed) {
  if(!ep.record || ep.drive)return;
  for(const auto& link:ep.path.transitions)
    if(link.id==decision.hint.transitionId) {
      ep.observedTransition=link;ep.hasObservedTransition=true;break;
    }
  std::ostringstream signature;
  signature<<ep.path.complete<<','<<direct<<','<<ep.guidance.beyondKnown<<','
           <<decision.hint.transitionId<<','<<decision.hint.onTransition;
  if(!direct)for(const auto& link:ep.path.transitions)signature<<','<<link.id;
  if(signature.str()==ep.tracedPlan && !decision.keys.jump && !decision.keys.climb)return;
  ep.tracedPlan=signature.str();
  auto& out=ep.teacherPaths;
  out<<"{\"elapsed_ms\":"<<elapsed<<",\"active_transition_id\":"<<decision.hint.transitionId
     <<",\"active_transition_type\":"<<std::quoted(transitionName(decision.hint.transitionType))
     <<",\"on_transition\":"<<decision.hint.onTransition<<",\"jump\":"<<decision.keys.jump
     <<",\"climb\":"<<decision.keys.climb;
  writeV2Plan(out,position,ep.goal,ep.guidance,ep.path,direct);out<<"}\n";
  if(!out)throw std::runtime_error("navdemos teacher-paths write failed");
}
// Teacher-only outcome evidence at physical ticks. NAVDEMO's 100-ms body flags
// omit native BS_JUMP and can miss support between decisions; never feed this
// privileged evidence into the learner or its observability calculation.
void traceTransitionFrame(Episode& ep,World& world,uint64_t elapsed) {
  if(!ep.transitionFrames.is_open() || !ep.hasObservedTransition)return;
  const auto& n=*ep.npc;const auto& link=ep.observedTransition;
  const auto position=NpcKeys::physicalPosition(n);
  const bool support=NavTransitions::supported(world,*ep.npc);
  const auto& mesh=NavBench::navigationMesh(world);
  const auto sides=mesh.transitionSides(link.start,link.end,meters(position));
  const auto sourceOffset=position-cm(link.start),endOffset=position-cm(link.end);
  NavMesh::Path landing;
  if(support) {
    if(sides.current.known && sides.end.known && sides.current.component!=sides.end.component) {
      // The memoized ground topology proves this path is disconnected. Avoid
      // flooding a whole island again at every supported physical tick.
      landing.reason="disconnected";landing.startSnapDistance=sides.current.snapDistance;
      landing.endSnapDistance=sides.end.snapDistance;
    }
    else landing=mesh.findPath(meters(position),link.end,false);
  }
  auto& out=ep.transitionFrames;
  out<<"{\"elapsed_ms\":"<<elapsed<<",\"transition_id\":"<<link.id
     <<",\"type\":"<<std::quoted(transitionName(link.type))<<",\"position_cm\":";
  vector(out,position);
  out<<",\"cv_active\":"<<n.characterVirtualActive()
     <<",\"source_polygon\":"<<sides.source.polygon<<",\"end_polygon\":"<<sides.end.polygon
     <<",\"current_polygon\":"<<sides.current.polygon
     <<",\"source_component\":"<<sides.source.component<<",\"end_component\":"<<sides.end.component
     <<",\"current_component\":"<<sides.current.component
     <<",\"source_ground_known\":"<<sides.source.known<<",\"end_ground_known\":"<<sides.end.known
     <<",\"current_ground_known\":"<<sides.current.known
     <<",\"side_method\":"<<std::quoted(sides.method)<<",\"takeoff_side\":"<<sides.takeoffSide
     <<",\"landing_side\":"<<sides.landingSide<<",\"source_position_cm\":";
  vector(out,cm(link.start));out<<",\"end_position_cm\":";vector(out,cm(link.end));
  out<<",\"takeoff_xz_distance_cm\":"<<std::hypot(sourceOffset.x,sourceOffset.z)
     <<",\"takeoff_y_offset_cm\":"<<sourceOffset.y
     <<",\"landing_xz_distance_cm\":"<<std::hypot(endOffset.x,endOffset.z)
     <<",\"landing_y_offset_cm\":"<<endOffset.y
     <<",\"source_ground_snap_m\":"<<sides.source.snapDistance
     <<",\"end_ground_snap_m\":"<<sides.end.snapDistance
     <<",\"current_ground_snap_m\":"<<sides.current.snapDistance
     <<",\"current_ground_xz_offset_cm\":"<<sides.current.horizontalDistance*100.f
     <<",\"current_ground_y_offset_cm\":"<<sides.current.verticalOffset*100.f
     <<",\"current_ground_position_cm\":";
  if(sides.current.known) vector(out,cm(sides.current.snapped));else out<<"null";
  out<<",\"native_jump\":"<<(n.isJump() || n.isJumpAnim())
     <<",\"native_climb\":"<<(n.bodyStateMasked()==BS_CLIMB)
     <<",\"in_air\":"<<n.isInAir()<<",\"falling\":"<<n.isFalling()
     <<",\"supported\":"<<support<<",\"landing_path_complete\":"<<landing.complete
     <<",\"landing_path_m\":"<<landing.length<<",\"landing_snap_m\":"<<landing.startSnapDistance
     <<",\"landing_path_reason\":"<<std::quoted(landing.reason)<<"}\n";
  if(!out)throw std::runtime_error("navdemos transition-frames write failed");
}
void observe(Episode& ep,World& w) {
  auto& n=*ep.npc;
  const auto eyeStart=Clock::now();
  ep.eye.update(n,w.tickCount());
  if(ep.population)ep.phaseUs[0]+=micros(eyeStart);
  const auto memoryStart=Clock::now();
  NavigationMemory::Pose pose;
  pose.position=n.position();pose.forward={std::cos(n.rotationRad()),0,std::sin(n.rotationRad())};pose.gameMs=w.tickCount();
  const auto& guild=w.script().guildVal();
  const float step=float(guild.step_height[n.guild()]);
  pose.stepHeightCm=step>0.f?step:50.f;
  pose.maxSlopeDegrees=std::acos(std::clamp(std::cos(float(guild.slide_angle[n.guild()])*Pi/180.f),0.f,1.f))*180.f/Pi;
  ep.memory.update(ep.eye.memorySamples(),pose);
  const auto memoryUs=micros(memoryStart);
  ep.memoryUpdateUs+=memoryUs;++ep.memoryUpdates;
  if(ep.population)ep.phaseUs[1]+=memoryUs;
  if(ep.memoryCostTrace)ep.memoryCostTrace->update(pose,ep.eye.memorySamples());
}
void tracePerceptionWater(Episode& ep,const NavDemoWriter::Snapshot& sample) {
  if(!ep.perceptionMonitor)return;
  const auto& view=*sample.memory;
  auto& out=ep.perceptionWaterObservations;
  const auto byte=[](float value) {return unsigned(std::lround(value*255.f));};
  const auto channels=[&](const NavigationMemory::Cell& cell) {
    out<<'['<<byte(cell.confidence)<<','<<byte(cell.free)<<','<<byte(cell.wall)<<','<<byte(cell.ledge)
       <<','<<byte(cell.drop)<<','<<byte(cell.water)<<','<<byte(cell.movable)<<']';
  };
  out<<"{\"elapsed_ms\":"<<sample.gameMs-ep.began<<",\"position_cm\":";vector(out,view.pose.position);
  out<<",\"forward\":";vector(out,view.pose.forward);
  out<<",\"water_hit_rays\":[";
  size_t hits=0,index=0;
  for(const auto& ray:ep.eye.memorySamples()) {
    const size_t rayIndex=index++;
    if(!ray.hit || ray.kind!=NavigationEye::HitKind::Water)continue;
    if(hits++)out<<',';
    out<<"{\"index\":"<<rayIndex<<",\"cached_far\":"<<(rayIndex>=ep.eye.rayCount())
       <<",\"observed_age_ms\":"<<sample.gameMs-ray.sampledAtMs<<",\"point_cm\":";vector(out,ray.point);
    out<<",\"origin_cm\":";vector(out,ray.origin);
    out<<",\"direction\":";vector(out,ray.direction);
    out<<",\"normal_y\":"<<ray.normalY<<'}';
  }
  out<<"],\"near_water_cells\":[";
  size_t cells=0,farWater=0;
  for(size_t row=0;row<view.near.size;++row)for(size_t column=0;column<view.near.size;++column) {
    const auto& cell=view.near.cells[row*view.near.size+column];
    if(byte(cell.water)==0)continue;
    if(cells++)out<<',';
    out<<'['<<row<<','<<column<<','<<byte(cell.water)<<']';
  }
  for(const auto& cell:view.far.cells)farWater+=byte(cell.water)>0;
  out<<"],\"far_water_cells\":"<<farWater<<",\"goal_line\":[";
  auto direction=ep.goal-view.pose.position;direction.y=0;
  const float length=direction.length();
  if(length>0)direction*=1.f/length;
  const Vec3 right(-view.pose.forward.z,0,view.pose.forward.x);
  for(unsigned distance=0;distance<=300;distance+=25) {
    if(distance)out<<',';
    const auto delta=direction*float(distance);
    const auto project=[&](const Vec3& axis) {return delta.x*axis.x+delta.z*axis.z;};
    const int column=int(std::floor(project(right)/view.near.cellCm+float(view.near.size)*.5f));
    const int row=int(std::floor(project(view.pose.forward)/view.near.cellCm+float(view.near.size)*.5f));
    out<<"{\"distance_cm\":"<<distance<<",\"channels\":";
    if(row>=0 && column>=0 && row<int(view.near.size) && column<int(view.near.size))
      channels(view.near.cells[size_t(row)*view.near.size+size_t(column)]);
    else out<<"null";
    out<<'}';
  }
  out<<"]}\n";
  if(!out)throw std::runtime_error("navdemos perception water observation write failed");
}
NavDemoWriter::Snapshot snapshot(Episode& ep,World& w,const KnownRoads::Guidance* guidance=nullptr) {
  auto& n=*ep.npc;
  const auto position=ep.teacherVersion>=2 ? NpcKeys::physicalPosition(n) : n.position();
  const auto viewStart=Clock::now();
  ep.memory.view(ep.view);
  const auto viewUs=micros(viewStart);
  ep.memoryViewUs+=viewUs;++ep.memoryViews;
  if(ep.population)ep.phaseUs[1]+=viewUs;
  if(ep.memoryCostTrace)ep.memoryCostTrace->view();
  NavDemoWriter::Snapshot sample;
  sample.gameMs=w.tickCount();sample.position=array(NpcKeys::physicalPosition(n));sample.bodyYawDeg=n.rotation();
  sample.body.velocityCmS=local(ep.velocity,n.rotationRad());
  sample.body.flags={uint8_t(n.isInAir()),uint8_t(n.isFalling()),uint8_t(n.isSlide()),
                     uint8_t(n.navigationClimbing()),uint8_t(n.isSwim())};
  const auto normal=n.navigationGroundNormal();sample.body.groundNormal=local(normal,n.rotationRad());
  sample.body.groundSlopeDeg=std::acos(std::clamp(normal.y,-1.f,1.f))*180.f/Pi;
  const auto head=n.navigationEyePose().forward;
  sample.body.headYawDeg=std::remainder(std::atan2(head.z,head.x)*180.f/Pi-n.rotation(),360.f);
  sample.body.headPitchDeg=std::atan2(head.y,std::hypot(head.x,head.z))*180.f/Pi;
  sample.body.headPitchDeg+=ep.eye.gazePitchDeg();
  if(ep.teacherVersion>=2) {
    const auto& roads=guidance?*guidance:ep.guidance;
    sample.roads.carrotDirection=direction(roads.carrot-position,n.rotationRad());
    sample.roads.carrotDistanceCm=(roads.carrot-position).length();
    sample.roads.remainingKnownCm=float(roads.remainingKnownCm);
    sample.roads.beyondKnown=roads.beyondKnown;
  } else {
    Vec3 next;const auto roads=KnownRoadsBench::readPath(w,n,ep.goal,next);
    sample.roads.nextDirection=direction(next-n.position(),n.rotationRad());sample.roads.nextDistanceCm=(next-n.position()).length();
    sample.roads.remainingVisualCm=float(roads.remainingCm);sample.roads.goalInKnown=roads.goalInKnown;
  }
  sample.roads.goalDirection=direction(ep.goal-position,n.rotationRad());sample.roads.goalDistanceCm=(ep.goal-position).length();
  sample.previous=ep.previous;sample.eye=&ep.eye.samples();sample.memory=&ep.view;
  tracePerceptionWater(ep,sample);
  return sample;
}
void livePhysicalFlags(Episode& ep) {
  if(!ep.live)return;
  CharacterVirtualDiagnostics diagnostics;
  if(!ep.npc->characterVirtualDiagnostics(diagnostics))return;
  ep.liveContact=ep.liveContact || diagnostics.npcMove.blocked || diagnostics.npcMove.steepContact;
  ep.liveSafety=ep.liveSafety || diagnostics.stairBlockedByPolicy;
  ep.liveRequestClipped=ep.liveRequestClipped || diagnostics.npcMove.requestClipped;
  ep.liveResolvedClipped=ep.liveResolvedClipped || diagnostics.npcMove.resolvedClipped;
}
void liveClearance(Episode& ep,World& w,uint64_t elapsed) {
  if(elapsed<ep.liveClearanceNext)return;
  ep.liveClearanceNext=elapsed+400;
  ep.liveEdgeClearanceCm=ep.liveWaterClearanceCm=500.f;
  ep.liveEdgeCensored=ep.liveWaterCensored=true;
  const auto position=NpcKeys::physicalPosition(*ep.npc);
  const float chest=float(w.script().guildVal().water_depth_chest[ep.npc->guild()]);
  const auto deep=[&](const Vec3& point) {
    const auto ground=w.physic()->landRay(point+Vec3(0,200,0));
    if(!ground.hasCol)return false;
    const auto water=NavLiveProbes::waterAbove(w,ground.v);
    return water.hasCol && std::isfinite(water.wdepth) && water.wdepth-ground.v.y>=chest;
  };
  if(deep(position)) {ep.liveWaterClearanceCm=0;ep.liveWaterCensored=false;}
  for(unsigned i=0;i<16;++i) {
    const float angle=float(i)*(2.f*Pi/16.f);
    const Vec3 direction(std::cos(angle),0,std::sin(angle));
    const auto edge=ep.npc->navigationEdgeAhead(direction,500.f);
    if(edge.found && edge.drop>=100.f) {ep.liveEdgeClearanceCm=std::min(ep.liveEdgeClearanceCm,edge.distance);ep.liveEdgeCensored=false;}
    for(float distance=25.f;distance<=ep.liveWaterClearanceCm;distance+=25.f) {
      if(deep(position+direction*distance)) {
        ep.liveWaterClearanceCm=distance;ep.liveWaterCensored=false;break;
      }
    }
  }
}
void liveDecision(Episode& ep,World& w,const NavDemoWriter::Snapshot& sample,const NpcKeys::KeyState& executed) {
  if(!ep.live)return;
  const auto elapsed=sample.gameMs-ep.began;
  livePhysicalFlags(ep);liveClearance(ep,w,elapsed);
  if(!ep.drive)ep.live->observation(sample,nullptr,elapsed,ep.decisions);
  auto& out=ep.live->decisions;
  const auto keys=[&](const NpcKeys::KeyState& value) {
    out<<"{\"forward\":"<<value.forward<<",\"strafe\":"<<value.strafe<<",\"turn\":"<<value.turn
       <<",\"walk\":"<<value.walk<<",\"jump\":"<<value.jump<<",\"climb\":"<<value.climb<<'}';
  };
  out<<"{\"elapsed_ms\":"<<elapsed<<",\"decision_index\":"<<ep.decisions<<",\"position_cm\":";
  vector(out,NpcKeys::physicalPosition(*ep.npc));out<<",\"yaw_deg\":"<<sample.bodyYawDeg<<",\"goal_cm\":";vector(out,ep.goal);
  out<<",\"carrot_cm\":";vector(out,ep.guidance.carrot);
  if(ep.liveGuidePlayer) {
    out<<",\"player_position_cm\":";vector(out,NpcKeys::physicalPosition(*ep.liveGuidePlayer));
    out<<",\"guide_distance_cm\":"<<(NpcKeys::physicalPosition(*ep.npc)-NpcKeys::physicalPosition(*ep.liveGuidePlayer)).length();
  }
  out<<",\"speed_cm_s\":"<<std::hypot(ep.velocity.x,ep.velocity.z)<<",\"keys\":";keys(executed);
  out<<",\"teacher_keys\":";keys(sample.keys);
  std::array<std::vector<double>,NavPolicy::headCount> probabilities;
  std::array<size_t,NavPolicy::headCount> selected={};
  std::array<double,NavPolicy::headCount> entropies={};
  const size_t heads=ep.drive?ep.policy->actionHeadCount():0;
  for(size_t i=0;i<heads;++i) {
    const auto& logits=ep.selected.logits[i];
    const double temperature=ep.policy->headTemperature(i);
    const double maximum=double(*std::max_element(logits.begin(),logits.end()))/temperature;
    double total=0;
    for(float logit:logits) {probabilities[i].push_back(std::exp(double(logit)/temperature-maximum));total+=probabilities[i].back();}
    for(double& value:probabilities[i]) {value/=total;if(value>0)entropies[i]-=value*std::log(value);}
    selected[i]=ep.sampled?ep.selectedIndices[i]:size_t(std::max_element(logits.begin(),logits.end())-logits.begin());
  }
  out<<",\"probabilities\":{";
  for(size_t i=0;i<heads;++i) {
    if(i)out<<',';
    out<<std::quoted(ep.policy->headName(i))<<":[";
    for(size_t j=0;j<probabilities[i].size();++j) {if(j)out<<',';out<<probabilities[i][j];}
    out<<']';
  }
  out<<"},\"selected\":{";
  for(size_t i=0;i<heads;++i) {if(i)out<<',';out<<std::quoted(ep.policy->headName(i))<<':'<<selected[i];}
  out<<"},\"chosen_probability\":{";
  for(size_t i=0;i<heads;++i) {if(i)out<<',';out<<std::quoted(ep.policy->headName(i))<<':'<<probabilities[i][selected[i]];}
  out<<"},\"entropy\":{";
  for(size_t i=0;i<heads;++i) {if(i)out<<',';out<<std::quoted(ep.policy->headName(i))<<':'<<entropies[i];}
  const auto& n=*ep.npc;
  out<<"},\"stuck\":"<<ep.blockage.active<<",\"body_blocked\":"<<ep.blockage.active<<",\"contact\":"<<ep.liveContact
     <<",\"safety_intervention\":"<<ep.liveSafety<<",\"peer_request_clipped\":"<<ep.liveRequestClipped
     <<",\"peer_resolved_clipped\":"<<ep.liveResolvedClipped<<",\"in_air\":"<<n.isInAir()
     <<",\"falling\":"<<n.isFalling()<<",\"swimming\":"<<n.isSwim()<<",\"climbing\":"<<n.navigationClimbing()
     <<",\"edge_clearance_cm\":"<<ep.liveEdgeClearanceCm<<",\"edge_clearance_censored\":"<<ep.liveEdgeCensored
     <<",\"water_clearance_cm\":"<<ep.liveWaterClearanceCm<<",\"water_clearance_censored\":"<<ep.liveWaterCensored
     <<",\"teacher_escape_required\":"<<(ep.teacherVersion==3 && ep.memoryTeacher.lastPlanStatus().escapeRequired)<<"}\n";
  if(!out)throw std::runtime_error("nav live: decision write failed");
  ep.liveContact=ep.liveSafety=ep.liveRequestClipped=ep.liveResolvedClipped=false;
}
std::vector<NonfiniteObservation::Field> nonfiniteFields(const NavDemoWriter::Snapshot& sample,bool sensors) {
  std::vector<NonfiniteObservation::Field> result;
  const auto scalar=[&](const std::string& name,float value) {
    if(std::isfinite(value))return;
    std::ostringstream out;out<<std::setprecision(9)<<value;
    result.emplace_back(name,out.str());
  };
  const auto values=[&](const std::string& name,const std::array<float,3>& value) {
    for(size_t i=0;i<value.size();++i)
      if(!std::isfinite(value[i]))scalar(name+'['+std::to_string(i)+']',value[i]);
  };
  values("position_cm",sample.position);scalar("body_yaw_deg",sample.bodyYawDeg);
  values("body_velocity_cm_s",sample.body.velocityCmS);values("ground_normal",sample.body.groundNormal);
  scalar("ground_slope_deg",sample.body.groundSlopeDeg);
  scalar("head_yaw_deg",sample.body.headYawDeg);scalar("head_pitch_deg",sample.body.headPitchDeg);
  values("roads_carrot_direction",sample.roads.carrotDirection);values("roads_next_direction",sample.roads.nextDirection);
  values("roads_goal_direction",sample.roads.goalDirection);
  scalar("carrot_distance_cm",sample.roads.carrotDistanceCm);scalar("next_distance_cm",sample.roads.nextDistanceCm);
  scalar("goal_distance_cm",sample.roads.goalDistanceCm);scalar("remaining_known_cm",sample.roads.remainingKnownCm);
  scalar("remaining_visual_cm",sample.roads.remainingVisualCm);
  if(sensors && sample.eye)for(size_t i=0;i<sample.eye->size();++i) {
    const auto& ray=(*sample.eye)[i];
    if(!std::isfinite(ray.maxRangeCm))scalar("eye["+std::to_string(i)+"].max_range_cm",ray.maxRangeCm);
    if(!std::isfinite(ray.distanceCm))scalar("eye["+std::to_string(i)+"].distance_cm",ray.distanceCm);
    if(!std::isfinite(ray.normalY))scalar("eye["+std::to_string(i)+"].normal_y",ray.normalY);
  }
  if(sensors && sample.memory) {
    const auto layer=[&](const char* name,const NavigationMemory::Layer& value) {
      for(size_t i=0;i<value.cells.size();++i) {
        const auto& cell=value.cells[i];
        const float channels[]={cell.confidence,cell.free,cell.wall,cell.ledge,cell.drop,cell.water,cell.movable};
        constexpr const char* names[]={"confidence","free","wall","ledge","drop","water","movable"};
        for(size_t c=0;c<7;++c)
          if(!std::isfinite(channels[c]))scalar(std::string(name)+'['+std::to_string(i)+"]."+names[c],channels[c]);
        // NaN ground height is the memory's documented unknown-cell sentinel.
        if(std::isinf(cell.groundHeightCm))scalar(std::string(name)+'['+std::to_string(i)+"].ground_height_cm",cell.groundHeightCm);
      }
    };
    layer("memory_near",sample.memory->near);layer("memory_far",sample.memory->far);
  }
  return result;
}
NonfiniteObservation observationError(Episode& ep,const NavDemoWriter::Snapshot& sample,const char* error,
                                     std::vector<NonfiniteObservation::Field> fields) {
    // Preserve the exact failed observation in the exception: encode precedes
    // recording, so this state cannot be recovered from the last good record.
    std::ostringstream out;out<<std::setprecision(9)<<std::boolalpha;
    out<<error<<"; navdemos slot="<<ep.slot<<" episode="<<ep.sequence
       <<" from="<<std::quoted(ep.from)<<" to="<<std::quoted(ep.to)
       <<" time_ms="<<sample.gameMs<<" elapsed_ms="<<sample.gameMs-ep.began;
    const auto values=[&](const char* name,const std::array<float,3>& v) {
      out<<' '<<name<<"=["<<v[0]<<','<<v[1]<<','<<v[2]<<']';
    };
    values("position_cm",sample.position);
    out<<" body_yaw_deg="<<sample.bodyYawDeg;
    values("body_velocity_cm_s",sample.body.velocityCmS);
    values("ground_normal",sample.body.groundNormal);
    out<<" ground_slope_deg="<<sample.body.groundSlopeDeg
       <<" head_yaw_deg="<<sample.body.headYawDeg<<" head_pitch_deg="<<sample.body.headPitchDeg;
    values("roads_carrot_direction",sample.roads.carrotDirection);
    values("roads_next_direction",sample.roads.nextDirection);
    values("roads_goal_direction",sample.roads.goalDirection);
    out<<" carrot_distance_cm="<<sample.roads.carrotDistanceCm
       <<" next_distance_cm="<<sample.roads.nextDistanceCm<<" goal_distance_cm="<<sample.roads.goalDistanceCm
       <<" remaining_known_cm="<<sample.roads.remainingKnownCm
       <<" remaining_visual_cm="<<sample.roads.remainingVisualCm
       <<" beyond_known="<<sample.roads.beyondKnown;
    return {out.str(),sample.gameMs,std::move(fields)};
}
NavPolicy::Features policyFeatures(Episode& ep,const NavDemoWriter::Snapshot& sample) {
  // Reject raw infinities too: distance and velocity normalization may otherwise
  // clamp them into finite features. Never replace a learner observation.
  auto fields=nonfiniteFields(sample,false);
  if(!fields.empty())throw observationError(ep,sample,"nav policy: non-finite observation",std::move(fields));
  try {
    return ep.policy->encode(sample,ep.policy->paceInput()?forcedWalk(ep):std::nullopt);
  }
  catch(const std::runtime_error& error) {
    const std::string message=error.what();
    if(message!="nav policy: non-finite learner feature" &&
       message!="nav policy: invalid eye range or hit kind" &&
       message!="nav policy: infinite memory ground height" &&
       message!="nav policy: memory channel outside [0,1]")throw;
    fields=nonfiniteFields(sample,true);
    if(fields.empty())throw; // Shape, range and unrelated failures stay fatal.
    throw observationError(ep,sample,error.what(),std::move(fields));
  }
}
void rejectObservation(Episode& ep,World& w,const NonfiniteObservation& error,const char* stage) {
  ep.trainingValid=false;ep.success=false;ep.failure="nonfinite_observation";
  ep.nonfiniteStage=stage;ep.nonfinite=std::make_unique<NonfiniteObservation>(error);
  // The entire episode is excluded by the updater, including its valid prefix.
  // Closing must not request another observation or a truncated bootstrap.
  ep.pending=false;finish(ep,w);
}
void bootstrap(Episode& ep,World& w) {
  // Evaluate a continuing task at the real truncated endpoint, without another
  // action or GRU step. Road progress is refreshed on a private guidance copy.
  observe(ep,w);
  auto guidance=ep.guidance;
  ep.memory.view(ep.view);
  KnownRoadsBench::readGuidance(w,*ep.npc,NpcKeys::physicalPosition(*ep.npc),ep.goal,guidance,memoryPassage(ep.view));
  const auto sample=snapshot(ep,w,&guidance);
  const auto features=policyFeatures(ep,sample);
  std::vector<NonfiniteObservation::Field> fields;
  const auto map=[&](const char* name,const auto& values) {
    for(size_t i=0;i<values.size();++i)if(!std::isfinite(values[i])) {
      std::ostringstream value;value<<std::setprecision(9)<<values[i];
      fields.emplace_back(std::string(name)+'['+std::to_string(i)+']',value.str());
    }
  };
  map("encoded_near",features.near);map("encoded_far",features.far);
  if(!fields.empty())throw observationError(ep,sample,"nav policy: non-finite bootstrap map feature",std::move(fields));
  NavDemoWriter writer((std::filesystem::path(ep.output)/"bootstrap.bin.gz").string(),demoVersion(ep));
  writer.append(sample);writer.finish();
}
NpcKeys::KeyState select(Episode& ep,NavPolicy::Result result) {
  ep.selected=std::move(result);
  for(size_t i=0;i<ep.policy->actionHeadCount();++i) {
    const auto& head=ep.selected.logits[i];
    const double temperature=ep.policy->headTemperature(i);
    const double maximum=*std::max_element(head.begin(),head.end())/temperature;
    std::vector<double> weights;double total=0;
    for(float value:head) {weights.push_back(std::exp(double(value)/temperature-maximum));total+=weights.back();}
    size_t index=size_t(std::max_element(head.begin(),head.end())-head.begin());
    if(ep.sampled) {
      auto& rng=ep.population?population.rngs[ep.slot]:ep.actionRng;
      const double draw=std::uniform_real_distribution<double>(0.,total)(rng);
      double cumulative=0;index=weights.size()-1;
      for(size_t j=0;j<weights.size();++j) {cumulative+=weights[j];if(draw<cumulative) {index=j;break;}}
    }
    ep.selectedIndices[i]=index;ep.selectedLogProb[i]=double(head[index])/temperature-maximum-std::log(total);
  }
  // Pace-input weights read the forced pace; older weights only get the key overridden.
  const auto walk=forcedWalk(ep);
  auto keys=ep.policy->keys(ep.selectedIndices,ep.policy->paceInput()?walk:std::nullopt);
  if(walk)keys.walk=*walk;
  return keys;
}
std::array<float,2> movementDirection(const NpcKeys::KeyState& keys) {
  // NpcKeys gives strafing priority over forward/backward motion.
  if(keys.strafe!=0)return {keys.strafe>0?1.f:-1.f,0};
  return {0,keys.forward>0?1.f:keys.forward<0?-1.f:0.f};
}
bool memoryDangerAhead(const NavDemoWriter::Snapshot& sample,const std::array<float,2>& movement) {
  if(movement[0]==0 && movement[1]==0)return false;
  const auto& near=sample.memory->near;
  for(size_t row=0;row<near.size;++row)for(size_t column=0;column<near.size;++column) {
    const auto& cell=near.cells[row*near.size+column];
    // Match channels quantized into NAVDEMO and the learner's observation.
    if(std::lround(cell.water*255.f)<128 && std::lround(cell.drop*255.f)<128)continue;
    const float right=(float(column)+.5f-float(near.size)*.5f)*near.cellCm;
    const float forward=(float(row)+.5f-float(near.size)*.5f)*near.cellCm;
    const float along=right*movement[0]+forward*movement[1];
    const float across=std::abs(right*movement[1]-forward*movement[0]);
    // A 15-cm body margin plus half a cell diagonal covers the commanded lane.
    const float approach=sample.body.velocityCmS[0]*right+sample.body.velocityCmS[2]*forward;
    if(along>0 && along<=100.f && across<=15.f+near.cellCm*.707107f && approach>0)return true;
  }
  return false;
}
std::vector<Vec3> daggerTeacherPath(const Episode& ep,const NavDemoWriter::Snapshot& sample) {
  const Vec3 position(sample.position[0],sample.position[1],sample.position[2]);
  std::vector<Vec3> path={position};
  if(ep.teacherVersion==3) {
    const float yaw=sample.bodyYawDeg*Pi/180.f,c=std::cos(yaw),s=std::sin(yaw);
    for(const auto& point:ep.memoryTeacher.plan())
      path.push_back(position+Vec3(point.forwardCm*c-point.rightCm*s,0,
                                   point.forwardCm*s+point.rightCm*c));
  }
  else {
    const Vec3 corner(sample.teacherCorner[0],sample.teacherCorner[1],sample.teacherCorner[2]);
    path.push_back(corner);
    // The hint can refer to a direct segment outside the retained corridor.
    // Only continue through that corridor when its corner matches the hint.
    for(size_t i=0;i<ep.path.corners.size();++i)if((cm(ep.path.corners[i])-corner).length()<1.f) {
      for(size_t j=i+1;j<ep.path.corners.size();++j)path.push_back(cm(ep.path.corners[j]));
      break;
    }
  }
  return path;
}
float daggerPathArc(const std::vector<Vec3>& path,const Vec3& position) {
  float closest=std::numeric_limits<float>::max(),arc=0,offset=0;
  for(size_t i=1;i<path.size();++i) {
    const auto delta=path[i]-path[i-1],relative=position-path[i-1];
    const float square=delta.x*delta.x+delta.z*delta.z;
    if(square==0)continue;
    const float fraction=std::clamp((relative.x*delta.x+relative.z*delta.z)/square,0.f,1.f);
    const auto error=relative-delta*fraction;
    const float distance=error.x*error.x+error.z*error.z,length=std::sqrt(square);
    if(distance<closest) {closest=distance;arc=offset+fraction*length;}
    offset+=length;
  }
  return arc;
}
DaggerEvidence daggerTrigger(Episode& ep,const NavDemoWriter::Snapshot& sample,const NpcKeys::KeyState& learner) {
  DaggerEvidence evidence;
  const auto movement=movementDirection(learner);
  const bool moving=movement[0]!=0 || movement[1]!=0;
  const auto now=sample.gameMs;
  ep.daggerMotion.push_back({{now,Vec3(sample.position[0],sample.position[1],sample.position[2]),
                             ep.learnerCommanded},daggerTeacherPath(ep,sample)});
  ep.learnerCommanded=moving;
  const auto cutoff=now>=2000?now-2000:0;
  while(ep.daggerMotion.size()>1 && ep.daggerMotion[1].time<=cutoff)ep.daggerMotion.pop_front();
  uint64_t commanded=0;
  for(size_t i=1;i<ep.daggerMotion.size();++i)
    if(ep.daggerMotion[i].commanded)commanded+=ep.daggerMotion[i].time-ep.daggerMotion[i-1].time;
  const auto displacement=ep.daggerMotion.back().position-ep.daggerMotion.front().position;
  evidence.progressWindowMs=now-ep.daggerMotion.front().time;
  evidence.targetDistanceCm=(ep.daggerMotion.back().position-ep.goal).length();
  evidence.followBand=ep.goalKind=="follow" && evidence.targetDistanceCm>=200.f && evidence.targetDistanceCm<=500.f;
  evidence.stalled=!evidence.followBand && moving && evidence.progressWindowMs>=2000 && commanded>=2000 &&
                   std::hypot(displacement.x,displacement.z)<30.f;
  // Freeze the earlier teacher polyline: replanning can change its length even
  // without movement. Progress is forward arc distance on that same route.
  if(evidence.progressWindowMs>=2000) {
    const auto& previous=ep.daggerMotion.front();
    evidence.pathProgressCm=daggerPathArc(previous.teacherPath,ep.daggerMotion.back().position)-
                            daggerPathArc(previous.teacherPath,previous.position);
  }
  auto teacher=movementDirection(sample.keys);
  if(ep.teacherVersion==3) {
    // The endpoint can lie behind an obstacle while the next route segment
    // correctly leads away from it. Preserve backward/sideways recovery keys.
    if(sample.keys.forward>=0 && sample.keys.strafe==0)
      for(const auto& point:ep.memoryTeacher.plan())
        if(std::hypot(point.rightCm,point.forwardCm)>1.f) {
          teacher={point.rightCm,point.forwardCm};break;
        }
  }
  else {
    const Vec3 delta(sample.teacherCorner[0]-sample.position[0],0,sample.teacherCorner[2]-sample.position[2]);
    const auto relative=local(delta,sample.bodyYawDeg*Pi/180.f);
    if(std::hypot(relative[0],relative[2])>=1.f)teacher={relative[0],relative[2]};
  }
  const float teacherLength=std::hypot(teacher[0],teacher[1]);
  if(teacherLength>0)evidence.headingCosine=(movement[0]*teacher[0]+movement[1]*teacher[1])/teacherLength;
  const bool disagree=moving && evidence.headingCosine<-.5f;
  if(disagree && !ep.headingDisagrees)ep.headingDisagreedAt=now;
  ep.headingDisagrees=disagree;
  evidence.headingMs=disagree?now-ep.headingDisagreedAt:0;
  evidence.memoryDanger=memoryDangerAhead(sample,movement);
  if(evidence.stalled)evidence.trigger="stall";
  else if(evidence.memoryDanger)evidence.trigger="memory_danger";
  else if(disagree && evidence.headingMs>2000 && evidence.pathProgressCm<=0)evidence.trigger="heading";
  return evidence;
}
NpcKeys::KeyState mixTeacher(Episode& ep,const NavDemoWriter::Snapshot& sample,NpcKeys::KeyState learner) {
  const auto now=sample.gameMs;
  const auto learnerKeys=learner;
  const auto evidence=ep.dagger?daggerTrigger(ep,sample,learner):DaggerEvidence{};
  const auto* trigger=evidence.trigger;
  if(ep.dagger && now>=ep.teacherUntil && trigger[0]!='\0') {
    ep.teacherUntil=now+1000;
    ep.takeoverReason=trigger;++ep.daggerInterventions;
  }
  const bool held=ep.dagger && now<ep.teacherUntil;
  ep.teacherControlled=held || std::bernoulli_distribution(ep.teacherBeta)(ep.mixingRng);
  if(!held)ep.takeoverReason=ep.teacherControlled?"beta":"";
  bool dart=false;
  if(ep.teacherControlled) {
    learner=sample.keys;++ep.teacherControlDecisions;
    if(ep.dagger && ep.dartEpisode && !ep.dartStarted) {
      ep.dartStarted=true;ep.dartRemaining=std::uniform_int_distribution<unsigned>(3,10)(ep.dartRng);
      ep.teacherUntil=std::max(ep.teacherUntil,now+uint64_t(ep.dartRemaining)*100);
    }
    if(ep.dartRemaining>0) {
      // Perturb only locomotion on teacher steps; the unperturbed corrective
      // label remains in NAVDEMO, including its maneuver head.
      learner.forward=float(std::uniform_int_distribution<int>(-1,1)(ep.dartRng));
      learner.strafe=float(std::uniform_int_distribution<int>(-1,1)(ep.dartRng));
      learner.turn=float(std::uniform_int_distribution<int>(-1,1)(ep.dartRng));
      learner.walk=std::bernoulli_distribution(.5)(ep.dartRng);
      --ep.dartRemaining;++ep.dartDecisions;dart=true;
    }
  }
  if(ep.dagger && ep.samples.is_open()) {
    ep.samples<<"{\"time_ms\":"<<now<<",\"elapsed_ms\":"<<now-ep.began
              <<",\"teacher_control\":"<<ep.teacherControlled<<",\"maneuver_mask\":"<<ep.maneuverMasked
              <<",\"dart\":"<<dart<<",\"takeover\":"<<std::quoted(ep.takeoverReason)
              <<",\"learner_keys\":["<<learnerKeys.forward<<','<<learnerKeys.strafe<<','<<learnerKeys.turn
              <<','<<int(learnerKeys.walk)<<','<<int(learnerKeys.jump)<<','<<int(learnerKeys.climb)<<']'
              <<",\"trigger\":"<<std::quoted(trigger)<<",\"stall\":"<<evidence.stalled
              <<",\"follow_band\":"<<evidence.followBand<<",\"target_distance_cm\":"<<evidence.targetDistanceCm
              <<",\"memory_danger\":"<<evidence.memoryDanger<<",\"heading_cosine\":"<<evidence.headingCosine
              <<",\"heading_disagreement_ms\":"<<evidence.headingMs
              <<",\"teacher_path_progress_cm\":"<<evidence.pathProgressCm
              <<",\"progress_window_ms\":"<<evidence.progressWindowMs<<"}\n";
    if(!ep.samples)throw std::runtime_error("navdemos DAgger sample write failed");
  }
  ep.maneuverMaskedDecisions+=ep.maneuverMasked;
  return learner;
}
NpcKeys::KeyState executePolicy(Episode& ep,const NavDemoWriter::Snapshot& sample) {
  if(!ep.drive)return sample.keys;
  auto executed=sample.keys;
  // Encoding reads only learner fields. The very same snapshot is written below with
  // the teacher's label, while previous keeps the action that the student actually held.
  const bool measure=ep.decisions>=PolicyWarmupDecisions;
  const auto populationStart=std::chrono::steady_clock::now();
  const auto features=policyFeatures(ep,sample);
  const auto encodeCost=micros(populationStart);
  if(ep.policy->weightVersion()==4) {
    const auto nonzero=[](const std::vector<float>& values,size_t area) {
      return std::any_of(values.begin()+std::ptrdiff_t(10*area),values.end(),[](float value) {return value>0;});
    };
    const bool near=nonzero(features.near,64*64),far=nonzero(features.far,50*100);
    ++ep.familiarInputDecisions;
    ep.familiarInputNonzeroDecisions+=near || far;
    ep.familiarNearInputNonzeroDecisions+=near;
    ep.familiarFarInputNonzeroDecisions+=far;
  }
  if(measure)ep.encodeUs.push_back(encodeCost);
  if(ep.population)ep.phaseUs[3]+=encodeCost;
  for(size_t i=0;i<ep.policyStates.size();++i) {
    const auto began=std::chrono::steady_clock::now();
    NavPolicy::Result result;
    try {
      result=ep.policy->step(features,ep.policyStates[i]);
    }
    catch(const std::runtime_error& error) {
      const std::string message=error.what();
      if(message!="nav policy: non-finite near map feature" && message!="nav policy: non-finite far map feature")throw;
      auto fields=nonfiniteFields(sample,true);
      const auto map=[&](const char* name,const auto& values) {
        for(size_t j=0;j<values.size();++j)if(!std::isfinite(values[j])) {
          std::ostringstream value;value<<std::setprecision(9)<<values[j];
          fields.emplace_back(std::string(name)+'['+std::to_string(j)+']',value.str());
        }
      };
      map("encoded_near",features.near);map("encoded_far",features.far);
      throw observationError(ep,sample,error.what(),std::move(fields));
    }
    const auto policyCost=micros(began);
    if(measure)ep.policyUs.push_back(policyCost);
    if(ep.population)ep.phaseUs[4]+=policyCost;
    if(i==0) {
      if(ep.population || ep.sampled)executed=select(ep,std::move(result));
      else {executed=result.keys;if(ep.live)ep.selected=std::move(result);}
    }
  }
  if(measure)ep.populationUs.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-populationStart).count());
  executed.headYaw=0;executed.headPitch=0;
  const std::array<bool,6> disagree={executed.forward!=sample.keys.forward,executed.strafe!=sample.keys.strafe,
    executed.turn!=sample.keys.turn,executed.walk!=sample.keys.walk,executed.jump!=sample.keys.jump,executed.climb!=sample.keys.climb};
  for(size_t i=0;i<disagree.size();++i)ep.disagreements[i]+=disagree[i];
  // The recurrent student observes every visited state. The same state's
  // teacher label is always recorded, regardless of who presses the keys.
  executed=mixTeacher(ep,sample,executed);
  if(ep.live)ep.live->observation(sample,&features,sample.gameMs-ep.began,ep.decisions);
  return executed;
}
// The follower's sight of the object it follows: one ray from its eyes to the
// object's torso. Hitting the object's own rig near the torso counts as seen.
bool followVisible(const Episode& ep,World& w) {
  const auto* physics=w.physic();
  if(physics==nullptr || ep.target==nullptr)return false;
  const auto eye=ep.npc->navigationEyePose().origin;
  const auto body=ep.goal+Vec3(0,100,0);
  const auto hit=navigationRay(*physics,eye,body,ep.npc);
  return !hit.hasCol || (hit.kind==DynamicWorld::NavigationHit::Npc && (hit.point-body).length()<=80.f);
}
void traceRoadsClosed(Episode& ep,uint64_t elapsed,const std::vector<RoadsPassageCall>& calls) {
  if(!ep.roadsClosed.is_open() || !ep.guidance.rebuilt)return;
  auto& out=ep.roadsClosed;
  const auto byte=[](float value) {return unsigned(std::lround(std::clamp(value,0.f,1.f)*255.f));};
  for(const auto& call:calls) {
    if(ep.roadsClosedLines>=ep.roadsClosedLimit)break;
    ++ep.roadsClosedLines;
    out<<"{\"elapsed_ms\":"<<elapsed<<",\"position_cm\":";vector(out,call.from);
    out<<",\"to_cm\":";vector(out,call.to);
    out<<",\"to_distance_cm\":"<<(call.to-call.from).length()<<",\"verdict\":"
       <<std::quoted(call.verdict==KnownRoads::Passage::Closed?"closed":call.verdict==KnownRoads::Passage::Free?"free":"unknown");
    if(call.verdict==KnownRoads::Passage::Closed) {
      const auto& c=call.closing;
      auto offset=c.center-call.from;offset.y=0;
      // "Under the body": the cell reaches within 40 cm of the feet.
      const float distance=offset.length();
      out<<",\"layer\":"<<std::quoted(c.far?"far":"near")<<",\"cell_cm\":"<<c.cellCm
         <<",\"cell_center_cm\":["<<c.center.x<<','<<c.center.z<<"],\"cell_distance_cm\":"<<distance
         <<",\"under_body\":"<<(distance-c.cellCm*.5f<=40.f)<<",\"along_cm\":"<<c.alongCm
         <<",\"channels\":{\"confidence\":"<<byte(c.cell.confidence)<<",\"free\":"<<byte(c.cell.free)
         <<",\"wall\":"<<byte(c.cell.wall)<<",\"ledge\":"<<byte(c.cell.ledge)<<",\"drop\":"<<byte(c.cell.drop)
         <<",\"water\":"<<byte(c.cell.water)<<",\"movable\":"<<byte(c.cell.movable)<<",\"familiar\":"<<byte(c.cell.familiar)<<'}';
    }
    out<<"}\n";
  }
  if(!out)throw std::runtime_error("navdemos roads closed trace write failed");
}
void traceRoadsPassages(Episode& ep,uint64_t elapsed,const std::vector<RoadsPassageCall>& calls) {
  if(!ep.roadsPassages.is_open())return;
  auto& out=ep.roadsPassages;
  out<<"{\"decision_index\":"<<ep.decisions<<",\"elapsed_ms\":"<<elapsed<<",\"calls\":[";
  for(size_t i=0;i<calls.size();++i) {
    if(i)out<<',';
    const auto& call=calls[i];
    out<<"{\"from_cm\":";vector(out,call.from);
    out<<",\"to_cm\":";vector(out,call.to);
    out<<",\"verdict\":"<<std::quoted(call.verdict==KnownRoads::Passage::Free?"Free":
                                       call.verdict==KnownRoads::Passage::Closed?"Closed":"Unknown")<<'}';
  }
  out<<"]}\n";
  if(!out)throw std::runtime_error("navdemos roads passage trace write failed");
}
void traceRoads(Episode& ep,uint64_t elapsed) {
  const auto& g=ep.guidance;
  ep.roadsRebuilds+=g.rebuilt;ep.roadsCatchUps+=g.caughtUp;ep.roadsProjectionsRejected+=g.projectionRejected;
  ep.roadsStartsMoved+=g.rebuilt && g.route.startSkipped>0;ep.roadsStartFallbacks+=g.rebuilt && g.route.startFallback;
  ep.roadsStartDepartures+=g.rebuilt && g.route.startDeparture;
  if(!ep.roadsGuidance.is_open())return;
  auto& out=ep.roadsGuidance;
  out<<"d "<<elapsed<<' '<<g.progressCm<<' '<<int(g.rebuilt)<<' '<<int(g.caughtUp)<<' '<<int(g.beyondKnown)<<' '
     <<g.route.startSkipped<<' '<<int(g.route.startFallback)+int(g.route.startDeparture)<<' '<<g.carrot.x<<' '<<g.carrot.y<<' '<<g.carrot.z<<' '
     <<g.remainingKnownCm<<' '<<int(g.projectionRejected)<<'\n';
  if(g.rebuilt) {
    out<<"p "<<g.polyline.size();
    for(const auto& point:g.polyline)out<<' '<<point.x<<' '<<point.y<<' '<<point.z;
    out<<'\n';
  }
  if(!out)throw std::runtime_error("navdemos roads guidance write failed");
}
void guidanceFor(Episode& ep,World& w,const Vec3& position) {
  if(ep.goalKind=="point") {
    ep.memory.view(ep.view);
    const bool traceClosed=ep.roadsClosed.is_open() && ep.roadsClosedLines<ep.roadsClosedLimit;
    if(ep.roadsPassages.is_open() || traceClosed) {
      std::vector<RoadsPassageCall> calls;
      KnownRoads::PassageCheck check=[&](const Vec3& from,const Vec3& to) {
        RoadsPassageCall call;call.from=from;call.to=to;
        call.verdict=roadsPassage(NavigationPassage::scan(ep.view,from,to,FollowTrail::corridorCm,
                                                          FollowTrail::footprintCm,traceClosed?&call.closing:nullptr));
        calls.push_back(call);
        return call.verdict;
      };
      KnownRoadsBench::readGuidance(w,*ep.npc,position,ep.goal,ep.guidance,check);
      traceRoadsClosed(ep,w.tickCount()-ep.began,calls);
      traceRoadsPassages(ep,w.tickCount()-ep.began,calls);
    }
    else
      KnownRoadsBench::readGuidance(w,*ep.npc,position,ep.goal,ep.guidance,memoryPassage(ep.view));
    traceRoads(ep,w.tickCount()-ep.began);
  }
  else {
    ep.guidance={};ep.guidance.carrot=ep.goal;ep.guidance.beyondKnown=true;
    if(ep.goalKind=="follow") {
      // Intent is visible in the existing learner fields: the physical target
      // remains the final goal, while steering ends at least 2.5 m before it.
      // The frozen memory planner receives this carrot like a road endpoint.
      const bool visible=followVisible(ep,w);
      ep.followVisibleDecisions+=visible;ep.followVisibleNow=visible;
      if(ep.followLine) {
        const auto delta=ep.goal-position;
        const float distance=delta.length();
        ep.guidance.carrot=position+delta*(distance>250.f?(distance-250.f)/distance:0.f);
      } else {
        // The carrot runs along where the follower has seen the object; only
        // its own near memory may permit a straight cut (game/followtrail.h).
        ep.memory.view(ep.view);
        ep.followTrail.observe(ep.goal,visible);
        const auto result=ep.followTrail.guide(position,ep.goal,ep.view);
        ep.guidance.carrot=result.carrot;
        ++ep.followSources[size_t(result.source)];ep.followLoopLimitedDecisions+=result.loopLimited;
        ep.followLoopLineDecisions+=result.loopLine;ep.followDetourDecisions+=result.detour;
        ep.followGuardedDecisions+=result.guarded;
        ep.followSource=result.source;
      }
      ep.guidance.beyondKnown=false;
    }
  }
}
void oracleMemory(NavMemoryTeacher::Input& input,const NavMesh::LocalRaster& raster) {
  const auto layer=[](auto& target,const NavMesh::RasterLayer& source,size_t size,float step) {
    if(source.size!=size || source.cellSize!=step || source.cells.size()!=target.size()) {
      throw std::runtime_error("navdemos oracle raster shape mismatch");
    }
    for(size_t i=0;i<target.size();++i) {
      const auto& cell=source.cells[i];
      if(cell.heightBoundary && cell.traversable) {
        throw std::runtime_error("navdemos oracle raster has a traversable height boundary");
      }
      if(cell.traversable && !cell.heightValid) {
        throw std::runtime_error("navdemos oracle raster has traversable ground without height");
      }
      auto& value=target[i];
      value={};
      value.channels[0]=255;
      value.channels[cell.traversable?1u:2u]=255;
      if(cell.heightBoundary) {
        value.channels[4]=255;
      }
      value.heightValid=uint8_t(cell.heightValid);
      value.groundHeightCm=cell.heightValid?cell.groundHeight*100.f:0.f;
    }
  };
  layer(input.near,raster.near,64,.25f);
  layer(input.far,raster.far,100,2.f);
}
void decideMemory(Episode& ep,World& w,uint64_t elapsed) {
  // This is the entire steering boundary. Snapshot construction may read the
  // world, but the teacher receives only fields exposed by learner_input().
  const auto position=NpcKeys::physicalPosition(*ep.npc);
  guidanceFor(ep,w,position);
  auto sample=snapshot(ep,w);
  auto input=NavMemoryTeacher::Input::fromLearner(sample.body,sample.roads,sample.previous,*sample.memory);
  if(ep.oracle) {
    // Evaluation adapter only: the frozen teacher and its other learner fields
    // are unchanged. Truth observations are never written as demonstrations.
    const auto raster=NavBench::navigationMesh(w).rasterLocal(meters(position),ep.npc->rotation());
    ep.oracleUnsupportedDecisions+=!raster.supportValid;
    if(raster.supportValid) {
      ep.oracleSnappedSupportDecisions+=!raster.supportExact;
      ep.oracleSupportSnapMaxCm=std::max(ep.oracleSupportSnapMaxCm,raster.supportSnapDistance*100.f);
      ep.oracleSupportHorizontalMaxCm=std::max(ep.oracleSupportHorizontalMaxCm,raster.supportHorizontalDistance*100.f);
      ep.oracleSupportVerticalAbsMaxCm=std::max(ep.oracleSupportVerticalAbsMaxCm,std::abs(raster.supportVerticalOffset)*100.f);
    }
    oracleMemory(input,raster);
  }
  sample.keys=ep.memoryTeacher.decide(input);++ep.replans;
  // The game sets the pace: a companion always runs after its object.
  if(ep.goalKind=="follow" && ep.followRun)sample.keys.walk=false;
  // A stand comparison may force the teacher's pace off follow, as --force-pace does for a policy.
  if(!ep.drive && ep.goalKind!="follow" && ep.teacherPace!=0)sample.keys.walk=ep.teacherPace==1;
  const auto branch=ep.memoryTeacher.lastBranch();
  if(!ep.drive) {
    // Far from the goal: outside the follow band, otherwise beyond 2 m.
    const float far=ep.goalKind=="follow"?500.f:200.f;
    ++ep.teacherBranches[size_t(branch)];
    if((position-ep.goal).length()>far) {
      ++ep.teacherFarDecisions;
      if(sample.keys.forward==0 && sample.keys.strafe==0)++ep.teacherFarIdle[size_t(branch)];
    }
  }
  ep.maneuverMasked=false;
  if(ep.dagger) {
    sample.keys.jump=false;sample.keys.climb=false;
    for(const auto& link:NavBench::navigationMesh(w).params().links) {
      if((position-cm(link.start)).length()<=300.f ||
         (link.bidirectional && (position-cm(link.end)).length()<=300.f)) {
        ep.maneuverMasked=true;break;
      }
    }
  }
  ep.startNotFoundDecisions+=!ep.memoryTeacher.lastPlanStatus().startFound;
  const auto& points=ep.memoryTeacher.plan();
  auto& out=ep.teacherPaths;
  out<<"{\"elapsed_ms\":"<<elapsed<<",\"position_cm\":";vector(out,position);
  out<<",\"yaw_deg\":"<<ep.npc->rotation()<<",\"goal_cm\":";vector(out,ep.goal);
  out<<",\"carrot_cm\":";vector(out,ep.guidance.carrot);
  out<<",\"beyond_known\":"<<ep.guidance.beyondKnown;
  if(ep.goalKind=="follow") {
    // Diagnosis only: the trail state and what the planner sees under the body.
    const auto& status=ep.memoryTeacher.lastPlanStatus();
    out<<",\"follow_source\":"<<std::quoted(ep.followSource==FollowTrail::Source::Count?"none":FollowTrail::name(ep.followSource))
       <<",\"follow_visible\":"<<ep.followVisibleNow<<",\"trail_points\":"<<ep.followTrail.points().size()
       <<",\"trail_lost\":"<<ep.followTrail.lost()<<",\"start_found\":"<<status.startFound
       <<",\"escape_required\":"<<status.escapeRequired<<",\"overlap_danger_cells\":"<<status.overlapDangerCells
       <<",\"origin_channels\":[";
    // Byte maximum per channel over the four near cells that meet at the body origin.
    const auto& near=sample.memory->near;
    std::array<unsigned,8> bytes={};
    for(size_t z=near.size/2-1;z<=near.size/2;++z)for(size_t x=near.size/2-1;x<=near.size/2;++x) {
      const auto& c=near.cells[z*near.size+x];
      const float values[]={c.confidence,c.free,c.wall,c.ledge,c.drop,c.water,c.movable,c.familiar};
      for(size_t k=0;k<8;++k)bytes[k]=std::max(bytes[k],unsigned(std::lround(values[k]*255.f)));
    }
    for(size_t k=0;k<8;++k)out<<(k?",":"")<<bytes[k];
    out<<']';
  }
  {
    const auto& status=ep.memoryTeacher.lastPlanStatus();
    out<<",\"teacher_branch\":"<<std::quoted(NavMemoryTeacher::name(branch))<<",\"body_exit_cm\":"<<status.bodyExitCm;
    // Diagnosis only (OPENGOTHIC_NAV_STALL_TRACE=1): at the first decision of every
    // stretch without a start cell, then every 5 s of it, the near memory around the
    // feet (+-10 cells) next to the navmesh truth in the same body frame.
    const auto* traced=std::getenv("OPENGOTHIC_NAV_STALL_TRACE");
    const bool stalled=!status.startFound;
    if(traced && std::string_view(traced)=="1" && stalled && (!ep.teacherStallPrevious || elapsed>=ep.teacherStallTraceAt+5000)) {
      ep.teacherStallTraceAt=elapsed;
      const auto raster=NavBench::navigationMesh(w).rasterLocal(meters(position),ep.npc->rotation());
      const auto& near=sample.memory->near;
      out<<",\"stall_patch\":{\"feet_y_cm\":"<<position.y<<",\"support\":"<<raster.supportValid<<",\"cells\":[";
      bool first=true;
      for(int z=int(near.size/2)-10;z<int(near.size/2)+10;++z)for(int x=int(near.size/2)-10;x<int(near.size/2)+10;++x) {
        const auto& c=near.cells[size_t(z)*near.size+size_t(x)];
        const auto& t=raster.near.cells[size_t(z)*raster.near.size+size_t(x)];
        const auto byte=[](float v) {return unsigned(std::lround(v*255.f));};
        out<<(first?"":",")<<'['<<x-int(near.size/2)<<','<<z-int(near.size/2)<<','<<byte(c.confidence)<<','<<byte(c.free)<<','
           <<byte(c.wall)<<','<<byte(c.ledge)<<','<<byte(c.drop)<<','<<byte(c.water)<<','<<byte(c.movable)<<','<<byte(c.familiar)<<',';
        if(std::isfinite(c.groundHeightCm))out<<std::lround(c.groundHeightCm-position.y);else out<<"null";
        out<<','<<t.traversable<<','<<t.heightBoundary<<',';
        if(t.heightValid)out<<std::lround(t.groundHeight*100.f-position.y);else out<<"null";
        out<<']';first=false;
      }
      out<<"]}";
    }
    ep.teacherStallPrevious=stalled;
  }
  out<<",\"local_plan_cm\":[";
  for(size_t i=0;i<points.size();++i) {if(i)out<<',';out<<'['<<points[i].rightCm<<','<<points[i].forwardCm<<']';}
  out<<"]}\n";
  if(!out)throw std::runtime_error("navdemos memory teacher path write failed");
  if(!points.empty()) {
    const float yaw=ep.npc->rotationRad();
    const auto& p=points.back();
    sample.teacherCorner=array(position+Vec3(p.forwardCm*std::cos(yaw)-p.rightCm*std::sin(yaw),0,
                                           p.forwardCm*std::sin(yaw)+p.rightCm*std::cos(yaw)));
  }
  sample.teacherPathCm=ep.memoryTeacher.planLengthCm();
  const auto executed=executePolicy(ep,sample);
  liveDecision(ep,w,sample,executed);
  if(ep.writer) {
    const double cost=ep.writer->append(sample);ep.writeUs+=cost;ep.maxWriteUs=std::max(ep.maxWriteUs,cost);
  }
  ep.backwardDecisions+=executed.forward<0;ep.strafeDecisions+=executed.strafe!=0;
  action(ep,elapsed,executed);NpcKeys::set(*ep.npc,executed,1000);ep.previous=executed;++ep.decisions;
  ep.failure=ep.blockage.active?"body_blocked":"";
}
void decide(Episode& ep,World& w) {
  auto& n=*ep.npc;const auto elapsed=w.tickCount()-ep.began;
  trajectory(ep,elapsed);
  if(ep.goalKind=="follow") {
    ep.followContactDecisions+=ep.followContact;
    ep.followContactEntries+=ep.followContact && !ep.followContactPrevious;
    ep.followContactPrevious=ep.followContact;ep.followContact=false;
  }
  if(ep.replay) {
    if(ep.replayIndex>=ep.replayActions.size())throw std::runtime_error("navdemos missing replay action");
    const auto& a=ep.replayActions[ep.replayIndex++];
    if(a.time!=elapsed)throw std::runtime_error("navdemos replay clock mismatch");
    NpcKeys::set(n,a.keys,1000);ep.previous=a.keys;++ep.decisions;return;
  }
  if(ep.drive || ep.teacherVersion>=2)measureBlocked(ep,elapsed);
  if(ep.targetTrajectory.is_open()) {
    const float distance=(NpcKeys::physicalPosition(n)-ep.goal).length();
    ep.targetTrajectory<<elapsed<<' '<<ep.goal.x<<' '<<ep.goal.y<<' '<<ep.goal.z<<' '
                       <<distance<<' '<<int(distance>=200.f && distance<=500.f)<<'\n';
  }
  if(ep.teacherVersion==3) {decideMemory(ep,w,elapsed);return;}
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
    guidanceFor(ep,w,position);
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
    // Retain the selected local corridor while approaching its next link. Equal
    // cost parallel drop/jump links otherwise alternate at every Detour query.
    // The corridor still ends at the shared carrot and is released after landing.
    const auto present=NavKeyTeacher::decideV2(body,ep.path,meters(ep.goal),carrot,false);
    const bool approaching=present.pathValid && !present.replan && present.hint.transitionId!=0 &&
                          !ep.path.corners.empty() && (cm(ep.path.corners.back())-ep.guidance.carrot).length()<50.f;
    if((!airborne && !approaching) || ep.path.corners.empty()) {
      ep.path=mesh.findPath(body.position,carrot);++ep.replans;
    }
    const bool direct=!airborne && !approaching && !ep.guidance.beyondKnown &&
                      mesh.straightWalkable(body.position,carrot);
    d=NavKeyTeacher::decideV2(body,ep.path,meters(ep.goal),carrot,direct);
    if(ep.target && !airborne && !d.hint.onTransition) {
      // These stand controls use the same target distance, previous action and
      // carrot exposed to the learner. Native jump/climb execution is retained.
      const float distance=(ep.goal-position).length();
      if(ep.targetObserved && elapsed>ep.targetObservedAt) {
        const auto movement=ep.goal-ep.previousTargetPosition;
        const float targetSpeed=std::hypot(movement.x,movement.z)*1000.f/float(elapsed-ep.targetObservedAt);
        if(targetSpeed>220.f)ep.targetRunningUntil=elapsed+500;
      }
      ep.previousTargetPosition=ep.goal;ep.targetObservedAt=elapsed;ep.targetObserved=true;
      d.keys.walk=ep.previous.walk;
      if(distance>400.f || (elapsed<ep.targetRunningUntil && distance>300.f))d.keys.walk=false;
      else if(distance<300.f)d.keys.walk=true;
      if(ep.goalKind=="follow" && (ep.guidance.carrot-position).length()<50.f) {
        d.keys.forward=0;d.keys.strafe=0;
        if(distance<220.f) {
          auto away=position-ep.goal;away.y=0;
          const auto relative=local(away,n.rotationRad());
          const bool sidewaysFirst=std::abs(relative[0])>std::abs(relative[2]);
          for(unsigned attempt=0;attempt<2;++attempt) {
            const bool sideways=attempt==0 ? sidewaysFirst : !sidewaysFirst;
            const float component=relative[sideways?0:2];
            if(component==0)continue;
            const float sign=component>0?1.f:-1.f;
            const float yaw=n.rotationRad();
            const auto step=sideways?Vec3(-std::sin(yaw),0,std::cos(yaw))*(sign*70.f):
                                     Vec3(std::cos(yaw),0,std::sin(yaw))*(sign*70.f);
            // NpcKeys executes a cardinal axis, not the diagonal away vector.
            // Verify that exact step and try the other away axis if blocked.
            if(!mesh.straightWalkable(meters(position),meters(position+step)))continue;
            if(sideways)d.keys.strafe=sign;
            else d.keys.forward=sign;
            break;
          }
        }
      }
    }
    if(ep.goalKind=="follow" && ep.followRun)d.keys.walk=false;
    traceV2Plan(ep,position,d,direct,elapsed);
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
  auto sample=snapshot(ep,w);sample.keys=d.keys;
  auto hint=d.hint;
  if(ep.teacherVersion==1 && ep.path.reason=="waynet_fallback") {
    const auto meshPath=NavBench::navigationMesh(w).findPath(body.position,meters(ep.goal));
    hint=NavKeyTeacher::decide(body,meshPath,meters(ep.goal)).hint;
  }
  sample.teacherCorner=array(cm(hint.nextCorner));sample.teacherPathCm=hint.remainingLength*100;
  const auto executed=executePolicy(ep,sample);
  liveDecision(ep,w,sample,executed);
  const auto recordStart=Clock::now();
  if(ep.writer) {const double cost=ep.writer->append(sample);ep.writeUs+=cost;ep.maxWriteUs=std::max(ep.maxWriteUs,cost);}
  action(ep,elapsed,executed);NpcKeys::set(n,executed,1000);ep.previous=executed;++ep.decisions;
  ep.backwardDecisions+=executed.forward<0;ep.strafeDecisions+=executed.strafe!=0;
  if(ep.population) {ep.phaseUs[6]+=micros(recordStart);ep.pending=true;ep.actionAt=w.tickCount();}
  if(d.hint.remainingLength*100+20<ep.best) {ep.best=d.hint.remainingLength*100;ep.progressAt=w.tickCount();}
  if(!d.pathValid)ep.failure="no_path:"+ep.path.reason;
  else if((ep.drive || ep.teacherVersion==2)?ep.blockage.active:w.tickCount()-ep.progressAt>=3000)ep.failure="body_blocked";
  else ep.failure.clear();
}
std::vector<Pair> readPairs(const std::string& path) {
  std::ifstream in(path);
  if(!in)throw std::runtime_error("navdemos cannot read pairs "+path);
  std::vector<Pair> result;std::string line;
  while(std::getline(in,line)) {
    if(line.empty() || line[0]=='#')continue;
    std::istringstream row(line);Pair pair;std::string pace,extra;
    if(!(row>>pair.from>>pair.to))throw std::runtime_error("navdemos pairs require FROM TO [walk|run] per line");
    if(row>>pace) {
      if((pace!="walk" && pace!="run") || row>>extra)
        throw std::runtime_error("navdemos pairs require FROM TO [walk|run] per line");
      pair.walk=pace=="walk";
    }
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
  if(ep.policy->paceInput())ep.populationWalk=pop.walk.value_or(pair.walk.value_or(false));
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
  ep.policy->configureMemory(ep.memory,std::getenv("OPENGOTHIC_NAV_JOURNEY_MEMORY"));
  if(ep.memory.journeyMemoryEnabled())KnownRoadsBench::attachFamiliar(w,npc,ep.memory);
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
    ep.writer=std::make_unique<NavDemoWriter>((std::filesystem::path(ep.output)/"samples.bin.gz").string(),demoVersion(ep));
  }
  if(const char* trace=std::getenv("OPENGOTHIC_NAV_PASSAGE_TRACE");trace && *trace) {
    ep.roadsGuidance=file(ep,"roads-guidance.txt");
    ep.roadsPassages=file(ep,"roads-passages.jsonl");
  }
  ep.initialState=initialState(ep);
  observe(ep,w);
  try {decide(ep,w);ep.next=ep.began+100;}
  catch(const NonfiniteObservation& error) {rejectObservation(ep,w,error,"decision");}
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
     <<",\"invalid_observation_episodes\":"<<pop.invalidObservationEpisodes
     <<",\"training_valid_episodes\":"<<pop.completed-pop.invalidObservationEpisodes
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
  const bool hasPace=a.size()>11 && (a.back()=="walk" || a.back()=="run");
  const auto arguments=a.size()-size_t(hasPace);
  if(arguments<11 || arguments>13)
    throw std::runtime_error("navdemos population NPC PAIRS LIMIT_MS WEIGHTS N SEED sample|argmax EPISODES RECORD [near|far [RUN_MS]] [walk|run]");
  if(legacy.active || population.active)throw std::runtime_error("navdemos already active");
  population=Population{};auto& pop=population;pop.output=output;
  pop.instance=w.script().findSymbolIndex(a[2]);auto* original=w.findNpcByInstance(pop.instance);
  if(!original)throw std::runtime_error("navdemos population missing NPC");
  pop.home=original->position();pop.pairs=readPairs(a[3]);pop.limit=std::stoull(a[4]);
  pop.policy=std::make_shared<NavPolicy>(a[5]);
  if(pop.policy->navDemoVersion()!=2)throw std::runtime_error("navdemos population requires a NAVDEMO v2 policy");
  if(hasPace)pop.walk=a.back()=="walk";
  const auto count=std::stoull(a[6]);pop.seed=std::stoull(a[7]);
  if(a[8]!="sample" && a[8]!="argmax")throw std::runtime_error("navdemos population action mode must be sample or argmax");
  pop.sampled=a[8]=="sample";pop.targetEpisodes=std::stoull(a[9]);
  if(a[10]!="0" && a[10]!="1")throw std::runtime_error("navdemos population RECORD must be 0 or 1");
  pop.record=a[10]=="1";
  if(arguments>11 && a[11]!="near" && a[11]!="far")throw std::runtime_error("navdemos population hero mode must be near or far");
  pop.near=arguments>11 && a[11]=="near";pop.runMs=arguments>12?std::stoull(a[12]):0;
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
  for(const auto& slot:pop.slots)pop.initialDecisions+=slot->decisions;
  pop.startupSeconds=std::chrono::duration<double>(Clock::now()-pop.wallStart).count();
  for(auto& slot:pop.slots)slot->phaseUs={};
}
void tracePerceptionSafety(Episode& ep,World& w,uint64_t elapsed) {
  if(!ep.perceptionMonitor)return;
  auto& n=*ep.npc;
  const auto p=NpcKeys::physicalPosition(n);
  const auto water=ep.live?NavLiveProbes::waterAbove(w,p):w.physic()->waterRay(p+Vec3(0,2,0),50.f);
  const float depth=water.hasCol && std::isfinite(water.wdepth)?std::max(0.f,water.wdepth-p.y):0.f;
  auto& out=ep.perceptionSafetyFrames;
  out<<"{\"elapsed_ms\":"<<elapsed<<",\"position_cm\":";vector(out,p);
  out<<",\"cv_active\":"<<n.characterVirtualActive()<<",\"supported\":"<<NavTransitions::supported(w,n)
     <<",\"in_air\":"<<n.isInAir()<<",\"falling\":"<<n.isFalling()
     <<",\"native_jump\":"<<(n.isJump() || n.isJumpAnim())
     <<",\"native_climb\":"<<n.navigationClimbing()<<",\"swimming\":"<<n.isSwim()
     <<",\"water_depth_cm\":"<<depth
     <<",\"deep_water_threshold_cm\":"<<w.script().guildVal().water_depth_chest[n.guild()]
     <<",\"hitpoints\":"<<n.attribute(ATR_HITPOINTS)
     <<",\"hitpoints_max\":"<<n.attribute(ATR_HITPOINTSMAX)<<"}\n";
  if(!out)throw std::runtime_error("navdemos perception safety frame write failed");
}
void tickLiveTarget(Episode& ep,World& w,uint64_t elapsed,uint64_t dt) {
  auto& target=*ep.target;
  if(ep.liveTargetPath.corners.empty() && elapsed>=ep.targetResume) {
    ep.liveTargetPath=NavBench::navigationMesh(w).findPath(meters(ep.liveTargetPosition),meters(ep.targetRoute[ep.targetLeg]->groundPos));
    if(!ep.liveTargetPath.complete || ep.liveTargetPath.corners.size()<2)
      throw std::runtime_error("nav live: player route leg has no complete Detour path");
    ep.liveTargetCorner=1;ep.liveTargetLegAt=elapsed;
    target.setDirection(cm(ep.liveTargetPath.corners[1])-target.position());
  }
  if(ep.liveTargetPath.corners.empty()) {
    if(elapsed>=ep.liveTargetTurnAt) {
      target.setDirection(ep.targetRoute[ep.targetLeg]->groundPos-target.position());
      ep.liveTargetTurnAt=UINT64_MAX;
    }
    target.setPosition(ep.liveTargetPosition);target.updateTransform();
    return;
  }
  const bool running=!ep.liveTargetWalkOnly && ep.targetLeg==2 && elapsed-ep.liveTargetLegAt<2000;
  if(!ep.liveTargetWalking || running!=ep.liveTargetRunning) {
    target.setWalkMode(running?WalkBit::WM_Run:WalkBit::WM_Walk);target.setAnim(Npc::Anim::Move);
    ep.liveTargetWalking=true;ep.liveTargetRunning=running;
  }
  float distance=float(dt)*(ep.liveTargetWalkOnly?.10f:running?.45f:.20f);
  auto position=ep.liveTargetPosition;
  while(distance>0 && ep.liveTargetCorner<ep.liveTargetPath.corners.size()) {
    const auto corner=cm(ep.liveTargetPath.corners[ep.liveTargetCorner]);
    const auto delta=corner-position;const float length=delta.length();
    if(length>0)target.setDirection(delta);
    if(length>distance) {position+=delta*(distance/length);distance=0;}
    else {position=corner;distance-=length;++ep.liveTargetCorner;}
  }
  ep.liveTargetPosition=position;target.setPosition(position);target.updateTransform();
  if(ep.liveTargetCorner==ep.liveTargetPath.corners.size()) {
    ep.liveTargetPath={};ep.targetLeg=(ep.targetLeg+1)%ep.targetRoute.size();
    ep.targetResume=elapsed+2000;ep.liveTargetTurnAt=elapsed+1000;
    target.setAnim(Npc::Anim::Idle);target.clearSpeed();ep.liveTargetWalking=false;
    target.setDirection(target.rotation()+180.f);
  }
}
void tickLiveGuide(Episode& ep,uint64_t elapsed) {
  if(ep.liveGuidePlayer==nullptr)return;
  auto& player=*ep.liveGuidePlayer;
  const auto leader=NpcKeys::physicalPosition(*ep.npc);
  const float movement=(leader-ep.liveGuideTrail.back().second).length();
  if(movement>.001f) {
    ep.liveGuideTraveledCm+=movement;ep.liveGuideTrail.emplace_back(ep.liveGuideTraveledCm,leader);
  }
  const double desired=ep.liveGuideTraveledCm-ep.liveGuideLagCm;
  while(ep.liveGuideTrail.size()>2 && ep.liveGuideTrail[1].first<=desired)ep.liveGuideTrail.pop_front();
  const auto& from=ep.liveGuideTrail[0];const auto& to=ep.liveGuideTrail[1];
  const float fraction=float(std::clamp((desired-from.first)/(to.first-from.first),0.,1.));
  const auto position=from.second+(to.second-from.second)*fraction;
  const auto delta=position-ep.liveGuidePosition;
  const bool moved=std::hypot(delta.x,delta.z)>.01f;
  if(moved)ep.liveGuideLastMotionAt=elapsed;
  const bool walking=moved || (ep.liveGuideWalking && elapsed-ep.liveGuideLastMotionAt<200);
  if(walking!=ep.liveGuideWalking) {
    player.setWalkMode(WalkBit::WM_Walk);player.setAnim(walking?Npc::Anim::Move:Npc::Anim::Idle);
    if(!walking)player.clearSpeed();
    ep.liveGuideWalking=walking;
  }
  if(moved)player.setDirection(delta);
  ep.liveGuidePosition=position;player.setPosition(position);player.updateTransform();
}
void tickEpisode(Episode& ep,World& w,uint64_t dt,bool truncate=false) {
  if(!ep.active)return;
  auto& n=*ep.npc;++ep.frames;if(n.characterVirtualActive())++ep.cvFrames;
  const auto p=NpcKeys::physicalPosition(n);
  ep.travelCm+=(p-ep.previousPosition).length();
  if(dt) {
    ep.velocity=(p-ep.previousPosition)*(1000.f/float(dt));
  }
  ep.previousPosition=p;
  if(ep.population)
    ep.fell=ep.fell || n.isFalling() || n.isInAir();
  if(const auto* player=w.player()) {
    const double distance=(n.position()-player->position()).length();
    ep.minHeroDistanceCm=std::min(ep.minHeroDistanceCm,distance);ep.maxHeroDistanceCm=std::max(ep.maxHeroDistanceCm,distance);
  }
  const auto elapsed=w.tickCount()-ep.began;
  tickLiveGuide(ep,elapsed);
  livePhysicalFlags(ep);
  tracePerceptionSafety(ep,w,elapsed);
  if(ep.target) {
    auto& target=*ep.target;
    if(target.isDown()) {ep.failure="target_down";finish(ep,w);return;}
    if(ep.liveTargetPlayer)tickLiveTarget(ep,w,elapsed,dt);
    else {
      const auto* destination=ep.targetRoute[ep.targetLeg];
      if(ep.targetQueued && target.isAiQueueEmpty() && (target.position()-destination->groundPos).length()<120.f) {
        ep.targetQueued=false;
        if(ep.goalKind=="follow")ep.targetLeg=(ep.targetLeg+1)%ep.targetRoute.size();
        else ep.targetLeg=(ep.targetLeg+1+size_t(ep.targetRng()%(ep.targetRoute.size()-1)))%ep.targetRoute.size();
        ep.targetResume=elapsed+(ep.goalKind=="approach"?2000+ep.targetRng()%2001:0);
      }
      if(!ep.targetQueued && elapsed>=ep.targetResume) {
        target.setWalkMode(ep.goalKind=="follow" && (ep.targetLeg%2)==0?WalkBit::WM_Run:WalkBit::WM_Walk);
        target.aiPush(AiQueue::aiGoToPoint(*ep.targetRoute[ep.targetLeg]));ep.targetQueued=true;
      }
    }
    ep.goal=NpcKeys::physicalPosition(target);
    const float distance=(p-ep.goal).length();
    if(!ep.contacted && distance<=500.f) {ep.contacted=true;ep.firstContactMs=elapsed;}
    else if(ep.contacted) {
      ep.followMs+=dt;
      if(distance>=200.f && distance<=500.f)ep.inBandMs+=dt;
      else if(distance>500.f)ep.followFarMs+=dt;
      else ep.followNearMs+=dt;
    }
    if(ep.goalKind=="follow") {
      CharacterVirtualDiagnostics diagnostics;
      if(n.characterVirtualDiagnostics(diagnostics))
        ep.followContact=ep.followContact || diagnostics.npcMove.blocked || diagnostics.npcMove.steepContact;
    }
  }
  traceTransitionFrame(ep,w,elapsed);
  const bool teacherSupported=ep.drive || ep.teacherVersion!=2 || !ep.record || NavTransitions::supported(w,n);
  const bool following=ep.goalKind=="follow";
  const bool arrived=!following && (n.position()-ep.goal).length()<(ep.goalKind=="approach"?200.f:100.f) &&
                     !n.isInAir() && !n.navigationClimbing() && teacherSupported;
  if((!ep.replay && arrived) || elapsed>=ep.limit || n.isDown() || truncate) {
    if(following) {
      ep.success=!n.isDown() && !truncate && elapsed>=ep.limit && ep.followMs>0 &&
                 double(ep.inBandMs)/double(ep.followMs)>=.8;
      // Following has an observation horizon, not a final path endpoint. The
      // last steering failure must not overwrite a successful tracking metric.
      if(ep.success)ep.failure.clear();
      else if(n.isDown())ep.failure="actor_down";
      else if(truncate)ep.failure="collection_limit";
      else ep.failure=ep.contacted?"follow_band_below_threshold":"follow_no_contact";
    }
    else {
      ep.success=arrived;
      if(!ep.success && ep.failure.empty())ep.failure=n.isDown()?"actor_down":truncate?"collection_limit":"path_timeout";
    }
    if(ep.population) {
      measureBlocked(ep,elapsed);reward(ep,w,arrived || n.isDown(),!arrived && !n.isDown());
      if(ep.record && !arrived && !n.isDown()) {
        try {bootstrap(ep,w);}
        catch(const NonfiniteObservation& error) {rejectObservation(ep,w,error,"bootstrap");return;}
      }
    }
    finish(ep,w);return;
  }
  observe(ep,w);
  if(w.tickCount()>=ep.next) {decide(ep,w);ep.next+=100;}
}
void tickPopulation(World& w,uint64_t dt) {
  auto& pop=population;const bool stop=pop.runMs && w.tickCount()-pop.began>=pop.runMs;
  for(size_t i=0;i<pop.slots.size();++i) {
    auto& ep=*pop.slots[i];if(ep.accounted)continue;
    const auto previousDecisions=ep.decisions;
    try {tickEpisode(ep,w,dt,stop);}
    catch(const NonfiniteObservation& error) {rejectObservation(ep,w,error,"decision");}
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
    ++pop.completed;pop.decisions+=ep.decisions;ep.accounted=true;
    if(!ep.trainingValid)++pop.invalidObservationEpisodes;
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
  if(pop.completed==pop.started &&
     std::none_of(pop.slots.begin(),pop.slots.end(),[](const auto& slot){return slot->active;}))populationSummary(w);
}

}

bool NavDemos::running() {return legacy.active || population.active;}
bool NavDemos::nonhostileTarget(const Npc& npc) {
  return legacy.active && legacy.goalKind=="approach" && legacy.target==&npc;
}
bool NavDemos::forceCharacterVirtual(const Npc& npc) {
  return (TestBench::npcDuelActor(npc) && Gothic::physicsFlags().npcCharacterVirtual) ||
         (settings.cv && ((legacy.active && (legacy.npc==&npc || legacy.target==&npc)) || preparingLegacy==&npc ||
                          (population.active && population.members.contains(&npc)) ||
                          NavTransitions::controls(npc)));
}
bool NavDemos::fullSimulation(const Npc& npc) {
  return TestBench::npcDuelActor(npc) || (legacy.active && legacy.target==&npc) ||
         (population.active && population.members.contains(&npc)) || forceCharacterVirtual(npc);
}
void NavDemos::reset() {legacy=Episode{};population=Population{};settings=DemoSettings{};preparingLegacy=nullptr;}
void NavDemos::command(const std::vector<std::string>& args,World& w,Npc& player,const std::string& output) {
  if(args.size()>=2 && args[1]=="dump-package") {
    if(running() || NavBench::running())throw std::runtime_error("navsim package requires an idle stand");
    NavSimPackage::write(args,w,output);return;
  }
  if(args.size()>=2 && (args[1]=="perception-hazards" || args[1]=="perception-shores" || args[1]=="water-probes")) {
    if(running() || NavBench::running())throw std::runtime_error("navdemos cannot probe hazards during an episode");
    NavPerceptionSafety::command(args,w,output);
    return;
  }
  if(args.size()>=2 && args[1]=="perception-replay") {
    if(running() || NavBench::running())throw std::runtime_error("navdemos cannot replay perception during an episode");
    NavPerceptionBench::command(args,w,player,output);
    return;
  }
  auto a=args;
  auto& ep=legacy;
  if(a.size()>=2 && a[1]=="live-probes") {
    if(running() || NavBench::running())throw std::runtime_error("nav live probes require an idle stand");
    NavLiveProbes::command(a,w,output);return;
  }
  if(a.size()>=2 && (a[1]=="live" || a[1]=="live-target" || a[1]=="live-guide")) {
    if(running() || NavBench::running() || a.size()!=3)
      throw std::runtime_error("navdemos live settings require an idle stand and one value");
    if(a[1]=="live") {
      if(a[2]!="on" && a[2]!="off")throw std::runtime_error("navdemos live on|off");
      settings.live=a[2]=="on";
    } else if(a[1]=="live-target") {
      if(a[2]!="player" && a[2]!="player-walk" && a[2]!="npc")throw std::runtime_error("navdemos live-target player|player-walk|npc");
      settings.liveTargetPlayer=a[2]!="npc";settings.liveTargetWalkOnly=a[2]=="player-walk";
    } else {
      if(a[2]!="player" && a[2]!="off")throw std::runtime_error("navdemos live-guide player|off");
      settings.liveGuidePlayer=a[2]=="player";
    }
    return;
  }
  if(a.size()>=2 && (a[1]=="perception-mode" || a[1]=="perception-monitor")) {
    if(running() || NavBench::running() || a.size()!=3)
      throw std::runtime_error("navdemos perception settings require an idle stand and one value");
    if(a[1]=="perception-mode") {
      if(a[2]=="baseline")settings.floorSampling=NavigationEye::FloorSampling::Baseline;
      else if(a[2]=="gaze")settings.floorSampling=NavigationEye::FloorSampling::WalkingGazeDown;
      else if(a[2]=="rows")settings.floorSampling=NavigationEye::FloorSampling::ForwardRows;
      else throw std::runtime_error("navdemos perception-mode baseline|gaze|rows");
    } else {
      if(a[2]!="on" && a[2]!="off")throw std::runtime_error("navdemos perception-monitor on|off");
      settings.perceptionMonitor=a[2]=="on";
    }
    return;
  }
  if(a.size()>=2 && (a[1]=="oracle" || a[1]=="record")) {
    if(running() || NavBench::running()) {
      throw std::runtime_error("navdemos cannot change evaluation settings during episode");
    }
    if(a.size()!=3 || (a[2]!="on" && a[2]!="off")) {
      throw std::runtime_error("navdemos oracle on|off | record on|off");
    }
    if(a[1]=="oracle") {
      settings.oracle=a[2]=="on";
    }
    else {
      settings.recordDemos=a[2]=="on";
    }
    return;
  }
  bool probesHandled=false;
  commandProbes(a,w,output,probesHandled);
  if(probesHandled)return;
  if(a.size()>=2 && a[1]=="follow-guidance") {
    if(running())throw std::runtime_error("navdemos cannot change follow guidance during an episode");
    if(a.size()!=3 || (a[2]!="trail" && a[2]!="line"))throw std::runtime_error("navdemos follow-guidance trail|line");
    settings.followLine=a[2]=="line";
    return;
  }
  if(a.size()>=2 && (a[1]=="teacher-exit" || a[1]=="teacher-pace" || a[1]=="follow-lost")) {
    // Comparison switches of the memory teacher's body exit and of the lost follow
    // carrot, and the teacher's forced pace off follow (follow keeps follow-run).
    if(running())throw std::runtime_error("navdemos cannot change teacher settings during an episode");
    if(a[1]=="teacher-exit" && a.size()==3 && (a[2]=="on" || a[2]=="off"))settings.teacherBodyExit=a[2]=="on";
    else if(a[1]=="follow-lost" && a.size()==3 && (a[2]=="aside" || a[2]=="last-seen"))settings.followLostAside=a[2]=="aside";
    else if(a[1]=="teacher-pace" && a.size()==3 && (a[2]=="own" || a[2]=="walk" || a[2]=="run"))
      settings.teacherPace=a[2]=="own"?0:a[2]=="walk"?1:2;
    else throw std::runtime_error("navdemos teacher-exit on|off | teacher-pace own|walk|run | follow-lost aside|last-seen");
    return;
  }
  if(a.size()>=2 && a[1]=="follow-run") {
    if(running())throw std::runtime_error("navdemos cannot change the follow teacher pace during an episode");
    if(a.size()!=3 || (a[2]!="on" && a[2]!="off"))throw std::runtime_error("navdemos follow-run on|off");
    settings.followRun=a[2]=="on";
    return;
  }
  if(a.size()>=2 && (a[1]=="drive-teacher" || a[1]=="dagger")) {
    if(running())throw std::runtime_error("navdemos cannot change drive settings during an episode");
    if(a[1]=="drive-teacher" && a.size()==3 && (a[2]=="auto" || a[2]=="2" || a[2]=="3"))
      settings.driveTeacher=a[2]=="auto"?0u:uint32_t(std::stoul(a[2]));
    else if(a[1]=="dagger" && a.size()==3 && (a[2]=="on" || a[2]=="off"))settings.dagger=a[2]=="on";
    else throw std::runtime_error("navdemos drive-teacher auto|2|3 | dagger on|off");
    return;
  }
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
  if(settings.live && settings.recordDemos)
    throw std::runtime_error("nav live: evaluation requires record off");
  if(a.size()>=2 && a[1]=="population") {
    if(settings.liveGuidePlayer)throw std::runtime_error("nav live: player guide requires a single episode");
    if(settings.oracle) {
      throw std::runtime_error("navdemos oracle evaluation requires a single v3 episode");
    }
    beginPopulation(a,w,player,output);return;
  }
  if(a.size()==3 && a[1]=="reachable") {reachable(w,a[2],output);return;}
  if(a.size()==2 && a[1]=="wait")return;
  const bool memory=a.size()>=2 && a[1]=="start-v3";
  const bool learnerObject=a.size()>=2 && a[1]=="drive-object";
  const bool learnerMoving=a.size()>=2 && a[1]=="drive-moving";
  const bool object=a.size()>=2 && (a[1]=="object-v3" || learnerObject);
  const bool moving=a.size()>=2 && (a[1]=="moving-v3" || learnerMoving);
  uint32_t scenarioTeacher=(memory || learnerObject || learnerMoving)?3:2;
  std::string targetSymbol,routeText,goalKind="point";
  uint64_t scenarioSeed=0;
  std::vector<const WayPoint*> targetRoute;
  if(object || moving) {
    if((object && a.size()!=(learnerObject?10u:7u)) || (moving && a.size()!=(learnerMoving?13u:10u)))
      throw std::runtime_error("navdemos object-v3 VERSION NPC START GOAL LIMIT | moving-v3 VERSION NPC START TARGET ROUTE follow|approach LIMIT SEED | drive-object NPC START GOAL LIMIT WEIGHTS BETA SEED ACTION_MODE | drive-moving NPC START TARGET ROUTE follow|approach LIMIT SCENARIO_SEED WEIGHTS BETA SEED ACTION_MODE");
    if(!learnerObject && !learnerMoving)scenarioTeacher=uint32_t(std::stoul(a[2]));
    if(scenarioTeacher!=2 && scenarioTeacher!=3)throw std::runtime_error("navdemos scenario teacher must be 2 or 3");
    if(object) {
      goalKind="object";
      if(learnerObject)a[1]="drive";
      else a={a[0],"start-v2",a[3],a[4],a[5],a[6]};
    }
    else {
      goalKind=a[learnerMoving?6:7];targetSymbol=a[learnerMoving?4:5];routeText=a[learnerMoving?5:6];
      scenarioSeed=std::stoull(a[learnerMoving?8:9]);
      if(goalKind!="follow" && goalKind!="approach")throw std::runtime_error("navdemos moving mode follow|approach");
      std::replace(routeText.begin(),routeText.end(),',',' ');std::istringstream in(routeText);std::string name;
      while(in>>name) {
        const auto* point=w.findPoint(name,false);
        if(!point)throw std::runtime_error("navdemos moving missing route waypoint "+name);
        if(!point->isConnected())point=w.findWayPoint(point->groundPos,[](const WayPoint& p){return p.isConnected();});
        if(!point)throw std::runtime_error("navdemos moving route lacks waynet point");
        if(std::find(targetRoute.begin(),targetRoute.end(),point)==targetRoute.end())targetRoute.push_back(point);
      }
      if(targetRoute.size()<(goalKind=="follow"?3u:2u))throw std::runtime_error("navdemos moving route needs distinct connected points");
      if(learnerMoving)a={a[0],"drive",a[2],a[3],targetRoute.front()->name,a[7],a[9],a[10],a[11],a[12]};
      else a={a[0],"start-v2",a[3],a[4],targetRoute.front()->name,a[8]};
    }
  } else if(memory)a[1]="start-v2";
  const bool replay=a.size()==6 && a[1]=="replay";
  const bool drive=a.size()>=7 && a.size()<=11 && a[1]=="drive";
  const bool current=a.size()>=6 && a.size()<=9 && a[1]=="drive-current";
  const bool speed=a.size()==8 && a[1]=="speed";
  const bool learner=drive || current || speed;
  const bool carrot=(a.size()>=2 && a[1]=="start-v2") || learner;
  if(!replay && !learner && !((a.size()==6 || a.size()==8 || a.size()==9) && (a[1]=="start" || carrot)))
    throw std::runtime_error("navdemos start|start-v2 NPC FROM TO LIMIT [lateral_cm yaw_deg fraction] | replay NPC FROM TO ACTIONS | drive NPC FROM TO LIMIT WEIGHTS [BETA SEED ACTION_MODE [WATCH_TRANSITION_ID]] | drive-current NPC GOAL LIMIT WEIGHTS [BETA SEED ACTION_MODE] | speed NPC FROM TO LIMIT WEIGHTS POPULATION");
  if(ep.active || population.active)throw std::runtime_error("navdemos already active");
  if(settings.liveGuidePlayer && moving)throw std::runtime_error("nav live: player guide requires a point or object goal");
  const auto* from=current?nullptr:w.findPoint(a[3],false);const auto* to=w.findPoint(a[current?3:4],false);
  auto* npc=w.findNpcByInstance(w.script().findSymbolIndex(a[2]));
  if(!npc || (!carrot && ((!from && !current) || !to)))throw std::runtime_error("navdemos missing NPC/waypoint");
  if(settings.liveGuidePlayer && npc==&player)throw std::runtime_error("nav live: player guide must differ from leader");
  const auto fromPosition=current?npc->position():(carrot?demoPoint(w,a[3]):from->groundPos);
  const auto goalPosition=carrot?demoPoint(w,a[current?3:4]):to->groundPos;
  ep=Episode{};ep.npc=npc;ep.from=current?"intentional":a[3];ep.to=a[current?3:4];ep.output=output;
  auto eyeConfig=ep.eye.config();eyeConfig.floorSampling=settings.floorSampling;ep.eye=NavigationEye(eyeConfig);
  ep.perceptionMonitor=settings.perceptionMonitor;
  if(settings.recordDemos && settings.floorSampling==NavigationEye::FloorSampling::ForwardRows)
    throw std::runtime_error("navdemos rows is evaluation-only; use record off");
  ep.cvMode=settings.cv;ep.heroNear=!current && settings.heroNear;
  ep.teacherVersion=(memory || object || moving)?scenarioTeacher:(carrot?2:1);
  if((ep.teacherVersion==3 || (ep.teacherVersion==2 && settings.recordDemos)) && !learner && !replay) {
    const auto* journey=std::getenv("OPENGOTHIC_NAV_JOURNEY_MEMORY");
    ep.memory.setJourneyMemory(journey==nullptr || std::string_view(journey)!="0");
    if(ep.teacherVersion==3 || ep.memory.journeyMemoryEnabled())
      KnownRoadsBench::attachFamiliar(w,*npc,ep.memory);
  }
  ep.oracle=settings.oracle;
  if(ep.oracle && ep.teacherVersion!=3) {
    throw std::runtime_error("navdemos oracle evaluation requires teacher v3");
  }
  if((ep.oracle || !settings.recordDemos) && std::filesystem::exists(std::filesystem::path(output)/"samples.bin.gz")) {
    throw std::runtime_error("navdemos evaluation output already contains demonstration samples");
  }
  ep.goalKind=goalKind;ep.scenarioSeed=scenarioSeed;ep.followLine=settings.followLine;ep.followRun=settings.followRun;
  ep.teacherBodyExit=settings.teacherBodyExit;ep.followLostAside=settings.followLostAside;ep.teacherPace=settings.teacherPace;
  ep.memoryTeacher.setBodyExit(ep.teacherBodyExit);ep.followTrail.setLostAside(ep.followLostAside);
  ep.liveTargetPlayer=moving && settings.liveTargetPlayer;
  ep.liveTargetWalkOnly=ep.liveTargetPlayer && settings.liveTargetWalkOnly;
  ep.goal=goalPosition;ep.replay=replay;ep.drive=learner;
  // Optional start arguments begin at 6.
  ep.limit=replay?0:std::stoull(a[current?4:5]);
  if(learner) {
    ep.sampled=true;
    const auto population=speed?std::stoul(a[7]):1;
    if(population==0 || population>1000)throw std::runtime_error("navdemos invalid policy population");
    ep.policy=std::make_unique<NavPolicy>(a[current?5:6]);
    requirePace(*ep.policy);
    ep.teacherVersion=settings.driveTeacher?settings.driveTeacher:(ep.policy->navDemoVersion()>=2?3:1);
    ep.policy->configureMemory(ep.memory,std::getenv("OPENGOTHIC_NAV_JOURNEY_MEMORY"));
    if(ep.memory.journeyMemoryEnabled() && ep.policy->navDemoVersion()>=2)
      KnownRoadsBench::attachFamiliar(w,*npc,ep.memory);
    const size_t betaIndex=current?6:7;
    if(!speed && a.size()>betaIndex)ep.teacherBeta=std::stod(a[betaIndex]);
    if(!std::isfinite(ep.teacherBeta) || ep.teacherBeta<0 || ep.teacherBeta>1)
      throw std::runtime_error("navdemos teacher beta must be in [0,1]");
    ep.dagger=settings.dagger;
    if(ep.dagger) {
      if(speed || ep.policy->navDemoVersion()<2 || !ep.memory.journeyMemoryEnabled())
        throw std::runtime_error("navdemos DAgger requires a single v2+ learner with journey memory 4");
      ep.teacherBeta=std::max(.1,ep.teacherBeta);
    }
    if(!speed && a.size()>betaIndex+1)ep.mixingSeed=std::stoull(a[betaIndex+1]);
    if(!speed && a.size()>betaIndex+2) {
      if(a[betaIndex+2]!="sample" && a[betaIndex+2]!="argmax")
        throw std::runtime_error("navdemos drive action mode must be sample or argmax");
      ep.sampled=a[betaIndex+2]=="sample";
    }
    ep.mixingRng.seed(ep.mixingSeed);ep.actionRng.seed(ep.mixingSeed);
    ep.dartRng.seed(ep.mixingSeed^0x44415254u);
    if(ep.dagger)ep.dartEpisode=std::bernoulli_distribution(.1)(ep.dartRng);
    ep.policyStates.resize(population);
    ep.policyUs.reserve(size_t((ep.limit+99)/100)*population);
    ep.encodeUs.reserve(size_t((ep.limit+99)/100));
    ep.populationUs.reserve(size_t((ep.limit+99)/100));
    if(!speed && a.size()>betaIndex+3) {
      // Passive outcome evidence for a frozen jump case; never used to steer.
      const auto id=std::stoul(a[betaIndex+3]);
      const auto path=NavBench::navigationMesh(w).findPath(meters(fromPosition),meters(goalPosition));
      for(const auto& link:path.transitions)
        if(link.id==id) {ep.observedTransition=link;ep.hasObservedTransition=true;break;}
      if(!ep.hasObservedTransition)throw std::runtime_error("navdemos watched transition is absent from the initial label path");
    }
  }
  const bool npcWasExternal=npc->isExternalControl();
  const PrepareLegacy preparing(npc);
  if(settings.cv)npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->clearSpeed();
  npc->setExternalControl(true);npc->clearGoTo();
  if(!current) {npc->setPosition(fromPosition);npc->updateTransform();}
  // Keyboard episodes must not inherit a native route's waypoint from the save.
  npc->attachToPoint(nullptr);
  if(!current)npc->setDirection(ep.goal-npc->position());
  npc->setWalkMode(WalkBit::WM_Walk);
  if(moving) {
    ep.targetRoute=std::move(targetRoute);ep.targetRng.seed(scenarioSeed);ep.targetLeg=1;
    ep.target=ep.liveTargetPlayer?&player:w.addNpc(w.script().findSymbolIndex(targetSymbol),ep.targetRoute.front()->groundPos);
    if(!ep.target)throw std::runtime_error("navdemos cannot spawn moving target "+targetSymbol);
    if(ep.target==ep.npc)throw std::runtime_error("nav live: player target must differ from follower");
    auto& target=*ep.target;
    ep.liveTargetWasExternal=target.isExternalControl();
    target.setProcessPolicy(NpcProcessPolicy::AiNormal);target.setInteraction(nullptr,true);
    target.clearAiQueue();target.clearState(false);target.clearSpeed();target.setExternalControl(true);
    if(goalKind=="approach") {
      // This is a navigation target, not a combat encounter. The cloned stand
      // NPC still follows the ordinary seeded wandering route below.
      auto& handle=target.handle();
      handle.flags=daedalus::NpcFlag(uint32_t(handle.flags)|uint32_t(daedalus::NpcFlag::IMMORTAL));
      target.setAttitude(ATT_FRIENDLY);target.setTempAttitude(ATT_FRIENDLY);
    }
    target.clearGoTo();target.attachToPoint(nullptr);
    if(ep.liveTargetPlayer) {
      ep.liveTargetPosition=ep.targetRoute.front()->groundPos;
      target.setAnim(Npc::Anim::Idle);target.setPosition(ep.liveTargetPosition);target.updateTransform();
    }
    target.setDirection(ep.targetRoute[1]->groundPos-target.position());
    target.setWalkMode(WalkBit::WM_Walk);
    if(!ep.liveTargetPlayer) {target.aiPush(AiQueue::aiGoToPoint(*ep.targetRoute[1]));ep.targetQueued=true;}
    ep.goal=NpcKeys::physicalPosition(target);
    if(goalKind=="follow")npc->setCharacterVirtualDiagnosticsEnabled(true);
  }
  if(!replay && !learner && a.size()>6) {
    const auto path=NavBench::navigationMesh(w).findPath(meters(fromPosition),meters(ep.goal));
    if(!path.complete || path.corners.size()<2) {
      auto out=file(ep,"episode.json");
      out<<"{\"success\":false,\"failure\":\"dagger_start_not_supported\",\"elapsed_ms\":0,\"decisions\":0}\n";
      return;
    }
    float left=path.length*(a.size()>8?std::stof(a[8]):.5f);
    Vec3 point=fromPosition,tangent=ep.goal-point;
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
  const auto* liveStand=std::getenv("OPENGOTHIC_NAV_LIVE");
  if(settings.live || settings.liveGuidePlayer || ep.liveTargetPlayer || (liveStand!=nullptr && std::string_view(liveStand)=="1")) {
    Vec3 start;NavMesh::Path approach;
    if(!livePlayerStart(ep,w,player,start,approach)) {
      auto out=file(ep,"episode.json");
      out<<"{\"success\":false,\"setup_rejected\":true,\"failure\":\"live_setup_no_safe_observer_position\","
            "\"elapsed_ms\":0,\"decisions\":0,\"diagnostics\":\"live-guide-start.json\"}\n";
      npc->setExternalControl(npcWasExternal);npc->clearSpeed();npc->setCharacterVirtualDiagnosticsEnabled(false);
      if(ep.target) {
        if(ep.liveTargetPlayer) {ep.target->setExternalControl(ep.liveTargetWasExternal);ep.target->clearSpeed();}
        else w.removeNpc(*ep.target);
        ep.target=nullptr;
      }
      return;
    }
    const auto leader=NpcKeys::physicalPosition(*npc);
    if(settings.liveGuidePlayer) {
      ep.liveGuidePlayer=&player;ep.liveGuideWasExternal=player.isExternalControl();
      // Start the trail at the selected physical floor, using the ground-only
      // Detour prefix instead of a straight segment through nearby geometry.
      auto points=approach.corners;points.front()=meters(start);points.back()=meters(leader);
      double length=0;
      for(size_t i=1;i<points.size();++i)length+=(cm(points[i])-cm(points[i-1])).length();
      double traveled=-length;ep.liveGuideTrail.emplace_back(traveled,start);
      for(size_t i=1;i<points.size();++i) {
        const auto previous=cm(points[i-1]),next=cm(points[i]);const float delta=(next-previous).length();
        if(delta<=.001f)continue;
        traveled+=delta;ep.liveGuideTrail.emplace_back(traveled,next);
      }
      ep.liveGuideTrail.back().first=0;
      ep.liveGuideLagCm=std::max(300.,length);ep.liveGuidePosition=start;
      player.setExternalControl(true);
    }
    if(ep.liveTargetPlayer) {ep.liveTargetPosition=start;ep.goal=start;}
    player.setInteraction(nullptr,true);player.clearAiQueue();player.clearState(false);player.clearGoTo();player.clearSpeed();
    player.setWalkMode(WalkBit::WM_Walk);player.setAnim(Npc::Anim::Idle);
    player.setPosition(start);
    player.setDirection(ep.liveTargetPlayer?ep.targetRoute[ep.targetLeg]->groundPos-start:leader-start);player.updateTransform();
  }
  else if(!current && settings.heroNear) {player.setPosition(npc->position()+Vec3(0,0,500));player.updateTransform();}
  NpcKeys::enable(*npc,true);ep.previousPosition=NpcKeys::physicalPosition(*npc);
  ep.began=ep.next=ep.progressAt=w.tickCount();ep.active=true;ep.wallStart=std::chrono::steady_clock::now();
  ep.expectedStart=NpcKeys::physicalPosition(*npc);
  if(ep.perceptionMonitor) {
    ep.perceptionSafetyFrames=file(ep,"perception-safety-frames.jsonl");
    ep.perceptionWaterObservations=file(ep,"perception-water-observations.jsonl");
  }
  if(ep.teacherVersion>=2 && !ep.drive) {
    const auto reference=NavBench::navigationMesh(w).findPath(meters(ep.expectedStart),meters(ep.goal));
    ep.detourLengthCm=reference.complete?reference.length*100.:0;
  }
  ep.minHeroDistanceCm=ep.maxHeroDistanceCm=(npc->position()-player.position()).length();
  ep.trajectory=file(ep,"trajectory.txt");
  if(!replay && ep.goalKind=="point" && ep.teacherVersion>=2) {
    ep.roadsGuidance=file(ep,"roads-guidance.txt");
    if(const char* trace=std::getenv("OPENGOTHIC_NAV_PASSAGE_TRACE");trace && *trace)
      ep.roadsPassages=file(ep,"roads-passages.jsonl");
    if(const char* trace=std::getenv("OPENGOTHIC_NAV_ROADS_CLOSED_TRACE");trace && *trace) {
      ep.roadsClosedLimit=std::stoull(trace);
      if(ep.roadsClosedLimit>0)ep.roadsClosed=file(ep,"roads-closed.jsonl");
    }
  }
  if(replay) {
    std::ifstream input(a[5]);RecordedKeys row;int walk=0,jump=0,climb=0;
    while(input>>row.time>>row.keys.forward>>row.keys.strafe>>row.keys.turn>>walk>>jump>>climb>>row.keys.headYaw>>row.keys.headPitch) {
      row.keys.walk=walk!=0;row.keys.jump=jump!=0;row.keys.climb=climb!=0;ep.replayActions.push_back(row);
    }
    if(!input.eof() || ep.replayActions.size()<2)throw std::runtime_error("invalid actions replay");
    ep.limit=ep.replayActions.back().time;
  } else {
    ep.actions=file(ep,"actions.txt");
    if(settings.recordDemos && !ep.oracle) {
      ep.writer=std::make_unique<NavDemoWriter>((std::filesystem::path(output)/"samples.bin.gz").string(),demoVersion(ep));
      if(ep.dagger)ep.samples=file(ep,"samples.jsonl");
    }
    if(ep.teacherVersion>=2) {
      ep.knownPaths=file(ep,"known-paths.jsonl");
      if(!ep.drive || ep.teacherVersion==3 || ep.dagger || ep.hasObservedTransition) {
        ep.teacherPaths=file(ep,"teacher-paths.jsonl");
        ep.transitionFrames=file(ep,"transition-frames.jsonl");
      }
    }
  }
  if(ep.target)ep.targetTrajectory=file(ep,"target-trajectory.txt");
  if(NavMemoryCostTrace::enabled(ep.output))
    ep.memoryCostTrace=std::make_unique<NavMemoryCostTrace>(std::filesystem::path(ep.output)/"memory-cost.bin",ep.memory);
  if(settings.live) {
    if(ep.replay)throw std::runtime_error("nav live: replay is not supported");
    const auto reference=NavBench::navigationMesh(w).findPath(meters(ep.expectedStart),meters(ep.goal));
    std::vector<NavMesh::Point> route;
    for(const auto* point:ep.targetRoute)route.push_back(meters(point->groundPos));
    ep.live=std::make_unique<NavLiveTrace>(ep.output,ep.drive?ep.policy.get():nullptr,reference,ep.eye.rayCount(),ep.liveTargetPlayer,route,bool(ep.liveGuidePlayer),ep.liveTargetWalkOnly);
    ep.npc->setCharacterVirtualDiagnosticsEnabled(true);
  }
  ep.initialState=initialState(ep);
  tracePerceptionSafety(ep,w,0);
  observe(ep,w);decide(ep,w);ep.next=ep.began+100;
}
void NavDemos::tick(World& w,uint64_t dt) {
  if(population.active)tickPopulation(w,dt);
  else tickEpisode(legacy,w,dt);
}
