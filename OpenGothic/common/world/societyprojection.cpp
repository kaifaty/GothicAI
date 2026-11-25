#include "societyprojection.h"

#include "asset/activitydefinitions.h"
#include "game/serialize.h"
#include "resources.h"
#include "world.h"
#include "world/objects/npc.h"
#include "worldchunkmanifest.h"

#include <Tempest/Log>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace {

constexpr Society::Time SampleInterval = 5*60;
constexpr uint32_t SaveMagic = 0x31434F53; // SOC1
constexpr uint16_t SaveVersion = 1;
constexpr uint32_t MaxSnapshotSize = 64u*1024u*1024u;
constexpr std::string_view RouteActor = "PAL_238_RITTER#0";
constexpr std::string_view RouteFrom = "NW_CITY_PALCAMP_03";
constexpr std::string_view RouteTo = "NW_CITY_PALCAMP_10";
constexpr std::string_view GuardActor = "MIL_322_MILIZ#0";
constexpr std::string_view GuardWaypoint = "NW_CITY_MERCHANT_TEMPLE_PLACE_01";
constexpr std::string_view GuardPost = "FP_STAND_CITY_17";
constexpr unsigned MaxGroup = 32;
constexpr std::string_view StandState = "ZS_STAND_WP";
constexpr std::string_view GuardState = "ZS_STAND_GUARDING";

uint64_t elapsedNs(SocietyProjection::ProfileClock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(SocietyProjection::ProfileClock::now()-start).count());
  }

bool sameWorldName(std::string_view a, std::string_view b) {
  return std::ranges::equal(a,b,[](unsigned char x, unsigned char y) { return std::toupper(x)==std::toupper(y); });
  }

std::string symbolName(World& world, size_t id) {
  if(id==size_t(-1))
    return "-";
  const auto name = world.rules().symbolName(id);
  return name.empty() ? std::to_string(id) : std::string(name);
  }

int64_t minuteOfDay(gtime time) {
  return time.hour()*60+time.minute();
  }

}

SocietyProjection::SocietyProjection(Society::World source, bool decisionsEnabled)
  :data(std::move(source)),sim(Society::makeSim(data,{.decisions=decisionsEnabled})),
   nextSample(data.startTime()+SampleInterval) {
  }

std::unique_ptr<SocietyProjection> SocietyProjection::open(World& world, double clockRate, bool fresh) {
  const auto* config = std::getenv("OPENGOTHIC_SOCIETY_CONFIG");
  const auto* weights = std::getenv("OPENGOTHIC_SOCIETY_WEIGHTS");
  const bool decisionsEnabled = config!=nullptr && *config!='\0';
  auto data = Society::loadWorld(std::getenv("OPENGOTHIC_SOCIETY_WORLD"),decisionsEnabled ? config : "");
  if(data.version!=3 || !sameWorldName(data.worldName,world.name()))
    throw std::runtime_error("export version or world name mismatch");
  const auto fingerprint = Resources::selectedBaseFingerprint();
  if(fingerprint.empty() || data.dataFingerprint!=fingerprint)
    throw std::runtime_error("base fingerprint mismatch");
  const auto now = world.time();
  if(data.startDay>now.day() || data.clockRate!=clockRate || data.startTime()>now.toInt()/1000 ||
     (fresh && data.startTime()!=now.toInt()/1000))
    throw std::runtime_error("startup clock mismatch");
  auto result = std::unique_ptr<SocietyProjection>(new SocietyProjection(std::move(data),decisionsEnabled));
  if(decisionsEnabled && weights!=nullptr && *weights!='\0') {
    try {
      result->policy = std::make_unique<SocietyPolicy>(weights,result->data);
      Tempest::Log::i("Society shadow policy: network");
      }
    catch(const std::exception& e) {
      Tempest::Log::e("Society shadow policy using teacher: ",e.what());
      }
    }
  else if(decisionsEnabled) {
    Tempest::Log::i("Society shadow policy: teacher");
    }
  return result;
  }

std::unique_ptr<SocietyProjection> SocietyProjection::start(World& world, double clockRate) {
  const auto* path = std::getenv("OPENGOTHIC_SOCIETY_WORLD");
  if(path==nullptr || *path=='\0')
    return {};
  try {
    auto result = open(world,clockRate,true);
    result->bind(world);
    result->selectPilot(world);
    result->events = Society::drainEvents(result->sim).size();
    Tempest::Log::i("Society shadow started: actors=",result->actorByCreation.size()," export_hash=",result->data.exportHash);
    return result;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("Society shadow inactive: ",e.what());
    return {};
    }
  }

