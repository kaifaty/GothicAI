#include "testbench_internal.h"
#include "testbench.h"
#include "appcadence.h"

#include <Tempest/MemWriter>
#include <Tempest/Pixmap>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "game/serialize.h"
#include "gothic.h"
#include "graphics/mesh/hitzones.h"
#include "graphics/mesh/protomesh.h"
#include "world/aiqueue.h"
#include "world/bullet.h"
#include "world/movablevobs.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

void prepareSpellFixture(World& world,Npc& player,std::string_view fixture) {
  const auto context=movablesContext();
  auto& state=context.movables;
  state.spellTrace=false;state.spellReleaseAt=0;state.spellWaitFrozen=-1;state.spellWasFrozen=false;state.spellEquipPending=0;
  player.setTarget(nullptr);player.closeWeapon(true);
  const auto before=world.physic()->physicsStats().bodies;
  size_t expected=0;
  for(const auto& actor:state.spellActors) expected+=(actor.npc->livingBody()?actor.npc->livingBody()->partCount():0)+(actor.npc->ragdollBody()?actor.npc->ragdollBody()->partCount():0);
  for(const auto* item:state.spellItems) expected+=size_t(item->isDynamic());
  for(auto& actor:state.spellActors) world.removeNpc(*actor.npc);
  for(auto* item:state.spellItems) world.removeItem(*item);
  if(expected) event("spell_physics_cleanup","\"before\":"+std::to_string(before)+",\"after\":"+
    std::to_string(world.physic()->physicsStats().bodies)+",\"expected_parts\":"+std::to_string(expected));
  state.spellActors.clear();state.spellItems.clear();context.crew.clear();context.subject=nullptr;
  if(!state.spellArenaReady) {
    if(!chooseRagdollArena(world,player)) {arenaAbort("spell fixture needs supported open arena");return;}
    state.spellArenaReady=true;
    std::vector<Npc*> nearby;
    for(uint32_t i=0;i<world.npcCount();++i)
      if(auto* npc=world.npcById(i);npc && npc!=&player && (npc->position()-player.position()).length()<2500) nearby.push_back(npc);
    for(auto* npc:nearby) world.removeNpc(*npc);
    }
  const auto center=context.ragdollArenaTarget-Vec3(0,80,300);
  auto hero=center-Vec3(0,0,700);
  if(const auto floor=world.physic()->landRay(hero+Vec3(0,300,0),1200);floor.hasCol) hero.y=floor.v.y;
  player.setPosition(hero);player.setDirection({0,0,1});player.updateTransform();
  const auto add=[&](std::string_view symbol,std::string role,Vec3 point,bool dead=false) {
    if(const auto floor=world.physic()->landRay(point+Vec3(0,300,0),1200);floor.hasCol) point.y=floor.v.y;
    auto* npc=world.addNpc(world.script().findSymbolIndex(symbol),point);
    if(!npc) {arenaAbort("spell actor insertion failed");return;}
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);npc->setDirection(hero-point);npc->updateTransform();
    npc->handle().attribute[ATR_HITPOINTS]=50000;npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
    if(dead) {npc->setRagdollDown(true);npc->prepareRagdoll();}
    state.spellActors.push_back({npc,std::move(role)});context.crew.push_back(npc);
    };
  add("BAU_904_BAUER","human",center);
  if(fixture=="wind" || fixture=="field")
    add("BDT_1013_BANDIT_L","companion",center+Vec3(-180,0,0));
  if(fixture=="wind") {
    add("TROLL","troll",center+Vec3(450,0,0));
    add("BAU_904_BAUER","behind",hero-Vec3(0,0,300));
    add("BAU_904_BAUER","outside_cone",hero+Vec3(550,0,0));
    }
  else if(fixture!="ice") {
    add("BAU_904_BAUER","corpse",center+Vec3(120,0,40),true);
    for(size_t i=0;i<3;++i) {
      auto at=center+Vec3(float(i)*110.f-110.f,0,-100);
      if(const auto floor=world.physic()->landRay(at+Vec3(0,300,0),900);floor.hasCol) at.y=floor.v.y;
      const auto symbol=world.script().findSymbolIndex(i==0?"ITMI_LUTE":(i==1?"ITMI_PAN":"ITMW_1H_MACE_L_01"));
      if(symbol==size_t(-1)) {arenaAbort("spell loose prop instance missing");return;}
      auto* item=world.addItem(symbol,at);
      if(item) {
        if(const auto* bounds=item->bBox()) item->setPosition(at.x,at.y-bounds[0].y+1.f,at.z);
        state.spellItems.push_back(item);
        }
      }
    }
  if(fixture=="ice" || fixture=="airborne") state.spellActors[0].npc->setExternalControl(false);
  context.subject=state.spellActors[0].npc;
  if(state.spellRunes.empty()) {
    for(size_t i=0;i<world.script().symbolsCount();++i) {
      const auto* symbol=world.script().findSymbol(i);
      if(!symbol || symbol->type()!=daedalus::DaedalusDataType::INSTANCE) continue;
      auto name=symbol->name();for(auto& c:name) c=char(std::toupper(static_cast<unsigned char>(c)));
      if(!name.starts_with("ITRU_") && !name.starts_with("ITSC_")) continue;
      auto candidate=std::make_unique<Item>(world,i,Item::T_Inventory);
      const auto id=candidate->spellId();
      if((id!=23 && id!=26 && id!=30 && id!=33 && id!=71) || state.spellRunes.contains(id)) continue;
      state.spellRunes[id]=i;player.addItem(i,candidate->isRune()?1:5);
      event("spell_physics_rune","\"id\":"+std::to_string(id)+",\"symbol\":"+jsonStr(name)+
        ",\"class_id\":"+std::to_string(i)+",\"projectile\":"+(candidate->isSpellShoot()?"true":"false"));
      }
    }
  player.handle().attribute[ATR_MANA]=50000;player.handle().attribute[ATR_MANAMAX]=50000;player.setTalentSkill(TALENT_MAGE,6);
  const auto target=center+Vec3(0,90,0);
  context.cam=C_Hold;lookFrom(target+Vec3(600,280,-300),target);
  event("spell_physics_setup","\"fixture\":"+jsonStr(fixture)+",\"default_enabled\":"+
    std::string(Gothic::settingsGetI("GAME","spell_physics")?"true":"false"));
  }

Vec3 movablePosition(const Matrix4x4& matrix) {
  return {matrix.at(3,0),matrix.at(3,1),matrix.at(3,2)};
  }

std::string movableRows(World& world,bool allMap) {
  auto& state=movablesContext().movables;
  std::string result="[";
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(allMap ? body.zenId==uint32_t(-1) :
       std::find(state.movableIds.begin(),state.movableIds.end(),body.id)==state.movableIds.end()) continue;
    if(result.size()>1) result+=',';
    result+="{\"id\":"+std::to_string(body.id)+",\"visual\":"+jsonStr(body.visual)+
      ",\"zen_id\":"+std::to_string(body.zenId)+",\"origin\":"+ragdollMatrix(body.origin)+
      ",\"class\":"+jsonStr(body.className)+",\"blocks_walking\":"+(body.blocksWalking?"true":"false")+
      ",\"legacy_collision\":"+(body.legacyCollision?"true":"false")+
      ",\"from_map\":"+((allMap || state.movableMapIds.contains(body.id))?"true":"false")+
      ",\"mass_kg\":"+ragdollNumber(body.mass)+",\"enabled\":"+(body.enabled?"true":"false")+
      ",\"sleeping\":"+(body.sleeping?"true":"false")+",\"npc_contact\":"+(body.touchedByNpc?"true":"false")+
      ",\"position_cm\":"+ragdollVector(movablePosition(body.transform))+
      ",\"transform\":"+ragdollMatrix(body.transform)+",\"velocity_cm_s\":"+ragdollVector(body.velocity)+
      ",\"max_displacement_cm\":"+ragdollNumber(state.movableTravel[body.id])+"}";
    }
  return result+"]";
  }

void movableBegin(World& world,std::string phase) {
  auto& state=movablesContext().movables;
  state.movableReference.clear();state.movableTravel.clear();state.movableNpcTouched.clear();
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(std::find(state.movableIds.begin(),state.movableIds.end(),body.id)!=state.movableIds.end()) {
      state.movableReference[body.id]=movablePosition(body.transform);
      state.movableTravel[body.id]=0;
      }
  state.movablePhase=std::move(phase);state.movablePhaseStart=world.tickCount();state.movableTrace=true;
  event("movable_begin","\"phase\":"+jsonStr(state.movablePhase)+",\"t_game_ms\":"+
    std::to_string(world.tickCount())+",\"bodies\":"+movableRows(world));
  }

void movableFields(World& world) {
  auto& state=movablesContext().movables;
  if(state.movableWakeTrace)
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(body.enabled)
        state.movableWakeTravel[body.id]=std::max(state.movableWakeTravel[body.id],
          (movablePosition(body.transform)-movablePosition(body.origin)).length());
  if(!state.movableTrace || state.movableMeasuring || state.movableSampleTick==world.tickCount()) return;
  state.movableSampleTick=world.tickCount();
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    const auto initial=state.movableReference.find(body.id);
    if(initial==state.movableReference.end()) continue;
    state.movableTravel[body.id]=std::max(state.movableTravel[body.id],(movablePosition(body.transform)-initial->second).length());
    if(body.touchedByNpc) state.movableNpcTouched.insert(body.id);
    }
  // Every native step is observed, but large JSON is emitted only at checkpoints.
  }

void movableCheckpoint(World& world) {
  auto& state=movablesContext().movables;
  movableFields(world);
  std::string touched="[";
  for(const auto id:state.movableNpcTouched) {if(touched.size()>1)touched+=',';touched+=std::to_string(id);}
  event("movable_checkpoint","\"phase\":"+jsonStr(state.movablePhase)+",\"elapsed_ms\":"+
    std::to_string(world.tickCount()-state.movablePhaseStart)+",\"npc_touched_ids\":"+touched+
    "],\"bodies\":"+movableRows(world)+",\"walker_cm\":"+
    (state.movableWalker?ragdollVector(state.movableWalker->position()):"null"));
  state.movableTrace=false;
  }

