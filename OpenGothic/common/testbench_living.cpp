#include "testbench_internal.h"
#include "testbench.h"
#include "appcadence.h"

#include <algorithm>
#include <cmath>

#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "gothic.h"
#include "graphics/mesh/hitzones.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/objects/interactive.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

LivingCounts livingCounts() {
  const auto context=livingContext();
  LivingCounts result;
  for(const auto* npc:context.crew) {
    if(TestBench::ragdollBodyDrawable(*npc)) ++result.models;
    if(!npc->isDown()) ++result.alive;
    const auto* living=npc->livingBody();const auto* dynamic=npc->ragdollBody();
    if(living) {if(living->isKinematic()) ++result.kinematic;result.parts+=living->partCount();}
    if(dynamic) {++result.dynamic;result.dynamicParts+=dynamic->partCount();}
    if(living && dynamic) ++result.duplicates;
    }
  return result;
  }

std::string livingCountFields(const LivingCounts& s, World& world) {
  return "\"models\":"+std::to_string(s.models)+",\"alive\":"+std::to_string(s.alive)+
    ",\"kinematic\":"+std::to_string(s.kinematic)+",\"parts\":"+std::to_string(s.parts)+
    ",\"dynamic\":"+std::to_string(s.dynamic)+",\"dynamic_parts\":"+std::to_string(s.dynamicParts)+
    ",\"duplicates\":"+std::to_string(s.duplicates)+",\"world_bodies\":"+std::to_string(world.physic()->physicsStats().bodies);
  }

void livingFields(World& world) {
  const auto context=livingContext();
  auto& state=context.living;
  const auto s=livingCounts();const auto stats=world.physic()->physicsStats();
  std::fprintf(context.csv,",%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.6f",s.models,s.alive,s.kinematic,s.parts,
    s.dynamic,s.dynamicParts,s.duplicates,stats.bodies,double(stats.stepMilliseconds));
  std::string actors="[";
  for(size_t i=0;i<context.crew.size();++i) {
    const auto* npc=context.crew[i];if(i) actors+=',';
    actors+="{\"index\":"+std::to_string(i)+",\"position_cm\":"+ragdollVector(npc->position())+
      ",\"animations\":"+jsonStr(npc->animNames());
    if(const auto* body=npc->livingBody()) {
      const auto pelvis=body->pelvisTransform();
      actors+=",\"native_pelvis_cm\":"+ragdollVector({pelvis.at(3,0),pelvis.at(3,1),pelvis.at(3,2)});
      }
    actors+='}';
    }
  event("living_frame",livingCountFields(s,world)+",\"phase\":"+jsonStr(state.livingPhase)+",\"actors\":"+actors+"]");
  }

void livingSeatFields(World& world) {
  const auto context=livingContext();
  auto& state=context.living;
  auto* npc=state.seatNpc;
  if(npc==nullptr || state.seatPhase.empty() || state.seatSampleTick==world.tickCount()) return;
  const float seconds=float(world.tickCount()-state.seatSampleTick)*.001f;
  state.seatSampleTick=world.tickCount();
  std::vector<Matrix4x4> pose;
  const auto* body=npc->livingBody();
  float nativeStep=0,corpseSpeed=0,itemSpeed=0;
  if(body && body->getPose(pose)) {
    if(state.seatPreviousParts.size()==pose.size())
      for(auto bone:body->boneIndices()) nativeStep=std::max(nativeStep,(Vec3(pose[bone].at(3,0),pose[bone].at(3,1),pose[bone].at(3,2))-state.seatPreviousParts[bone]).length());
    state.seatPreviousParts.resize(pose.size());
    for(auto bone:body->boneIndices()) state.seatPreviousParts[bone]={pose[bone].at(3,0),pose[bone].at(3,1),pose[bone].at(3,2)};
    }
  else {
    state.seatPreviousParts.clear();
    }
  const auto* corpse=state.seatCorpse ? state.seatCorpse->ragdollBody() : nullptr;
  if(corpse) for(const auto& part:corpse->state().parts) corpseSpeed=std::max(corpseSpeed,part.linearVelocity.length());
  std::string items="[";
  for(size_t i=0;i<state.seatItems.size();++i) {
    const auto p=state.seatItems[i]->position();
    itemSpeed=std::max(itemSpeed,state.seatItems[i]->physicsVelocity().length());
    if(state.seatPreviousItems.size()==state.seatItems.size() && seconds>0)
      itemSpeed=std::max(itemSpeed,(p-state.seatPreviousItems[i]).length()/seconds);
    if(i) items+=',';
    items+=ragdollVector(p);
    }
  state.seatPreviousItems.clear();
  for(const auto* item:state.seatItems) state.seatPreviousItems.push_back(item->position());
  const auto* mob=npc->interactive();
  event("living_seat_frame","\"phase\":"+jsonStr(state.seatPhase)+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"attached\":"+(mob==state.seatMob?"true":"false")+",\"seated\":"+(mob==state.seatMob && npc->bodyStateMasked()==BS_SIT?"true":"false")+
    ",\"mob_state\":"+std::to_string(mob?mob->stateId():-99)+",\"animations\":"+jsonStr(npc->animNames())+
    ",\"living_parts\":"+std::to_string(body?body->partCount():0)+",\"corpse_parts\":"+std::to_string(corpse?corpse->partCount():0)+
    ",\"native_step_cm\":"+ragdollNumber(nativeStep)+",\"corpse_speed_cm_s\":"+ragdollNumber(corpseSpeed)+
    ",\"item_speed_cm_s\":"+ragdollNumber(itemSpeed)+",\"items_cm\":"+items+"]");
  }