void SocietyProjection::bind(World& world) {
  struct Actor {
    Npc* npc;
    std::string symbol;
    Tempest::Vec3 position;
    uint32_t index;
    };
  std::vector<Actor> actors;
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    auto* npc = world.npcById(i);
    actors.push_back({npc,symbolName(world,npc->instanceSymbol()),npc->position(),i});
    }
  // The export orders equal symbols by startup position and then their native insertion index.
  std::sort(actors.begin(),actors.end(),[](const Actor& a, const Actor& b) {
    return std::tie(a.symbol,a.position.x,a.position.z,a.position.y,a.index) <
           std::tie(b.symbol,b.position.x,b.position.z,b.position.y,b.index);
    });

  if(actors.empty() || actors.size()!=data.npcs.size())
    throw std::runtime_error("startup NPC count mismatch");
  // Start VOBs are outside the exported graph. The exporter uses the nearest waynet
  // point for them and for any other actor without an exported current waypoint.
  std::unordered_set<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) { points.emplace(&point); });
  uint32_t ordinal = 0;
  for(uint32_t i=0; i<data.npcs.size(); ++i) {
    const auto& actor = actors[i];
    ordinal = i!=0 && actors[i-1].symbol==actor.symbol ? ordinal+1 : 0;
    const auto key = actor.symbol+"#"+std::to_string(ordinal);
    if(key!=data.npcs.key[i] || actor.symbol!=data.npcs.id[i] || actor.npc->guild()!=data.npcs.guild[i] ||
       (!actor.npc->isPlayer() && !actor.npc->isDead() && !actor.npc->isMonster())!=bool(data.npcs.person[i]))
      throw std::runtime_error("startup NPC identity mismatch at "+key);
    const auto* at = actor.npc->currentWayPoint();
    if(!points.contains(at))
      at = world.findWayPoint(actor.position);
    if(!points.contains(at) || at->name!=data.points.name[data.npcs.startPoint[i]])
      throw std::runtime_error("startup NPC placement mismatch at "+key);

    auto routine = data.npcs.routineBegin[i];
    const auto end = data.npcs.routineBegin[i+1];
    actor.npc->forEachRoutine([&](const Npc::RoutineInfo& row) {
      if(routine==end || data.routines.start[routine]!=minuteOfDay(row.start) ||
         data.routines.end[routine]!=minuteOfDay(row.end) ||
         data.stateNames[data.routines.state[routine]]!=symbolName(world,row.state.ptr) ||
         data.routines.pointName[routine]!=row.point)
        throw std::runtime_error("startup NPC routine mismatch at "+key);
      const auto point = data.routines.point[routine];
      if((row.waypoint==nullptr)!=(point==Society::NoPoint) ||
         (row.waypoint!=nullptr && row.waypoint->name!=data.points.name[point]))
        throw std::runtime_error("startup NPC routine point mismatch at "+key);
      ++routine;
      });
    if(routine!=end || !actorByCreation.emplace(actor.npc->creationSeq(),i).second)
      throw std::runtime_error("startup NPC routine count or creation identity mismatch at "+key);
    }
  }