void prepareMovableFixture(World& world,Npc& player) {
  const auto context=movablesContext();
  auto& state=context.movables;
  if(state.movableReady) {arenaAbort("movable tavern fixture already exists");return;}
  Npc* host=nullptr;
  for(uint32_t i=0;i<world.npcCount();++i) {
    auto* npc=world.npcById(i);if(!npc)continue;
    auto name=symName(world,npc);
    for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
    if(name.find("CORAGON")!=std::string::npos || name.find("KORAGON")!=std::string::npos) {host=npc;break;}
    }
  if(!host) {arenaAbort("movable fixture requires Coragon in the loaded Khorinis world");return;}
  state.noiseHost=host;
  const auto all=world.physic()->movableVobSnapshot();
  // Use the same authored geometry subset in ON and OFF fixtures. Rejected
  // map props remain in the real world, outside the dynamic test catalog.
  std::unordered_set<uint32_t> allowedMapVobs;
  for(const auto& body:world.physic()->movableVobContacts(1.f))
    if(body.disposition=="ok" || body.disposition=="item")allowedMapVobs.insert(body.zenId);
  std::map<std::string,size_t> noncollidingClasses,mapClasses;
  for(const auto& body:all)
    if(!body.legacyCollision)++noncollidingClasses[body.className];
  const auto classCounts=[](const std::map<std::string,size_t>& counts) {
    std::string result="{";
    for(const auto& [name,count]:counts) {
      if(result.size()>1)result+=',';
      result+=jsonStr(name)+":"+std::to_string(count);
      }
    return result+"}";
    };
  uintptr_t tableId=0;std::string tableVisual,utensilVisual="ITFO_BEER.3DS",barrelVisual="NW_HARBOUR_BARREL_01.3DS";
  if(!Resources::loadMesh(utensilVisual))utensilVisual.clear();
  if(!Resources::loadMesh(barrelVisual))barrelVisual.clear();
  Matrix4x4 tableTransform;float nearest=1400.f*1400.f;
  for(const auto& body:all) {
    if(!allowedMapVobs.contains(body.zenId))continue;
    if(body.className=="utensil" && utensilVisual.empty()) utensilVisual=body.visual;
    if(body.className=="barrel" && barrelVisual.empty()) barrelVisual=body.visual;
    if(body.className!="table")continue;
    const auto distance=(movablePosition(body.transform)-host->position()).quadLength();
    if(distance<nearest) {nearest=distance;tableId=body.id;tableVisual=body.visual;tableTransform=body.transform;}
    }
  if(!tableId || utensilVisual.empty()) {arenaAbort("Coragon tavern needs a real table and an available utensil mesh");return;}
  const auto* tableMesh=Resources::loadMesh(tableVisual);
  const auto* dishMesh=Resources::loadMesh(utensilVisual);
  if(!tableMesh || !dishMesh) {arenaAbort("movable fixture real mesh unavailable");return;}
  const auto* tableBounds=tableMesh->bbox();const auto* dishBounds=dishMesh->bbox();
  auto tableCenter=(tableBounds[0]+tableBounds[1])*.5f;tableTransform.project(tableCenter);
  auto tableTop=tableCenter;tableTop.y=-std::numeric_limits<float>::infinity();
  for(unsigned i=0;i<8;++i) {
    Vec3 point((i&1)?tableBounds[1].x:tableBounds[0].x,(i&2)?tableBounds[1].y:tableBounds[0].y,(i&4)?tableBounds[1].z:tableBounds[0].z);
    tableTransform.project(point);tableTop.y=std::max(tableTop.y,point.y);
    }
  state.movableTable=tableId;state.movableCenter=tableCenter;
  for(const auto& body:all) {
    if(!allowedMapVobs.contains(body.zenId))continue;
    const auto delta=movablePosition(body.transform)-tableCenter;
    if(delta.x*delta.x+delta.z*delta.z<900.f*900.f && std::abs(delta.y)<350) {
      state.movableIds.push_back(body.id);state.movableMapIds.insert(body.id);++mapClasses[body.className];
      }
    }
  const auto add=[&](const std::string& visual,const Vec3& support) {
    const auto* mesh=Resources::loadMesh(visual);if(!mesh)return uintptr_t(0);
    const auto* bounds=mesh->bbox();
    asset::VirtualObject data;data.type=asset::VirtualObjectType::zCVob;data.id=uint32_t(-1);
    data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
    data.position={support.x-(bounds[0].x+bounds[1].x)*.5f,support.y-bounds[0].y+.2f,support.z-(bounds[0].z+bounds[1].z)*.5f};
    data.visual=std::make_shared<asset::VisualMesh>();data.visual->type=asset::VisualType::MESH;data.visual->name=visual;
    const auto before=world.physic()->movableVobSnapshot();
    auto fixture=std::make_unique<MovableFixture>();
    fixture->visual.setVisual(data,world,false,false);
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(data.position.x,data.position.y,data.position.z);
    fixture->visual.setObjMatrix(matrix);
    const auto* rule=MovableVobs::classifyVisual(visual);
    const bool testMug=visual==utensilVisual;
    if(!testMug && rule==nullptr)return uintptr_t(0);
    fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,testMug?.12f:rule->massMultiplier,
                                              testMug?false:rule->blocksWalking,testMug?"utensil":rule->className,true);
    if(fixture->body.isEmpty())return uintptr_t(0);
    fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& matrix) {view->setObjMatrix(matrix);});
    state.movableFixtures.emplace_back(std::move(fixture));
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(std::none_of(before.begin(),before.end(),[&](const auto& previous){return previous.id==body.id;})) {
        state.movableIds.push_back(body.id);return body.id;
        }
    return uintptr_t(0);
    };
  // Same real mesh instances in both modes, supported by the actual tavern tabletop.
  const float spacing=std::max(22.f,std::max(dishBounds[1].x-dishBounds[0].x,dishBounds[1].z-dishBounds[0].z)+3.f);
  for(unsigned i=0;i<6;++i)
    if(!add(utensilVisual,tableTop+Vec3((float(i%3)-1)*spacing,0,(float(i/3)-.5f)*spacing))) {
      arenaAbort("movable tabletop utensil insertion failed");return;
      }
  Vec3 laneStart,laneEnd;bool laneFound=false;
  for(unsigned i=0;i<16 && !laneFound;++i) {
    const float angle=float(i)*float(M_PI)/8.f;
    const Vec3 direction(std::cos(angle),0,std::sin(angle)),side(-direction.z,0,direction.x);
    const auto center=Vec3(tableCenter.x,host->position().y,tableCenter.z)+direction*300.f;
    laneStart=center-side*150.f;laneEnd=center+side*150.f;
    bool supported=true;
    for(auto* point:{&laneStart,&laneEnd}) {
      const auto floor=world.physic()->landRay(*point+Vec3(0,80,0),180);
      if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-host->position().y)>60) {supported=false;break;}
      point->y=floor.v.y;
      }
    if(supported && !world.physic()->ray(laneStart+Vec3(0,50,0),laneEnd+Vec3(0,50,0)).hasCol &&
       !world.physic()->ray(laneStart+Vec3(0,120,0),laneEnd+Vec3(0,120,0)).hasCol) laneFound=true;
    }
  if(!laneFound) {arenaAbort("Coragon tavern lacks a supported 3m walking lane");return;}
  state.movableWalkStart=laneStart;state.movableWalkEnd=laneEnd;
  state.movableDish=add(utensilVisual,(laneStart+laneEnd)*.5f);
  if(!state.movableDish) {arenaAbort("movable floor utensil insertion failed");return;}
  bool hasBarrel=false;
  for(const auto& body:all)
    if(body.className=="barrel" && allowedMapVobs.contains(body.zenId) && state.movableMapIds.contains(body.id))hasBarrel=true;
  if(!hasBarrel && !barrelVisual.empty()) {
    const auto side=Vec3::normalize(laneEnd-laneStart);
    const auto point=(laneStart+laneEnd)*.5f+side*110.f;
    add(barrelVisual,point);
    }
  // Keep the original tavern population active during the untouched interval.
  player.setPosition(laneStart);player.setDirection(tableCenter-laneStart);player.updateTransform();
  context.cam=C_Hold;lookFrom(laneStart+Vec3(0,180,0),tableTop);
  state.movableReady=true;
  std::string massExamples="[";std::unordered_set<std::string> massClasses;
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(!body.enabled || !massClasses.insert(body.className).second)continue;
    if(massExamples.size()>1)massExamples+=',';
    massExamples+="{\"class\":"+jsonStr(body.className)+",\"visual\":"+jsonStr(body.visual)+
      ",\"kg\":"+ragdollNumber(body.mass)+"}";
    }
  event("movable_setup","\"world\":"+jsonStr(world.name())+",\"host\":"+jsonStr(symName(world,host))+
    ",\"host_cm\":"+ragdollVector(host->position())+",\"table_id\":"+std::to_string(tableId)+
    ",\"floor_dish_id\":"+std::to_string(state.movableDish)+",\"tabletop_utensils\":6,\"temporary_vobs\":"+
    std::to_string(state.movableFixtures.size())+",\"world_load_ms\":"+ragdollNumber(float(world.loadMilliseconds()))+
    ",\"map_vobs\":"+std::to_string(state.movableMapIds.size())+",\"map_classes\":"+classCounts(mapClasses)+
    ",\"noncolliding_map_classes\":"+classCounts(noncollidingClasses)+
    ",\"enabled\":"+(world.physic()->movableVobsEnabled()?"true":"false")+
    ",\"walk_start_cm\":"+ragdollVector(laneStart)+",\"walk_end_cm\":"+ragdollVector(laneEnd)+
    ",\"mass_examples\":"+massExamples+"],\"bodies\":"+movableRows(world));
  }

float noiseFacingError(const Npc& npc,const Vec3& point) {
  const auto direction=point-npc.position();
  const float desired=std::atan2(direction.z,direction.x)*180.f/float(M_PI);
  return std::abs(std::remainder(desired-npc.rotation(),360.f));
  }

uint32_t noiseNpcId(const Npc* npc) {
  auto& state=movablesContext().movables;
  if(npc==nullptr)return 0;
  const auto found=state.noiseNpcIds.find(npc);
  if(found!=state.noiseNpcIds.end())return found->second;
  const auto id=++state.noiseNextNpcId;
  state.noiseNpcIds.emplace(npc,id);
  return id;
  }

std::string noiseListenerRows(World& world) {
  auto& state=movablesContext().movables;
  std::string rows="[";
  for(const auto& listener:state.noiseListeners) {
    if(rows.size()>1)rows+=',';
    const auto& npc=*listener.npc;
    rows+="{\"role\":"+jsonStr(listener.role)+",\"npc_id\":"+std::to_string(noiseNpcId(&npc))+
      ",\"npc\":"+jsonStr(symName(world,&npc))+
      ",\"position_cm\":"+ragdollVector(npc.position())+",\"head_cm\":"+ragdollVector(npc.mapHeadBone())+
      ",\"yaw_deg\":"+ragdollNumber(npc.rotation())+
      ",\"distance_cm\":"+ragdollNumber((npc.position()-state.noiseSource).length())+
      ",\"occlusion\":"+ragdollNumber(world.physic()->soundOclusion(state.noiseSource,npc.mapHeadBone()))+
      ",\"facing_error_deg\":"+ragdollNumber(noiseFacingError(npc,state.noiseSource))+
      ",\"hero_error_deg\":"+ragdollNumber(noiseFacingError(npc,world.player()->position()))+
      ",\"heard\":"+std::to_string(listener.heard)+",\"external\":"+(npc.isExternalControl()?"true":"false")+
      ",\"eligible\":"+(npc.canHearPhysicsNoise()?"true":"false")+
      ",\"process_policy\":"+std::to_string(int(npc.processPolicy()))+
      ",\"dialogue\":"+(Gothic::inst().isNpcInDialog(npc)?"true":"false")+"}";
    }
  return rows+"]";
  }

void noiseBegin(World& world,std::string_view phase,const Vec3& source) {
  auto& state=movablesContext().movables;
  state.noiseTrace=true;state.noisePhase=phase;state.noiseStart=world.tickCount();state.noiseSource=source;
  state.noiseHasPushed=false;
  state.noisePerceptions.clear();
  for(auto& listener:state.noiseListeners)listener.heard=0;
  }

// Select actual supported tavern/world floor, using the same acoustic query as
// hearing. No synthetic wall, injected noise, spell sound or direct NPC turn.
bool noiseFloor(World& world,const Vec3& source,float distance,bool wall,Vec3& result,bool distant) {
  auto& state=movablesContext().movables;
  float best=-1;
  for(unsigned step=0;step<96;++step) {
    const float angle=float(step)*float(M_PI)/48.f;
    auto point=source+Vec3(std::cos(angle)*distance,0,std::sin(angle)*distance);
    const auto floor=world.physic()->landRay(point+Vec3(0,180,0),400);
    if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-state.movableWalkStart.y)>70)continue;
    point.y=floor.v.y;
    bool free=true;
    for(const auto offset:{Vec3(35,0,0),Vec3(-35,0,0),Vec3(0,0,35),Vec3(0,0,-35)})
      if(world.physic()->ray(point+Vec3(0,85,0),point+offset+Vec3(0,85,0)).hasCol)free=false;
    if(!free)continue;
    const float occlusion=world.physic()->soundOclusion(source,point+Vec3(0,160,0));
    if(wall) {
      // 1200 cm is the engine's maximum physical-impact radius. This
      // endpoint is inside that radius but inaudible even at maximum volume.
      if(distance<=1200.f*(1.f-occlusion)+100.f || occlusion<.12f)continue;
      } else if(!distant && occlusion>.05f)continue;
    const float score=wall?occlusion:float(step==0 ? 2 : 1);
    if(score>best) {best=score;result=point;}
    }
  return best>=0;
  }

Npc* noiseAddListener(World& world,std::string role,const Vec3& point,Npc* npc) {
  auto& state=movablesContext().movables;
  if(npc==nullptr)npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),point);
  if(!npc) {arenaAbort("physics noise listener insertion failed");return nullptr;}
  npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setInteraction(nullptr,true);
  npc->closeWeapon(true);npc->setPosition(point);npc->setExternalControl(false);
  npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
  npc->setDirection(point-state.noiseSource);npc->updateTransform();
  // A native wait keeps the normal actor in this fixture without teleporting
  // or rotating it during observation. Hearing executes before this wait gate.
  npc->aiPush(AiQueue::aiWait(60000));
  const auto callback=world.script().findSymbolIndex("B_ASSESSQUIETSOUND");
  if(callback!=size_t(-1))npc->setPerceptionEnable(PERC_ASSESSQUIETSOUND,callback);
  state.noiseListeners.push_back({npc,std::move(role),point,0});
  return npc;
  }

