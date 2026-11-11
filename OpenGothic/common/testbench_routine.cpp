#include "testbench_internal.h"

#include <algorithm>
#include <cmath>

#include "game/gamescript.h"
#include "gothic.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include "game/serialize.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/worldview.h"
#include "world/objects/interactive.h"

using namespace Tempest;

namespace testbench_detail {

const char* questReason(uint8_t hazards) {
  return hazards==3 ? "drop+water" : hazards==2 ? "water" : hazards==1 ? "drop" : "none";
  }

bool chooseQuestSite(World& world, bool water, QuestSite& site,
                     const std::function<bool(QuestSite&)>& accept) {
  if(site.ready && (!accept || accept(site))) return true;
  site.ready=false;
  std::vector<const WayPoint*> points;
  if(!water)
    if(const auto* point=world.findPoint("NW_XARDAS_TOWER_VALLEY_02",false)) points.push_back(point);
  world.forEachWayPoint([&](const WayPoint& point) {
    const auto& n=point.name;
    const bool wanted=water ? n.find("HABOUR")!=std::string::npos || n.find("HARBOR")!=std::string::npos ||
      n.find("HARBOUR")!=std::string::npos || n.find("WATER")!=std::string::npos || n.find("LAKE")!=std::string::npos :
      n.find("XARDAS_TOWER")!=std::string::npos;
    if(wanted && !point.underWater) points.push_back(&point);
    });
  if(points.size()>1) std::sort(points.begin()+(!water ? 1 : 0),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  if(points.size()>100) points.resize(100);
  auto* physics=world.physic();
  for(const auto* point:points) {
    const auto floor=physics->landRay(point->position()+Vec3(0,150,0),500);
    if(!floor.hasCol || floor.n.y<.9f) continue;
    const auto dry=floor.v;
    const auto wet=physics->waterRay(dry,15);
    if(wet.hasCol) continue;
    bool support=true;
    for(float x:{-25.f,25.f}) for(float z:{-25.f,25.f}) {
      const auto corner=physics->landRay(dry+Vec3(x,40,z),100);
      support=support && corner.hasCol && corner.n.y>.85f && std::abs(corner.v.y-dry.y)<15;
      }
    if(!support) continue;
    for(int a=0;a<24;++a) {
      const float angle=float(a)*float(M_PI)/12.f;
      const Vec3 direction(std::cos(angle),0,std::sin(angle));
      for(float distance:{200.f,350.f,500.f,700.f}) {
        const auto edge=dry+direction*distance;
        const auto bottom=physics->landRay(edge+Vec3(0,100,0),6000);
        if(!bottom.hasCol) continue;
        const auto liquid=physics->waterRay(bottom.v+Vec3(0,5,0),0);
        if(water ? !liquid.hasCol || liquid.wdepth-bottom.v.y<150 || liquid.wdepth>dry.y+80 : dry.y-bottom.v.y<1200)
          continue;
        if(physics->ray(dry+Vec3(0,110,0),edge+Vec3(0,110,0)).hasCol) continue;
        site={true,dry,direction,water ? liquid.wdepth : 0,point->name};
        if(accept && !accept(site)) {site.ready=false;continue;}
        return true;
        }
      }
    }
  return false;
  }

void questFields(World& world, Npc& npc, bool csv) {
  const auto context=routineContext();
  auto& benchState=context.routine;
  const auto& q=npc.ragdollQuestReturnState();
  const auto pos=npc.position();
  auto pelvis=pos;
  if(const auto* body=npc.ragdollBody()) {
    const auto matrix=body->pelvisTransform();pelvis={matrix.at(3,0),matrix.at(3,1),matrix.at(3,2)};
    }
  const auto water=world.physic()->waterRay(pelvis,0);
  const bool submerged=water.hasCol && pelvis.y<water.wdepth-5;
  if(benchState.questLaunched) {
    benchState.questSawDrop|=benchState.questStable.y-pelvis.y>RagdollSave::QuestReturn::maxFall;
    benchState.questSawWater|=submerged;
    }
  const auto count=npc.ragdollQuestReturnCount();
  if(count>benchState.questLastReturns)
    event("immortal_guard_return","\"case\":"+jsonStr(benchState.questCase)+",\"reason\":"+jsonStr(questReason(q.hazards))+
      ",\"from_cm\":"+ragdollVector(npc.ragdollQuestReturnFrom())+",\"to_cm\":"+ragdollVector(pos)+
      ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"count\":"+std::to_string(count));
  benchState.questLastReturns=count;
  if(!csv) return;
  const auto now=std::chrono::steady_clock::now().time_since_epoch();
  const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  const double wall=benchState.questWallNs ? double(ns-benchState.questWallNs)/1e6 : 0;benchState.questWallNs=ns;
  std::fprintf(context.csv,",%d,%d,%d,%d,%u,%u,%s,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%.6f",
    int(npc.isImmortal()),Gothic::settingsGetI("GAME","ragdoll_quest_guard"),int(q.hasAnchor),int(q.armed),unsigned(q.hazards),count,
    questReason(q.hazards),double(q.anchor.x),double(q.anchor.y),double(q.anchor.z),double(pos.x),double(pos.y),double(pos.z),
    double(benchState.questStable.y-pos.y),int(submerged),int(npc.isUnconscious()),wall);
  event("immortal_guard_frame","\"case\":"+jsonStr(benchState.questCase)+
    ",\"actor_id\":"+std::to_string(world.npcId(&npc))+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"immortal\":"+(npc.isImmortal()?"true":"false")+",\"enabled\":"+(Gothic::settingsGetI("GAME","ragdoll_quest_guard")?"true":"false")+
    ",\"has_anchor\":"+(q.hasAnchor?"true":"false")+",\"armed\":"+(q.armed?"true":"false")+
    ",\"hazards\":"+std::to_string(q.hazards)+",\"returns\":"+std::to_string(count)+",\"reason\":"+jsonStr(questReason(q.hazards))+
    ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"position_cm\":"+ragdollVector(pos)+",\"pelvis_cm\":"+ragdollVector(pelvis)+
    ",\"water_y_cm\":"+(water.hasCol?ragdollNumber(water.wdepth):"null")+",\"in_water\":"+(submerged?"true":"false")+
    ",\"unconscious\":"+(npc.isUnconscious()?"true":"false")+",\"dead\":"+(npc.isDead()?"true":"false")+
    ",\"parts\":"+std::to_string(npc.ragdollBody()?npc.ragdollBody()->partCount():0));
  }

bool routineReturnRestore(World& world,Routine::RoutineReturnCase& c) {
  const auto context=routineContext();
  auto& benchState=context.routine;
  if(!c.npc || world.npcId(c.npc)==uint32_t(-1) || c.npc->instanceSymbol()!=world.script().findSymbolIndex(c.symbol)) {
    arenaAbort("routine-return fixture actor is no longer active in the world");return false;
    }
  auto& npc=*c.npc;
  npc.setInteraction(nullptr,true);npc.clearAiQueue();npc.clearState(true);
  {
  MemReader reader(c.fixture);Serialize in(reader);in.setContext(&world);in.setVersion(Serialize::Current);
  npc.load(in,c.npcId,"/routine-return/");npc.postValidate();
  }
  npc.setExternalControl(false);npc.prepareRagdoll();npc.updateTransform();
  const bool valid=!npc.isDown() && !npc.ragdollBody() && npc.attribute(ATR_HITPOINTS)==c.hp;
  event("routine_return_fixture_reset","\"case\":"+jsonStr(c.label)+",\"attempt\":"+std::to_string(benchState.routineRetry?2:1)+
    ",\"actor_id\":"+std::to_string(world.npcId(&npc))+",\"hp\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
    ",\"alive\":"+(!npc.isDown()?"true":"false")+",\"external\":"+(npc.isExternalControl()?"true":"false")+
    ",\"valid\":"+(valid?"true":"false"));
  if(!valid)arenaAbort("routine-return fixture did not restore its original live actor");
  return valid;
  }

void routineReturnBegin(World& world,Npc& player) {
  const auto context=routineContext();
  auto& benchState=context.routine;
  auto& c=benchState.routineCases[benchState.routineIndex];
  // Restore before setDayTime can retire a corpse from the active NPC array.
  if(benchState.routineRetry && !routineReturnRestore(world,c))return;
  world.setDayTime(12,0);
  const auto support=world.physic()->landRay(c.start+Vec3(0,100,0),500);
  if(!support.hasCol || support.n.y<.7f) {arenaAbort("routine-return start has no walkable support");return;}
  c.start=support.v;
  auto* npc=findNpc(world,player,c.symbol);
  bool hasRoutine=false;
  if(npc)npc->forEachRoutine([&](const Npc::RoutineInfo&) {hasRoutine=true;});
  const auto* nearest=world.findWayPoint(c.start);
  if(!npc || !npc->isHuman() || npc->isDown() || !hasRoutine || !npc->currentTaPoint() ||
     !nearest || (c.start-nearest->pos).length()<1000 || (c.start-nearest->pos).length()>2000) {
    arenaAbort("routine-return requires a live human with a daily routine and a 10-20m off-waynet start");return;
    }
  const auto* skeleton=npc->visualSkeleton();
  if(!skeleton || skeleton->findNode("BIP01 L FOOT")==size_t(-1) || skeleton->findNode("BIP01 R FOOT")==size_t(-1)) {
    arenaAbort("routine-return human has no native foot bones");return;
    }
  // The hero is parked far away, so residency tiers would make the actor and its neighbours
  // Simplified: no dry anchor, no ragdoll, graph movement and ghost bodies. Keep the whole
  // world on the native near-hero tier; the override is bench-only and never saved.
  for(uint32_t i=0;i<world.npcCount();++i)
    if(auto* other=world.npcById(i))other->setSimTierOverride(NpcSimTier::Full);
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);
  npc->clearGoTo();npc->stopWalking();npc->closeWeapon(true);npc->setAnim(Npc::Anim::Idle);npc->setExternalControl(true);
  npc->setPosition(c.start);npc->updateTransform();
  if(c.fixture.empty()) {
    c.npc=npc;c.npcId=world.npcId(npc);c.hp=npc->attribute(ATR_HITPOINTS);
    MemWriter writer(c.fixture);Serialize out(writer);out.setContext(&world);out.setVersion(Serialize::Current);
    npc->save(out,c.npcId,"/routine-return/");
    }
  const auto* target=npc->currentTaPoint();
  // One dry parking point, verified more than 87 m from all fixture starts and TA goals.
  const auto* parking=world.findWayPoint("NW_PATH_TO_MONASTER_MONSTER22");
  if(!parking) {arenaAbort("routine-return fixed hero parking waypoint is missing");return;}
  const auto floor=world.physic()->landRay(parking->pos+Vec3(0,100,0),500);
  if(!floor.hasCol || floor.n.y<.7f || world.physic()->waterRay(floor.v,15).hasCol) {
    arenaAbort("routine-return fixed hero parking has no dry support");return;
    }
  const auto hero=floor.v;
  player.closeWeapon(true);player.setPosition(hero);player.updateTransform();
  // The hero is not the measured actor: keep him from sliding off a remote slope.
  context.pinned=&player;context.pinAt=hero;
  benchState.routineNpc=npc;benchState.routinePhase=1;benchState.routineSince=world.tickCount();
  benchState.routineUp=benchState.routineFrame=0;benchState.routineTravel=benchState.routineMaxStep=0;
  benchState.routineJumps=benchState.routineMobJumps=benchState.routineClamps=0;benchState.routineBelow=false;benchState.routineMinHero=(npc->position()-hero).length();
  benchState.routinePreviousMob=nullptr;benchState.routineHasRoute=false;
  benchState.routineRouteLength=0;benchState.routineDeadline=90000;benchState.routinePathFirst.clear();benchState.routinePathLast.clear();benchState.routinePathSample.clear();
  benchState.routineGuardBase=benchState.routineReturns=npc->ragdollQuestReturnCount();
  benchState.routinePreviousGuard=npc->ragdollQuestReturnState();
  benchState.routinePrevious=npc->position();
  size_t ignored=0;const Vec3 bounds[2]={};std::string label;
  const auto hit=world.physic()->rayClothCamera(c.start+Vec3(0,100,0),nearest->pos+Vec3(0,100,0),"",bounds,ignored,&label);
  Vec3 shore={};float shoreDepth=0,shoreDistance=std::numeric_limits<float>::infinity();
  for(int x=-10;x<=10;++x)for(int z=-10;z<=10;++z) {
    const auto sample=c.start+Vec3(float(x)*100,300,float(z)*100);
    const auto bottom=world.physic()->landRay(sample,3000);
    if(!bottom.hasCol)continue;
    const auto water=world.physic()->waterRay(bottom.v+Vec3(0,1,0),0);
    const auto distance=std::hypot(sample.x-c.start.x,sample.z-c.start.z);
    if(water.hasCol && water.wdepth-bottom.v.y>1 && water.wdepth-bottom.v.y<100 && distance<shoreDistance) {
      shore=bottom.v;shoreDepth=water.wdepth-bottom.v.y;shoreDistance=distance;
      }
    }
  event("routine_return_begin","\"case\":"+jsonStr(c.label)+",\"attempt\":"+std::to_string(benchState.routineRetry?2:1)+
    ",\"npc\":"+jsonStr(c.symbol)+",\"type\":"+jsonStr(c.type)+",\"start_cm\":"+ragdollVector(c.start)+
    ",\"nearest\":"+jsonStr(nearest->name)+",\"nearest_cm\":"+ragdollVector(nearest->pos)+
    ",\"distance_cm\":"+ragdollNumber((c.start-nearest->pos).length())+",\"blocker\":"+jsonStr(label)+
    ",\"blocked\":"+(hit.hasCol?"true":"false")+",\"hit_cm\":"+ragdollVector(hit.v)+
    ",\"indoor\":"+(world.isIndoor(c.start+Vec3(0,100,0))?"true":"false")+
    ",\"room\":"+jsonStr(world.roomAt(c.start+Vec3(0,100,0)))+
    ",\"shallow_cm\":"+ragdollVector(shore)+",\"shallow_depth_cm\":"+ragdollNumber(shoreDepth)+
    ",\"shallow_distance_cm\":"+(std::isfinite(shoreDistance)?ragdollNumber(shoreDistance):"null")+
    ",\"target\":"+jsonStr(target->name)+",\"target_cm\":"+ragdollVector(target->pos)+
    ",\"hero_cm\":"+ragdollVector(hero)+",\"routines\":"+destructionRoutineRows(*npc));
  }

void routineReturnSummary() {
  const auto context=routineContext();
  auto& benchState=context.routine;
  event("routine_return_summary","\"cases\":"+std::to_string(benchState.routineCases.size())+
    ",\"failed\":"+std::to_string(benchState.routineRetries.size())+",\"repeated\":"+std::to_string(benchState.routineRetryIndex)+",\"complete\":true");
  }

void routineReturnResult(World& world,Npc& player,bool arrived,bool preparation=true) {
  const auto context=routineContext();
  auto& benchState=context.routine;
  auto& c=benchState.routineCases[benchState.routineIndex];
  const auto& npc=*benchState.routineNpc;
  const auto* ta=npc.currentTaPoint();
  const auto* visible=world.findWayPoint(npc.position(),[&](const WayPoint& wp) {return npc.canRayHitPoint(wp.pos);});
  preparation=preparation && benchState.routineMinHero>3000;
  const bool pass=preparation && arrived && !benchState.routineJumps && !benchState.routineBelow;
  event("routine_return_result","\"case\":"+jsonStr(c.label)+",\"attempt\":"+std::to_string(benchState.routineRetry?2:1)+
    ",\"pass\":"+(pass?"true":"false")+",\"preparation_valid\":"+(preparation?"true":"false")+
    ",\"arrived\":"+(arrived?"true":"false")+",\"elapsed_ms\":"+std::to_string(benchState.routineUp?world.tickCount()-benchState.routineUp:0)+
    ",\"position_cm\":"+ragdollVector(npc.position())+",\"travel_cm\":"+ragdollNumber(benchState.routineTravel)+
    ",\"direct_cm\":"+(ta?ragdollNumber((benchState.routineUpPosition-ta->pos).length()):"null")+
    ",\"route_length_cm\":"+(benchState.routineHasRoute?ragdollNumber(benchState.routineRouteLength):"null")+
    ",\"deadline_ms\":"+std::to_string(benchState.routineDeadline)+
    ",\"target_distance_cm\":"+(ta?ragdollNumber((npc.position()-ta->pos).length()):"null")+
    ",\"max_step_cm\":"+ragdollNumber(benchState.routineMaxStep)+",\"jumps\":"+std::to_string(benchState.routineJumps)+
    ",\"mob_alignment_jumps\":"+std::to_string(benchState.routineMobJumps)+
    ",\"below_world\":"+(benchState.routineBelow?"true":"false")+",\"min_hero_cm\":"+ragdollNumber(benchState.routineMinHero)+
    ",\"guard_clamps\":"+std::to_string(benchState.routineClamps)+
    ",\"guard_returns\":"+std::to_string(npc.ragdollQuestReturnCount()-benchState.routineGuardBase)+
    ",\"state\":"+jsonStr(stateName(world,npc))+",\"animation\":"+jsonStr(npc.animNames())+
    ",\"queue\":"+std::to_string(npc.aiQueueSize())+",\"policy\":"+jsonStr(policyName(npc.processPolicy()))+
    ",\"nearest_visible\":"+jsonStr(visible?visible->name:"")+",\"path_first\":"+jsonStr(benchState.routinePathFirst)+
    ",\"path_last\":"+jsonStr(benchState.routinePathLast));
  if(!benchState.routineRetry)c.failed=!pass;
  benchState.routinePhase=0;benchState.routineNpc=nullptr;
  // Measurement is complete; reset now so the next noon cannot retire this actor.
  if(!routineReturnRestore(world,c))return;
  if(benchState.routineRetry) {
    ++benchState.routineRetryIndex;
    if(benchState.routineRetryIndex<benchState.routineRetries.size()) {
      benchState.routineIndex=benchState.routineRetries[benchState.routineRetryIndex];routineReturnBegin(world,player);
      }
    else routineReturnSummary();
    }
  }

// Inspect the native current leg and remaining route; never construct or assign a path.
void routineReturnObservePath(World& world,const Npc& npc) {
  const auto context=routineContext();
  auto& benchState=context.routine;
  const auto& c=benchState.routineCases[benchState.routineIndex];
  const auto* target=npc.currentTaPoint();
  auto path=npc.navigationPath();
  std::vector<const WayPoint*> route;
  if(const auto* go=npc.currentGoToPoint())route.push_back(go);
  while(const auto* point=path.pop())
    if(route.empty() || route.back()!=point)route.push_back(point);
  const auto* first=route.empty()?nullptr:route.front();
  const auto* last=route.empty()?nullptr:route.back();
  const bool measuring=benchState.routinePhase==4 && !benchState.routineHasRoute && target && last==target;
  float length=0;auto previous=measuring?benchState.routineUpPosition:npc.position();
  std::string points="[";
  for(const auto* point:route) {
    length+=(point->pos-previous).length();previous=point->pos;
    if(points.size()>1)points+=',';
    points+=jsonStr(point->name);
    }
  points+=']';
  if(measuring) {
    benchState.routineHasRoute=true;benchState.routineRouteLength=length;
    benchState.routinePathFirst=first?first->name:"";benchState.routinePathLast=last?last->name:"";
    // Game positions are centimetres; 0.8 m/s is 80 cm/s.
    benchState.routineDeadline=std::max(uint64_t(90000),uint64_t(std::ceil(1.5*double(length)/80.0*1000.0)));
    }
  if(!measuring && points==benchState.routinePathSample)return;
  benchState.routinePathSample=points;
  event("routine_return_path","\"case\":"+jsonStr(c.label)+",\"attempt\":"+std::to_string(benchState.routineRetry?2:1)+
    ",\"tick_ms\":"+std::to_string(world.tickCount())+",\"position_cm\":"+ragdollVector(npc.position())+
    ",\"deadline_sample\":"+(measuring?"true":"false")+",\"target\":"+jsonStr(target?target->name:"")+
    ",\"path\":"+points+",\"route_length_cm\":"+(target && last==target?ragdollNumber(length):"null")+
    ",\"deadline_ms\":"+std::to_string(benchState.routineDeadline));
  }

void routineReturnTick(World& world,Npc& player,PlayerControl& ctrl) {
  const auto context=routineContext();
  auto& benchState=context.routine;
  auto* npc=benchState.routineNpc;if(!npc)return;
  const auto now=world.tickCount();
  const auto& c=benchState.routineCases[benchState.routineIndex];
  if(benchState.routinePhase>=3)routineReturnObservePath(world,*npc);
  if(benchState.routinePhase==1) {
    if(now-benchState.routineSince<1200)return;
    const auto& anchor=npc->ragdollQuestReturnState();
    if(!anchor.hasAnchor || (anchor.anchor-c.start).length()>20) {
      arenaAbort("routine-return did not establish its native dry anchor");benchState.routinePhase=0;return;
      }
    npc->setRagdollDown(false);npc->prepareRagdoll();
    if(!npc->ragdollBody() || !npc->isUnconscious()) {
      arenaAbort("routine-return native KO did not create a real ragdoll");benchState.routinePhase=0;return;
      }
    npc->addRagdollImpulse(Vec3(20,0,0));
    benchState.routinePreviousGuard=npc->ragdollQuestReturnState();
    benchState.routinePhase=2;benchState.routineSince=now;benchState.routinePrevious=npc->position();
    event("routine_return_ko","\"case\":"+jsonStr(c.label)+",\"parts\":"+std::to_string(npc->ragdollBody()->partCount())+",\"hp\":"+std::to_string(npc->attribute(ATR_HITPOINTS)));
    return;
    }
  const auto position=npc->position();
  const float step=(position-benchState.routinePrevious).length();
  const auto returns=npc->ragdollQuestReturnCount();
  const auto& q=npc->ragdollQuestReturnState();
  const auto& previousGuard=benchState.routinePreviousGuard;
  const bool clamp=previousGuard.armed && previousGuard.hazards!=0 && !q.armed &&
    npc->isStanding() && (position-previousGuard.anchor).length()<3 && step>100;
  const bool guard=returns!=benchState.routineReturns || clamp;
  auto* mob=npc->interactive();
  const bool attachedMob=mob && !mob->isLadder() && !mob->isTrueDoor(*npc) && mob->isAttached(*npc);
  const bool mobAlignment=benchState.routinePhase==4 && step>100 && !guard && attachedMob && mob!=benchState.routinePreviousMob;
  if(clamp)++benchState.routineClamps;
  benchState.routinePreviousGuard=q;
  if(mobAlignment) {
    ++benchState.routineMobJumps;
    event("routine_return_mob_alignment","\"case\":"+jsonStr(c.label)+",\"attempt\":"+std::to_string(benchState.routineRetry?2:1)+
      ",\"tick_ms\":"+std::to_string(now)+",\"from_cm\":"+ragdollVector(benchState.routinePrevious)+
      ",\"to_cm\":"+ragdollVector(position)+",\"step_cm\":"+ragdollNumber(step)+",\"mob\":"+jsonStr(mob->schemeName()));
    }
  if(step>100 && !guard && !mobAlignment)++benchState.routineJumps;
  benchState.routineMaxStep=std::max(benchState.routineMaxStep,guard?0.f:step);
  if(benchState.routinePhase==4 && !guard && !mobAlignment)benchState.routineTravel+=step;
  benchState.routineReturns=returns;benchState.routinePrevious=position;
  benchState.routinePreviousMob=attachedMob?mob:nullptr;
  benchState.routineMinHero=std::min(benchState.routineMinHero,(position-player.position()).length());
  const auto floor=world.physic()->landRay(position+Vec3(0,100,0),20000);
  const auto capsule=npc->navigationCapsule();
  const auto physicsPosition=npc->navigationPhysicsPosition();
  const float capsuleBottom=physicsPosition.y+npc->navigationClearance()-capsule.y;
  const auto capsuleFloor=world.physic()->landRay(physicsPosition+Vec3(0,npc->navigationClearance()+1,0),20000);
  const bool belowRoot=floor.hasCol && position.y<floor.v.y-20.f;
  const bool belowCapsule=capsuleFloor.hasCol && capsuleBottom<capsuleFloor.v.y-20.f;
  // The streamed WorldView carries the importer's zero mesh bbox; the BSP root bounds the level.
  const bool belowBounds=capsuleBottom<world.bspBounds().first.y;
  if(benchState.routinePhase==4 && (belowCapsule || belowBounds))benchState.routineBelow=true;
  // Animation roots and feet may go below support while the physical capsule is above it.
  const auto left=npc->mapBone("BIP01 L FOOT"),right=npc->mapBone("BIP01 R FOOT");
  const auto leftFloor=world.physic()->landRay(left+Vec3(0,100,0),20000);
  const auto rightFloor=world.physic()->landRay(right+Vec3(0,100,0),20000);
  const bool belowLeft=!leftFloor.hasCol || left.y<leftFloor.v.y-5.f;
  const bool belowRight=!rightFloor.hasCol || right.y<rightFloor.v.y-5.f;
  const auto state=npc->bodyObservationState();
  const auto* ta=npc->currentTaPoint();const auto goal=npc->navigationGoal();const auto* go=npc->currentGoToPoint();
  const bool workingMob=attachedMob && mob->isStaticState();
  size_t ignored=0;const Vec3 bounds[2]={};std::string blocker;
  const auto hit=world.physic()->rayClothCamera(position+Vec3(0,100,0),
    (go?go->pos:ta?ta->pos:position)+Vec3(0,100,0),"",bounds,ignored,&blocker);
  if(guard)event("routine_return_guard","\"case\":"+jsonStr(c.label)+",\"attempt\":"+std::to_string(benchState.routineRetry?2:1)+
    ",\"from_cm\":"+ragdollVector(npc->ragdollQuestReturnFrom())+",\"to_cm\":"+ragdollVector(position)+
    ",\"hazards\":"+std::to_string(npc->ragdollQuestReturnState().hazards));
  event("routine_return_frame","\"case\":"+jsonStr(c.label)+",\"attempt\":"+std::to_string(benchState.routineRetry?2:1)+
    ",\"sample_index\":"+std::to_string(benchState.routineFrame++)+",\"tick_ms\":"+std::to_string(now)+",\"phase\":"+std::to_string(benchState.routinePhase)+
    ",\"position_cm\":"+ragdollVector(position)+",\"step_cm\":"+ragdollNumber(step)+",\"guard\":"+(guard?"true":"false")+
    ",\"mob_alignment\":"+(mobAlignment?"true":"false")+
    ",\"guard_from_cm\":"+ragdollVector(npc->ragdollQuestReturnFrom())+",\"floor_y_cm\":"+(floor.hasCol?ragdollNumber(floor.v.y):"null")+
    ",\"physics_position_cm\":"+ragdollVector(physicsPosition)+
    ",\"capsule_floor_y_cm\":"+(capsuleFloor.hasCol?ragdollNumber(capsuleFloor.v.y):"null")+
    ",\"capsule_bottom_y_cm\":"+ragdollNumber(capsuleBottom)+",\"root_below_support\":"+(belowRoot?"true":"false")+
    ",\"capsule_below_support\":"+(belowCapsule?"true":"false")+",\"below_world_bounds\":"+(belowBounds?"true":"false")+
    ",\"state\":"+jsonStr(stateName(world,*npc))+",\"started\":"+(npc->isStateStarted()?"true":"false")+
    ",\"animation\":"+jsonStr(npc->animNames())+",\"policy\":"+jsonStr(policyName(npc->processPolicy()))+
    ",\"queue\":"+std::to_string(npc->aiQueueSize())+",\"ai_wait_ms\":"+std::to_string(state.aiWaitMs)+
    ",\"ani_wait_ms\":"+std::to_string(state.aniWaitMs)+",\"output_wait_ms\":"+std::to_string(state.outputWaitMs)+
    ",\"go_to\":"+jsonStr(go?go->name:"")+",\"goal\":"+jsonStr(goal.name)+",\"goal_cm\":"+ragdollVector(goal.position)+
    ",\"target_distance_cm\":"+(ta?ragdollNumber((position-ta->pos).length()):"null")+
    ",\"blocked\":"+(hit.hasCol?"true":"false")+",\"blocker\":"+jsonStr(blocker)+",\"hit_cm\":"+ragdollVector(hit.v)+
    ",\"left_foot_cm\":"+ragdollVector(left)+",\"right_foot_cm\":"+ragdollVector(right)+
    ",\"left_floor_y_cm\":"+(leftFloor.hasCol?ragdollNumber(leftFloor.v.y):"null")+
    ",\"right_floor_y_cm\":"+(rightFloor.hasCol?ragdollNumber(rightFloor.v.y):"null")+
    ",\"left_foot_below_support\":"+(belowLeft?"true":"false")+",\"right_foot_below_support\":"+(belowRight?"true":"false")+
    ",\"working_mob\":"+(workingMob?"true":"false")+",\"mob\":"+jsonStr(mob?mob->schemeName():"")+
    ",\"external\":"+(npc->isExternalControl()?"true":"false")+",\"in_air\":"+(npc->isInAir()?"true":"false")+
    ",\"hero_distance_cm\":"+ragdollNumber((position-player.position()).length())+
    ",\"target\":"+jsonStr(ta?ta->name:"")+",\"down\":"+(npc->isDown()?"true":"false")+
    ",\"ragdoll\":"+(npc->ragdollBody()?"true":"false")+",\"blend\":"+(npc->isRagdollBlending()?"true":"false"));
  if(benchState.routinePhase==2) {
    if(npc->ragdollBody() && npc->ragdollBody()->settled()) {
      exec("standup "+c.symbol,world,player,ctrl);benchState.routinePhase=3;benchState.routineSince=now;
      }
    else if(now-benchState.routineSince>=20000)routineReturnResult(world,player,false,false);
    return;
    }
  if(benchState.routinePhase==3) {
    if(!npc->isDown() && !npc->ragdollBody() && !npc->isRagdollBlending() && npc->isStanding()) {
      const auto* nearest=world.findWayPoint(position);
      const auto distance=nearest?(position-nearest->pos).length():0;
      if(distance<1000 || distance>2000) {routineReturnResult(world,player,false,false);return;}
      benchState.routinePhase=4;benchState.routineUp=now;benchState.routineUpPosition=position;
      routineReturnObservePath(world,*npc);
      event("routine_return_up","\"case\":"+jsonStr(c.label)+",\"attempt\":"+std::to_string(benchState.routineRetry?2:1)+
        ",\"tick_ms\":"+std::to_string(now)+",\"position_cm\":"+ragdollVector(position)+",\"distance_cm\":"+ragdollNumber(distance)+
        ",\"route_length_cm\":"+(benchState.routineHasRoute?ragdollNumber(benchState.routineRouteLength):"null")+
        ",\"deadline_ms\":"+std::to_string(benchState.routineDeadline)+",\"path_first\":"+jsonStr(benchState.routinePathFirst)+
        ",\"path_last\":"+jsonStr(benchState.routinePathLast));
      }
    else if(now-benchState.routineSince>=15000)routineReturnResult(world,player,false,false);
    return;
    }
  if(benchState.routinePhase==4) {
    const auto* activity=npc->currentWayPoint();
    const bool atRoutine=npc->isStateStarted() && npc->isInRoutine(npc->currentState()) && !goal.active &&
      (workingMob || (activity && activity->isFreePoint() && (position-activity->pos).length()<=300));
    const bool arrived=now-benchState.routineUp<=benchState.routineDeadline && !npc->isDown() && ((ta && (position-ta->pos).length()<=300 && npc->isStanding()) || atRoutine);
    if(arrived || now-benchState.routineUp>=benchState.routineDeadline)routineReturnResult(world,player,arrived);
    }
  }

bool routineCommand(const RoutineContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  auto& benchState=context.routine;
  const auto& c=a[0];
  if(c=="routine-return" && a.size()==8 && a[1]=="case") {
    if(benchState.routineRetry) {arenaAbort("routine-return case after retry");return true;}
    benchState.routineCases.push_back({a[2],a[3],a[7],Vec3(num(a,4,0),num(a,5,0),num(a,6,0))});
    benchState.routineIndex=benchState.routineCases.size()-1;routineReturnBegin(w,pl);
    }
  else if(c=="routine-return" && a.size()==2 && a[1]=="retry-failures") {
    benchState.routineRetry=true;
    for(size_t i=0;i<benchState.routineCases.size();++i)if(benchState.routineCases[i].failed)benchState.routineRetries.push_back(i);
    if(benchState.routineRetries.empty())routineReturnSummary();
    else {benchState.routineIndex=benchState.routineRetries.front();routineReturnBegin(w,pl);}
    }
  else if(c=="guard" && a.size()==6 && a[1]=="setup") {
    Npc* npc=&context.subjectOf(pl);
    const bool water=a[3]=="water", enabled=a[5]=="on";
    if(npc==nullptr || (a[3]!="water" && a[3]!="cliff") || a[4]!="normal" || (a[5]!="on" && a[5]!="off")) {
      arenaAbort("guard setup expects watched actor, label cliff|water normal on|off");return true;
      }
    auto& site=water ? benchState.questWater : benchState.questCliff;
    if(!chooseQuestSite(w,water,site)) { arenaAbort("no supported dry quest-guard hazard site");return true; }
    Gothic::settingsSetI("GAME","ragdoll_quest_guard",enabled);
    auto& flags=npc->handle().flags;
    flags=daedalus::NpcFlag(uint32_t(flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
    npc->setAttitude(ATT_NEUTRAL);
    if(npc!=&pl && npc->isHuman()) {
      npc->handle().attribute[ATR_HITPOINTSMAX]=100;
      npc->handle().attribute[ATR_HITPOINTS]=2;
      for(auto& protection:npc->handle().protection) protection=0;
      }
    pl.closeWeapon(true);pl.handle().attribute[ATR_STRENGTH]=1000;
    pl.handle().damage_type=1<<size_t(daedalus::DamageType::BLUNT);
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);npc->setPosition(site.dry);npc->setDirection(site.direction);npc->updateTransform();
    auto hero=site.dry-site.direction*300;
    if(const auto floor=w.physic()->landRay(hero+Vec3(0,300,0),1500);floor.hasCol) hero.y=floor.v.y;
    if(npc!=&pl) {pl.setPosition(hero);pl.updateTransform();}
    // Keep unrelated attacks from changing the dry-anchor setup or the filmed recovery.
    for(uint32_t id=0;id<w.npcCount();++id)
      if(auto* other=w.npcById(id);other!=nullptr && other!=&pl && other!=npc &&
         (other->position()-site.dry).length()<4000)
        exec("freeze id:"+std::to_string(id),w,pl,ctrl);
    benchState.questTrace=true;benchState.questNpc=npc;benchState.questCase=a[2];benchState.questWaterCase=water;
    benchState.questSawDrop=benchState.questSawWater=benchState.questWait=benchState.questLaunched=benchState.questStandingWait=false;benchState.questLastReturns=0;benchState.questWallNs=0;
    event("immortal_guard_case","\"case\":"+jsonStr(a[2])+",\"actor_id\":"+std::to_string(w.npcId(npc))+
      ",\"immortal\":"+(npc->isImmortal()?"true":"false")+",\"enabled\":"+(enabled?"true":"false")+
      ",\"site\":"+jsonStr(site.waypoint)+",\"dry_cm\":"+ragdollVector(site.dry)+",\"direction\":"+ragdollVector(site.direction)+
      ",\"water_y_cm\":"+ragdollNumber(site.waterY));
    }
  else if(c=="guard" && (a.size()==2 || (a.size()==3 && a[2]=="safe")) && a[1]=="launch") {
    auto* npc=benchState.questNpc;if(npc==nullptr || npc->isDown()) {arenaAbort("guard launch needs living actor");return true;}
    benchState.questStable=npc->position();const auto& q=npc->ragdollQuestReturnState();
    if(npc->isHuman() && !npc->isPlayer() && Gothic::settingsGetI("GAME","ragdoll_quest_guard") && (!q.hasAnchor || (q.anchor-benchState.questStable).length()>3)) {
      arenaAbort("guard launch lacks a real stable dry anchor");return true;
      }
    event("immortal_guard_launch","\"case\":"+jsonStr(benchState.questCase)+",\"stable_cm\":"+ragdollVector(benchState.questStable)+
      ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"immortal\":"+(npc->isImmortal()?"true":"false"));
    benchState.questLaunched=true;
    const auto before=npc->attribute(ATR_HITPOINTS);
    npc->takeDamage(pl,nullptr);npc->prepareRagdoll();
    event("immortal_guard_damage","\"case\":"+jsonStr(benchState.questCase)+",\"hp_before\":"+std::to_string(before)+
      ",\"hp_after\":"+std::to_string(npc->attribute(ATR_HITPOINTS))+",\"unconscious\":"+(npc->isUnconscious()?"true":"false")+
      ",\"dead\":"+(npc->isDead()?"true":"false")+",\"melee\":true");
    if(before!=2 || npc->attribute(ATR_HITPOINTS)!=1 || !npc->isUnconscious() || npc->isDead() || !npc->ragdollBody()) {arenaAbort("real melee damage did not produce HP2 to HP1 native KO");return true;}
    const auto direction=(benchState.questWaterCase?benchState.questWater:benchState.questCliff).direction;
    if(a.size()==2) npc->addRagdollImpulse(direction*1000.f+Vec3(0,50,0));
    }
  else if(c=="guard" && a.size()==2 && a[1]=="excluded") {
    const auto* npc=benchState.questNpc;
    if(npc==nullptr || (!npc->isPlayer() && npc->isHuman())) {arenaAbort("guard exclusion needs hero or non-human");return true;}
    const auto& q=npc->ragdollQuestReturnState();
    event("immortal_guard_excluded","\"case\":"+jsonStr(benchState.questCase)+",\"human\":"+(npc->isHuman()?"true":"false")+
      ",\"hero\":"+(npc->isPlayer()?"true":"false")+",\"monster\":"+(npc->isMonster()?"true":"false")+
      ",\"has_anchor\":"+(q.hasAnchor?"true":"false")+",\"armed\":"+(q.armed?"true":"false")+
      ",\"returns\":"+std::to_string(npc->ragdollQuestReturnCount()));
    if(q.hasAnchor || q.armed || npc->ragdollQuestReturnCount()!=0) arenaAbort("excluded actor acquired return protection");
    }
  else if(c=="guard" && a.size()==2 && a[1]=="hazard") {
    benchState.questWait=true;benchState.questDeadline=context.time+10000;
    }
  else if(c=="guard" && a.size()==3 && a[1]=="checkpoint") {
    const auto* npc=benchState.questNpc;if(npc==nullptr) {arenaAbort("guard checkpoint lost actor");return true;}
    const auto& q=npc->ragdollQuestReturnState();
    event("immortal_guard_checkpoint","\"case\":"+jsonStr(benchState.questCase)+",\"label\":"+jsonStr(a[2])+
      ",\"anchor_cm\":"+ragdollVector(q.anchor)+",\"angle\":"+ragdollNumber(q.angle)+
      ",\"has_anchor\":"+(q.hasAnchor?"true":"false")+",\"armed\":"+(q.armed?"true":"false")+",\"hazards\":"+std::to_string(q.hazards));
    benchState.questLastReturns=npc->ragdollQuestReturnCount();
    }
  else if(c=="guard" && a.size()==3 && a[1]=="wait-standing") {
    if(benchState.questNpc==nullptr) {arenaAbort("guard standing wait lost actor");return true;}
    // Let the actual get-up/draw animation finish while preventing a new combat action.
    benchState.questNpc->setExternalControl(true);
    benchState.questStandingWait=true;benchState.questDeadline=context.time+uint64_t(num(a,2,5000));
    }
  else if(c=="guard" && a.size()==2 && a[1]=="finish") {
    const auto* npc=benchState.questNpc;
    if(npc==nullptr) {arenaAbort("guard finish lost actor");return true;}
    event("immortal_guard_end","\"case\":"+jsonStr(benchState.questCase)+",\"position_cm\":"+ragdollVector(npc->position())+
      ",\"stable_cm\":"+ragdollVector(benchState.questStable)+",\"returns\":"+std::to_string(npc->ragdollQuestReturnCount())+
      ",\"drop_observed\":"+(benchState.questSawDrop?"true":"false")+",\"water_observed\":"+(benchState.questSawWater?"true":"false")+
      ",\"dead\":"+(npc->isDead()?"true":"false")+",\"unconscious\":"+(npc->isUnconscious()?"true":"false")+
      ",\"standing\":"+(npc->isStanding()?"true":"false")+",\"armed\":"+(npc->ragdollQuestReturnState().armed?"true":"false")+
      ",\"parts\":"+std::to_string(npc->ragdollBody()?npc->ragdollBody()->partCount():0));
    }
  else if(c=="guard" && a.size()==2 && a[1]=="cleanup") {
    auto* npc=benchState.questNpc;
    if(npc==nullptr) {arenaAbort("guard cleanup lost actor");return true;}
    benchState.questNpc=nullptr;
    if(context.subject==npc) exec("watch player",w,pl,ctrl);
    if(npc!=&pl) w.removeNpc(*npc); else npc->setExternalControl(false);
    }
  else return false;
  return true;
  }

}