void SocietyProjection::selectPilot(World& world) {
  const auto* key = std::getenv("OPENGOTHIC_SOCIETY_PILOT_NPC");
  const auto* group = std::getenv("OPENGOTHIC_SOCIETY_PILOT_GROUP");
  const bool single = key!=nullptr && *key!='\0';
  const bool multiple = group!=nullptr && *group!='\0';
  if(const auto* file=std::getenv("OPENGOTHIC_SOCIETY_PILOT_CSV"); file!=nullptr && *file!='\0') {
    profileCsv.open(file,std::ios::app);
    if(!profileCsv)
      Tempest::Log::e("Society pilot profile could not open: ",file);
    else if(profileCsv.tellp()==std::streampos(0))
      profileCsv << "tick_ms,game_s,selected,held,admitted,released,skipped,gesture_repasses,unique_held,unique_released,"
                    "advance_ns,eligibility_ns,sample_ns,checked,blocked_stage,blocked_lock,blocked_near,blocked_camera,"
                    "deferred_retained,bridge_ns,wake_ns,wake_count,sleeping,bridge_outside_tick_ns\n";
    }
  if(!single && !multiple)
    return;
  if(sim.config.decisions) {
    Tempest::Log::e("Society stationary routine pilot inactive: requires routine-only simulation");
    return;
    }
  if(multiple) {
    const std::string_view count(group);
    const auto end = count.data()+count.size();
    unsigned limit = 0;
    const auto [next,error] = std::from_chars(count.data(),end,limit);
    if(single || error!=std::errc() || next!=end || limit==0 || limit>MaxGroup) {
      Tempest::Log::e("Society stationary group pilot inactive: specify 1 to 32 actors without a single pilot");
      return;
      }
    const auto* wideGroup = std::getenv("OPENGOTHIC_SOCIETY_PILOT_WIDE");
    wideGroupPilot = wideGroup!=nullptr && std::string_view(wideGroup)=="1";
    groupLimit = wideGroupPilot ? unsigned(data.npcs.size()) : limit;
    const auto* earlyTick = std::getenv("OPENGOTHIC_SOCIETY_PILOT_EARLY");
    earlyTickPilot = earlyTick!=nullptr && std::string_view(earlyTick)=="1";
    const auto* animation = std::getenv("OPENGOTHIC_SOCIETY_PILOT_ANIM");
    animationPilot = earlyTickPilot || (animation!=nullptr && std::string_view(animation)=="1");
    heldGroup.reserve(groupLimit);
    nextGroup.reserve(groupLimit);
    heldPoints.resize(data.npcs.size());
    nextPoints.resize(data.npcs.size());
    actorTransitions.resize(data.npcs.size());
    if(persistentEnabled())
      sleeping.resize(data.npcs.size());
    }
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    const auto* npc = world.npcById(i);
    const auto found = actorByCreation.find(npc->creationSeq());
    const auto type = npc->handlePtr()->type;
    if(found==actorByCreation.end() || !data.npcs.person[found->second] || !npc->isHuman() || npc->isDead() ||
       (type!=NpcType::AMBIENT && !(multiple && type==NpcType::MAIN)))
      continue;
    const auto actor = found->second;
    if(multiple) {
      if(wideGroupPilot) {
        npc->forEachRoutine([&](const Npc::RoutineInfo& row) {
          const auto* activity = world.rules().eventActivity(row.state);
          if(row.waypoint!=nullptr && activity!=nullptr && Npc::supportsSocietyActivity(*activity))
            groupActors.try_emplace(npc->creationSeq(),actor);
          });
        continue;
        }
      for(auto row=data.npcs.routineBegin[actor]; row<data.npcs.routineBegin[actor+1]; ++row) {
        const auto activity = std::string_view(data.stateNames[data.routines.state[row]]);
        if(data.routines.pointName[row].starts_with("NW_CITY_") &&
           (activity==GuardState || (activity==StandState && type==NpcType::AMBIENT))) {
          groupActors.emplace(npc->creationSeq(),actor);
          break;
          }
        }
      }
    else if(data.npcs.key[actor]==key) {
      pilot = actor;
      pilotCreation = npc->creationSeq();
      routePilot = data.npcs.key[pilot]==RouteActor;
      const auto* post = std::getenv("OPENGOTHIC_SOCIETY_PILOT_POST");
      guardPostPilot = post!=nullptr && std::string_view(post)=="1" && data.npcs.key[pilot]==GuardActor;
      Tempest::Log::i("Society stationary routine pilot: ",data.npcs.key[pilot]);
      if(guardPostPilot)
        Tempest::Log::i("Society guarded post pilot: ",GuardActor," point=",GuardPost);
      return;
      }
    }
  if(multiple)
    Tempest::Log::i("Society stationary group pilot: limit=",groupLimit," selected=",groupActors.size(),
                    " animation=",animationPilot," early_tick=",earlyTickPilot," wide=",wideGroupPilot);
  else
    Tempest::Log::e("Society stationary routine pilot inactive: no matching live ambient human NPC");
  }

uint8_t SocietyProjection::policyMode() const {
  if(policy)
    return 2;
  return sim.config.decisions ? 1 : 0;
  }

void SocietyProjection::save(World& world, Serialize& archive) const {
  if(sim.s.now!=world.time().toInt()/1000)
    return;
  try {
    const auto snapshot = Society::serialize(sim);
    if(snapshot.empty() || snapshot.size()>MaxSnapshotSize)
      throw std::runtime_error("snapshot size is outside the save format limits");
    std::vector<uint32_t> nativeIds(data.npcs.size(),Society::NoNpc);
    for(uint32_t i=0; i<world.npcCount(); ++i) {
      const auto* npc = world.npcById(i);
      const auto found = actorByCreation.find(npc->creationSeq());
      if(found!=actorByCreation.end() && symbolName(world,npc->instanceSymbol())==data.npcs.id[found->second])
        nativeIds[found->second] = i;
      }
    archive.setEntry("worlds/",world.name(),"/society");
    archive.write(SaveMagic,SaveVersion,policyMode(),uint32_t(nativeIds.size()),uint32_t(snapshot.size()));
    archive.writeBytes(nativeIds.data(),nativeIds.size()*sizeof(nativeIds[0]));
    archive.writeBytes(snapshot.data(),snapshot.size());
    if(policy)
      policy->saveState(archive);
    Tempest::Log::i("Society shadow saved: game_s=",sim.s.now," bytes=",snapshot.size());
    }
  catch(const std::exception& e) {
    Tempest::Log::e("Society snapshot omitted: ",e.what());
    }
  }