void noiseRetireListeners() {
  auto& state=movablesContext().movables;
  for(const auto& listener:state.noiseListeners) {
    listener.npc->clearAiQueue();listener.npc->setExternalControl(true);
    }
  state.noiseListeners.clear();
  }

void prepareNoiseFixture(World& world,Npc& player,bool tipping) {
  auto& state=movablesContext().movables;
  if(!state.movableReady)prepareMovableFixture(world,player);
  if(!state.movableReady)return;
  state.noiseTipNegative=tipping;
  if(tipping) {
    // The nearest table is wedged between the counter and pub frame; the two
    // eastern tables have fixed candles. The southern map table has only the
    // frame beside its local +Z edge. Tip towards its free -Z side.
    uintptr_t tableId=0;
    Vec3 tableCenter;
    float southernmost=std::numeric_limits<float>::infinity();
    for(const auto& body:world.physic()->movableVobSnapshot()) {
      if(body.className!="table" || !body.enabled || !state.movableMapIds.contains(body.id))continue;
      const auto* mesh=Resources::loadMesh(body.visual);if(!mesh)continue;
      const auto* bounds=mesh->bbox();
      auto center=(bounds[0]+bounds[1])*.5f;body.transform.project(center);
      if(center.z<southernmost) {southernmost=center.z;tableId=body.id;tableCenter=center;}
      }
    if(!tableId) {arenaAbort("noise fixture lacks its southern tavern table");return;}
    state.movableTable=tableId;state.movableCenter=tableCenter;
    }
  for(uint32_t i=0;i<world.npcCount();++i)
    if(auto* npc=world.npcById(i);npc && npc!=&player && (npc->position()-state.movableCenter).length()<1400.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  state.noiseTrace=true;
  event("physics_noise_setup","\"world\":"+jsonStr(world.name())+",\"table_id\":"+
    std::to_string(state.movableTable)+",\"enabled\":"+(world.physic()->physicsNoiseEnabled()?"true":"false"));
  }

void noisePushTable(World& world,Npc* culprit) {
  auto& state=movablesContext().movables;
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(body.id!=state.movableTable)continue;
    const auto* mesh=Resources::loadMesh(body.visual);if(!mesh)break;
    const auto* bounds=mesh->bbox();auto center=(bounds[0]+bounds[1])*.5f;body.transform.project(center);
    auto point=(bounds[0]+bounds[1])*.5f;
    point.y=bounds[1].y;
    // Push across the narrower tabletop edge so the native hull rotates,
    // rather than receiving another centre-of-mass translation.
    const bool narrowX=bounds[1].x-bounds[0].x<bounds[1].z-bounds[0].z;
    if(narrowX)point.x=bounds[state.noiseTipNegative?0:1].x;
    else point.z=bounds[state.noiseTipNegative?0:1].z;
    body.transform.project(point);
    auto direction=point-center;direction.y=0;
    direction=Vec3::normalize(direction);
    // A real native impulse couple tips the table in place: the equal COM
    // reaction cancels linear momentum, preserving the 5 m hearing geometry.
    const Vec3 force=direction*(6.f*body.mass),reaction=force*-1.f;
    const auto pushed=world.physic()->applyMovableVobImpulse(center,140.f,
      [force,mass=body.mass](const Vec3&,float targetMass) {
        return std::abs(targetMass-mass)<.001f ? force : Vec3();
        },culprit,&point);
    const auto reacted=world.physic()->applyMovableVobImpulse(center,140.f,
      [reaction,mass=body.mass](const Vec3&,float targetMass) {
        return std::abs(targetMass-mass)<.001f ? reaction : Vec3();
        },culprit,&center);
    state.noiseTableInitial=body.transform;state.noiseTableMaximumTilt=0;state.noiseHasPushed=true;
    event("physics_noise_push","\"phase\":"+jsonStr(state.noisePhase)+",\"table_id\":"+
      std::to_string(body.id)+",\"affected\":"+std::to_string(pushed)+",\"mass_kg\":"+ragdollNumber(body.mass)+
      ",\"impulse_ns\":"+ragdollVector(force)+",\"application_point_cm\":"+ragdollVector(point)+
      ",\"reaction_impulse_ns\":"+ragdollVector(reaction)+",\"reaction_point_cm\":"+ragdollVector(center)+
      ",\"reaction_affected\":"+std::to_string(reacted)+
      ",\"transform\":"+ragdollMatrix(body.transform)+
      ",\"spell\":false,\"culprit\":"+(culprit?jsonStr(symName(world,culprit)):"null"));
    if(!pushed || !reacted)arenaAbort("physics noise silent table impulse couple was blocked");
    return;
    }
  arenaAbort("physics noise table absent");
  }