bool livingCommand(const LivingContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  auto& state=context.living;
  const auto& c=a[0];
  if(c=="living-seat" && a.size()==2 && a[1]=="setup") {
    Interactive* mob=w.availableMob(pl,"BENCH");
    if(!mob) mob=w.availableMob(pl,"CHAIR");
    if(!mob) {arenaAbort("sitting fixture needs an available real BENCH or CHAIR nearby");return true;}
    state.seatMob=mob;
    auto stand=mob->nearestPoint(pl)+Vec3(0,0,-80);
    const auto ground=w.physic()->landRay(stand+Vec3(0,200,0),500);
    if(!ground.hasCol) {arenaAbort("sitting fixture has no supported floor");return true;}
    stand.y=ground.v.y;
    pl.setPosition(stand-Vec3(0,0,350));pl.updateTransform();
    exec("clear 600",w,pl,ctrl);
    const auto symbol=w.script().findSymbolIndex("BDT_1013_BANDIT_L");
    auto* npc=w.addNpc(symbol,stand);
    auto* corpse=w.addNpc(symbol,stand+Vec3(100,0,60));
    if(!npc || !corpse) {arenaAbort("sitting fixture actors unavailable");return true;}
    for(auto* actor:{npc,corpse}) {actor->clearAiQueue();actor->clearState(false);actor->setExternalControl(true);}
    npc->setDirection(mob->position()-stand);npc->updateTransform();
    corpse->setLastHitBy(&pl);corpse->setRagdollDown(true);corpse->prepareRagdoll();
    state.seatNpc=npc;state.seatCorpse=corpse;context.subject=npc;
    for(const float x:{-35.f,35.f}) {
      auto matrix=Matrix4x4::mkIdentity();matrix.translate(stand+Vec3(x,25,35));
      auto* item=w.addItemDyn(w.script().findSymbolIndex("ItPo_Health_01"),matrix,size_t(-1));
      if(!item) {arenaAbort("sitting fixture dropped item unavailable");return true;}
      state.seatItems.push_back(item);
      }
    Gothic::settingsSetI("GAME","living_bodies",1);
    event("living_seat_setup","\"scheme\":"+jsonStr(mob->schemeName())+",\"mob_cm\":"+ragdollVector(mob->position())+
      ",\"start_cm\":"+ragdollVector(stand)+",\"items\":2");
    }
  else if(c=="living-seat" && a.size()==2 && a[1]=="sit") {
    if(!state.seatNpc || !state.seatNpc->livingBody()) {arenaAbort("sitting fixture requires its existing native living rig");return true;}
    for(auto* item:state.seatItems) item->setPhysicsEnable(w);
    state.seatPhase="sit";state.seatPreviousItems.clear();state.seatSampleTick=w.tickCount();
    for(const auto* item:state.seatItems) state.seatPreviousItems.push_back(item->position());
    std::vector<Matrix4x4> native;
    if(state.seatNpc->livingBody()->getPose(native)) {
      state.seatPreviousParts.resize(native.size());
      for(auto bone:state.seatNpc->livingBody()->boneIndices()) state.seatPreviousParts[bone]={native[bone].at(3,0),native[bone].at(3,1),native[bone].at(3,2)};
      }
    const auto from=state.seatNpc->position();
    const bool attached=state.seatNpc->setInteraction(state.seatMob);
    event("living_seat_attach",std::string("\"attached\":")+(attached?"true":"false")+",\"jump_cm\":"+
      ragdollNumber((state.seatNpc->position()-from).length()));
    if(!attached) arenaAbort("real NPC bench attachment failed");
    }
  else if(c=="living-seat" && a.size()==2 && a[1]=="stand") {
    if(!state.seatNpc) return true;
    state.seatNpc->setInteraction(nullptr,false);
    state.seatPhase="stand";
    }
  else if(c=="living-seat" && a.size()==2 && a[1]=="shift") {
    if(!state.seatNpc) return true;
    state.seatNpc->setInteraction(nullptr,true);
    state.seatNpc->setPosition(state.seatNpc->position()+Vec3(80,0,0));state.seatNpc->updateTransform();
    state.seatPhase="shift";
    event("living_seat_shift","\"distance_cm\":80");
    }
  else if(c=="hero-mob" && a.size()==2) {
    // the hero takes the nearest free mob of a scheme (BENCH, CHAIR, CAULDRON, BSANVIL, LAB, ...) as when the player
    // uses it - the render rig at mobs (hero-rig-audit.bench); leave: it gets up, lets go
    if(a[1]=="leave") {
      pl.setInteraction(nullptr,false);
      return true;
      }
    Interactive* mob=w.availableMob(pl,a[1]);
    if(mob!=nullptr) {
      // the player is let on a mob only at its point and with its item (a scoop, a flask)
      if(!mob->useItem().empty()) {
        const size_t item=w.script().findSymbolIndex(mob->useItem());
        if(item!=size_t(-1) && pl.itemCount(item)==0) pl.addItem(item,1);
        }
      pl.setPosition(mob->nearestPoint(pl));pl.updateTransform();
      }
    const bool ok=mob!=nullptr && pl.setInteraction(mob);
    event("hero_mob","\"scheme\":"+jsonStr(a[1])+",\"found\":"+(mob?"true":"false")+",\"used\":"+(ok?"true":"false"));
    }
  else if(c=="living" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","living_bodies",a[1]=="on");state.livingPhase=a[1];
    }
  else if(c=="living" && a.size()==2 && a[1]=="radius-check") {
    auto* npc=context.subject;
    if(!npc || npc->isDown()) {arenaAbort("living radius needs a live watched actor");return true;}
    const auto hero=pl.position();
    const auto offset=pl.centerPosition()-pl.position();
    const float distances[]{1950,2010,2190,2210,2100,2010,1990,2010};
    const bool expected[]{true,true,true,false,false,false,true,true};
    bool pass=true;
    for(size_t i=0;i<8;++i) {
      pl.setPosition(npc->centerPosition()+Vec3(distances[i],0,0)-offset);pl.updateTransform();
      npc->prepareLivingBody();
      const bool present=npc->livingBody()!=nullptr;
      pass&=present==expected[i];
      event("living_radius_check","\"distance_cm\":"+ragdollNumber((pl.centerPosition()-npc->centerPosition()).length())+
        ",\"expected\":"+(expected[i]?"true":"false")+",\"present\":"+(present?"true":"false"));
      }
    pl.setPosition(hero);pl.updateTransform();npc->prepareLivingBody();
    if(!pass) arenaAbort("living radius hysteresis check failed");
    }
  else if(c=="living" && a.size()==2 && a[1]=="setup") {
    if(!chooseRagdollArena(w,pl)) {arenaAbort("living fixture needs an empty supported arena");return true;}
    exec("clear 2500",w,pl,ctrl);
    const auto symbol=w.script().findSymbolIndex("BDT_1013_BANDIT_L");
    for(size_t i=0;i<10;++i) {
      auto* npc=w.addNpc(symbol,context.ragdollArenaPlaces[i+5]);
      if(!npc) {arenaAbort("living ten-NPC insertion failed");return true;}
      npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
      npc->setDirection(pl.position()-npc->position());npc->updateTransform();context.crew.push_back(npc);
      if(i<4) {
        state.livingWalkers.emplace_back();auto& walker=state.livingWalkers.back();walker.npc=npc;
        walker.ends[0]=WayPoint(npc->position()-Vec3(0,0,300),"FP_LIVING_BENCH_A");
        walker.ends[1]=WayPoint(npc->position(),"FP_LIVING_BENCH_B");
        for(auto& point:walker.ends) {
          const auto floor=w.physic()->landRay(point.pos+Vec3(0,200,0),500);
          if(!floor.hasCol) {arenaAbort("living walking path has no floor");return true;}
          point.pos.y=floor.v.y;point.groundPos=point.pos;
          }
        npc->setWalkMode(WalkBit::WM_Walk);
        }
      }
    state.livingFixture=true;state.livingHero=pl.position();context.subject=context.crew.back();
    exec("watch watched",w,pl,ctrl);
    if(!context.frameRagdollArena(10)) {arenaAbort("living ten-model camera failed");return true;}
    event("living_setup","\"actors\":10,\"walkers\":4,\"hero_cm\":"+ragdollVector(state.livingHero));
    }
  else if(c=="living" && a.size()==2 && a[1]=="motion") state.livingMotion=true;
  else if(c=="living" && a.size()==3 && a[1]=="debug") state.livingDebug=a[2]=="on";
  else if(c=="living" && a.size()==2 && (a[1]=="far" || a[1]=="near")) {
    pl.setPosition(state.livingHero+(a[1]=="far" ? Vec3(0,0,-6000) : Vec3()));pl.updateTransform();
    state.livingPhase=a[1]=="far" ? "far" : "on";
    }
  else if(c=="living" && a.size()==2 && a[1]=="ko") {
    auto* npc=context.subject;if(!npc || npc->isDown()) {arenaAbort("living handoff needs a live watched NPC");return true;}
    npc->setLastHitBy(&pl);npc->setRagdollDown(false);npc->prepareRagdoll();state.livingPhase="ko";
    }
  else if(c=="living" && a.size()==3 && a[1]=="check") {
    const auto counts=livingCounts();const bool ko=a[2]=="ko",on=a[2]=="on" || ko;
    size_t hits=0;
    for(const auto* npc:context.crew) if(!npc->isDown()) {
      const auto head=npc->mapHeadBone();float fraction=1;
      if(npc->rayHitZone(head+Vec3(100,0,0),head-Vec3(100,0,0),fraction)!=HitZones::None) ++hits;
      }
    const bool pass=counts.models==10 && counts.alive==(ko?9u:10u) && counts.duplicates==0 &&
      counts.kinematic==(on?(ko?9u:10u):0u) && counts.parts==(on?(ko?216u:240u):0u) &&
      counts.dynamic==(ko?1u:0u) && counts.dynamicParts==(ko?24u:0u) && hits==counts.alive;
    event("living_check","\"mode\":"+jsonStr(a[2])+",\"pass\":"+(pass?"true":"false")+
      ",\"ordinary_hits\":"+std::to_string(hits)+","+livingCountFields(counts,w));
    state.livingPhase=a[2];
    if(!pass) arenaAbort("living body/count/hit checkpoint failed");
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="living" && a.size()==3 && a[1]=="measure") {
    const auto counts=livingCounts();state.livingMode=a[2]=="on";
    if(!state.livingFixture || counts.models!=10 || counts.alive!=10 ||
       counts.parts!=(state.livingMode?240u:0u) || !AppCadence::normalClock()) {arenaAbort("living measurement precondition failed");return true;}
    state.livingPhysics.reserve(20000);state.livingPhysics.clear();state.livingOverflow=false;
    AppCadence::counts(true,10,0,uint32_t(counts.parts),0,0);
    if(!AppCadence::arm(state.livingMode)) {arenaAbort("living cadence arm failed");return true;}
    state.livingMeasuring=true;
    event("living_measure_before",livingCountFields(counts,w));
    }
  else if(c=="living" && a.size()==2 && a[1]=="report") {
    state.livingMeasuring=false;const auto counts=livingCounts();
    AppCadence::counts(false,uint32_t(counts.models),0,uint32_t(counts.parts),0,0);
    event("living_measure_after",livingCountFields(counts,w)+",\"overflow\":"+(state.livingOverflow?"true":"false"));
    if(auto* file=std::fopen((outDir+"/living-physics.csv").c_str(),"w")) {
      std::fprintf(file,"sample,t_game_ms,dt_ms,step_ms,native_bodies\n");
      for(size_t i=0;i<state.livingPhysics.size();++i) {
        const auto& sample=state.livingPhysics[i];std::fprintf(file,"%zu,%llu,%llu,%.9g,%zu\n",i,
          (unsigned long long)sample.time,(unsigned long long)sample.dt,double(sample.milliseconds),sample.bodies);
        }
      std::fclose(file);
      }
    }
#endif
  else return false;
  return true;
  }

}
