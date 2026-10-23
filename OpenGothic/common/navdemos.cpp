#include "navdemos.h"
#include "navbench.h"
#include "navkeyteacher.h"
#include "navmemoryteacher.h"
#include "navmemorycosttrace.h"
#include "navdemowriter.h"
#include "navpolicy.h"
#include "navtransitions.h"
#include "game/knownroadsbench.h"
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
#include <random>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {
using Tempest::Vec3;
constexpr float Pi=3.14159265358979323846f;
constexpr uint64_t PolicyWarmupDecisions=30;
struct RecordedKeys { uint64_t time=0; NpcKeys::KeyState keys; };
struct MotionSample { uint64_t time=0; Vec3 position; bool commanded=false; };
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
  bool contacted=false,targetQueued=false;
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
  std::deque<MotionSample> daggerMotion;
  std::mt19937_64 mixingRng,actionRng,dartRng;
  KnownRoads::Guidance guidance;
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
  std::deque<MotionSample> motion;
  uint64_t blockedSpans=0,blockedMs=0,maxBlockedMs=0,blockedSince=0,blockedUntil=0;
  bool blocked=false;
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

struct Pair {std::string from,to;};
struct Population {
  bool active=false,record=true,sampled=false,near=false;
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
struct Settings {
  bool cv=false,heroNear=true,killArmed=false,killFired=false;
  bool oracle=false,recordDemos=true;
  bool perceptionMonitor=false;
  bool dagger=false;
  uint32_t driveTeacher=0;
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

NavMesh::Point meters(const Vec3& p) {return {p.x*.01f,p.y*.01f,p.z*.01f};}
Vec3 cm(const NavMesh::Point& p) {return {p[0]*100,p[1]*100,p[2]*100};}
std::array<float,3> array(const Vec3& p) {return {p.x,p.y,p.z};}
void jsonNumber(std::ostream& out,double value) {if(std::isfinite(value))out<<value;else out<<"null";}
void vector(std::ostream& out,const Vec3& p) {
  out<<'[';jsonNumber(out,p.x);out<<',';jsonNumber(out,p.y);out<<',';jsonNumber(out,p.z);out<<']';
}
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
  const auto position=NpcKeys::physicalPosition(*ep.npc);
  const auto route=NavBench::navigationMesh(world).findPath(meters(position),meters(ep.goal));
  ep.rewardPathValid=route.complete;
  // Training-only state shares the reward query to the final goal. The actor's
  // observation and the teacher's local carrot route remain unchanged.
  ep.critic={ep.lastPathCm,{},ep.blocked,ep.npc->isInAir(),route.complete};
  if(!route.complete) {++ep.invalidRewardPaths;return ep.lastPathCm;}
  ep.critic.pathCm=double(route.length)*100.;
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
    for(size_t i=0;i<ep.policy->actionHeadCount();++i) {if(i)out<<',';out<<ep.selectedIndices[i];}
    out<<"],\"logits\":[";bool first=true;
    for(const auto& head:ep.selected.logits)for(float value:head) {if(!first)out<<',';first=false;out<<value;}
    out<<"],\"log_probs\":[";double joint=0;
    for(size_t i=0;i<ep.policy->actionHeadCount();++i) {if(i)out<<',';out<<ep.selectedLogProb[i];joint+=ep.selectedLogProb[i];}
    out<<"],\"log_prob\":"<<joint<<",\"path_before_cm\":"<<ep.lastPathCm<<",\"path_after_cm\":"<<after
       <<",\"path_valid\":"<<(beforeValid && ep.rewardPathValid)<<",\"critic_before\":";
    criticState(out,beforeCritic);out<<",\"critic_after\":";criticState(out,ep.critic);
    out<<",\"terminated\":"<<terminated<<",\"truncated\":"<<truncated
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
  if(ep.targetTrajectory.is_open() && ep.decisions && elapsed>(ep.decisions-1)*100) {
    const float distance=(NpcKeys::physicalPosition(*ep.npc)-ep.goal).length();
    ep.targetTrajectory<<elapsed<<' '<<ep.goal.x<<' '<<ep.goal.y<<' '<<ep.goal.z<<' '
                       <<distance<<' '<<int(distance>=200.f && distance<=500.f)<<'\n';
  }
  if(!ep.replay) {NpcKeys::KeyState end;end.walk=true;action(ep,elapsed,end);if(ep.writer)ep.writer->finish();}
  ep.trajectory.close();ep.actions.close();ep.knownPaths.close();ep.teacherPaths.close();ep.transitionFrames.close();ep.samples.close();ep.targetTrajectory.close();ep.perceptionSafetyFrames.close();ep.perceptionWaterObservations.close();
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
     <<",\"memory_update_us\":"<<(ep.memoryUpdates?ep.memoryUpdateUs/double(ep.memoryUpdates):0.)
     <<",\"memory_view_us\":"<<(ep.memoryViews?ep.memoryViewUs/double(ep.memoryViews):0.)
     <<",\"memory_updates\":"<<ep.memoryUpdates<<",\"memory_views\":"<<ep.memoryViews
     <<",\"perception_mode\":"<<std::quoted(perceptionMode(ep.eye.config().floorSampling))
     <<",\"perception_monitor\":"<<ep.perceptionMonitor
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
       <<",\"blocked_spans\":"<<ep.blockedSpans<<",\"blocked_ms\":"<<ep.blockedMs
       <<",\"max_blocked_ms\":"<<ep.maxBlockedMs<<",\"first_contact_ms\":"<<(ep.contacted?int64_t(ep.firstContactMs):-1)
       <<",\"follow_ms\":"<<ep.followMs<<",\"in_band_ms\":"<<ep.inBandMs
       <<",\"follow_fraction\":"<<(ep.followMs?double(ep.inBandMs)/double(ep.followMs):0);
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
  if(ep.drive) {
    out<<",\"teacher_beta\":"<<ep.teacherBeta<<",\"mixing_seed\":"<<ep.mixingSeed
       <<",\"dagger\":"<<ep.dagger<<",\"dagger_interventions\":"<<ep.daggerInterventions
       <<",\"dart_episode\":"<<ep.dartEpisode<<",\"dart_decisions\":"<<ep.dartDecisions
       <<",\"maneuver_masked_decisions\":"<<ep.maneuverMaskedDecisions
       <<",\"action_mode\":"<<std::quoted(ep.sampled?"sample":"argmax")
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
  NpcKeys::enable(*ep.npc,false);ep.npc->stopWalking();ep.active=false;
  ep.memoryCostTrace.reset();
  if(ep.target) {
    ep.target->clearAiQueue();ep.target->clearGoTo();ep.target->stopWalking();
    world.removeNpc(*ep.target);ep.target=nullptr;
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
// Stand-only point syntax shared by route qualification and v2 recording. Coordinates
// are centimetres, like the saved world and NAVDEMO poses; mesh queries use metres.
Vec3 demoPoint(World& world,const std::string& name) {
  if(name.rfind("cm:",0)!=0) {
    const auto* point=world.findPoint(name,false);
    if(!point)throw std::runtime_error("navdemos missing waypoint "+name);
    return point->groundPos;
  }
  std::string values=name.substr(3);std::replace(values.begin(),values.end(),',',' ');
  std::istringstream in(values);Vec3 point;std::string extra;
  if(!(in>>point.x>>point.y>>point.z) || (in>>extra) ||
     !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
    throw std::runtime_error("navdemos invalid cm:x,y,z point "+name);
  return point;
}
const char* transitionName(NavMesh::LinkType type) {
  switch(type) {
    case NavMesh::LinkType::Jump:return "jump";
    case NavMesh::LinkType::Climb:return "climb";
    case NavMesh::LinkType::Drop:return "drop";
  }
  return "unknown";
}
void writeV2Plan(std::ostream& out,const Vec3& position,const Vec3& goal,
                 const KnownRoads::Guidance& guidance,const NavMesh::Path& path,bool direct) {
  out<<",\"position_cm\":";vector(out,position);out<<",\"goal_cm\":";vector(out,goal);
  out<<",\"carrot_cm\":";vector(out,guidance.carrot);
  out<<",\"beyond_known\":"<<guidance.beyondKnown<<",\"remaining_known_cm\":"<<guidance.remainingKnownCm
     <<",\"direct_walkable\":"<<direct<<",\"complete\":"<<(direct || path.complete)
     <<",\"reason\":"<<std::quoted(direct?"direct_walkable":path.reason)
     <<",\"length_m\":"<<(direct?(guidance.carrot-position).length()*.01f:path.length)
     <<",\"start_snap_m\":"<<path.startSnapDistance<<",\"end_snap_m\":"<<path.endSnapDistance
     <<",\"corners_cm\":[";
  if(direct) {vector(out,position);out<<',';vector(out,guidance.carrot);}
  else for(size_t i=0;i<path.corners.size();++i) {if(i)out<<',';vector(out,cm(path.corners[i]));}
  out<<"],\"transitions\":[";
  if(!direct)for(size_t i=0;i<path.transitions.size();++i) {
    if(i)out<<',';
    const auto& link=path.transitions[i];
    out<<"{\"id\":"<<link.id<<",\"type\":"<<std::quoted(transitionName(link.type))
       <<",\"corner_index\":"<<link.cornerIndex<<",\"start_cm\":";
    vector(out,cm(link.start));out<<",\"end_cm\":";vector(out,cm(link.end));out<<'}';
  }
  out<<']';
}
void planV2(World& world,Npc& npc,std::ostream& out,const std::string& label,
            const std::string& from,const std::string& to) {
  const auto position=demoPoint(world,from),goal=demoPoint(world,to);
  KnownRoads::Guidance guidance;
  KnownRoadsBench::readGuidance(world,npc,position,goal,guidance);
  const auto& mesh=NavBench::navigationMesh(world);
  const auto path=mesh.findPath(meters(position),meters(guidance.carrot));
  const bool direct=!guidance.beyondKnown && mesh.straightWalkable(meters(position),meters(guidance.carrot));
  out<<"{\"label\":"<<std::quoted(label)<<",\"start\":"<<std::quoted(from)<<",\"goal\":"<<std::quoted(to);
  writeV2Plan(out,position,goal,guidance,path,direct);
  // A generated coordinate start must have actual support at its own elevation;
  // Detour snapping to a nearby rooftop or a floor beneath it is insufficient.
  const auto floor=world.physic()->landRay(position+Vec3(0,20,0),40.f);
  out<<",\"start_supported\":"<<(floor.hasCol && floor.n.y>=.65f && std::abs(floor.v.y-position.y)<=20.f)<<"}\n";
  if(!out)throw std::runtime_error("navdemos plan-v2 write failed");
}
void detourPlanV3(World& world,std::ostream& out,const std::string& label,
                  const std::string& from,const std::string& to) {
  // Evaluation only: this path goes to the actual final goal and never enters
  // NavMemoryTeacher or its learner-input boundary.
  const auto start=demoPoint(world,from),goal=demoPoint(world,to);
  const auto& mesh=NavBench::navigationMesh(world);
  const auto path=mesh.findPath(meters(start),meters(goal));
  out<<"{\"label\":"<<std::quoted(label)<<",\"start\":"<<std::quoted(from)<<",\"goal\":"<<std::quoted(to)
     <<",\"start_cm\":";vector(out,start);out<<",\"goal_cm\":";vector(out,goal);
  out<<",\"complete\":"<<path.complete<<",\"reason\":"<<std::quoted(path.reason)
     <<",\"length_cm\":";jsonNumber(out,path.length*100.f);
  out<<",\"start_snap_cm\":";jsonNumber(out,path.startSnapDistance*100.f);
  out<<",\"end_snap_cm\":";jsonNumber(out,path.endSnapDistance*100.f);
  out<<",\"straight_walkable\":"<<mesh.straightWalkable(meters(start),meters(goal))
     <<",\"corners_cm\":[";
  for(size_t i=0;i<path.corners.size();++i) {
    if(i!=0) {
      out<<',';
    }
    vector(out,cm(path.corners[i]));
  }
  out<<"],\"transitions\":[";
  for(size_t i=0;i<path.transitions.size();++i) {
    if(i!=0) {
      out<<',';
    }
    const auto& link=path.transitions[i];
    out<<"{\"id\":"<<link.id<<",\"type\":"<<std::quoted(transitionName(link.type))<<",\"start_cm\":";
    vector(out,cm(link.start));out<<",\"end_cm\":";vector(out,cm(link.end));out<<'}';
  }
  out<<"]}\n";
  if(!out) {
    throw std::runtime_error("navdemos detour-plans-v3 write failed");
  }
}
float pointSegmentXZSquared(const Vec3& point,const Vec3& start,const Vec3& end) {
  const float dx=end.x-start.x,dz=end.z-start.z;
  const float square=dx*dx+dz*dz;
  const float t=square>0?std::clamp(((point.x-start.x)*dx+(point.z-start.z)*dz)/square,0.f,1.f):0.f;
  const float x=point.x-start.x-t*dx,z=point.z-start.z-t*dz;
  return x*x+z*z;
}
float segmentSquareXZSquared(const Vec3& start,const Vec3& end,float x0,float z0,float x1,float z1) {
  const std::array<float,2> a={start.x,start.z},delta={end.x-start.x,end.z-start.z};
  const std::array<float,2> low={x0,z0},high={x1,z1};
  float enter=0,leave=1;
  bool intersects=true;
  for(size_t axis=0;axis<2;++axis) {
    if(std::abs(delta[axis])<.000001f) {
      if(a[axis]<low[axis] || a[axis]>high[axis]) {
        intersects=false;break;
      }
      continue;
    }
    float first=(low[axis]-a[axis])/delta[axis],last=(high[axis]-a[axis])/delta[axis];
    if(first>last) {
      std::swap(first,last);
    }
    enter=std::max(enter,first);leave=std::min(leave,last);
    if(enter>leave) {
      intersects=false;break;
    }
  }
  if(intersects) {
    return 0;
  }
  const auto pointSquare=[&](const Vec3& point) {
    const float dx=std::max({0.f,x0-point.x,point.x-x1});
    const float dz=std::max({0.f,z0-point.z,point.z-z1});
    return dx*dx+dz*dz;
  };
  float square=std::min(pointSquare(start),pointSquare(end));
  for(float x:{x0,x1}) {
    for(float z:{z0,z1}) {
      square=std::min(square,pointSegmentXZSquared(Vec3(x,0,z),start,end));
    }
  }
  return square;
}
void oracleCorridor(World& world,std::ostream& out,const std::string& label,
                    const std::string& from,float yaw,const std::string& to) {
  constexpr float Begin=500,End=1500,Radius=15;
  const auto start=demoPoint(world,from),goal=demoPoint(world,to);
  const auto& mesh=NavBench::navigationMesh(world);
  const auto path=mesh.findPath(meters(start),meters(goal));
  const float pathCm=path.length*100.f;
  const char* status=!path.complete?"incomplete_path":!path.transitions.empty()?"offmesh_transition":
                     pathCm<=Begin?"short_path":"ok";
  NavMesh::LocalRaster raster;
  const bool evaluated=std::string_view(status)=="ok";
  if(evaluated) {
    raster=mesh.rasterLocal(meters(start),yaw);
    if(!raster.supportValid) {
      status="unsupported_pose";
    }
  }
  const bool available=std::string_view(status)=="ok";
  std::vector<std::pair<Vec3,Vec3>> corridor;
  if(available) {
    float offset=0;
    const float radians=yaw*Pi/180.f;
    for(size_t i=1;i<path.corners.size();++i) {
      const auto a=cm(path.corners[i-1]),b=cm(path.corners[i]);
      const float extent=(b-a).length();
      if(extent>0 && offset+extent>Begin && offset<End) {
        const float first=std::clamp((Begin-offset)/extent,0.f,1.f);
        const float last=std::clamp((End-offset)/extent,0.f,1.f);
        const auto near=local(a+(b-a)*first-start,radians),far=local(a+(b-a)*last-start,radians);
        corridor.emplace_back(Vec3(near[0],0,near[2]),Vec3(far[0],0,far[2]));
      }
      offset+=extent;
      if(offset>=End) {
        break;
      }
    }
  }
  out<<"{\"label\":"<<std::quoted(label)<<",\"start\":"<<std::quoted(from)<<",\"goal\":"<<std::quoted(to)
     <<",\"start_cm\":";vector(out,start);out<<",\"goal_cm\":";vector(out,goal);
  out<<",\"yaw_deg\":"<<yaw<<",\"complete\":"<<path.complete<<",\"reason\":"<<std::quoted(path.reason)
     <<",\"length_cm\":";jsonNumber(out,pathCm);
  out<<",\"transition_count\":"<<path.transitions.size()<<",\"corridor_begin_cm\":"<<Begin
     <<",\"corridor_end_cm\":"<<End<<",\"radius_cm\":"<<Radius
     <<",\"corridor_arclength\":\"detour_3d\",\"corridor_available\":"<<available
     <<",\"corridor_status\":"<<std::quoted(status)<<",\"raster_evaluated\":"<<evaluated<<",\"raster_support_valid\":";
  if(evaluated) {
    out<<raster.supportValid;
  }
  else {
    out<<"null";
  }
  out<<",\"raster_support_exact\":";
  if(evaluated) {
    out<<raster.supportExact;
  }
  else {
    out<<"null";
  }
  out<<",\"raster_support_snap_cm\":";
  if(evaluated && raster.supportValid) {
    jsonNumber(out,raster.supportSnapDistance*100.f);
  }
  else {
    out<<"null";
  }
  out<<",\"raster_support_horizontal_cm\":";
  if(evaluated && raster.supportValid) {
    jsonNumber(out,raster.supportHorizontalDistance*100.f);
  }
  else {
    out<<"null";
  }
  out<<",\"raster_support_vertical_offset_cm\":";
  if(evaluated && raster.supportValid) {
    jsonNumber(out,raster.supportVerticalOffset*100.f);
  }
  else {
    out<<"null";
  }
  out<<",\"corners_cm\":[";
  for(size_t i=0;i<path.corners.size();++i) {
    if(i!=0) {
      out<<',';
    }
    vector(out,cm(path.corners[i]));
  }
  out<<"],\"native_cells\":[";
  size_t count=0;
  if(available) {
    const auto layer=[&](const NavMesh::RasterLayer& value,bool near) {
      if(value.size==0 || value.cells.size()!=value.size*value.size) {
        throw std::runtime_error("navdemos oracle corridor raster shape mismatch");
      }
      const float step=value.cellSize*100.f,half=float(value.size)*.5f;
      for(size_t row=0;row<value.size;++row) {
        for(size_t column=0;column<value.size;++column) {
          const float x0=(float(column)-half)*step,z0=(float(row)-half)*step;
          if(!near && std::abs(x0+step*.5f)<800 && std::abs(z0+step*.5f)<800) {
            continue;
          }
          const bool touched=std::any_of(corridor.begin(),corridor.end(),[&](const auto& segment) {
            return segmentSquareXZSquared(segment.first,segment.second,x0,z0,x0+step,z0+step)<=Radius*Radius;
          });
          if(!touched) {
            continue;
          }
          if(count!=0) {
            out<<',';
          }
          const auto& cell=value.cells[row*value.size+column];
          if(cell.heightBoundary && cell.traversable) {
            throw std::runtime_error("navdemos oracle corridor has a traversable height boundary");
          }
          if(cell.traversable && !cell.heightValid) {
            throw std::runtime_error("navdemos oracle corridor has traversable ground without height");
          }
          out<<"{\"layer\":"<<std::quoted(near?"near":"far")<<",\"row\":"<<row<<",\"column\":"<<column
             <<",\"truth_passable\":"<<cell.traversable<<",\"height_valid\":"<<cell.heightValid
             <<",\"ground_height_cm\":";jsonNumber(out,cell.heightValid?cell.groundHeight*100.f:0.f);
          out<<",\"height_boundary\":"<<cell.heightBoundary<<'}';
          ++count;
        }
      }
    };
    layer(raster.near,true);layer(raster.far,false);
  }
  out<<"],\"native_cell_count\":"<<count<<"}\n";
  if(!out) {
    throw std::runtime_error("navdemos oracle-corridors write failed");
  }
}
void probeObject(World& world,Npc& npc,std::ostream& out,const std::string& label,
                 const std::string& from,const std::string& to,
                 const std::string& openingIn={},const std::string& openingOut={}) {
  const auto start=demoPoint(world,from),goal=demoPoint(world,to);
  Vec3 openingNormal=start-goal;
  if(!openingIn.empty() || !openingOut.empty()) {
    const auto* inside=world.findPoint(openingIn,false);
    const auto* outside=world.findPoint(openingOut,false);
    if(!inside || !outside) {
      throw std::runtime_error("navdemos object probe missing exact opening IN/OUT alias");
    }
    openingNormal=outside->groundPos-inside->groundPos;
  }
  openingNormal.y=0;
  const float normalLength=openingNormal.length();
  if(normalLength<.001f) {
    throw std::runtime_error("navdemos object probe opening normal has no horizontal direction");
  }
  openingNormal*=1.f/normalLength;
  const Vec3 tangent(-openingNormal.z,0,openingNormal.x);
  const auto old=npc.position();const float yaw=npc.rotation();
  npc.setPosition(start);npc.updateTransform();npc.setDirection(goal-start);
  const auto eyes=npc.navigationEyePose().origin;
  // Keep the original centre ray exactly. Extra samples stay within a
  // 130-cm-wide vertical opening at the unchanged authored floor goal.
  const auto look=goal+Vec3(0,100,0);
  const auto hit=world.physic()->navigationRay(eyes,look,&npc);
  const bool visibleCentre=!hit.hasCol || (hit.point-look).length()<5.f;
  bool visibleEdge=false;
  size_t visibleSamples=visibleCentre?1u:0u;
  std::ostringstream samples;
  samples<<std::setprecision(9)<<std::boolalpha;
  const auto sample=[&](const Vec3& point,bool hasCollision,const Vec3& hitPoint,bool visible) {
    samples<<"{\"look_cm\":";vector(samples,point);
    samples<<",\"visible\":"<<visible<<",\"has_collision\":"<<hasCollision;
    if(hasCollision) {
      samples<<",\"hit_cm\":";vector(samples,hitPoint);
    }
    samples<<'}';
  };
  sample(look,hit.hasCol,hit.point,visibleCentre);
  for(float height:{40.f,100.f,180.f}) {
    for(float width:{-65.f,0.f,65.f}) {
      if(width==0 && height==100.f) {
        continue;
      }
      const auto edgeLook=goal+tangent*width+Vec3(0,height,0);
      const auto edgeHit=world.physic()->navigationRay(eyes,edgeLook,&npc);
      const bool visible=!edgeHit.hasCol || (edgeHit.point-edgeLook).length()<5.f;
      visibleEdge=visibleEdge || visible;
      visibleSamples+=visible?1u:0u;
      samples<<',';
      sample(edgeLook,edgeHit.hasCol,edgeHit.point,visible);
    }
  }
  const auto& mesh=NavBench::navigationMesh(world);
  const auto path=mesh.findPath(meters(start),meters(goal));
  const bool blocked=!mesh.straightWalkable(meters(start),meters(goal));
  const auto floor=world.physic()->landRay(start+Vec3(0,20,0),40.f);
  out<<"{\"label\":"<<std::quoted(label)<<",\"start\":"<<std::quoted(from)<<",\"goal\":"<<std::quoted(to)
     <<",\"visible_centre\":"<<visibleCentre<<",\"visible_edge\":"<<visibleEdge
     <<",\"visible\":"<<(visibleCentre || visibleEdge)
     <<",\"visibility_criterion\":\"centre_or_opening_edge_v1\",\"visible_samples\":"<<visibleSamples
     <<",\"opening_in\":"<<std::quoted(openingIn)<<",\"opening_out\":"<<std::quoted(openingOut)
     <<",\"opening_normal\":";vector(out,openingNormal);
  out<<",\"look_samples\":["<<samples.str()<<"],\"detour_complete\":"<<path.complete
     <<",\"detour_length_cm\":"<<path.length*100.f<<",\"direct_cm\":"<<(goal-start).length()
     <<",\"straight_blocked\":"<<blocked<<",\"start_supported\":"
     <<(floor.hasCol && floor.n.y>=.65f && std::abs(floor.v.y-start.y)<=20.f)
     <<",\"eyes_cm\":";vector(out,eyes);out<<",\"look_cm\":";vector(out,look);
  out<<",\"detour_cm\":[";
  for(size_t i=0;i<path.corners.size();++i) {if(i)out<<',';vector(out,cm(path.corners[i]));}
  out<<"]}\n";
  npc.setPosition(old);npc.updateTransform();npc.setDirection(yaw);
  if(!out)throw std::runtime_error("navdemos object probe write failed");
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
    return ep.policy->encode(sample);
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
  KnownRoadsBench::readGuidance(w,*ep.npc,NpcKeys::physicalPosition(*ep.npc),ep.goal,guidance);
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
  NpcKeys::KeyState keys;
  keys.forward=float(int(ep.selectedIndices[0])-1);keys.strafe=float(int(ep.selectedIndices[1])-1);
  keys.turn=float(int(ep.selectedIndices[2])-1);keys.walk=ep.selectedIndices[3]!=0;
  keys.jump=ep.policy->weightVersion()>=2?ep.selectedIndices[4]==1:ep.selectedIndices[4]!=0;
  keys.climb=ep.policy->weightVersion()>=2?ep.selectedIndices[4]==2:ep.selectedIndices[5]!=0;
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
    if(std::lround(cell.water*255.f)==0 && std::lround(cell.drop*255.f)==0 &&
       std::lround(cell.ledge*255.f)==0)continue;
    const float right=(float(column)+.5f-float(near.size)*.5f)*near.cellCm;
    const float forward=(float(row)+.5f-float(near.size)*.5f)*near.cellCm;
    const float along=right*movement[0]+forward*movement[1];
    const float across=std::abs(right*movement[1]-forward*movement[0]);
    // A 15-cm body margin plus half a cell diagonal covers the commanded lane.
    if(along>0 && along<=150.f && across<=15.f+near.cellCm*.707107f)return true;
  }
  return false;
}
const char* daggerTrigger(Episode& ep,const NavDemoWriter::Snapshot& sample,const NpcKeys::KeyState& learner) {
  const auto movement=movementDirection(learner);
  const bool moving=movement[0]!=0 || movement[1]!=0;
  const auto now=sample.gameMs;
  ep.daggerMotion.push_back({now,Vec3(sample.position[0],sample.position[1],sample.position[2]),
                            ep.learnerCommanded});
  ep.learnerCommanded=moving;
  const auto cutoff=now>=2000?now-2000:0;
  while(ep.daggerMotion.size()>1 && ep.daggerMotion[1].time<=cutoff)ep.daggerMotion.pop_front();
  uint64_t commanded=0;
  for(size_t i=1;i<ep.daggerMotion.size();++i)
    if(ep.daggerMotion[i].commanded)commanded+=ep.daggerMotion[i].time-ep.daggerMotion[i-1].time;
  const auto displacement=ep.daggerMotion.back().position-ep.daggerMotion.front().position;
  const bool stalled=moving && now-ep.daggerMotion.front().time>=2000 && commanded>=2000 &&
                     std::hypot(displacement.x,displacement.z)<30.f;
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
  const bool disagree=moving && movement[0]*teacher[0]+movement[1]*teacher[1]<0;
  if(disagree && !ep.headingDisagrees)ep.headingDisagreedAt=now;
  ep.headingDisagrees=disagree;
  if(stalled)return "stall";
  if(memoryDangerAhead(sample,movement))return "memory_danger";
  if(disagree && now-ep.headingDisagreedAt>1000)return "heading";
  return "";
}
NpcKeys::KeyState mixTeacher(Episode& ep,const NavDemoWriter::Snapshot& sample,NpcKeys::KeyState learner) {
  const auto now=sample.gameMs;
  const auto* trigger=ep.dagger?daggerTrigger(ep,sample,learner):"";
  if(ep.dagger && now>=ep.teacherUntil && trigger[0]!='\0') {
    ep.teacherUntil=now+uint64_t(std::uniform_int_distribution<unsigned>(10,20)(ep.mixingRng))*100;
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
              <<",\"dart\":"<<dart<<",\"takeover\":"<<std::quoted(ep.takeoverReason)<<"}\n";
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
    if(i==0)executed=(ep.population || ep.sampled)?select(ep,std::move(result)):result.keys;
  }
  if(measure)ep.populationUs.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-populationStart).count());
  executed.headYaw=0;executed.headPitch=0;
  const std::array<bool,6> disagree={executed.forward!=sample.keys.forward,executed.strafe!=sample.keys.strafe,
    executed.turn!=sample.keys.turn,executed.walk!=sample.keys.walk,executed.jump!=sample.keys.jump,executed.climb!=sample.keys.climb};
  for(size_t i=0;i<disagree.size();++i)ep.disagreements[i]+=disagree[i];
  // The recurrent student observes every visited state. The same state's
  // teacher label is always recorded, regardless of who presses the keys.
  return mixTeacher(ep,sample,executed);
}
void guidanceFor(Episode& ep,World& w,const Vec3& position) {
  if(ep.goalKind=="point")KnownRoadsBench::readGuidance(w,*ep.npc,position,ep.goal,ep.guidance);
  else {
    ep.guidance={};ep.guidance.carrot=ep.goal;ep.guidance.beyondKnown=true;
    if(ep.goalKind=="follow") {
      // Intent is visible in the existing learner fields: the physical target
      // remains the final goal, while steering ends 2.5 m before it. The frozen
      // memory planner receives this carrot just like any local road endpoint.
      const auto delta=ep.goal-position;
      const float distance=delta.length();
      ep.guidance.carrot=position+delta*(distance>250.f?(distance-250.f)/distance:0.f);
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
  out<<",\"beyond_known\":"<<ep.guidance.beyondKnown<<",\"local_plan_cm\":[";
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
  if(ep.writer) {
    const double cost=ep.writer->append(sample);ep.writeUs+=cost;ep.maxWriteUs=std::max(ep.maxWriteUs,cost);
  }
  ep.backwardDecisions+=executed.forward<0;ep.strafeDecisions+=executed.strafe!=0;
  action(ep,elapsed,executed);NpcKeys::set(*ep.npc,executed,1000);ep.previous=executed;++ep.decisions;
  ep.failure=ep.blocked?"body_blocked":"";
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
  const auto recordStart=Clock::now();
  if(ep.writer) {const double cost=ep.writer->append(sample);ep.writeUs+=cost;ep.maxWriteUs=std::max(ep.maxWriteUs,cost);}
  action(ep,elapsed,executed);NpcKeys::set(n,executed,1000);ep.previous=executed;++ep.decisions;
  ep.backwardDecisions+=executed.forward<0;ep.strafeDecisions+=executed.strafe!=0;
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
  for(const auto& slot:pop.slots)pop.initialDecisions+=slot->decisions;
  pop.startupSeconds=std::chrono::duration<double>(Clock::now()-pop.wallStart).count();
  for(auto& slot:pop.slots)slot->phaseUs={};
}
void tracePerceptionSafety(Episode& ep,World& w,uint64_t elapsed) {
  if(!ep.perceptionMonitor)return;
  auto& n=*ep.npc;
  const auto p=NpcKeys::physicalPosition(n);
  const auto water=w.physic()->waterRay(p+Vec3(0,2,0),50.f);
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
  tracePerceptionSafety(ep,w,elapsed);
  if(ep.target) {
    auto& target=*ep.target;
    if(target.isDown()) {ep.failure="target_down";finish(ep,w);return;}
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
    ep.goal=NpcKeys::physicalPosition(target);
    const float distance=(p-ep.goal).length();
    if(!ep.contacted && distance<=500.f) {ep.contacted=true;ep.firstContactMs=elapsed;}
    else if(ep.contacted) {ep.followMs+=dt;if(distance>=200.f && distance<=500.f)ep.inBandMs+=dt;}
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
  return settings.cv && ((legacy.active && (legacy.npc==&npc || legacy.target==&npc)) || preparingLegacy==&npc ||
                          (population.active && population.members.contains(&npc)) ||
                          NavTransitions::controls(npc));
}
bool NavDemos::fullSimulation(const Npc& npc) {
  return (legacy.active && legacy.target==&npc) ||
         (population.active && population.members.contains(&npc)) || forceCharacterVirtual(npc);
}
void NavDemos::reset() {legacy=Episode{};population=Population{};settings=Settings{};preparingLegacy=nullptr;}
void NavDemos::command(const std::vector<std::string>& args,World& w,Npc& player,const std::string& output) {
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
  if(a.size()>=2 && a[1]=="oracle-corridors") {
    if(running() || NavBench::running()) {
      throw std::runtime_error("navdemos cannot query oracle corridors during episode");
    }
    if(a.size()!=3) {
      throw std::runtime_error("navdemos oracle-corridors FILE (rows LABEL START YAW_DEG GOAL)");
    }
    std::ifstream input(a[2]);
    if(!input) {
      throw std::runtime_error("navdemos cannot read oracle-corridors file");
    }
    std::ofstream out(std::filesystem::path(output)/"oracle-corridors.jsonl");
    if(!out) {
      throw std::runtime_error("navdemos cannot write oracle-corridors.jsonl");
    }
    out<<std::setprecision(9)<<std::boolalpha;
    std::string line;
    while(std::getline(input,line)) {
      std::istringstream row(line);
      std::string label,from,to,extra;
      float yaw=0;
      if(!(row>>label) || label[0]=='#') {
        continue;
      }
      if(!(row>>from>>yaw>>to) || (row>>extra) || !std::isfinite(yaw)) {
        throw std::runtime_error("navdemos oracle-corridors rows LABEL START YAW_DEG GOAL");
      }
      oracleCorridor(w,out,label,from,yaw,to);
    }
    if(!input.eof()) {
      throw std::runtime_error("navdemos oracle-corridors read failed");
    }
    out.close();
    if(!out) {
      throw std::runtime_error("navdemos oracle-corridors write failed");
    }
    return;
  }
  if(a.size()>=2 && a[1]=="detour-plans-v3") {
    if(running() || NavBench::running()) {
      throw std::runtime_error("navdemos cannot query Detour plans during episode");
    }
    if(a.size()!=3) {
      throw std::runtime_error("navdemos detour-plans-v3 FILE (rows LABEL START GOAL)");
    }
    std::ifstream input(a[2]);
    if(!input) {
      throw std::runtime_error("navdemos cannot read detour-plans-v3 file");
    }
    std::ofstream out(std::filesystem::path(output)/"detour-plans-v3.jsonl");
    if(!out) {
      throw std::runtime_error("navdemos cannot write detour-plans-v3.jsonl");
    }
    out<<std::setprecision(9)<<std::boolalpha;
    std::string line;
    while(std::getline(input,line)) {
      std::istringstream row(line);
      std::string label,from,to,extra;
      if(!(row>>label) || label[0]=='#') {
        continue;
      }
      if(!(row>>from>>to) || (row>>extra)) {
        throw std::runtime_error("navdemos detour-plans-v3 rows LABEL START GOAL");
      }
      detourPlanV3(w,out,label,from,to);
    }
    if(!input.eof()) {
      throw std::runtime_error("navdemos detour-plans-v3 read failed");
    }
    out.close();
    if(!out) {
      throw std::runtime_error("navdemos detour-plans-v3 write failed");
    }
    return;
  }
  if(a.size()>=2 && a[1]=="points-v3") {
    if(running()) {
      throw std::runtime_error("navdemos cannot export points during episode");
    }
    if(a.size()!=2) {
      throw std::runtime_error("navdemos points-v3");
    }
    std::ofstream out(std::filesystem::path(output)/"points-v3.jsonl");
    if(!out) {
      throw std::runtime_error("navdemos cannot write points-v3.jsonl");
    }
    out<<std::setprecision(9)<<std::boolalpha;
    w.forEachWayPoint([&](const WayPoint& point) {
      out<<"{\"name\":"<<std::quoted(point.name)<<",\"position_cm\":";vector(out,point.groundPos);
      out<<",\"connected\":"<<point.isConnected()<<",\"freepoint\":"<<point.isFreePoint()<<"}\n";
    });
    if(!out) {
      throw std::runtime_error("navdemos points-v3 write failed");
    }
    return;
  }
  if(a.size()>=2 && (a[1]=="probe-v3" || a[1]=="probes-v3")) {
    if(running()) {
      throw std::runtime_error("navdemos cannot probe during episode");
    }
    const bool batch=a[1]=="probes-v3";
    if((batch && a.size()!=4) || (!batch && a.size()!=5 && a.size()!=6 && a.size()!=8)) {
      throw std::runtime_error("navdemos probe-v3 NPC START GOAL [LABEL [IN_ALIAS OUT_ALIAS]] | probes-v3 NPC FILE");
    }
    auto* npc=w.findNpcByInstance(w.script().findSymbolIndex(a[2]));
    if(!npc) {
      throw std::runtime_error("navdemos probe-v3 missing NPC");
    }
    std::ofstream out(std::filesystem::path(output)/"object-candidates.jsonl",std::ios::app);
    if(!out) {
      throw std::runtime_error("navdemos cannot write object-candidates.jsonl");
    }
    out<<std::setprecision(9)<<std::boolalpha;
    if(!batch) {
      probeObject(w,*npc,out,a.size()>=6?a[5]:"0",a[3],a[4],a.size()==8?a[6]:"",a.size()==8?a[7]:"");
    }
    else {
      std::ifstream in(a[3]);
      if(!in) {
        throw std::runtime_error("navdemos cannot read probes-v3 file");
      }
      std::string line;
      while(std::getline(in,line)) {
        if(line.empty() || line[0]=='#') {
          continue;
        }
        std::istringstream row(line);std::string label,from,to,openingIn,openingOut,extra;
        if(!(row>>label>>from>>to)) {
          throw std::runtime_error("navdemos probes-v3 rows LABEL START GOAL [IN_ALIAS OUT_ALIAS]");
        }
        if(row>>openingIn) {
          if(!(row>>openingOut) || (row>>extra)) {
            throw std::runtime_error("navdemos probes-v3 rows LABEL START GOAL [IN_ALIAS OUT_ALIAS]");
          }
        }
        probeObject(w,*npc,out,label,from,to,openingIn,openingOut);
      }
    }
    return;
  }
  if(a.size()>=2 && (a[1]=="plan-v2" || a[1]=="plans-v2")) {
    if(running())throw std::runtime_error("navdemos cannot query a new route during an episode");
    const bool batch=a[1]=="plans-v2";
    if((batch && a.size()!=4) || (!batch && a.size()!=5 && a.size()!=6))
      throw std::runtime_error("navdemos plan-v2 NPC FROM TO [LABEL] | plans-v2 NPC PAIRS_FILE (LABEL FROM TO)");
    auto* npc=w.findNpcByInstance(w.script().findSymbolIndex(a[2]));
    if(!npc)throw std::runtime_error("navdemos plan-v2 missing NPC");
    std::ofstream out(std::filesystem::path(output)/"plans-v2.jsonl",std::ios::app);
    if(!out)throw std::runtime_error("navdemos cannot write plans-v2.jsonl");
    out<<std::setprecision(9)<<std::boolalpha;
    if(!batch)planV2(w,*npc,out,a.size()==6?a[5]:"0",a[3],a[4]);
    else {
      std::ifstream input(a[3]);
      if(!input)throw std::runtime_error("navdemos cannot read plans-v2 pairs");
      std::string line;
      while(std::getline(input,line)) {
        if(line.empty() || line[0]=='#')continue;
        std::istringstream row(line);std::string label,from,to,extra;
        if(!(row>>label>>from>>to) || (row>>extra))
          throw std::runtime_error("navdemos plans-v2 pairs require LABEL FROM TO per line");
        planV2(w,*npc,out,label,from,to);
      }
    }
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
  if(a.size()>=2 && a[1]=="population") {
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
  const auto* from=current?nullptr:w.findPoint(a[3],false);const auto* to=w.findPoint(a[current?3:4],false);
  auto* npc=w.findNpcByInstance(w.script().findSymbolIndex(a[2]));
  if(!npc || (!carrot && ((!from && !current) || !to)))throw std::runtime_error("navdemos missing NPC/waypoint");
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
  ep.goalKind=goalKind;ep.scenarioSeed=scenarioSeed;
  ep.goal=goalPosition;ep.replay=replay;ep.drive=learner;
  // Optional start arguments begin at 6.
  ep.limit=replay?0:std::stoull(a[current?4:5]);
  if(learner) {
    ep.sampled=true;
    const auto population=speed?std::stoul(a[7]):1;
    if(population==0 || population>1000)throw std::runtime_error("navdemos invalid policy population");
    ep.policy=std::make_unique<NavPolicy>(a[current?5:6]);
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
    ep.target=w.addNpc(w.script().findSymbolIndex(targetSymbol),ep.targetRoute.front()->groundPos);
    if(!ep.target)throw std::runtime_error("navdemos cannot spawn moving target "+targetSymbol);
    auto& target=*ep.target;
    target.setProcessPolicy(NpcProcessPolicy::AiNormal);target.setInteraction(nullptr,true);
    target.clearAiQueue();target.clearState(false);target.clearSpeed();target.setExternalControl(true);
    if(goalKind=="approach") {
      // This is a navigation target, not a combat encounter. The cloned stand
      // NPC still follows the ordinary seeded wandering route below.
      auto& handle=target.handle();
      handle.flags=zenkit::NpcFlag(uint32_t(handle.flags)|uint32_t(zenkit::NpcFlag::IMMORTAL));
      target.setAttitude(ATT_FRIENDLY);target.setTempAttitude(ATT_FRIENDLY);
    }
    target.clearGoTo();target.attachToPoint(nullptr);target.setDirection(ep.targetRoute[1]->groundPos-target.position());
    target.setWalkMode(WalkBit::WM_Walk);target.aiPush(AiQueue::aiGoToPoint(*ep.targetRoute[1]));ep.targetQueued=true;
    ep.goal=NpcKeys::physicalPosition(target);
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
  if(!current && settings.heroNear) {player.setPosition(npc->position()+Vec3(0,0,500));player.updateTransform();}
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
  ep.initialState=initialState(ep);
  tracePerceptionSafety(ep,w,0);
  observe(ep,w);decide(ep,w);ep.next=ep.began+100;
}
void NavDemos::tick(World& w,uint64_t dt) {
  if(population.active)tickPopulation(w,dt);
  else tickEpisode(legacy,w,dt);
}