void movableCandleSetup(World& world,Npc& player) {
  const auto context=movablesContext();
  auto& state=context.movables;
  const auto bodies=world.physic()->movableVobSnapshot();
  const DynamicWorld::MovableVobSnapshot* table=nullptr;
  const DynamicWorld::MovableVobSnapshot* candle=nullptr;
  float bestClearance=-1;
  bool negative=false;
  for(const auto& candidate:bodies) {
    if(!candidate.enabled || (candidate.zenId!=21133 && candidate.zenId!=21134))continue;
    const auto* mesh=Resources::loadMesh(candidate.visual);if(!mesh)continue;
    const auto* bounds=mesh->bbox();
    auto inverse=candidate.transform;inverse.inverse();
    const DynamicWorld::MovableVobSnapshot* supported=nullptr;
    for(const auto& item:bodies) {
      if(!item.enabled || item.visual.find("NW_CITY_CANDLE_01")==std::string::npos)continue;
      const auto* candleMesh=Resources::loadMesh(item.visual);if(!candleMesh)continue;
      const auto* cb=candleMesh->bbox();
      auto bottom=Vec3((cb[0].x+cb[1].x)*.5f,cb[0].y,(cb[0].z+cb[1].z)*.5f);
      item.transform.project(bottom);inverse.project(bottom);
      if(bottom.x>=bounds[0].x && bottom.x<=bounds[1].x && bottom.z>=bounds[0].z && bottom.z<=bounds[1].z &&
         std::abs(bottom.y-bounds[1].y)<5.f) {supported=&item;break;}
      }
    if(supported==nullptr)continue;
    const bool narrowX=bounds[1].x-bounds[0].x<bounds[1].z-bounds[0].z;
    auto center=(bounds[0]+bounds[1])*.5f;candidate.transform.project(center);
    for(const bool side:{false,true}) {
      auto edge=(bounds[0]+bounds[1])*.5f;
      if(narrowX)edge.x=bounds[side?0:1].x;
      else edge.z=bounds[side?0:1].z;
      candidate.transform.project(edge);
      auto direction=edge-center;direction.y=0;direction=Vec3::normalize(direction);
      float clearance=250.f;
      for(float height:{-20.f,0.f,30.f}) {
        const auto from=edge+direction*15.f+Vec3(0,height,0),to=from+direction*250.f;
        const auto hit=world.physic()->ray(from,to);
        if(hit.hasCol)clearance=std::min(clearance,(hit.v-from).length());
        }
      if(clearance>bestClearance) {bestClearance=clearance;table=&candidate;candle=supported;negative=side;}
      }
    }
  if(table==nullptr || candle==nullptr) {arenaAbort("candle fixture requires a movable candle on map table 21133 or 21134");return;}
  if(bestClearance<80.f) {arenaAbort("candle table has no clear narrow tipping edge");return;}
  const auto* tableMesh=Resources::loadMesh(table->visual);
  const auto* candleMesh=Resources::loadMesh(candle->visual);
  const auto* tb=tableMesh->bbox();const auto* cb=candleMesh->bbox();
  state.candleTableBounds[0]=tb[0];state.candleTableBounds[1]=tb[1];
  state.candleSupport=Vec3((cb[0].x+cb[1].x)*.5f,cb[0].y,(cb[0].z+cb[1].z)*.5f);
  state.candleTableInitial=table->transform;state.candleInitial=movablePosition(candle->transform);
  state.candleBody=candle->id;state.candleZenId=candle->zenId;state.candleTableZenId=table->zenId;
  state.candleAttachments.clear();
  auto inverse=candle->transform;inverse.inverse();
  std::string attachments="[";unsigned flames=0,lights=0;
  for(const auto& child:world.vobAttachmentSnapshot(candle->zenId)) {
    const bool flame=child.visual.find("FIRE_COMPLETE_A0.TGA")!=std::string::npos;
    if(!flame && !child.isLight)continue;
    if(!child.hasRenderedPosition) {arenaAbort("candle child lacks a real rendered position");return;}
    Movables::CandleAttachment sample;
    sample.zenId=child.zenId;sample.visual=child.visual;sample.light=child.isLight;
    sample.initial=child.renderedPosition;sample.local=child.renderedPosition;inverse.project(sample.local);
    state.candleAttachments.emplace_back(sample);
    flames+=flame;lights+=child.isLight;
    if(attachments.size()>1)attachments+=',';
    attachments+="{\"zen_id\":"+std::to_string(child.zenId)+",\"parent_zen_id\":"+std::to_string(child.parentZenId)+
      ",\"visual\":"+jsonStr(child.visual)+",\"light\":"+(child.isLight?"true":"false")+
      ",\"position_cm\":"+ragdollVector(child.renderedPosition)+",\"candle_local_cm\":"+ragdollVector(sample.local)+"}";
    }
  if(flames!=1 || lights<1 || lights>2) {arenaAbort("candle fixture requires its real fire sprite and one or two real lights");return;}
  state.movableTable=table->id;state.noiseTipNegative=negative;
  state.movableCenter=(tb[0]+tb[1])*.5f;table->transform.project(state.movableCenter);
  for(uint32_t i=0;i<world.npcCount();++i)
    if(auto* npc=world.npcById(i);npc && npc!=&player && (npc->position()-state.movableCenter).length()<1400.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  auto edge=(tb[0]+tb[1])*.5f;
  if(tb[1].x-tb[0].x<tb[1].z-tb[0].z)edge.x=tb[negative?0:1].x;
  else edge.z=tb[negative?0:1].z;
  table->transform.project(edge);
  auto outward=edge-state.movableCenter;outward.y=0;outward=Vec3::normalize(outward);
  const auto camera=state.movableCenter+outward*240.f+Vec3(-outward.z,0,outward.x)*100.f+Vec3(0,150,0);
  auto hero=state.movableCenter-outward*300.f;
  if(const auto floor=world.physic()->landRay(hero+Vec3(0,180,0),400);floor.hasCol)hero.y=floor.v.y;
  player.setPosition(hero);player.setDirection(state.movableCenter-hero);player.updateTransform();
  context.cam=C_Hold;lookFrom(camera,state.movableCenter+Vec3(0,15,0));
  state.candleMaximumTilt=0;state.candleMaximumTravel=0;state.candleSamples=0;
  state.candleLeftTable=false;state.candleSnapshotMissing=false;state.candleCaptured=false;
  event("movable_candle_setup","\"world\":"+jsonStr(world.name())+",\"table_zen_id\":"+
    std::to_string(table->zenId)+",\"candle_zen_id\":"+std::to_string(candle->zenId)+
    ",\"table_mass_kg\":"+ragdollNumber(table->mass)+",\"candle_mass_kg\":"+ragdollNumber(candle->mass)+
    ",\"clearance_cm\":"+ragdollNumber(bestClearance)+",\"negative_edge\":"+(negative?"true":"false")+
    ",\"table_transform\":"+ragdollMatrix(table->transform)+",\"candle_transform\":"+ragdollMatrix(candle->transform)+
    ",\"attachments\":"+attachments+"]");
  }

void movableCandleFields(World& world) {
  const auto context=movablesContext();
  auto& state=context.movables;
  if(!state.candleTrace)return;
  ++state.candleSamples;
  const auto bodies=world.physic()->movableVobSnapshot();
  const DynamicWorld::MovableVobSnapshot* table=nullptr;
  const DynamicWorld::MovableVobSnapshot* candle=nullptr;
  for(const auto& body:bodies) {
    if(body.zenId==state.candleTableZenId)table=&body;
    if(body.id==state.candleBody)candle=&body;
    }
  if(table==nullptr || candle==nullptr || !table->enabled || !candle->enabled) {state.candleSnapshotMissing=true;return;}
  const auto* before=state.candleTableInitial.data();const auto* after=table->transform.data();
  const Vec3 a(before[4],before[5],before[6]),b(after[4],after[5],after[6]);
  const float tilt=std::acos(std::clamp((a.x*b.x+a.y*b.y+a.z*b.z)/(a.length()*b.length()),-1.f,1.f))*180.f/float(M_PI);
  state.candleMaximumTilt=std::max(state.candleMaximumTilt,tilt);
  state.candleMaximumTravel=std::max(state.candleMaximumTravel,(movablePosition(candle->transform)-state.candleInitial).length());
  auto bottom=state.candleSupport;candle->transform.project(bottom);
  auto inverse=table->transform;inverse.inverse();inverse.project(bottom);
  const auto* bounds=state.candleTableBounds;
  state.candleLeftTable|=bottom.x<bounds[0].x-3.f || bottom.x>bounds[1].x+3.f ||
    bottom.z<bounds[0].z-3.f || bottom.z>bounds[1].z+3.f || std::abs(bottom.y-bounds[1].y)>5.f;
  const auto children=world.vobAttachmentSnapshot(state.candleZenId);
  for(auto& sample:state.candleAttachments) {
    const auto child=std::find_if(children.begin(),children.end(),[&](const auto& item) {return item.zenId==sample.zenId;});
    if(child==children.end() || !child->hasRenderedPosition) {state.candleSnapshotMissing=true;continue;}
    auto expected=sample.local;candle->transform.project(expected);
    sample.maximumError=std::max(sample.maximumError,(child->renderedPosition-expected).length());
    sample.maximumTravel=std::max(sample.maximumTravel,(child->renderedPosition-sample.initial).length());
    }
  if(tilt>45.f && state.candleLeftTable && !state.candleCaptured) {
    context.shot="movable-candle-tip";state.candleCaptured=true;
    }
  }

void movableCandlePush(World& world) {
  auto& state=movablesContext().movables;
  if(state.candleAttachments.empty()) {arenaAbort("candle setup is required before push");return;}
  noiseBegin(world,"candle",state.movableCenter);
  state.candleStart=world.tickCount();state.candleTrace=true;
  noisePushTable(world);
  movableCandleFields(world);
  }

void movableCandleCheckpoint(World& world) {
  auto& state=movablesContext().movables;
  movableCandleFields(world);
  std::string attachments="[";float maximumError=0;
  for(const auto& sample:state.candleAttachments) {
    if(attachments.size()>1)attachments+=',';
    attachments+="{\"zen_id\":"+std::to_string(sample.zenId)+",\"visual\":"+jsonStr(sample.visual)+
      ",\"light\":"+(sample.light?"true":"false")+",\"max_local_offset_error_cm\":"+ragdollNumber(sample.maximumError)+
      ",\"max_displacement_cm\":"+ragdollNumber(sample.maximumTravel)+"}";
    maximumError=std::max(maximumError,sample.maximumError);
    }
  event("movable_candle_checkpoint","\"elapsed_ms\":"+std::to_string(world.tickCount()-state.candleStart)+
    ",\"samples\":"+std::to_string(state.candleSamples)+",\"table_maximum_tilt_deg\":"+ragdollNumber(state.candleMaximumTilt)+
    ",\"candle_maximum_displacement_cm\":"+ragdollNumber(state.candleMaximumTravel)+
    ",\"candle_left_tabletop\":"+(state.candleLeftTable?"true":"false")+
    ",\"missing_render_snapshot\":"+(state.candleSnapshotMissing?"true":"false")+
    ",\"max_local_offset_error_cm\":"+ragdollNumber(maximumError)+",\"tip_frame_captured\":"+(state.candleCaptured?"true":"false")+
    ",\"attachments\":"+attachments+"]");
  state.candleTrace=false;
  }

void noiseTableCase(World& world,Npc& player,bool push) {
  const auto context=movablesContext();
  auto& state=context.movables;
  noiseRetireListeners();
  noiseBegin(world,"table",state.movableCenter);
  Vec3 near,wall,far;bool hasWall=false;
  if(!noiseFloor(world,state.noiseSource,500,false,near)) {arenaAbort("noise fixture lacks a clear 5m listener position");return;}
  for(float distance:{500.f,600.f,700.f,800.f,900.f,1000.f,1100.f})
    if(noiseFloor(world,state.noiseSource,distance,true,wall)) {hasWall=true;break;}
  if(!hasWall) {arenaAbort("noise fixture lacks an acoustically occluded floor behind a real wall");return;}
  bool hasFar=false;
  for(float distance:{1800.f,2000.f,2400.f,2800.f})
    if(noiseFloor(world,state.noiseSource,distance,false,far,true)) {hasFar=true;break;}
  if(!hasFar) {arenaAbort("noise fixture lacks a supported distant listener position");return;}
  if(!noiseAddListener(world,"near",near) || !noiseAddListener(world,"wall",wall) || !noiseAddListener(world,"far",far))return;
  // The hero's bearing is visibly different from the table at the near NPC.
  const auto side=Vec3::normalize(near-state.noiseSource);
  auto hero=near+Vec3(-side.z,0,side.x)*220.f;
  const auto floor=world.physic()->landRay(hero+Vec3(0,180,0),400);
  if(floor.hasCol)hero.y=floor.v.y;
  player.setPosition(hero);player.setDirection(state.noiseSource-hero);player.updateTransform();
  context.cam=C_Hold;lookFrom(near+Vec3(0,250,0),state.noiseSource);
  event("physics_noise_case","\"phase\":\"table\",\"source_cm\":"+ragdollVector(state.noiseSource)+
    ",\"listeners\":"+noiseListenerRows(world));
  if(push)noisePushTable(world);
  }

void noiseDropItems(World& world,unsigned count) {
  auto& state=movablesContext().movables;
  const auto instance=world.script().findSymbolIndex("ITFO_BEER");
  if(instance==size_t(-1)) {arenaAbort("noise fixture beer Item instance unavailable");return;}
  for(unsigned i=0;i<count;++i) {
    auto matrix=Matrix4x4::mkIdentity();
    // Native Item proxies have a 30 cm minimum diameter, even for this small
    // mesh. Keep them separate until their ordinary simultaneous floor hits.
    const float x=count==1 ? 0.f : (float(i%3)-1.f)*36.f;
    const float z=count==1 ? 0.f : (float(i/3)-1.5f)*36.f;
    matrix.translate(state.noiseSource+Vec3(x,100.f,z));
    auto* item=world.addItemDyn(instance,matrix,size_t(-1));
    if(!item || item->physicsMass()<=0) {arenaAbort("noise fixture real dynamic cup Item has no native mass");return;}
    event("physics_noise_drop","\"phase\":"+jsonStr(state.noisePhase)+",\"ordinal\":"+
      std::to_string(i)+",\"item\":\"ITFO_BEER\",\"mass_kg\":"+ragdollNumber(item->physicsMass())+
      ",\"position_cm\":"+ragdollVector(item->position())+",\"drop_height_cm\":100");
    }
  }

void noiseItemCase(World& world,Npc& player,std::string_view phase) {
  auto& state=movablesContext().movables;
  noiseRetireListeners();
  auto source=(state.movableWalkStart+state.movableWalkEnd)*.5f;
  noiseBegin(world,phase,source);
  Vec3 near,far;
  if(!noiseFloor(world,source,110,false,near) || !noiseFloor(world,source,600,false,far,true)) {
    arenaAbort("noise cup fixture lacks supported near/far floor");return;
    }
  if(phase=="dialogue" && state.noiseHost==nullptr) {arenaAbort("physics noise dialogue needs the actual Coragon host");return;}
  auto* listener=noiseAddListener(world,phase=="dialogue"?"dialogue":"near",near,
    phase=="dialogue"?state.noiseHost:nullptr);
  if(!listener)return;
  if(!noiseAddListener(world,"far",far))return;
  player.setPosition(near+Vec3(0,0,80));player.setDirection(near-player.position());player.updateTransform();
  event("physics_noise_case","\"phase\":"+jsonStr(phase)+",\"source_cm\":"+ragdollVector(source)+
    ",\"listeners\":"+noiseListenerRows(world));
  if(phase=="dialogue") {
    listener->clearAiQueue();listener->aiPush(AiQueue::aiProcessInfo(player));
    // Opening the actual output pipe takes a native tick; a separate command
    // verifies the real dialog state before releasing the cup.
    return;
    }
  noiseDropItems(world,phase=="burst"?10:1);
  }

void noiseCheckpoint(World& world) {
  auto& state=movablesContext().movables;
  std::string perceptions="[";
  for(const auto& [npc,count]:state.noisePerceptions) {
    if(perceptions.size()>1)perceptions+=',';
    perceptions+="{\"npc_id\":"+std::to_string(npc)+",\"count\":"+std::to_string(count)+"}";
    }
  std::string table="null";
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(body.id==state.movableTable) {table=ragdollMatrix(body.transform);break;}
  event("physics_noise_checkpoint","\"phase\":"+jsonStr(state.noisePhase)+",\"elapsed_ms\":"+
    std::to_string(world.tickCount()-state.noiseStart)+",\"listeners\":"+noiseListenerRows(world)+
    ",\"perceptions\":"+perceptions+"],\"table_transform\":"+table+
    ",\"table_maximum_tilt_deg\":"+ragdollNumber(state.noiseTableMaximumTilt)+
    ",\"table_tilt_above_45_deg\":"+(state.noiseTableMaximumTilt>45.f?"true":"false"));
  }

void noiseFields(World& world) {
  auto& state=movablesContext().movables;
  if(!state.noiseTrace || state.movableMeasuring || !state.noiseHasPushed || state.noisePhase!="table" || world.tickCount()-state.noiseStart>2000)return;
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(body.id==state.movableTable) {
      const auto* before=state.noiseTableInitial.data();const auto* after=body.transform.data();
      const Vec3 a(before[4],before[5],before[6]),b(after[4],after[5],after[6]);
      const float cosine=(a.x*b.x+a.y*b.y+a.z*b.z)/(a.length()*b.length());
      const float tilt=std::acos(std::clamp(cosine,-1.f,1.f))*180.f/float(M_PI);
      state.noiseTableMaximumTilt=std::max(state.noiseTableMaximumTilt,tilt);
      }
  for(const auto& listener:state.noiseListeners)
    if(listener.role=="near")
      event("physics_noise_turn","\"elapsed_ms\":"+std::to_string(world.tickCount()-state.noiseStart)+
        ",\"npc_id\":"+std::to_string(noiseNpcId(listener.npc))+",\"facing_error_deg\":"+
        ragdollNumber(noiseFacingError(*listener.npc,state.noiseSource))+
        ",\"hero_error_deg\":"+ragdollNumber(noiseFacingError(*listener.npc,world.player()->position())));
  }

void prepareMovableCast(World& world,Npc& player) {
  const auto context=movablesContext();
  auto& state=context.movables;
  // Existing native actors stop only after the 60-second untouched observation.
  for(uint32_t i=0;i<world.npcCount();++i) {
    auto* npc=world.npcById(i);
    if(npc && npc!=&player && (npc->position()-state.movableCenter).length()<1400) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
    }
  player.closeWeapon(true);
  Vec3 target,caster;bool found=false;
  const auto npcFreeLane=[&](const Vec3& from,const Vec3& to) {
    const auto segment=to-from;
    const float lengthSquared=segment.x*segment.x+segment.z*segment.z;
    if(lengthSquared<=1.f)return false;
    // A stopped NPC still intercepts the real projectile. Check every actor,
    // including the caster endpoint, with a conservative 100 cm lane radius.
    // Height separates the tavern floor from actors upstairs or downstairs.
    for(uint32_t id=0;id<world.npcCount();++id) {
      const auto* other=world.npcById(id);if(!other || other==&player)continue;
      const auto center=other->collosionCenter();
      const float t=std::clamp(((center.x-from.x)*segment.x+(center.z-from.z)*segment.z)/lengthSquared,0.f,1.f);
      const auto closest=from+segment*t+Vec3(0,100,0);
      const float dx=center.x-closest.x,dz=center.z-closest.z;
      if(dx*dx+dz*dz<100.f*100.f && std::abs(center.y-closest.y)<160.f)return false;
      }
    const auto side=Vec3(-segment.z,0,segment.x)*(60.f/std::sqrt(lengthSquared));
    for(const auto offset:{Vec3(),side,side*-1.f}) {
      const auto hit=world.physic()->rayNpc(from+offset+Vec3(0,100,0),to+offset+Vec3(0,100,0),&player);
      if(hit.npcHit!=nullptr)return false;
      }
    return true;
    };
  for(unsigned i=0;i<16 && !found;++i) {
    const float angle=float(i)*float(M_PI)/8.f;
    const Vec3 direction(std::cos(angle),0,std::sin(angle));
    caster=Vec3(state.movableCenter.x,state.movableWalkStart.y,state.movableCenter.z)-direction*400.f;
    target=Vec3(state.movableCenter.x,state.movableWalkStart.y,state.movableCenter.z)+direction*180.f;
    bool valid=true;
    for(auto* point:{&caster,&target}) {
      const auto floor=world.physic()->landRay(*point+Vec3(0,80,0),180);
      if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-state.movableWalkStart.y)>40) {valid=false;break;}
      point->y=floor.v.y;
      // Endpoint body volumes must not be hidden in furniture or walls.
      for(const auto side:{Vec3(25,0,0),Vec3(-25,0,0),Vec3(0,0,25),Vec3(0,0,-25)})
        if(world.physic()->ray(*point+Vec3(0,85,0),*point+side+Vec3(0,85,0)).hasCol)valid=false;
      }
    if(valid && npcFreeLane(caster,target) &&
       !world.physic()->ray(caster+Vec3(0,145,0),target+Vec3(0,145,0)).hasCol)found=true;
    }
  if(!found) {arenaAbort("tavern fireball needs supported clear caster/target endpoints across furniture");return;}
  player.setPosition(caster);player.setDirection(target-caster);player.updateTransform();
  context.cam=C_Hold;lookFrom(caster+Vec3(0,185,0),state.movableCenter+Vec3(0,40,0));
  auto* npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),target);
  if(!npc) {arenaAbort("movable real fireball target insertion failed");return;}
  npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
  npc->handle().attribute[ATR_HITPOINTS]=npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
  npc->setDirection(player.position()-target);npc->updateTransform();state.spellActors.push_back({npc,"movable_target"});
  for(size_t i=0;i<world.script().symbolsCount() && !state.spellRunes.contains(23);++i) {
    const auto* symbol=world.script().findSymbol(i);if(!symbol || symbol->type()!=daedalus::DaedalusDataType::INSTANCE)continue;
    auto name=symbol->name();for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
    if(!name.starts_with("ITRU_") && !name.starts_with("ITSC_"))continue;
    auto item=std::make_unique<Item>(world,i,Item::T_Inventory);
    if(item->spellId()==23) {state.spellRunes[23]=i;player.addItem(i,item->isRune()?1:5);}
    }
  player.handle().attribute[ATR_MANA]=player.handle().attribute[ATR_MANAMAX]=50000;player.setTalentSkill(TALENT_MAGE,6);
  Gothic::settingsSetI("GAME","spell_physics",1);
  event("movable_cast_setup","\"spell_id\":23,\"npc_free_lane_radius_cm\":100,\"target_cm\":"+
    ragdollVector(target)+",\"caster_cm\":"+ragdollVector(player.position()));
  }