std::unique_ptr<SocietyProjection> SocietyProjection::restore(World& world, double clockRate, Serialize& archive) {
  const auto* path = std::getenv("OPENGOTHIC_SOCIETY_WORLD");
  if(path==nullptr || *path=='\0')
    return {};
  try {
    if(!archive.setEntry("worlds/",world.name(),"/society"))
      return {};
    uint32_t magic=0, actors=0, size=0;
    uint16_t version=0;
    uint8_t mode=0;
    archive.read(magic,version,mode,actors,size);
    if(magic!=SaveMagic || version!=SaveVersion || size==0 || size>MaxSnapshotSize)
      throw std::runtime_error("unsupported society save section");
    auto result = open(world,clockRate,false);
    if(mode!=result->policyMode() || actors!=result->data.npcs.size())
      throw std::runtime_error("snapshot policy mode or actor count mismatch");
    std::vector<uint32_t> nativeIds(actors);
    archive.readBytes(nativeIds.data(),nativeIds.size()*sizeof(nativeIds[0]));
    std::string snapshot(size,'\0');
    archive.readBytes(snapshot.data(),snapshot.size());
    Society::deserialize(result->sim,snapshot);
    if(result->sim.s.now!=world.time().toInt()/1000)
      throw std::runtime_error("snapshot clock mismatch");
    if(result->policy)
      result->policy->restoreState(archive);
    for(uint32_t i=0; i<actors; ++i) {
      if(nativeIds[i]==Society::NoNpc)
        continue;
      const auto* npc = world.npcById(nativeIds[i]);
      if(npc==nullptr || symbolName(world,npc->instanceSymbol())!=result->data.npcs.id[i] ||
         !result->actorByCreation.emplace(npc->creationSeq(),i).second)
        throw std::runtime_error("snapshot native NPC identity mismatch at "+result->data.npcs.key[i]);
      }
    result->nextSample = result->sim.s.now+SampleInterval;
    result->selectPilot(world);
    Tempest::Log::i("Society shadow restored: actors=",result->actorByCreation.size()," game_s=",result->sim.s.now);
    return result;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("Society shadow restore inactive: ",e.what());
    return {};
    }
  }

bool SocietyProjection::advance(World& world) {
  if(!profileCsv.is_open())
    return advanceSimulation(world);
  const auto start = ProfileClock::now();
  const bool result = advanceSimulation(world);
  advanceNs = elapsedNs(start);
  return result;
  }

bool SocietyProjection::advanceSimulation(World& world) {
  const auto now = world.time().toInt()/1000;
  if(now<sim.s.now) {
    Tempest::Log::e("Society shadow stopped: game clock moved backwards");
    return false;
    }
  if(now==sim.s.now)
    return true;
  try {
    while(Society::advance(sim,decisions,now)!=0) {
      if(policy) {
        try {
          policy->choose(decisions,choices);
          }
        catch(const std::exception& e) {
          policy.reset();
          Tempest::Log::e("Society shadow policy using teacher: ",e.what());
          }
        }
      if(!policy)
        Society::teacher(sim,decisions,choices);
      Society::act(sim,choices);
      }
    const auto changes = Society::drainEvents(sim);
    events += changes.size();
    if(sleepingCount!=0)
      for(const auto& event:changes) {
        if(event.npc>=sleeping.size())
          continue;
        auto* npc = sleeping[event.npc].npc;
        if(npc==nullptr)
          continue;
        switch(event.kind) {
          case Society::EventKind::RoutineEntry:
          case Society::EventKind::TravelStarted:
          case Society::EventKind::Arrived:
          case Society::EventKind::ActivityStarted:
          case Society::EventKind::Teleported:
            wake(world,*npc);
            break;
          default:
            break;
          }
        }
    return true;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("Society shadow stopped: ",e.what());
    return false;
    }
  }

bool SocietyProjection::isPilot(const Npc& npc) const {
  if(groupLimit!=0)
    return nextGroup.size()<groupLimit && groupActors.contains(npc.creationSeq());
  return pilot!=Society::NoNpc && npc.creationSeq()==pilotCreation;
  }

std::string_view SocietyProjection::routineState(const Npc& npc) const {
  if(groupLimit==0)
    return guardPostPilot ? GuardState : StandState;
  const auto activity = sim.s.state[groupActors.at(npc.creationSeq())];
  const auto state = activity!=Society::NoState ? std::string_view(data.stateNames[activity]) : std::string_view();
  return wideGroupPilot || state==GuardState ? state : StandState;
  }

const WayPoint* SocietyProjection::routinePoint(const Npc& npc) const {
  if(wideGroupPilot && (npc.activityStage()==ActivityEvents::Stage::Wait ||
                        (persistentEnabled() && npc.interactive()!=nullptr)))
    return npc.societyWaitingPoint();
  return npc.currentWayPoint();
  }

bool SocietyProjection::holdsRoutineAfterTick(const Npc& npc) const {
  if(npc.societySleeping)
    return true;
  if(groupLimit==0 || heldGroup.empty())
    return false;
  const auto found = groupActors.find(npc.creationSeq());
  return found!=groupActors.end() && heldPoints[found->second]!=nullptr &&
         heldPoints[found->second]==routinePoint(npc);
  }