void movableSaveSnapshot(World& world,std::string_view label) {
  std::string npcPins="[",worldPins="[";
  for(uint32_t id=0;id<world.npcCount();++id) {
    const auto* npc=world.npcById(id);if(!npc)continue;
    for(size_t i=0;i<npc->stuckArrowCount();++i) {
      Npc::ArrowPinProbe pin;
      if(!npc->stuckArrowProbe(i,pin)) {arenaAbort("saved NPC arrow lacks a rendered node");return;}
      if(npcPins.size()>1)npcPins+=',';
      npcPins+="{\"npc_id\":"+std::to_string(world.npcId(npc))+",\"instance\":"+jsonStr(symName(world,npc))+
        ",\"index\":"+std::to_string(i)+",\"bone\":"+std::to_string(pin.bone)+
        ",\"local\":"+ragdollMatrix(pin.local)+",\"world\":"+ragdollMatrix(pin.world)+
        ",\"expected\":"+ragdollMatrix(npc->renderedBone(pin.bone)*pin.local)+"}";
      }
    }
  for(size_t i=0;i<world.stuckArrowCount();++i) {
    Matrix4x4 matrix,local;uint32_t zenId=uint32_t(-1);
    if(!world.stuckArrowTransform(i,matrix)) {arenaAbort("saved world arrow lacks a rendered node");return;}
    const bool attached=world.stuckArrowAttachment(i,zenId,local);
    if(worldPins.size()>1)worldPins+=',';
    worldPins+="{\"index\":"+std::to_string(i)+",\"zen_id\":"+(attached?std::to_string(zenId):"null")+
      ",\"world\":"+ragdollMatrix(matrix)+",\"local\":"+(attached?ragdollMatrix(local):"null")+"}";
    }
  event("movable_save_snapshot","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"movable_vobs\":"+(Gothic::physicsFlags().movableVobs?"true":"false")+
    ",\"arrow_sticking\":"+(Gothic::physicsFlags().arrowSticking?"true":"false")+
    ",\"bodies\":"+movableRows(world,true)+",\"npc_pins\":"+npcPins+"],\"world_pins\":"+worldPins+"]");
  }

void movablePin(World& world,Npc& player,std::string_view target) {
  auto& state=movablesContext().movables;
  const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
  if(symbol==size_t(-1)) {arenaAbort("movable save fixture arrow instance missing");return;}
  Item arrow(world,symbol,Item::T_Inventory);
  Vec3 from,to;
  const auto npcHit=[&](Npc& npc,const Vec3& start,const Vec3& end,float& distance) {
    return &npc!=&player && npc.rayHitZone(start,end,distance)!=HitZones::None;
    };
  if(target=="table") {
    bool found=false;
    for(const auto& body:world.physic()->movableVobSnapshot()) {
      if(body.id!=state.movableTable)continue;
      const auto* mesh=Resources::loadMesh(body.visual);
      if(!mesh)break;
      const auto* bounds=mesh->bbox();
      const float infinity=std::numeric_limits<float>::infinity();
      Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);
      for(unsigned corner=0;corner<8;++corner) {
        Vec3 point(bounds[corner&1].x,bounds[(corner>>1)&1].y,bounds[(corner>>2)&1].z);
        body.transform.project(point);
        low.x=std::min(low.x,point.x);low.y=std::min(low.y,point.y);low.z=std::min(low.z,point.z);
        high.x=std::max(high.x,point.x);high.y=std::max(high.y,point.y);high.z=std::max(high.z,point.z);
        }
      // The fireball leaves test dishes on and around the table. Select an
      // exposed part of this exact map body, using the production visual query.
      for(float x:{.5f,.2f,.8f,.35f,.65f,.1f,.9f}) {
        for(float z:{.5f,.2f,.8f,.35f,.65f,.1f,.9f}) {
          const Vec3 start(low.x+(high.x-low.x)*x,high.y+80.f,low.z+(high.z-low.z)*z);
          const Vec3 end(start.x,low.y-10.f,start.z);
          const auto hit=world.physic()->rayMovableVob(start,end);
          if(!hit || hit->zenId!=body.zenId || world.physic()->rayArrow(start,end,npcHit).npcHit)continue;
          from=start;to=end;found=true;break;
          }
        if(found)break;
        }
      break;
      }
    if(!found) {arenaAbort("movable save table has no exposed native shot path");return;}
    }
  else if(target=="npc") {
    if(state.spellActors.empty()) {arenaAbort("movable save NPC is absent");return;}
    auto& npc=*state.spellActors[0].npc;
    if(npc.isDown()) {arenaAbort("movable save arrow needs its living NPC target");return;}
    const auto* skeleton=npc.visualSkeleton();
    const auto bone=skeleton?arrowBone(*skeleton,"SPINE2"):size_t(-1);
    if(bone==size_t(-1)) {arenaAbort("movable save NPC has no chest bone");return;}
    const auto chest=movablePosition(npc.renderedBone(bone));
    bool found=false;
    for(const auto direction:{Vec3(0,1,0),Vec3(1,0,0),Vec3(-1,0,0),Vec3(0,0,1),Vec3(0,0,-1)}) {
      const auto start=chest+direction*100.f,end=chest-direction*50.f;
      if(world.physic()->rayArrow(start,end,npcHit).npcHit!=&npc)continue;
      from=start;to=end;found=true;break;
      }
    if(!found) {arenaAbort("movable save NPC has no exposed precise shot path");return;}
    }
  else if(target=="world") {
    const auto point=state.movableWalkStart;
    const auto floor=world.physic()->landRay(point+Vec3(0,80,0),180);
    if(!floor.hasCol) {arenaAbort("movable save static arrow has no floor");return;}
    to=floor.v;from=to+Vec3(0,60,0);
    }
  else {arenaAbort("unknown movable save arrow target");return;}
  auto& bullet=world.shootArrow(arrow,from,Vec3::normalize(to-from),DynamicWorld::bulletSpeed);
  bullet.setOrigin(&player);bullet.setPrecise(1.f);
  event("movable_save_shot","\"target\":"+jsonStr(target)+",\"from_cm\":"+ragdollVector(from)+",\"to_cm\":"+ragdollVector(to));
  }

void pushMovablePinnedTable(World& world) {
  uint32_t zenId=uint32_t(-1);Matrix4x4 local;
  for(size_t i=0;i<world.stuckArrowCount();++i)
    if(world.stuckArrowAttachment(i,zenId,local))break;
  if(zenId==uint32_t(-1)) {arenaAbort("movable push needs an arrow attached to map furniture");return;}
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(body.zenId!=zenId)continue;
    const auto* mesh=Resources::loadMesh(body.visual);
    if(!mesh)break;
    const auto* bounds=mesh->bbox();auto center=(bounds[0]+bounds[1])*.5f;body.transform.project(center);
    const auto pushed=world.physic()->applyMovableVobImpulse(center,180.f,
      [](const Vec3&,float mass) {return Vec3(.9f,.3f,.45f)*mass;});
    event("movable_save_push","\"zen_id\":"+std::to_string(zenId)+",\"affected\":"+std::to_string(pushed));
    if(!pushed)arenaAbort("movable saved table impulse was blocked");
    return;
    }
  arenaAbort("movable pinned map furniture is absent");
  }