bool SocietyProjection::holdRoutine(const Npc& npc, std::string_view blocker) {
  const auto& state = sim.s;
  const bool multiple = groupLimit!=0;
  const auto actor = multiple ? groupActors.at(npc.creationSeq()) : pilot;
  const auto place = state.place[actor];
  const auto activity = state.state[actor];
  const auto* point = routinePoint(npc);
  const bool guard = multiple ? activity!=Society::NoState && data.stateNames[activity]==GuardState : guardPostPilot;
  if(blocker.empty()) {
    if(state.flags[actor] & Society::Moving)
      blocker = "model_moving";
    else if(state.entry[actor]==Society::NoEntry || state.entry[actor]!=state.stateEntry[actor])
      blocker = "model_routine_entry";
    else if(state.stateEnd[actor]<=state.now)
      blocker = "model_deadline";
    else if(activity==Society::NoState || (!wideGroupPilot && data.stateNames[activity]!=(guard ? GuardState : StandState)))
      blocker = "model_state";
    else if(place==Society::NoPoint || point==nullptr)
      blocker = "model_waypoint_mismatch";
    else if(!wideGroupPilot && point->isFreePoint() &&
            (!guard || (!multiple && (point->name!=GuardPost || data.points.name[place]!=GuardWaypoint))))
      blocker = "model_waypoint_mismatch";
    else if(!point->isFreePoint() && ((!multiple && guard) || point->name!=data.points.name[place]))
      blocker = "model_waypoint_mismatch";
    else if(npc.currentRoutineIndex()!=int(state.entry[actor]))
      blocker = "model_routine_mismatch";
    else if(multiple && ((!wideGroupPilot && !data.points.name[place].starts_with("NW_CITY_")) ||
                        data.routines.point[data.entryIndex(actor,state.entry[actor])]!=place))
      blocker = "model_routine_point";
    }
  const bool hold = blocker.empty();
  if(multiple) {
    if(profileCsv.is_open()) {
      ++groupChecked;
      if(blocker=="native_activity_stage")
        ++groupStageBlocked;
      else if(blocker=="native_waypoint_lock" || blocker=="native_guard_lock")
        ++groupLockBlocked;
      else if(blocker=="hero_near")
        ++groupNearBlocked;
      else if(blocker=="native_pose_needed" || blocker=="camera_visible" || blocker=="no_camera")
        ++groupCameraBlocked;
      }
    if(hold) {
      if(nextPoints[actor]!=nullptr)
        return nextPoints[actor]==point;
      nextPoints[actor] = point;
      nextGroup.push_back({actor,point});
      }
    return hold;
    }
  pilotResult = hold ? "admitted" : blocker;
  if(hold!=pilotHolding) {
    pilotHolding = hold;
    Tempest::Log::i("Society stationary routine ",hold ? "held" : "released",": ",data.npcs.key[pilot],
                    " game_s=",state.now);
    }
  return hold;
  }

void SocietyProjection::recordExecution(bool skipped) {
  if(groupLimit!=0) {
    ++(skipped ? groupSkipped : groupRepasses);
    return;
    }
  if(skipped) {
    pilotResult = "skipped";
    ++pilotSkipped;
    }
  else {
    pilotResult = "gesture_repass";
    ++pilotGestureRepasses;
    }
  }

void SocietyProjection::recordEarlyTick(const Npc& npc) {
  recordExecution(true);
  if(!profileCsv.is_open())
    return;
  const auto actor = groupActors.at(npc.creationSeq());
  constexpr uint8_t EarlyTick = 4;
  auto& visited = actorTransitions[actor];
  if(visited & EarlyTick)
    return;
  visited |= EarlyTick;
  Tempest::Log::i("Society group early tick: ",data.npcs.key[actor]," actor=",actor,
                  " tick=",npc.world().tickCount()," create=",npc.creationSeq());
  }

void SocietyProjection::schedule(uint32_t actor, uint64_t now) {
  auto& record = sleeping[actor];
  const auto& npc = *record.npc;
  const auto& activity = *npc.activityEvents.activity;
  const auto* point = activity.point;
  auto next = npc.aiState.loopNextTime;
  if(point!=nullptr && point->timed() && npc.handlePtr()->aivar[19]==point->poseStage) {
    auto gesture = npc.aiState.sTime + uint64_t(point->timerSeconds+1)*1000;
    if(point->form!=ActivityDefinitions::PointActivity::Form::Guard)
      gesture = std::max(gesture,npc.aiState.loopNextTime);
    if(npc.societyPoseSuspended)
      gesture = gesture>poseLead ? gesture-poseLead : 0;
    next = std::min(next,gesture);
    } else if(activity.mob!=nullptr && activity.mob->timer && npc.societyPoseSuspended) {
    const auto threshold = npc.aiState.sTime + uint64_t(activity.mob->timer->timerSeconds+1)*1000;
    const auto gesture = std::max(threshold,npc.aiState.loopNextTime);
    const auto prepare = gesture>poseLead ? gesture-poseLead : 0;
    next = std::min(next,prepare);
    }
  record.stepAt = std::max(next,now+1);
  deadlines.emplace(record.stepAt,actor);
  }

void SocietyProjection::sleep(World& world, Npc& npc) {
  const auto start = beginEligibility();
  const auto actor = groupActors.at(npc.creationSeq());
  auto& record = sleeping[actor];
  record.npc = &npc;
  record.entered = world.wobj.simTickClock;
  record.poseClock = world.wobj.npcAnimationClock;
  record.routineAt = std::min(npc.aiState.eTime.toInt(),npc.activityEvents.deadline.toInt());
  routineDeadlines.emplace(record.routineAt,actor);
  npc.societySleeping = true;
  npc.societyPoseSuspended = !npc.societyGestureNeedsPose(poseLead);
  world.wobj.updateSocietyDispatch(npc);
  ++sleepingCount;
  const auto position = npc.position();
  const auto chunk = ChunkId::fromPosition(position.x,position.z);
  record.cell = {chunk.x,chunk.z};
  auto& cell = cells[record.cell];
  Tempest::Vec3 lo, hi;
  AnimationLod::View::bodyBounds(npc,lo,hi);
  lo = Tempest::Vec3(std::min(lo.x,position.x),std::min(lo.y,position.y),std::min(lo.z,position.z));
  hi = Tempest::Vec3(std::max(hi.x,position.x),std::max(hi.y,position.y),std::max(hi.z,position.z));
  if(cell.actors.empty()) {
    cell.lo = lo;
    cell.hi = hi;
    } else {
    cell.lo = Tempest::Vec3(std::min(cell.lo.x,lo.x),std::min(cell.lo.y,lo.y),std::min(cell.lo.z,lo.z));
    cell.hi = Tempest::Vec3(std::max(cell.hi.x,hi.x),std::max(cell.hi.y,hi.y),std::max(cell.hi.z,hi.z));
    }
  cell.actors.push_back(actor);
  schedule(actor,world.tickCount());
  if(start!=ProfileClock::time_point{})
    bridgeNs += elapsedNs(start);
  }

void SocietyProjection::resumePose(World& world, Npc& npc) {
  if(!npc.societyPoseSuspended)
    return;
  const auto actor = groupActors.at(npc.creationSeq());
  npc.societyPoseSuspended = false;
  const auto from = sleeping[actor].poseClock;
  const auto to = world.wobj.npcAnimationClock;
  if(to>from) {
    npc.deferSocietyAnimation(to-from);
    world.wobj.societyPoseWakePending = true;
    }
  world.wobj.updateSocietyDispatch(npc);
  }

void SocietyProjection::wake(World& world, Npc& npc) {
  if(!npc.societySleeping)
    return;
  const auto start = beginEligibility();
  const auto actor = groupActors.at(npc.creationSeq());
  auto& record = sleeping[actor];
  resumePose(world,npc);
  npc.societySleeping = false;
  npc.simTick().pending = 0;
  // Dormant actors have no live tags or nutrition. Skip the idle interval on return.
  npc.lastEventTime = world.tickCount();
  npc.foodPoolTick = world.tickCount();
  world.wobj.updateSocietyDispatch(npc);
  if(record.committed)
    heldGroup.push_back({actor,heldPoints[actor]});
  if(record.stepAt!=0)
    deadlines.erase({record.stepAt,actor});
  routineDeadlines.erase({record.routineAt,actor});
  auto cell = cells.find(record.cell);
  if(cell!=cells.end()) {
    std::erase(cell->second.actors,actor);
    if(cell->second.actors.empty())
      cells.erase(cell);
    }
  record = {};
  --sleepingCount;
  ++wakeCount;
  if(start!=ProfileClock::time_point{})
    wakeNs += elapsedNs(start);
  }

void SocietyProjection::wakeAll(World& world) {
  for(auto& record:sleeping)
    if(record.npc!=nullptr)
      wake(world,*record.npc);
  }

void SocietyProjection::runDeadlines(World& world, uint64_t dt) {
  const auto start = beginEligibility();
  const auto now = world.tickCount();
  const auto gameNow = world.time().toInt();
  const auto lead = 4*AnimationLod::config().rate*dt;
  if(lead>poseLead) {
    poseLead = lead;
    for(uint32_t actor=0; actor<sleeping.size(); ++actor) {
      auto& record = sleeping[actor];
      if(record.npc!=nullptr && record.stepAt!=0) {
        deadlines.erase({record.stepAt,actor});
        record.stepAt = now;
        deadlines.emplace(now,actor);
        }
      }
    }
  while(!routineDeadlines.empty() && routineDeadlines.begin()->first<=gameNow)
    wake(world,*sleeping[routineDeadlines.begin()->second].npc);
  while(!deadlines.empty() && deadlines.begin()->first<=now) {
    const auto actor = deadlines.begin()->second;
    deadlines.erase(deadlines.begin());
    auto& record = sleeping[actor];
    record.stepAt = 0;
    auto& npc = *record.npc;
    if(npc.societyPoseSuspended && npc.societyGestureNeedsPose(poseLead))
      resumePose(world,npc);
    const bool loopDue = npc.aiState.loopNextTime<=now;
    const auto* point = npc.activityEvents.activity->point;
    const bool gestureDue = point!=nullptr ?
                            point->timed() && (loopDue || point->form==ActivityDefinitions::PointActivity::Form::Guard) &&
                            npc.pointGestureDue() :
                            loopDue && npc.mobGestureDue();
    if(loopDue || gestureDue) {
      const auto elapsed = world.wobj.societyDueStep(npc,record.entered);
      if(elapsed>0) {
        if(gestureDue) {
          wake(world,npc);
          // Only the ordinary cadence interval belongs to the native step.
          npc.simTick().pending = elapsed-dt;
          continue;
          }
        npc.aiState.loopNextTime = now + npc.perceptionTimeClampt();
        }
      }
    schedule(actor,now);
    }
  if(start!=ProfileClock::time_point{})
    bridgeOutsideTickNs += elapsedNs(start);
  }