void saveMovableSession(World& world,const std::string& label) {
  if(label.empty() || label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
    throw std::runtime_error("invalid movable save label");
  std::vector<uint8_t> bytes;
  {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
  Gothic::inst().gameSession()->save(out,"movable "+label,preview);}
  std::ofstream file(outDir+"/session-"+label+".sav",std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!file)throw std::runtime_error("cannot write complete movable GameSession archive");
  movableSaveSnapshot(world,"saved");
  event("movable_session_save","\"archive\":"+jsonStr("session-"+label+".sav")+",\"bytes\":"+
    std::to_string(bytes.size())+",\"world\":"+jsonStr(world.name())+",\"bodies\":"+movableRows(world));
  }

bool noiseCommand(const MovablesContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  auto& state=context.movables;
  const auto& c=a[0];
  if(c=="noise" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","physics_noise",a[1]=="on");
    w.physic()->setPhysicsNoiseEnabled(a[1]=="on");
    }
  else if(c=="noise" && a.size()==2 && a[1]=="setup") prepareNoiseFixture(w,pl,true);
  else if(c=="noise" && a.size()==2 && a[1]=="table") noiseTableCase(w,pl);
  else if(c=="noise" && a.size()==2 && a[1]=="listeners") noiseTableCase(w,pl,false);
  else if(c=="noise" && a.size()==2 && a[1]=="push") noisePushTable(w);
  else if(c=="noise" && a.size()==2 && (a[1]=="cup" || a[1]=="burst" || a[1]=="dialogue")) noiseItemCase(w,pl,a[1]);
  else if(c=="noise" && a.size()==2 && a[1]=="dialogue-drop") {
    const auto* listener=state.noiseListeners.empty()?nullptr:state.noiseListeners.front().npc;
    const bool active=listener && Gothic::inst().isNpcInDialog(*listener);
    event("physics_noise_dialogue","\"active\":"+std::string(active?"true":"false")+
      ",\"npc_id\":"+std::to_string(noiseNpcId(listener)));
    if(!active) {arenaAbort("physics noise dialogue fixture did not open an actual native dialogue");return true;}
    state.noiseStart=w.tickCount();
    noiseDropItems(w,1);
    }
  else if(c=="noise" && a.size()==2 && a[1]=="checkpoint") noiseCheckpoint(w);
  else return false;
  return true;
  }

bool movableCommand(const MovablesContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  auto& state=context.movables;
  const auto& c=a[0];
  if(c=="movable" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","movable_vobs",a[1]=="on");
    w.physic()->setMovableVobsEnabled(a[1]=="on");
    }
  else if(c=="movable" && a.size()==2 && a[1]=="setup") prepareMovableFixture(w,pl);
  else if(c=="movable" && a.size()==2 && a[1]=="candle-setup") movableCandleSetup(w,pl);
  else if(c=="movable" && a.size()==2 && a[1]=="candle-push") movableCandlePush(w);
  else if(c=="movable" && a.size()==2 && a[1]=="candle-checkpoint") movableCandleCheckpoint(w);
  else if(c=="movable" && a.size()==2 && a[1]=="wake") {
    state.movableWakeStart=w.tickCount();state.movableWakeTrace=true;state.movableWakeTravel.clear();
    say("movable wake: "+std::to_string(w.physic()->wakeMovableVobs()));
    }
  else if(c=="movable" && a.size()==2 && a[1]=="drift") {
    std::FILE* f = std::fopen((outDir+"/movable-drift.jsonl").c_str(),"a");
    size_t moved = 0, total = 0;
    for(const auto& body:w.physic()->movableVobSnapshot()) {
      if(!body.enabled) continue;
      ++total;
      const auto shift = (movablePosition(body.transform)-movablePosition(body.origin)).length();
      const auto* before=body.origin.data();const auto* after=body.transform.data();
      const Vec3 ua(before[4],before[5],before[6]),ub(after[4],after[5],after[6]);
      const float tilt=std::acos(std::clamp((ua.x*ub.x+ua.y*ub.y+ua.z*ub.z)/(ua.length()*ub.length()),-1.f,1.f))*180.f/float(M_PI);
      if(shift>1.f || tilt>1.f) ++moved;
      if(f!=nullptr)
        std::fprintf(f,"%s\n",("{\"world\":"+jsonStr(w.name())+",\"zen_id\":"+std::to_string(body.zenId)+
          ",\"visual\":"+jsonStr(body.visual)+",\"shift_cm\":"+ragdollNumber(shift)+",\"tilt_deg\":"+ragdollNumber(tilt)+
          ",\"max_shift_cm\":"+ragdollNumber(std::max(shift,state.movableWakeTravel[body.id]))+
          ",\"elapsed_ms\":"+std::to_string(w.tickCount()-state.movableWakeStart)+
          ",\"sleeping\":"+(body.sleeping?"true":"false")+"}").c_str());
      }
    if(f!=nullptr) std::fclose(f);
    say("movable drift: "+std::to_string(moved)+" of "+std::to_string(total)+" moved more than 1 cm or 1 deg");
    }
  else if(c=="movable" && (a.size()==2 || a.size()==3) && a[1]=="clamps") {
    const float margin = a.size()==3 ? std::stof(a[2]) : 1.f;
    const auto all = w.physic()->movableVobContacts(margin);
    std::FILE* f = std::fopen((outDir+"/movable-clamps.jsonl").c_str(),"a");
    size_t touched = 0;
    for(const auto& body:all) {
      std::string row = "{\"world\":"+jsonStr(w.name())+",\"zen_id\":"+std::to_string(body.zenId)+
        ",\"visual\":"+jsonStr(body.visual)+",\"class\":"+jsonStr(body.className)+
        ",\"mass_kg\":"+ragdollNumber(body.mass)+",\"position_cm\":"+ragdollVector(body.position)+
        ",\"enabled\":"+(body.enabled?"true":"false")+",\"disposition\":"+jsonStr(body.disposition)+
        ",\"classification_ms\":"+ragdollNumber(float(w.physic()->movableVobValidationMilliseconds()))+
        ",\"hull_min_cm\":"+ragdollVector(body.hullMin)+",\"hull_max_cm\":"+ragdollVector(body.hullMax)+
        ",\"contacts\":[";
      for(size_t i=0;i<body.contacts.size();++i) {
        const auto& t = body.contacts[i];
        if(i>0) row += ",";
        row += "{\"other\":"+jsonStr(t.other)+",\"category\":"+std::to_string(t.category)+
          ",\"other_zen_id\":"+std::to_string(t.otherZenId)+
          ",\"interactive\":"+(t.interactive?"true":"false")+",\"below\":"+(t.below?"true":"false")+",\"side\":"+std::to_string(t.side)+
          ",\"other_min_cm\":"+ragdollVector(t.otherMin)+",\"other_max_cm\":"+ragdollVector(t.otherMax)+
          ",\"depth_cm\":"+ragdollNumber(t.depth)+",\"count\":"+std::to_string(t.count)+
          ",\"axis\":"+ragdollVector(t.axis)+",\"point_cm\":"+ragdollVector(t.point)+
          ",\"point_y_min_cm\":"+ragdollNumber(t.pointYMin)+",\"point_y_max_cm\":"+ragdollNumber(t.pointYMax)+
          ",\"material\":"+jsonStr(t.material)+",\"sector\":"+jsonStr(t.sector)+"}";
        }
      row += "]}";
      if(!body.contacts.empty()) ++touched;
      if(f!=nullptr) std::fprintf(f,"%s\n",row.c_str());
      }
    if(f!=nullptr) std::fclose(f);
    say("movable clamps: "+std::to_string(all.size())+" bodies, "+std::to_string(touched)+" with contacts");
    event("movable_clamps","\"world\":"+jsonStr(w.name())+",\"bodies\":"+std::to_string(all.size())+
      ",\"with_contacts\":"+std::to_string(touched)+",\"margin_cm\":"+ragdollNumber(margin)+
      ",\"classification_ms\":"+ragdollNumber(float(w.physic()->movableVobValidationMilliseconds())));
    }
  else if(c=="movable" && a.size()==2 && a[1]=="observe") {
    state.movableIds.clear();state.movableMapIds.clear();
    for(const auto& body:w.physic()->movableVobSnapshot())
      if(body.zenId!=uint32_t(-1)) {state.movableIds.push_back(body.id);state.movableMapIds.insert(body.id);}
    for(uint32_t id=0;id<w.npcCount();++id)
      if(auto* npc=w.npcById(id);npc && npc!=&pl && (npc->position()-pl.position()).length()<1800) {
        if(!npc->isDown()) {
          npc->clearAiQueue();npc->clearState(false);npc->stopWalking();
          }
        npc->setExternalControl(true);
        }
    }
  else if(c=="movable" && a.size()==3 && a[1]=="snapshot") movableSaveSnapshot(w,a[2]);
  else if(c=="movable" && a.size()==3 && a[1]=="pin") movablePin(w,pl,a[2]);
  else if(c=="movable" && a.size()==2 && a[1]=="settle-target") {
    if(state.spellActors.empty()) {arenaAbort("movable save NPC is absent");return true;}
    context.subject=state.spellActors[0].npc;
    if(!context.subject->stuckArrowCount()) {arenaAbort("movable save NPC arrow did not pin before death");return true;}
    context.subject->setRagdollDown(true);context.subject->prepareRagdoll();
    }
  else if(c=="movable" && a.size()==2 && a[1]=="push-pinned") pushMovablePinnedTable(w);
  else if(c=="movable" && a.size()==3 && a[1]=="begin") movableBegin(w,a[2]);
  else if(c=="movable" && a.size()==2 && a[1]=="checkpoint") movableCheckpoint(w);
  else if(c=="movable" && a.size()==2 && a[1]=="walk") {
    if(!state.movableReady) {arenaAbort("movable walking test needs the tavern fixture");return true;}
    auto* npc=w.addNpc(w.script().findSymbolIndex("BAU_904_BAUER"),state.movableWalkStart);
    if(!npc) {arenaAbort("movable walker insertion failed");return true;}
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
    npc->setDirection(state.movableWalkEnd-state.movableWalkStart);npc->updateTransform();npc->setWalkMode(WalkBit::WM_Walk);
    state.movableWalker=npc;
    const auto side=Vec3::normalize(state.movableWalkEnd-state.movableWalkStart);
    pl.setPosition(state.movableWalkStart+Vec3(-side.z,0,side.x)*110.f);pl.updateTransform();
    const auto target=Vec3(state.movableCenter.x,state.movableWalkStart.y,state.movableCenter.z);
    const bool enabled=w.physic()->movableVobsEnabled();
    w.physic()->setMovableVobsEnabled(false);const bool off=npc->testMove(target);
    w.physic()->setMovableVobsEnabled(true);const bool on=npc->testMove(target);
    w.physic()->setMovableVobsEnabled(enabled);
    event("movable_walk_setup","\"off_passable\":"+std::string(off?"true":"false")+
      ",\"on_passable\":"+(on?"true":"false")+",\"actor_id\":"+std::to_string(w.npcId(npc))+
      ",\"table_target_cm\":"+ragdollVector(target)+",\"floor_dish_id\":"+std::to_string(state.movableDish));
    state.movableDestination=WayPoint(state.movableWalkEnd,"FP_MOVABLE_TAVERN_WALK");
    state.movableDestination.groundPos=state.movableWalkEnd;
    movableBegin(w,"walk");npc->aiPush(AiQueue::aiGoToPoint(state.movableDestination));
    }
  else if(c=="movable" && a.size()==2 && a[1]=="cast-ready") prepareMovableCast(w,pl);
  else if(c=="movable" && a.size()==2 && a[1]=="blast") {
    movableBegin(w,"blast");exec("spell cast 600",w,pl,ctrl);
    event("movable_blast","\"spell_id\":"+std::to_string(state.spellCastingId));
    }
  else if(c=="movable" && a.size()==3 && a[1]=="save") {
    try {saveMovableSession(w,a[2]);}
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="movable" && a.size()==2 && a[1]=="reload") {
    size_t tables=0,utensils=0;
    for(const auto& body:w.physic()->movableVobSnapshot()) {tables+=body.className=="table";utensils+=body.className=="utensil";}
    event("movable_session_reload","\"world\":"+jsonStr(w.name())+",\"world_load_ms\":"+
      ragdollNumber(float(w.loadMilliseconds()))+",\"tables\":"+std::to_string(tables)+",\"utensils\":"+
      std::to_string(utensils)+",\"bodies\":"+std::to_string(w.physic()->physicsStats().bodies));
    // The fixture's extra dishes are temporary vobs, outside the map's saved
    // tree. A complete reload must recreate the authored map furniture.
    if(!tables)arenaAbort("full movable save reload has no registered map furniture");
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="movable" && a.size()==3 && a[1]=="measure") {
    if(!state.movableReady || !AppCadence::normalClock() || !AppCadence::arm(a[2]=="on")) {
      arenaAbort("movable normal-game cadence measurement cannot start");return true;
      }
    state.movableMeasuring=true;state.movablePhysics.clear();state.movablePhysics.reserve(20000);state.movableOverflow=false;
    state.noiseMeasuredImpacts=0;state.noiseMeasuredHeard=0;state.noiseMeasuredRays=0;
    for(auto& listener:state.noiseListeners)listener.heard=0;
    AppCadence::counts(true,uint32_t(state.movableIds.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("movable_measure_before","\"enabled\":"+std::string(w.physic()->movableVobsEnabled()?"true":"false")+
      ",\"bodies\":"+movableRows(w));
    }
  else if(c=="movable" && a.size()==2 && a[1]=="report") {
    state.movableMeasuring=false;
    AppCadence::counts(false,uint32_t(state.movableIds.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("movable_measure_after","\"overflow\":"+std::string(state.movableOverflow?"true":"false")+
      ",\"bodies\":"+movableRows(w));
    if(state.noiseTrace)
      event("physics_noise_measure_after","\"enabled\":"+std::string(w.physic()->physicsNoiseEnabled()?"true":"false")+
        ",\"impacts\":"+std::to_string(state.noiseMeasuredImpacts)+",\"perceptions\":"+
        std::to_string(state.noiseMeasuredHeard)+",\"native_rays\":"+std::to_string(state.noiseMeasuredRays)+
        ",\"listeners\":"+noiseListenerRows(w));
    if(auto* file=std::fopen((outDir+"/movable-physics.csv").c_str(),"w")) {
      std::fprintf(file,"sample,t_game_ms,dt_ms,step_ms,native_bodies\n");
      for(size_t i=0;i<state.movablePhysics.size();++i) {const auto& row=state.movablePhysics[i];
        std::fprintf(file,"%zu,%llu,%llu,%.9g,%zu\n",i,(unsigned long long)row.time,(unsigned long long)row.dt,double(row.milliseconds),row.bodies);}
      std::fclose(file);
      } else arenaAbort("cannot write movable native step timing samples");
    }
#endif
  else return false;
  return true;
  }

bool spellCommand(const MovablesContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  auto& state=context.movables;
  const auto& c=a[0];
  if(c=="spell" && a.size()==3 && a[1]=="fixture") {
    prepareSpellFixture(w,pl,a[2]);
    if(!state.spellActors.empty()) exec("watch watched",w,pl,ctrl);
    }
  else if(c=="spell" && (a.size()==2 || a.size()==3) && a[1]=="guard-fixture") {
    prepareSpellFixture(w,pl,"wind");
    if(state.spellActors.empty()) return true;
    const bool water=a.size()==3 && a[2]=="water";
    state.spellGuardCase=water?"water":"cliff";
    auto& site=water?context.questWater:context.questCliff;
    // Find a supported lip within reach of the finite wind impulse. Include
    // caster height and intermediate shelves when choosing the trajectory.
    Vec3 cliffFloor,hero;
    const auto targetOffset=state.spellActors[0].npc->collosionCenter()-state.spellActors[0].npc->position();
    const auto casterOffset=pl.collosionCenter()-pl.position();
    const auto reachesHazard=[&](const Vec3& start,const Vec3& caster) {
      const auto delta=start+targetOffset-caster-casterOffset;
      const float distance=delta.length();
      if(distance<1.f || distance>=1000.f) return false;
      auto velocity=Vec3::normalize(delta/distance+Vec3(0,.35f,0))*(450.f*(1.f-distance/1000.f)/75.f*100.f);
      auto at=start;
      // Reject short landings on the lip or an intermediate shelf. This only
      // chooses a fixture; the recorded throw still comes from the real spell.
      for(unsigned frame=0;frame<180;++frame) {
        at+=velocity/30.f;
        velocity.y-=DynamicWorld::gravityMS*100.f/30.f;
        const auto floor=w.physic()->landRay(Vec3(at.x,start.y+150.f,at.z),6000);
        if(!floor.hasCol) return false;
        if(water) {
          const auto liquid=w.physic()->waterRay(floor.v+Vec3(0,5,0),0);
          if(liquid.hasCol && at.y<liquid.wdepth-150.f && floor.v.y<liquid.wdepth-150.f) return true;
          }
        if(at.y<=floor.v.y)
          return !water && start.y-floor.v.y>1000.f && floor.n.y>.5f;
        }
      return false;
      };
    const auto findLip=[&](QuestSite& site) {
      const auto original=site.dry;
      const auto side=Vec3(-site.direction.z,0,site.direction.x);
      for(float lateral : {0.f,-50.f,50.f,-100.f,100.f,-200.f,200.f}) {
        for(float advance=700;advance>=-150;advance-=10) {
          const auto at=original+site.direction*advance+side*lateral;
          const auto center=w.physic()->landRay(at+Vec3(0,150,0),500);
          bool supported=center.hasCol && center.n.y>.85f;
          for(const auto delta:{Vec3(),site.direction*50.f,side*35.f,side*-35.f,site.direction*-35.f}) {
            const auto floor=w.physic()->landRay(Vec3(at.x,center.v.y,at.z)+delta+Vec3(0,40,0),100);
            supported=supported && floor.hasCol && floor.n.y>.85f && std::abs(floor.v.y-center.v.y)<15.f &&
                      !w.physic()->waterRay(floor.v,15.f).hasCol;
            }
          for(float distance:{25.f,50.f,75.f}) {
            const auto floor=w.physic()->landRay(center.v+site.direction*distance+Vec3(0,40,0),100);
            // A low upward hop can land on a rising lip before reaching the drop.
            supported=supported && (!floor.hasCol || floor.v.y<=center.v.y+2.f);
            }
          if(!supported) continue;
          bool reachableEdge=false;
          for(float distance:{100.f,125.f,150.f}) {
            const auto floor=w.physic()->landRay(center.v+site.direction*distance+Vec3(0,40,0),500);
            if(!floor.hasCol || center.v.y-floor.v.y>60.f) {reachableEdge=true;break;}
            }
          if(!reachableEdge) continue;
          bool hazard=false;
          for(float distance:{100.f,150.f,200.f,350.f,500.f,700.f}) {
            const auto edge=center.v+site.direction*distance;
            const auto floor=w.physic()->landRay(edge+Vec3(0,100,0),6000);
            if(!floor.hasCol || w.physic()->ray(center.v+Vec3(0,110,0),edge+Vec3(0,110,0)).hasCol) continue;
            const auto liquid=w.physic()->waterRay(floor.v+Vec3(0,5,0),0);
            if(water ? !liquid.hasCol || liquid.wdepth-floor.v.y<150.f || liquid.wdepth>center.v.y : center.v.y-floor.v.y<=1200.f) continue;
            cliffFloor=floor.v;
            if(water) site.waterY=liquid.wdepth;
            hazard=true;break;
            }
          if(!hazard) continue;
          for(float distance:{150.f,200.f,250.f,300.f}) {
            const auto caster=center.v-site.direction*distance;
            const auto floor=w.physic()->landRay(caster+Vec3(0,100,0),300);
            if(!floor.hasCol || floor.n.y<.85f ||
               w.physic()->waterRay(floor.v,15.f).hasCol ||
               w.physic()->ray(floor.v+Vec3(0,110,0),center.v+Vec3(0,110,0)).hasCol) continue;
            if(!reachesHazard(center.v,floor.v)) continue;
            site.dry=center.v;hero=floor.v;return true;
            }
          }
        }
      return false;
    };
    if(!chooseQuestSite(w,water,site,findLip)) {arenaAbort("wind guard lacks supported lip and viable throw");return true;}
    auto* npc=state.spellActors[0].npc;
    for(size_t i=1;i<state.spellActors.size();++i) w.removeNpc(*state.spellActors[i].npc);
    state.spellActors.resize(1);context.crew.clear();context.crew.push_back(npc);
    // The hazard site is separate from the spell arena cleared above. Keep
    // nearby monsters from interrupting the real cast with a hurt animation.
    constexpr float clearRadius=3500.f;
    std::vector<Npc*> monsters;
    for(uint32_t i=0;i<w.npcCount();++i)
      if(auto* other=w.npcById(i);other && other->isMonster() &&
         ((other->position()-hero).length()<clearRadius || (other->position()-site.dry).length()<clearRadius))
        monsters.push_back(other);
    for(auto* monster:monsters) w.removeNpc(*monster);
    npc->handle().flags=daedalus::NpcFlag(uint32_t(npc->handle().flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
    npc->handle().attribute[ATR_HITPOINTS]=npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
    npc->setPosition(site.dry);npc->setDirection(site.direction);npc->updateTransform();
    pl.setPosition(hero);pl.setDirection(site.direction);pl.updateTransform();
    Gothic::settingsSetI("GAME","spell_physics",1);Gothic::settingsSetI("GAME","ragdoll_quest_guard",1);
    state.spellGuardTrace=true;state.spellGuardObservedReturns=0;
    context.subject=npc;exec("watch watched",w,pl,ctrl);
    event("spell_guard_setup","\"actor_id\":"+std::to_string(w.npcId(npc))+
      ",\"case\":"+jsonStr(state.spellGuardCase)+",\"water_y_cm\":"+ragdollNumber(site.waterY)+
      ",\"site\":"+jsonStr(site.waypoint)+",\"stable_cm\":"+ragdollVector(site.dry)+
      ",\"hero_cm\":"+ragdollVector(hero)+",\"direction\":"+ragdollVector(site.direction)+
      ",\"cleared_monsters\":"+std::to_string(monsters.size())+",\"clear_radius_cm\":"+ragdollNumber(clearRadius)+
      ",\"cliff_floor_cm\":"+ragdollVector(cliffFloor)+",\"support_margin_cm\":50");
    }
  else if(c=="spell" && a.size()==3 && a[1]=="target") {
    auto actor=std::find_if(state.spellActors.begin(),state.spellActors.end(),[&](const auto& value) {return value.role==a[2];});
    if(actor==state.spellActors.end()) {arenaAbort("spell target role missing");return true;}
    if(a[2]=="troll") {
      actor->npc->setPosition(state.spellActors[0].npc->position()+Vec3(220,0,0));
      actor->npc->updateTransform();
      }
    std::iter_swap(state.spellActors.begin(),actor);
    context.subject=state.spellActors[0].npc;
    exec("watch watched",w,pl,ctrl);
    event("spell_selected_target","\"role\":"+jsonStr(a[2])+",\"actor_id\":"+std::to_string(w.npcId(context.subject)));
    }
  else if(c=="spell" && a.size()==3 && a[1]=="case") {
    state.spellCase=a[2];state.spellTrace=true;
    event("spell_physics_case","\"case\":"+jsonStr(a[2]));
    }
  else if(c=="spell" && a.size()==3 && a[1]=="feature")
    Gothic::settingsSetI("GAME","spell_physics",a[2]=="on");
  else if(c=="spell" && a.size()==3 && a[1]=="equip") {
    const int32_t id=int32_t(num(a,2,0));
    const auto rune=state.spellRunes.find(id);
    if(rune==state.spellRunes.end()) {arenaAbort("requested real spell rune not found");return true;}
    pl.useItem(rune->second,Item::NSLOT,true);
    if(pl.drawSpell(id)) state.spellCastingId=id;
    else {state.spellEquipPending=id;state.spellEquipUntil=w.tickCount()+3000;}
    }
  else if(c=="spell" && a.size()>=2 && a[1]=="cast") {
    if(state.spellActors.empty()) {arenaAbort("real spell needs a target");return true;}
    auto* target=state.spellActors[0].npc;pl.setTarget(target);pl.setDirection(target->position()-pl.position());pl.updateTransform();
    const auto result=pl.beginCastSpell();
    event("spell_physics_cast","\"id\":"+std::to_string(state.spellCastingId)+",\"begin_result\":"+
      std::to_string(int(result))+",\"active_item\":"+std::to_string(pl.bodyObservationState().weaponSymbol)+
      ",\"target_in_air\":"+(target->isInAir()?"true":"false"));
    if(result!=Npc::BC_Invest && result!=Npc::BC_Cast) {arenaAbort("real spell beginCastSpell rejected");return true;}
    state.spellReleaseAt=result==Npc::BC_Invest?w.tickCount()+uint64_t(num(a,2,500)):0;
    }
  else if(c=="spell" && a.size()==3 && a[1]=="wait-return") {
    state.spellWaitFrozen=4;state.spellWaitUntil=w.tickCount()+uint64_t(num(a,2,20000));
    }
  else if(c=="spell" && a.size()>=3 && a[1]=="wait-frozen") {
    state.spellWaitFrozen=a[2]=="on"?1:0;state.spellWaitUntil=w.tickCount()+uint64_t(num(a,3,25000));
    }
  else if(c=="spell" && a.size()>=2 && (a[1]=="wait-airborne" || a[1]=="wait-field")) {
    state.spellWaitFrozen=a[1]=="wait-airborne"?2:3;state.spellWaitUntil=w.tickCount()+uint64_t(num(a,2,6000));
    }
  else if(c=="spell" && a.size()==2 && a[1]=="stop") state.spellTrace=false;
  else if(c=="spell" && a.size()==2 && a[1]=="clear") {
    state.spellTrace=false;state.spellReleaseAt=0;state.spellWaitFrozen=-1;state.spellGuardTrace=false;
    pl.setTarget(nullptr);exec("watch player",w,pl,ctrl);
    for(auto& actor:state.spellActors) w.removeNpc(*actor.npc);
    for(auto* item:state.spellItems) w.removeItem(*item);
    state.spellActors.clear();state.spellItems.clear();context.crew.clear();
    }
  else return false;
  return true;
  }

bool physicsNoiseDebug(const MovablesContext& context,World& world,Npc& player,std::string_view command,
                                  std::string& value,std::string& error) {
  auto& state=context.movables;
  const auto fail=[&](std::string_view message) {error=message;return false;};
  const auto resetCounts=[&] {
    state.noiseBridgeImpacts=0;state.noiseBridgeLoudImpacts=0;state.noiseBridgeCulpritImpacts=0;
    state.noiseBridgeUnseenCulpritImpacts=0;state.noiseBridgeCupImpacts=0;state.noiseBridgeCupMaxRadius=0;
    state.noiseBridgeDialogueImpacts=0;state.noiseBridgeCombatImpacts=0;
    state.noiseBridgeHeard.clear();
    state.noiseBridgeEvents.clear();
    };
  if(command=="setup") {
    world.physic()->setMovableVobsEnabled(true);world.physic()->setPhysicsNoiseEnabled(true);
    prepareNoiseFixture(world,player);
    if(!state.movableReady || !state.noiseHost)return fail("Coragon tavern fixture unavailable in this save");
    world.script().skipImportantInfos(*state.noiseHost,player);
    player.handle().attribute[ATR_HITPOINTS]=100000;
    player.handle().attribute[ATR_HITPOINTSMAX]=100000;
    state.noiseBridgeDebug=true;
    }
  if(!state.noiseBridgeDebug || !state.noiseHost)return fail("physics_noise setup required");
  auto& npc=*state.noiseHost;
  if(command.ends_with("-ready")) {
    noiseRetireListeners();
    const bool quiet=command=="quiet-ready";
    if(command!="loud-ready" && command!="quiet-ready" && command!="dialogue-ready" &&
       command!="combat-ready" && command!="throttle-ready")return fail("unknown physics_noise ready mode");
    const auto source=quiet ? (state.movableWalkStart+state.movableWalkEnd)*.5f : state.movableCenter;
    noiseBegin(world,command,source);
    Vec3 point;
    if(!noiseFloor(world,source,quiet?600.f:500.f,false,point))return fail("supported clear listener floor unavailable");
    if(!noiseAddListener(world,"bridge",point,&npc))return fail("Coragon listener unavailable");
    // The real instigator is behind the listener together with the impact.
    // Its identity must be absent from the NPC's event even though native physics knows it.
    auto hero=source+Vec3(0,0,100);
    if(command=="dialogue-ready" || command=="combat-ready")hero=point+Vec3(0,0,80);
    const auto floor=world.physic()->landRay(hero+Vec3(0,250,0),600);
    if(floor.hasCol)hero.y=floor.v.y;
    player.setPosition(hero);player.setDirection(point-hero);player.updateTransform();
    if(command=="dialogue-ready") {
      npc.clearAiQueue();npc.aiPush(AiQueue::aiProcessInfo(player));
      }
    if(command=="combat-ready") {
      const auto state=world.script().findSymbolIndex("ZS_ATTACK");
      if(state==size_t(-1))return fail("native ZS_ATTACK unavailable");
      npc.clearAiQueue();npc.setTarget(&player);
      npc.aiPush(AiQueue::aiStartState(ScriptFn(state),0,&player,nullptr,""));
      }
    resetCounts();
    }
  else if(command=="loud-hit" || command=="dialogue-hit" || command=="combat-hit" || command=="throttle-hit") {
    if(command=="dialogue-hit" && !Gothic::inst().isNpcInDialog(npc))return fail("actual native dialogue has not opened");
    if(command=="combat-hit" && npc.weaponState()==WeaponState::NoWeapon)return fail("actual native combat has not drawn a weapon");
    if(command!="throttle-hit")resetCounts();
    noisePushTable(world,&player);
    }
  else if(command=="cup-hit") {resetCounts();noiseDropItems(world,1);}
  else if(command!="setup" && command!="sample")return fail("unknown physics_noise command");
  const bool routine=npc.currentState().isValid() && npc.isInRoutine(npc.currentState());
  std::string heard="[";
  for(auto tick:state.noiseBridgeHeard) {if(heard.size()>1)heard+=',';heard+=std::to_string(tick);}
  std::string bridgeEvents="[";
  for(auto tick:state.noiseBridgeEvents) {if(bridgeEvents.size()>1)bridgeEvents+=',';bridgeEvents+=std::to_string(tick);}
  value="{\"npc\":"+jsonStr(symName(world,&npc))+",\"position_cm\":"+ragdollVector(npc.position())+
    ",\"source_cm\":"+ragdollVector(state.noiseSource)+",\"distance_cm\":"+
    ragdollNumber((npc.position()-state.noiseSource).length())+",\"facing_error_deg\":"+
    ragdollNumber(noiseFacingError(npc,state.noiseSource))+",\"can_see_hero\":"+(npc.canSeeNpc(player,false)?"true":"false")+
    ",\"eligible\":"+(npc.canHearPhysicsNoise()?"true":"false")+
    ",\"external\":"+(npc.isExternalControl()?"true":"false")+
    ",\"investigating\":"+(npc.isPhysicsInvestigating()?"true":"false")+
    ",\"player_distance_cm\":"+ragdollNumber((npc.position()-player.position()).length())+
    ",\"dialogue\":"+(Gothic::inst().isNpcInDialog(npc)?"true":"false")+
    ",\"combat\":"+(npc.weaponState()!=WeaponState::NoWeapon?"true":"false")+
    ",\"ai_queue_empty\":"+(npc.isAiQueueEmpty()?"true":"false")+
    ",\"routine_state\":"+(routine?"true":"false")+",\"state\":"+jsonStr(stateName(world,npc))+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"native_impacts\":"+std::to_string(state.noiseBridgeImpacts)+
    ",\"native_loud_impacts\":"+std::to_string(state.noiseBridgeLoudImpacts)+
    ",\"native_culprit_impacts\":"+std::to_string(state.noiseBridgeCulpritImpacts)+
    ",\"native_unseen_culprit_loud_impacts\":"+std::to_string(state.noiseBridgeUnseenCulpritImpacts)+
    ",\"native_cup_impacts\":"+std::to_string(state.noiseBridgeCupImpacts)+
    ",\"native_cup_max_radius_cm\":"+ragdollNumber(state.noiseBridgeCupMaxRadius)+
    ",\"audible_dialogue_loud_impacts\":"+std::to_string(state.noiseBridgeDialogueImpacts)+
    ",\"audible_combat_loud_impacts\":"+std::to_string(state.noiseBridgeCombatImpacts)+
    ",\"native_heard_ms\":"+heard+"],\"bridge_event_ms\":"+bridgeEvents+"]}";
  return true;
  }

void physicsNoise(const MovablesContext& context,const PhysicsNoise& noise,std::string_view sound,bool played) {
  auto& state=context.movables;
  if(state.noiseBridgeDebug && (noise.position-state.noiseSource).quadLength()<250.f*250.f) {
    ++state.noiseBridgeImpacts;
    if(noise.volume>=.35f)++state.noiseBridgeLoudImpacts;
    if(noise.culprit)++state.noiseBridgeCulpritImpacts;
    if(noise.what.find("BEER")!=std::string::npos) {
      ++state.noiseBridgeCupImpacts;
      state.noiseBridgeCupMaxRadius=std::max(state.noiseBridgeCupMaxRadius,noise.radius);
      }
    if(auto* world=Gothic::inst().world();world && state.noiseHost && noise.volume>=.35f) {
      auto& npc=*state.noiseHost;
      const float radius=noise.radius*std::max(0.f,1.f-world->physic()->soundOclusion(npc.mapHeadBone(),noise.position));
      if(npc.qDistTo(noise.position)<=radius*radius) {
        if(noise.culprit && !npc.canSeeNpc(*noise.culprit,false))++state.noiseBridgeUnseenCulpritImpacts;
        if(Gothic::inst().isNpcInDialog(npc))++state.noiseBridgeDialogueImpacts;
        if(npc.weaponState()!=WeaponState::NoWeapon && stateName(*world,npc)=="ZS_ATTACK")++state.noiseBridgeCombatImpacts;
        }
      }
    }
  if(context.scenarioPath==nullptr || !state.noiseTrace)return;
  if(state.movableMeasuring) {++state.noiseMeasuredImpacts;return;}
  const char* material="unknown";
  switch(noise.material) {
    case MAT_WOOD:material="wood";break;
    case MAT_METAL:material="metal";break;
    case MAT_STONE:material="stone";break;
    case MAT_CLAY:material="clay";break;
    case MAT_GLAS:material="glass";break;
    case MAT_LEATHER:material="leather";break;
    default:break;
    }
  auto* world=Gothic::inst().world();
  std::string listeners="[";
  if(world)for(const auto& listener:state.noiseListeners) {
    if(listeners.size()>1)listeners+=',';
    const auto head=listener.npc->mapHeadBone();
    const auto occlusion=world->physic()->soundOclusion(head,noise.position);
    listeners+="{\"role\":"+jsonStr(listener.role)+",\"npc_id\":"+std::to_string(noiseNpcId(listener.npc))+
      ",\"distance_cm\":"+ragdollNumber((head-noise.position).length())+
      ",\"occlusion\":"+ragdollNumber(occlusion)+",\"effective_radius_cm\":"+
      ragdollNumber(noise.radius*(1.f-occlusion))+"}";
    }
  event("physics_noise","\"phase\":"+jsonStr(state.noisePhase)+",\"t_game_ms\":"+
    std::to_string(world?world->tickCount():0)+",\"body\":"+std::to_string(noise.body)+
    ",\"position_cm\":"+ragdollVector(noise.position)+",\"volume\":"+ragdollNumber(noise.volume)+
    ",\"radius_cm\":"+ragdollNumber(noise.radius)+",\"impulse_ns\":"+ragdollNumber(noise.impulse)+
    ",\"material\":"+jsonStr(material)+",\"surface_material\":"+std::to_string(int(noise.surface))+
    ",\"what\":"+jsonStr(noise.what)+",\"culprit\":"+
    (world && noise.culprit?std::to_string(noiseNpcId(noise.culprit)):"null")+
    ",\"sound\":"+jsonStr(sound)+",\"played\":"+(played?"true":"false")+
    ",\"listeners\":"+listeners+"]");
  }

void physicsNoiseHeard(const MovablesContext& context,const Npc& npc,const PhysicsNoise& noise,bool callbackDispatched) {
  auto& state=context.movables;
  if(state.noiseBridgeDebug && &npc==state.noiseHost)
    if(auto* world=Gothic::inst().world())state.noiseBridgeHeard.push_back(world->tickCount());
  if(context.scenarioPath==nullptr || !state.noiseTrace)return;
  if(state.movableMeasuring) {
    ++state.noiseMeasuredHeard;
    for(auto& listener:state.noiseListeners)
      if(listener.npc==&npc)++listener.heard;
    return;
    }
  auto* world=Gothic::inst().world();if(!world)return;
  const auto id=noiseNpcId(&npc);
  ++state.noisePerceptions[id];
  std::string_view role;
  for(auto& listener:state.noiseListeners)
    if(listener.npc==&npc) {++listener.heard;role=listener.role;}
  event("physics_noise_heard","\"phase\":"+jsonStr(state.noisePhase)+",\"t_game_ms\":"+
    std::to_string(world->tickCount())+",\"npc_id\":"+std::to_string(id)+",\"role\":"+jsonStr(role)+
    ",\"body\":"+std::to_string(noise.body)+",\"position_cm\":"+ragdollVector(noise.position)+
    ",\"perception\":"+std::to_string(int(PERC_ASSESSQUIETSOUND))+
    ",\"callback_dispatched\":"+(callbackDispatched?"true":"false")+
    ",\"culprit\":"+(noise.culprit?std::to_string(noiseNpcId(noise.culprit)):"null"));
  }

}