void SocietyProjection::updateInterest(World& world) {
  const auto start = beginEligibility();
  const auto* hero = world.player();
  AnimationLod::View next;
  if(hero==nullptr || !world.wobj.makeAnimationLodView(next)) {
    view.reset();
    wakeAll(world);
    if(start!=ProfileClock::time_point{})
      bridgeOutsideTickNs += elapsedNs(start);
    return;
    }
  const auto at = hero->position();
  const bool unchanged = view && view->sameView(next) && at==viewHero;
  if(!unchanged) {
    view = next;
    viewHero = at;
    interestWakes.clear();
    for(const auto& [key,cell]:cells) {
      const Tempest::Vec3 closest(std::clamp(at.x,cell.lo.x,cell.hi.x),
                                  std::clamp(at.y,cell.lo.y,cell.hi.y),
                                  std::clamp(at.z,cell.lo.z,cell.hi.z));
      // The existing hero distance test is also applied on each candidate.
      if((closest-at).quadLength()>=FarDistance*FarDistance && !next.couldSee(cell.lo,cell.hi))
        continue;
      for(const auto actor:cell.actors)
        if(!world.societyVisibilityBlocker(*sleeping[actor].npc,next).empty())
          interestWakes.push_back(actor);
      }
    for(const auto actor:interestWakes)
      wake(world,*sleeping[actor].npc);
    }
  if(start!=ProfileClock::time_point{})
    bridgeOutsideTickNs += elapsedNs(start);
  }

std::optional<bool> SocietyProjection::stepRoute(Npc& npc, const WayPoint* destination, uint64_t dt, float destDist,
                                                  std::string_view blocker) {
  const auto& state = sim.s;
  if(blocker.empty()) {
    if(destination==nullptr)
      blocker = "native_route_unavailable";
    else if(destination->name!=RouteTo)
      blocker = "native_route_destination";
    else if(!(state.flags[pilot] & Society::Moving))
      blocker = "model_not_walking";
    else if(state.place[pilot]==Society::NoPoint || state.dest[pilot]==Society::NoPoint ||
            data.points.name[state.place[pilot]]!=RouteFrom || data.points.name[state.dest[pilot]]!=RouteTo)
      blocker = "model_route_mismatch";
    else if(state.entry[pilot]!=state.stateEntry[pilot] || npc.currentRoutineIndex()!=int(state.entry[pilot]))
      blocker = "model_routine_mismatch";
    }
  const auto result = blocker.empty() ? std::string_view("society") : blocker;
  routeHeld = blocker.empty();
  if(routeResult!=result) {
    const auto at = npc.position();
    Tempest::Log::i("Society route: npc=",RouteActor," result=",result," steps=",routeSteps,
                    " tick=",npc.world().tickCount()," game_s=",state.now,
                    " x=",at.x," y=",at.y," z=",at.z);
    routeResult = result;
    }
  if(!routeHeld)
    return {};

  // Society admits the route and schedules this step. The existing native
  // executor retains ownership of the waypoint cursor, pose and collider.
  ++routeSteps;
  const bool walking = npc.stepSocietyWaypoint(dt,destDist);
  if(!walking) {
    routeHeld = false;
    routeResult = "arrived";
    Tempest::Log::i("Society route: npc=",RouteActor," result=arrived steps=",routeSteps,
                    " tick=",npc.world().tickCount()," game_s=",state.now);
    }
  return walking;
  }

void SocietyProjection::sample(World& world) {
  const auto start = beginEligibility();
  size_t admitted = 0, released = 0;
  const auto transition = [&](const GroupHold& entry, bool hold) {
    const auto flag = uint8_t(hold ? 1 : 2);
    auto& visited = actorTransitions[entry.actor];
    if((visited & flag)==0) {
      visited |= flag;
      ++(hold ? uniqueHeld : uniqueReleased);
      }
    Tempest::Log::i("Society group routine ",hold ? "held" : "released",": ",data.npcs.key[entry.actor],
                    " actor=",entry.actor," point=",entry.point->name," game_s=",sim.s.now," tick=",world.tickCount());
    };
  if(groupLimit!=0) {
    for(const auto& entry:heldGroup) {
      if(nextPoints[entry.actor]!=entry.point) {
        ++released;
        transition(entry,false);
        if(persistentEnabled())
          heldPoints[entry.actor] = nullptr;
        }
      }
    for(const auto& entry:nextGroup)
      if(heldPoints[entry.actor]!=entry.point) {
        ++admitted;
        transition(entry,true);
        }
    if(persistentEnabled()) {
      heldGroup.clear();
      for(const auto& entry:nextGroup) {
        auto& record = sleeping[entry.actor];
        if(record.npc!=nullptr)
          record.committed = true;
        else
          heldGroup.push_back(entry);
        heldPoints[entry.actor] = entry.point;
        nextPoints[entry.actor] = nullptr;
        }
      } else {
      for(const auto& entry:heldGroup)
        heldPoints[entry.actor] = nullptr;
      heldPoints.swap(nextPoints);
      heldGroup.swap(nextGroup);
      }
    nextGroup.clear();
    }
  sampleShadow(world);
  if(profileCsv.is_open()) {
    sampleNs = elapsedNs(start);
    profileTick = world.tickCount();
    profileHeldCount = heldGroup.size()+sleepingCount;
    admittedCount = admitted;
    releasedCount = released;
    if(!persistentEnabled())
      writeProfile();
    } else {
    writeProfile();
    }
  }

void SocietyProjection::writeProfile() {
  if(profileCsv.is_open() && profileTick==0)
    return;
  if(profileCsv.is_open()) {
    profileCsv << profileTick << ',' << sim.s.now << ',' << groupActors.size() << ',' << profileHeldCount << ','
               << admittedCount << ',' << releasedCount << ',' << groupSkipped << ',' << groupRepasses << ',' << uniqueHeld << ','
               << uniqueReleased << ',' << advanceNs << ',' << eligibilityNs << ',' << sampleNs << ',' << groupChecked << ','
               << groupStageBlocked << ',' << groupLockBlocked << ',' << groupNearBlocked << ',' << groupCameraBlocked << ','
               << groupDeferred << ',' << bridgeNs+bridgeOutsideTickNs+eligibilityNs+sampleNs << ',' << wakeNs << ',' << wakeCount << ','
               << sleepingCount << ',' << bridgeOutsideTickNs << '\n';
    }
  profileTick = 0;
  groupSkipped = groupRepasses = groupDeferred = 0;
  groupChecked = groupStageBlocked = groupLockBlocked = groupNearBlocked = groupCameraBlocked = 0;
  eligibilityNs = bridgeNs = bridgeOutsideTickNs = wakeNs = sampleNs = 0;
  wakeCount = 0;
  }

void SocietyProjection::sampleShadow(World& world) {
  if(pilot!=Society::NoNpc) {
    if(pilotResult!=previousPilotResult) {
      Tempest::Log::i("Society pilot tick: npc=",data.npcs.key[pilot]," result=",pilotResult,
                      " skipped=",pilotSkipped," gesture_repass=",pilotGestureRepasses,
                      " game_s=",sim.s.now," tick=",world.tickCount());
      previousPilotResult = pilotResult;
      }
    pilotResult = "routine_not_ticked";
    }
  if(sim.s.now<nextSample)
    return;
  nextSample = sim.s.now+SampleInterval;
  size_t people = 0, stateMatches = 0, routineMatches = 0, withinTenMeters = 0;
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    auto* npc = world.npcById(i);
    const auto found = actorByCreation.find(npc->creationSeq());
    if(found==actorByCreation.end() || npc->isDead() || npc->isPlayer() || npc->isMonster())
      continue;
    const auto actor = found->second;
    if(!data.npcs.person[actor])
      continue;
    ++people;
    const auto state = sim.s.state[actor];
    if(state==Society::NoState ? !npc->currentState().isValid() :
       npc->isStateStarted() && data.stateNames[state]==world.rules().symbolName(npc->currentState().ptr))
      ++stateMatches;
    const auto entry = sim.s.entry[actor];
    if(npc->currentRoutineIndex()==(entry==Society::NoEntry ? -1 : int(entry)))
      ++routineMatches;
    const auto place = sim.s.place[actor];
    if(place==Society::NoPoint)
      continue;
    auto at = data.points.pos[place];
    if(sim.s.flags[actor] & Society::Moving) {
      const auto span = sim.s.arrive[actor]-sim.s.depart[actor];
      const auto fraction = span>0 ? double(sim.s.now-sim.s.depart[actor])/double(span) : 1.0;
      at = sim.paths.along(place,sim.s.dest[actor],fraction);
      }
    if((npc->position()-Tempest::Vec3(at[0],at[1],at[2])).quadLength()<=1000.f*1000.f)
      ++withinTenMeters;
    }
  Tempest::Log::i("Society shadow: game_s=",sim.s.now," events=",events," people=",people,
                  " routine_match=",routineMatches," state_match=",stateMatches," within_10m=",withinTenMeters);
  events = 0;
  }
