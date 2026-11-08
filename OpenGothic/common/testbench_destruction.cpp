#include "testbench_internal.h"
#include "testbench.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "gothic.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "resources.h"
#include "world/aiqueue.h"
#include "world/bullet.h"
#include "world/movablevobs.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

size_t arrowBone(const Skeleton& skeleton, std::string_view token) {
  const auto compact=[](std::string_view text) {
    std::string result;
    for(const char value:text) {
      const auto c=static_cast<unsigned char>(value);
      if(std::isalnum(c)) result+=char(std::toupper(c));
      }
    return result;
    };
  const auto wanted=compact(token);
  for(size_t i=0;i<skeleton.nodes.size();++i)
    if(compact(skeleton.nodes[i].name).find(wanted)!=std::string::npos) return i;
  return size_t(-1);
  }

bool destructionOwns(Destruction& state,uintptr_t id) {
  return std::find(state.destructionIds.begin(),state.destructionIds.end(),id)!=state.destructionIds.end();
  }

std::string destructionRows(Destruction& state,World& world) {
  std::string result="[";
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(!destructionOwns(state,body.id))continue;
    if(result.size()>1)result+=',';
    result+="{\"id\":"+std::to_string(body.id)+",\"visual\":"+jsonStr(body.visual)+
      ",\"class\":"+jsonStr(body.className)+",\"destructible\":"+(body.destructible?"true":"false")+
      ",\"broken\":"+(body.broken?"true":"false")+",\"health\":"+ragdollNumber(body.health)+
      ",\"max_health\":"+ragdollNumber(body.maxHealth)+",\"mass_kg\":"+ragdollNumber(body.mass)+
      ",\"sleeping\":"+(body.sleeping?"true":"false")+",\"npc_contact\":"+(body.touchedByNpc?"true":"false")+
      ",\"position_cm\":"+ragdollVector(movablePosition(body.transform))+",\"velocity_cm_s\":"+
      ragdollVector(body.velocity)+"}";
    }
  return result+"]";
  }

std::string destructionFragments(Destruction& state,World& world) {
  std::string result="[";
  for(const auto& part:world.physic()->fragmentSnapshot()) {
    if(!destructionOwns(state,part.parentId))continue;
    if(result.size()>1)result+=',';
    const auto key=std::make_pair(part.parentId,part.piece);
    // Water at the centre of mass (surface at most 50 cm under it) and a static surface straight above
    // within 20 m: a float in a pool below the arena is not a piece under the ground.
    const auto& center=part.centerOfMass;
    const auto water=world.physic()->waterRay(center,50);
    const auto above=world.physic()->rayStatic(center+Vec3(0,2000,0),center);
    result+="{\"parent_id\":"+std::to_string(part.parentId)+",\"piece\":"+std::to_string(part.piece)+",\"bond_group\":"+std::to_string(part.bondGroup)+
      ",\"sleeping\":"+(part.sleeping?"true":"false")+",\"position_cm\":"+ragdollVector(movablePosition(part.transform))+
      ",\"velocity_cm_s\":"+ragdollVector(part.velocity)+",\"max_displacement_cm\":"+
      ragdollNumber(state.destructionFragmentTravel[key])+",\"center_cm\":"+ragdollVector(center)+
      ",\"in_water\":"+(water.hasCol?"true":"false")+",\"under_ground\":"+(above.hasCol?"true":"false")+"}";
    }
  return result+"]";
  }

void destructionFields(Destruction& state,const DestructionContext& context,World& world) {
  if(!state.destructionTrace)return;
  const auto elapsed=world.tickCount()-state.destructionStart;
  if((state.destructionCase=="drop1" || state.destructionCase=="drop5") && elapsed<=2500 &&
     elapsed>=state.destructionFallSample) {
    state.destructionFallSample=elapsed+100;
    event("destruction_fall","\"case\":"+jsonStr(state.destructionCase)+",\"elapsed_ms\":"+
      std::to_string(elapsed)+",\"bodies\":"+destructionRows(state,world));
    }
  const bool sleepSample=state.destructionLastBreak!=0 && !state.destructionSleepSampled &&
                         world.tickCount()-state.destructionLastBreak>=15000;
  if(context.movableMeasuring && !sleepSample)return;
  for(const auto& part:world.physic()->fragmentSnapshot()) {
    if(!destructionOwns(state,part.parentId))continue;
    const auto key=std::make_pair(part.parentId,part.piece);
    const auto point=movablePosition(part.transform);
    const auto [entry,inserted]=state.destructionFragmentOrigins.emplace(key,point);
    state.destructionFragmentTravel[key]=std::max(state.destructionFragmentTravel[key],(point-entry->second).length());
    }
  if(sleepSample) {
    state.destructionSleepSampled=true;
    event("destruction_sleep15","\"case\":"+jsonStr(state.destructionCase)+",\"elapsed_since_last_break_ms\":"+
      std::to_string(world.tickCount()-state.destructionLastBreak)+",\"fragments\":"+destructionFragments(state,world));
    }
  }

uintptr_t destructionAdd(Destruction& state,const DestructionContext& context,World& world,std::string_view name,const Vec3& support) {
  const std::string visual(name);
  const auto* mesh=Resources::loadMesh(visual);const auto* rule=MovableVobs::classifyVisual(visual);
  if(!mesh || !rule)return 0;
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
  fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
  if(fixture->body.isEmpty())return 0;
  fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& pose){view->setObjMatrix(pose);});
  fixture->visual.setupFracture(world,fixture->body);
  state.destructionFixtures.emplace_back(std::move(fixture));
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;})) {
      state.destructionIds.push_back(body.id);context.movableIds.push_back(body.id);return body.id;
      }
  return 0;
  }

bool destructionDropFloor(const DestructionContext& context,World& world,Vec3& result) {
  // A five-metre drop must start under open sky, not intersect the tavern roof.
  for(float distance:{600.f,900.f,1200.f,1600.f,2000.f})
    for(unsigned i=0;i<64;++i) {
      const float angle=float(i)*float(M_PI)/32.f;
      const auto point=context.movableCenter+Vec3(std::cos(angle)*distance,0,std::sin(angle)*distance);
      const auto floor=world.physic()->landRay(point+Vec3(0,180,0),500);
      if(!floor.hasCol || floor.n.y<.95f || std::abs(floor.v.y-context.movableWalkStart.y)>200)continue;
      bool clear=true;
      for(const auto delta:{Vec3(),Vec3(70,0,0),Vec3(-70,0,0),Vec3(0,0,70),Vec3(0,0,-70)}) {
        const auto support=world.physic()->landRay(floor.v+delta+Vec3(0,30,0),60);
        if(!support.hasCol || support.n.y<.95f || std::abs(support.v.y-floor.v.y)>2 ||
           world.physic()->ray(floor.v+delta+Vec3(0,10,0),floor.v+delta+Vec3(0,720,0)).hasCol)clear=false;
        }
      if(clear) {result=floor.v;return true;}
      }
  return false;
  }

bool destructionSupported(World& world,std::string_view visual,const Vec3& at,Vec3& support,Vec3& failed) {
  const auto* mesh=Resources::loadMesh(std::string(visual));
  if(!mesh) {failed=at;return false;}
  const auto* bounds=mesh->bbox();
  const auto size=bounds[1]-bounds[0];
  const auto center=world.physic()->landRay(at+Vec3(0,30,0),80);
  if(!center.hasCol || center.n.y<.8f || std::abs(center.v.y-at.y)>20.f) {failed=at;return false;}
  // Check the whole identity-oriented footprint: all corners, edge midpoints,
  // and centre. The body also needs clear space up to its original mesh top.
  for(int x=-1;x<=1;++x)
    for(int z=-1;z<=1;++z) {
      const auto point=Vec3(at.x+size.x*.5f*float(x),center.v.y,at.z+size.z*.5f*float(z));
      const auto floor=world.physic()->landRay(point+Vec3(0,30,0),80);
      if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-center.v.y)>3.f ||
         world.physic()->ray(point+Vec3(0,2,0),point+Vec3(0,size.y+2.f,0)).hasCol) {
        failed=point;return false;
        }
      }
  support=center.v;return true;
  }

void destructionSetup(Destruction& state,const DestructionContext& context,World& world,Npc& player,std::string_view name) {
  if(!state.destructionReady) {
    if(!context.movableReady)prepareMovableFixture(world,player);
    if(!context.movableReady)return;
    prepareMovableCast(world,player);
    if(context.spellActors.empty())return;
    state.destructionCaster=player.position();state.destructionTarget=context.spellActors.back().npc->position();
    state.destructionDirection=Vec3::normalize(state.destructionTarget-state.destructionCaster);
    state.destructionDirection.y=0;
    state.destructionReady=true;
    }
  noiseRetireListeners();
  if(state.destructionActor && state.destructionActor!=&player) {
    state.destructionActor->clearAiQueue();state.destructionActor->setExternalControl(true);
    }
  state.destructionActor=nullptr;
  state.destructionShooting=false;state.destructionTrace=false;
  state.destructionFixtures.clear();state.destructionIds.clear();
  state.destructionFragmentOrigins.clear();state.destructionFragmentTravel.clear();
  state.destructionCase=name;state.destructionArrows=0;
  player.clearAiQueue();player.clearGoTo();player.stopWalking();player.closeWeapon(true);
  player.setPosition(state.destructionCaster);player.setDirection(state.destructionDirection);player.updateTransform();
  auto direction=state.destructionDirection,side=Vec3(-direction.z,0,direction.x);
  auto source=state.destructionTarget;
  if(name=="drop1" || name=="drop5") {
    if(!destructionDropFloor(context,world,source)) {arenaAbort("destruction drop needs flat open-sky native ground");return;}
    const float height=name=="drop1"?100.f:500.f;
    if(!destructionAdd(state,context,world,"NW_HARBOUR_CRATE_01.3DS",source+Vec3(0,height,0))) {
      arenaAbort("destruction drop crate insertion failed");return;
      }
    player.setPosition(source-direction*250.f);player.updateTransform();
    context.cam=C_Hold;lookFrom(source-direction*450.f+side*180.f+Vec3(0,250,0),source+Vec3(0,200,0));
    event("destruction_drop_setup","\"case\":"+jsonStr(name)+",\"height_cm\":"+ragdollNumber(height)+
      ",\"floor_cm\":"+ragdollVector(source)+",\"clear_vertical_cm\":720");
    }
  else if(name=="fireball" || name=="five") {
    const unsigned count=name=="five"?5:2;
    // The functional pair sits beyond the edge of the authored tavern table.
    if(name=="five") {
      // The functional pair stays in Coragon's tavern. Performance uses real
      // open stock-world ground, where the complete ring and its flight lane
      // fit without walls or unsupported footprint corners.
      if(const auto* anchor=world.findPoint("NW_XARDAS_TOWER_VALLEY_091",false)) {
        player.setPosition(anchor->pos);player.updateTransform();
        }
      if(!chooseRagdollArena(world,player)) {arenaAbort("destruction five needs native open stock-world ground");return;}
      const auto original=context.ragdollArenaTarget-Vec3(0,80,0);
      direction=Vec3(0,0,1);side=Vec3(-1,0,0);
      bool found=false;Vec3 caster;
      for(float forward:{0.f,-100.f,100.f,-200.f,200.f}) {
        for(float lateral:{0.f,-100.f,100.f,-200.f,200.f}) {
          const auto candidate=original+direction*forward+side*lateral;
          const auto floor=world.physic()->landRay(candidate+Vec3(0,300,0),900);
          if(!floor.hasCol || floor.n.y<.98f)continue;
          auto center=candidate;center.y=floor.v.y;
          bool supported=true;Vec3 failed;
          for(unsigned i=0;i<count;++i) {
            const float angle=float(i)*float(2*M_PI)/float(count)+float(M_PI)/10.f;
            const auto at=center+(side*std::cos(angle)+direction*std::sin(angle))*140.f;
            Vec3 support;
            if(!destructionSupported(world,(i%2)?"NW_HARBOUR_BARREL_01.3DS":"NW_HARBOUR_CRATE_01.3DS",at,support,failed)) {
              supported=false;break;
              }
            // The native blast must reach every original centre through the
            // actual world mesh, before any fixture body is inserted.
            const auto* mesh=Resources::loadMesh((i%2)?"NW_HARBOUR_BARREL_01.3DS":"NW_HARBOUR_CRATE_01.3DS");
            const auto height=mesh->bbox()[1].y-mesh->bbox()[0].y;
            if(world.physic()->ray(center+Vec3(0,92,0),support+Vec3(0,height*.5f,0)).hasCol) {
              supported=false;failed=at;break;
              }
            }
          auto hero=center-direction*500.f;
          const auto heroFloor=world.physic()->landRay(hero+Vec3(0,300,0),900);
          if(!heroFloor.hasCol || heroFloor.n.y<.98f || std::abs(heroFloor.v.y-center.y)>30.f) supported=false;
          else {
            hero.y=heroFloor.v.y;
            if(world.physic()->ray(hero+Vec3(0,145,0),center+Vec3(0,92,0)).hasCol ||
               world.physic()->rayNpc(hero+Vec3(0,100,0),center+Vec3(0,100,0),&player).npcHit!=nullptr) supported=false;
            }
          event("destruction_ring_candidate","\"source_cm\":"+ragdollVector(center)+",\"supported\":"+
            (supported?"true":"false")+",\"failed_probe_cm\":"+(supported?"null":ragdollVector(failed)));
          if(supported) {source=center;caster=hero;found=true;break;}
          }
        if(found)break;
        }
      if(!found) {arenaAbort("destruction five ring lacks native support, open blast rays, or clear caster lane");return;}
      state.destructionCaster=caster;state.destructionTarget=source;state.destructionDirection=direction;
      player.setPosition(caster);player.setDirection(direction);player.updateTransform();
      auto* target=context.spellActors[0].npc;target->setPosition(source);target->setDirection(caster-source);target->updateTransform();
      event("destruction_site","\"case\":\"five\",\"location\":\"stock_outdoor_arena\",\"waypoint\":"+
        jsonStr(context.ragdollArenaWaypoint)+",\"caster_cm\":"+ragdollVector(caster)+",\"target_cm\":"+ragdollVector(source));
      }
    for(unsigned i=0;i<count;++i) {
      // Put a gap towards the caster, so the rear crate cannot intercept the
      // real fireball before it reaches the NPC at the centre of the ring.
      const float angle=float(i)*float(2*M_PI)/float(count)+float(M_PI)/10.f;
      const auto at=name=="five" ? source+(side*std::cos(angle)+direction*std::sin(angle))*140.f :
                                      source+side*(i?100.f:-100.f)+direction*40.f;
      // Start below tabletop height: the ground query must not mistake
      // nearby authored furniture for the floor of the tavern.
      const auto floor=world.physic()->landRay(at+Vec3(0,30,0),80);
      const bool supported=floor.hasCol && floor.n.y>.8f && std::abs(floor.v.y-source.y)<=20.f;
      event("destruction_preparation","\"case\":"+jsonStr(name)+",\"ordinal\":"+std::to_string(i)+
        ",\"at_cm\":"+ragdollVector(at)+",\"has_floor\":"+(floor.hasCol?"true":"false")+
        ",\"floor_cm\":"+ragdollVector(floor.v)+",\"normal\":"+ragdollVector(floor.n)+
        ",\"supported\":"+(supported?"true":"false"));
      if(!supported || !destructionAdd(state,context,world,(i%2)?"NW_HARBOUR_BARREL_01.3DS":"NW_HARBOUR_CRATE_01.3DS",floor.v)) {
        arenaAbort("destruction fireball needs supported real crate/barrel positions");return;
        }
      }
    if(name=="five") {
      const auto blast=context.spellActors[0].npc->collosionCenter();
      const bool flightClear=!world.physic()->ray(state.destructionCaster+Vec3(0,145,0),blast).hasCol;
      unsigned visible=0;
      for(const auto& body:world.physic()->movableVobSnapshot()) {
        if(!destructionOwns(state,body.id))continue;
        auto center=(body.localBounds[0]+body.localBounds[1])*.5f;body.transform.project(center);
        const auto hit=world.physic()->ray(blast,center);
        // The final intersection is with the tested object's own shell.
        if(!hit.hasCol || (hit.v-center).length()<=85.f)++visible;
        }
      event("destruction_five_los","\"flight_clear\":"+std::string(flightClear?"true":"false")+
        ",\"unobstructed_bodies\":"+std::to_string(visible)+",\"blast_cm\":"+ragdollVector(blast));
      if(!flightClear || visible!=count) {arenaAbort("destruction five native bodies obstruct the real fireball or one another");return;}
      }
    if(name=="fireball") {
      noiseBegin(world,name,source+Vec3(0,60,0));
      Vec3 near;bool listenerFloor=false;
      // The same blast also knocks the authored table. Its earlier impact has
      // a 7.2m hearing radius; a listener beyond it can identify the louder break
      // through the unchanged native hearing cooldown and acoustic queries.
      for(float distance:{1000.f,1100.f,1200.f})
        if(noiseFloor(world,context.noiseSource,distance,false,near)) {listenerFloor=true;break;}
      if(!listenerFloor) {arenaAbort("destruction needs a clear supported 10-12m listener floor");return;}
      if(!noiseAddListener(world,"destruction-near",near))return;
      event("destruction_listener_setup","\"case\":"+jsonStr(name)+",\"position_cm\":"+ragdollVector(near)+
        ",\"break_source_cm\":"+ragdollVector(context.noiseSource)+",\"distance_cm\":"+
        ragdollNumber((near-context.noiseSource).length())+",\"table_distance_cm\":"+
        ragdollNumber((near-context.movableCenter).length()));
      } else context.noiseTrace=false;
    destructionCamera(world,source,direction);
    }
  else if(name=="arrows" || name=="walk-hero" || name=="walk-npc") {
    source=state.destructionTarget+direction*80.f;
    // The fireball target is no longer a participant in these cases.
    auto* spellTarget=context.spellActors[0].npc;
    spellTarget->setPosition(state.destructionCaster);spellTarget->updateTransform();
    const auto floor=world.physic()->landRay(source+Vec3(0,30,0),80);
    if(!floor.hasCol || !destructionAdd(state,context,world,"NW_HARBOUR_CRATE_01.3DS",floor.v)) {
      arenaAbort("destruction single-crate insertion failed");return;
      }
    source=floor.v;
    if(name!="arrows") {
      const auto approach=direction*-1.f;
      auto start=source-approach*170.f;
      const auto ground=world.physic()->landRay(start+Vec3(0,80,0),180);
      if(!ground.hasCol) {arenaAbort("destruction walking actor has no native floor");return;}
      start.y=ground.v.y;
      auto* actor=name=="walk-hero"?&player:world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),start);
      if(!actor) {arenaAbort("destruction walking actor unavailable");return;}
      actor->clearAiQueue();actor->clearState(false);actor->stopWalking();actor->closeWeapon(true);
      actor->setPosition(start);actor->setDirection(approach);actor->updateTransform();actor->setWalkMode(WalkBit::WM_Walk);
      if(actor!=&player) {actor->setExternalControl(false);actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
        player.setPosition(start+side*160.f);player.updateTransform();}
      state.destructionActor=actor;state.destructionActorStart=start;
      state.destructionDestination=WayPoint(source+approach*170.f,"FP_DESTRUCTION_CRATE_WALK");
      state.destructionDestination.groundPos=source+approach*170.f;
      }
    context.cam=C_Hold;lookFrom(source-direction*280.f+side*230.f+Vec3(0,190,0),source+Vec3(0,55,0));
    }
  else {arenaAbort("unknown destruction fixture case");return;}
  state.destructionStart=world.tickCount();
  event("destruction_setup","\"case\":"+jsonStr(name)+",\"world\":"+jsonStr(world.name())+
    ",\"host\":"+jsonStr(symName(world,context.noiseHost))+",\"enabled\":"+
    (world.physic()->destructionEnabled()?"true":"false")+",\"location\":"+
    jsonStr(name=="five"?"stock_outdoor_arena":name=="fireball"?"coragon_tavern":"coragon_area")+",\"bodies\":"+destructionRows(state,world)+
    ",\"spell_target_cm\":"+ragdollVector(context.spellActors[0].npc->collosionCenter()));
  }

void destructionBegin(Destruction& state,const DestructionContext& context,World& world) {
  state.destructionTrace=true;state.destructionStart=world.tickCount();
  state.destructionSupportNext=1000;
  state.destructionLastBreak=0;state.destructionSleepSampled=false;
  state.destructionFallSample=0;
  state.destructionFragmentOrigins.clear();state.destructionFragmentTravel.clear();
  if(state.destructionCase=="arrows") {state.destructionShooting=true;state.destructionNextArrow=world.tickCount();}
  if(state.destructionCase=="drop1" || state.destructionCase=="drop5")
    for(auto& fixture:state.destructionFixtures)fixture->body.addImpulse(Vec3(0,-.001f,0));
  if(state.destructionCase=="walk-npc" && state.destructionActor)
    state.destructionActor->aiPush(AiQueue::aiGoToPoint(state.destructionDestination));
  unsigned pins=0;
  for(size_t i=0;i<world.stuckArrowCount();++i) {uint32_t zenId;Matrix4x4 local;if(world.stuckArrowAttachment(i,zenId,local))++pins;}
  event("destruction_begin","\"case\":"+jsonStr(state.destructionCase)+",\"bodies\":"+destructionRows(state,world)+
    ",\"world_pinned_arrows\":"+std::to_string(pins)+",\"spell_target_cm\":"+
    ragdollVector(context.spellActors[0].npc->collosionCenter())+",\"spell_target\":"+
    jsonStr(symName(world,context.spellActors[0].npc))+",\"spell_target_npc_id\":"+
    std::to_string(world.npcId(context.spellActors[0].npc)));
  }

void destructionShoot(Destruction& state,World& world,Npc& player) {
  if(!state.destructionShooting || world.tickCount()<state.destructionNextArrow)return;
  const auto all=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(all.begin(),all.end(),[&state](const auto& body){return destructionOwns(state,body.id);});
  if(found==all.end() || found->broken || state.destructionArrows>=30) {state.destructionShooting=false;return;}
  const auto* mesh=Resources::loadMesh(found->visual);if(!mesh) {arenaAbort("destruction arrow crate mesh missing");return;}
  const auto* bounds=mesh->bbox();
  auto center=(bounds[0]+bounds[1])*.5f;found->transform.project(center);
  auto top=Vec3((bounds[0].x+bounds[1].x)*.5f,bounds[1].y,(bounds[0].z+bounds[1].z)*.5f);found->transform.project(top);
  const auto from=top+Vec3(0,80,0);
  const auto hit=world.physic()->rayMovableVob(from,center);
  if(!hit || hit->visual!=found->visual || (movablePosition(hit->transform)-movablePosition(found->transform)).length()>1.f) {
    arenaAbort("destruction arrow has no exposed native path to its exact crate");return;
    }
  const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
  if(symbol==size_t(-1)) {arenaAbort("destruction arrow instance unavailable");return;}
  Item arrow(world,symbol,Item::T_Inventory);
  auto& bullet=world.shootArrow(arrow,from,Vec3::normalize(center-from),DynamicWorld::bulletSpeed);
  bullet.setOrigin(&player);bullet.setPrecise(1.f);
  ++state.destructionArrows;state.destructionNextArrow=world.tickCount()+500;
  event("destruction_arrow","\"ordinal\":"+std::to_string(state.destructionArrows)+",\"health_before\":"+
    ragdollNumber(found->health)+",\"from_cm\":"+ragdollVector(from)+",\"to_cm\":"+ragdollVector(center));
  }

void destructionCheckpoint(Destruction& state,const DestructionContext& context,World& world) {
  destructionFields(state,context,world);
  unsigned attached=0;
  for(size_t i=0;i<world.stuckArrowCount();++i) {
    uint32_t zenId;Matrix4x4 local;
    if(world.stuckArrowAttachment(i,zenId,local))++attached;
    }
  event("destruction_checkpoint","\"case\":"+jsonStr(state.destructionCase)+",\"elapsed_ms\":"+
    std::to_string(world.tickCount()-state.destructionStart)+",\"arrows\":"+std::to_string(state.destructionArrows)+
    ",\"bodies\":"+destructionRows(state,world)+",\"fragments\":"+destructionFragments(state,world)+
    ",\"listeners\":"+noiseListenerRows(world)+",\"world_pinned_arrows\":"+std::to_string(attached)+
    ",\"actor_start_cm\":"+ragdollVector(state.destructionActorStart)+",\"actor_cm\":"+
    (state.destructionActor?ragdollVector(state.destructionActor->position()):"null"));
  state.destructionShooting=false;
  }

std::string destructionFloorProbe(const Npc& npc,std::string_view label,const Vec3& point) {
  const auto root=npc.position();
  const Vec3 from(point.x,std::max(root.y+20.f,point.y+20.f),point.z);
  const Vec3 to(point.x,std::min(root.y,point.y)-600.f,point.z);
  const auto hit=npc.world().physic()->landRay(from,from.y-to.y);
  return "{\"label\":"+jsonStr(label)+",\"reference_cm\":"+ragdollVector(point)+
    ",\"ray_from_cm\":"+ragdollVector(from)+",\"ray_to_cm\":"+ragdollVector(to)+
    ",\"found\":"+(hit.hasCol?"true":"false")+
    ",\"point_cm\":"+(hit.hasCol?ragdollVector(hit.v):"null")+
    ",\"normal\":"+(hit.hasCol?ragdollVector(hit.n):"null")+
    ",\"material\":"+std::to_string(int(hit.mat))+
    ",\"sector\":"+(hit.sector?jsonStr(hit.sector):"null")+
    ",\"interactive\":"+(hit.vob?"true":"false")+
    ",\"vertical_delta_cm\":"+(hit.hasCol?ragdollNumber(point.y-hit.v.y):"null")+"}";
  }

void destructionSupportSample(Destruction& state,const DestructionContext& context,World& world) {
  if(context.spellActors.empty() || context.spellActors[0].npc==nullptr) {
    event("destruction_error","\"reason\":\"support probe has no native fireball target\"");
    return;
    }
  const auto& npc=*context.spellActors[0].npc;
  state.destructionSkinSample="null";
  state.destructionSkinRequest=&npc;
  const bool drawable=TestBench::ragdollBodyDrawable(npc);
  state.destructionSkinRequest=nullptr;
  const auto* rig=npc.ragdollBody()!=nullptr ? npc.ragdollBody() : npc.livingBody();
  std::vector<Matrix4x4> native;
  const bool nativePose=rig && rig->getPose(native);
  std::string bones="[";
  if(const auto* skeleton=npc.visualSkeleton())
    for(std::string_view name:{"BIP01 PELVIS","BIP01 SPINE2","BIP01 L THIGH","BIP01 L CALF","BIP01 L FOOT"}) {
      const auto bone=arrowBone(*skeleton,name);
      if(bone==size_t(-1))continue;
      if(bones.size()>1)bones+=',';
      bones+="{\"name\":"+jsonStr(name)+",\"bone\":"+std::to_string(bone)+
        ",\"render\":"+ragdollMatrix(npc.renderedBone(bone))+
        ",\"animation\":"+ragdollMatrix(npc.animationBone(bone));
      if(nativePose && bone<native.size())bones+=",\"native\":"+ragdollMatrix(native[bone]);
      bones+='}';
      }
  bones+=']';
  std::string contacts="[";
  size_t supports=0,invisible=0,unknown=0;
  float maximumGap=0;
  if(rig!=nullptr)
    for(const auto& contact:world.physic()->ragdollSupports(*rig,2.f)) {
      const bool support=contact.axis.y<-.25f;
      // Source-triangle contacts may penetrate the floor. Report their signed
      // separation, while hulls retain a distance that exposes empty cavities.
      const float visibleGap=contact.sourceTriangle!=uint32_t(-1) ?
          Vec3::dotProduct(contact.point1-contact.visiblePoint,contact.axis*-1.f) : contact.visibleDistance;
      if(support) {
        ++supports;
        if(!contact.visible)++invisible;
        if(contact.visibleDistance<0.f)++unknown;
        else maximumGap=std::max(maximumGap,visibleGap);
        }
      if(contacts.size()>1)contacts+=',';
      contacts+="{\"bone\":"+std::to_string(contact.bone)+
        ",\"part_body_id\":"+std::to_string(contact.partBodyId)+
        ",\"support_body_id\":"+std::to_string(contact.supportBodyId)+
        ",\"layer\":"+std::to_string(contact.category)+",\"kind\":"+jsonStr(contact.kind)+
        ",\"visual\":"+jsonStr(contact.visual)+",\"parent_id\":"+std::to_string(contact.parentId)+
        ",\"zen_id\":"+std::to_string(contact.zenId)+",\"piece\":"+
        (contact.piece==size_t(-1)?"null":std::to_string(contact.piece))+
        ",\"source_triangle\":"+(contact.sourceTriangle==uint32_t(-1)?"null":std::to_string(contact.sourceTriangle))+
        ",\"sleeping\":"+(contact.sleeping?"true":"false")+
        ",\"broken\":"+(contact.broken?"true":"false")+
        ",\"visible\":"+(contact.visible?"true":"false")+
        ",\"supports_weight\":"+(support?"true":"false")+
        ",\"depth_cm\":"+ragdollNumber(contact.depth)+
        ",\"normal\":"+ragdollVector(contact.axis*-1.f)+
        ",\"part_contact_cm\":"+ragdollVector(contact.point1)+
        ",\"support_contact_cm\":"+ragdollVector(contact.point2)+
        ",\"visible_distance_cm\":"+(contact.visibleDistance<0.f?"null":ragdollNumber(contact.visibleDistance))+
        ",\"visible_gap_cm\":"+(contact.visibleDistance<0.f?"null":ragdollNumber(visibleGap))+
        ",\"visible_penetration_cm\":"+(contact.visibleDistance<0.f?"null":ragdollNumber(std::max(0.f,-visibleGap)))+
        ",\"visible_point_cm\":"+(contact.visibleDistance<0.f?"null":ragdollVector(contact.visiblePoint))+"}";
      }
  contacts+=']';
  event("destruction_support_sample","\"elapsed_ms\":"+std::to_string(world.tickCount()-state.destructionStart)+
    ",\"actor\":"+jsonStr(symName(world,&npc))+",\"actor_id\":"+std::to_string(world.npcId(&npc))+
    ",\"body_drawable\":"+(drawable?"true":"false")+
    ",\"ragdoll\":"+(npc.ragdollBody()?"true":"false")+
    ",\"rig_kind\":"+jsonStr(npc.ragdollBody()?"ragdoll":npc.livingBody()?"living":"none")+
    ",\"kinematic\":"+(rig && rig->isKinematic()?"true":"false")+
    ",\"sleeping\":"+(rig && rig->isSleeping()?"true":"false")+
    ",\"reaction_weight\":"+ragdollNumber(rig?rig->reactionWeight():0.f)+
    ",\"ragdoll_enabled\":"+(Gothic::physicsFlags().ragdoll?"true":"false")+
    ",\"spell_physics_enabled\":"+(Gothic::physicsFlags().spellPhysics?"true":"false")+
    ",\"npc_character_virtual\":"+(Gothic::physicsFlags().npcCharacterVirtual?"true":"false")+
    ",\"cv_active\":"+(npc.characterVirtualActive()?"true":"false")+
    ",\"visual_root_cm\":"+ragdollVector(npc.characterVisualPosition())+
    ",\"external_control\":"+(npc.isExternalControl()?"true":"false")+
    ",\"process_policy\":"+std::to_string(int(npc.processPolicy()))+
    ",\"body_state\":"+std::to_string(int(npc.bodyState()))+
    ",\"state\":"+jsonStr(stateName(world,npc))+",\"state_time\":"+std::to_string(npc.stateTime())+
    ",\"quest_return_count\":"+std::to_string(npc.ragdollQuestReturnCount())+
    ",\"quest_return_armed\":"+(npc.ragdollQuestReturnState().armed?"true":"false")+
    ",\"quest_return_hazards\":"+std::to_string(npc.ragdollQuestReturnState().hazards)+
    ",\"animations\":"+jsonStr(npc.animNames())+
    ",\"hp\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
    ",\"dead\":"+(npc.isDead()?"true":"false")+
    ",\"down\":"+(npc.isDown()?"true":"false")+
    ",\"in_air\":"+(npc.isInAir()?"true":"false")+
    ",\"falling\":"+(npc.isFalling()?"true":"false")+
    ",\"standing\":"+(npc.isStanding()?"true":"false")+
    ",\"spell_velocity_cm_s\":"+ragdollVector(npc.spellVelocity())+
    ",\"bones\":"+bones+",\"rendered_skin\":"+state.destructionSkinSample+
    ",\"position_cm\":"+ragdollVector(npc.position())+",\"supports\":"+std::to_string(supports)+
    ",\"invisible_supports\":"+std::to_string(invisible)+",\"unknown_supports\":"+std::to_string(unknown)+
    ",\"maximum_visible_gap_cm\":"+ragdollNumber(maximumGap)+",\"contacts\":"+contacts);
  }

bool destructionMeleeEquip(World& world,Npc& actor,std::string_view weapon) {
  actor.clearAiQueue();actor.clearState(false);actor.clearGoTo();actor.stopWalking();actor.closeWeapon(true);
  actor.setTarget(nullptr);actor.setExternalControl(true);
  actor.handle().attribute[ATR_STRENGTH]=100;
  actor.setTalentSkill(TALENT_1H,2);actor.setTalentSkill(TALENT_2H,2);
  if(weapon=="fist")actor.setToFistMode();
  else {
    const auto symbol=world.script().findSymbolIndex(weapon=="1h"?"ITMW_1H_COMMON_01":"ITMW_2H_BAU_AXE");
    if(symbol==size_t(-1)) {arenaAbort("destruction melee stock weapon symbol absent");return false;}
    actor.setToFightMode(symbol);
    const auto* equipped=actor.currentMeleeWeapon();
    if(!equipped || equipped->clsId()!=symbol || equipped->is2H()!=(weapon=="2h")) {
      arenaAbort("destruction melee stock weapon did not equip");return false;
      }
    }
  return true;
  }

bool destructionMeleeProbe(Destruction& state,World& world,Npc& actor,const DynamicWorld::MovableVobSnapshot& body,std::string_view stage) {
  const auto& guild=world.script().guildVal();const auto gl=actor.guild();
  const bool fist=actor.weaponState()==WeaponState::Fist;
  const auto* weapon=fist?nullptr:actor.currentMeleeWeapon();
  const float reach=float(guild.fight_range_base[gl]+(fist?guild.fight_range_fist[gl]:
    weapon && weapon->is2H()?guild.fight_range_2ha[gl]:guild.fight_range_1ha[gl]))+float(weapon?weapon->swordLength():0);
  const auto origin=actor.collosionCenter();
  unsigned contacts=0;float closest=std::numeric_limits<float>::infinity();Vec3 point;
  for(int yaw=-(fist?20:60);yaw<=(fist?20:60);yaw+=10)
    for(int pitch=-40;pitch<=20;pitch+=20) {
      const float a=actor.rotationRad()+float(yaw)*float(M_PI)/180.f,p=float(pitch)*float(M_PI)/180.f;
      const Vec3 direction(std::cos(a)*std::cos(p),std::sin(p),std::sin(a)*std::cos(p));
      const auto end=origin+direction*reach;
      const auto target=world.physic()->rayMovableVob(origin,end);
      if(!target || target->visual!=body.visual || target->zenId!=body.zenId ||
         (movablePosition(target->transform)-movablePosition(body.transform)).length()>1.f)continue;
      const auto hit=world.physic()->ray(origin,end);
      if(!hit.hasCol)continue;
      ++contacts;const float distance=(hit.v-origin).length();
      if(distance<closest) {closest=distance;point=hit.v;}
      }
  auto local=point;auto inverse=body.transform;inverse.inverse();inverse.project(local);
  event("destruction2_melee_probe","\"case\":"+jsonStr(state.destructionCase)+",\"stage\":"+jsonStr(stage)+
    ",\"actor_cm\":"+ragdollVector(actor.position())+",\"origin_cm\":"+ragdollVector(origin)+
    ",\"reach_cm\":"+ragdollNumber(reach)+",\"contact_rays\":"+std::to_string(contacts)+
    ",\"closest_contact_cm\":"+(contacts?ragdollNumber(closest):"null")+
    ",\"contact_point_cm\":"+(contacts?ragdollVector(point):"null")+
    ",\"contact_local_cm\":"+(contacts?ragdollVector(local):"null"));
  return contacts>0;
  }

void destructionMeleeSetup(Destruction& state,const DestructionContext& context,World& world,Npc& player,std::string_view kind,std::string_view weapon,bool npc) {
  if((kind!="crate" && kind!="barrel" && kind!="fence") || (weapon!="fist" && weapon!="1h" && weapon!="2h")) {
    arenaAbort("destruction melee expects crate|barrel|fence and fist|1h|2h");return;
    }
  state.destructionMeleeRunning=false;state.destructionTrace=false;
  state.destructionFixtures.clear();state.destructionIds.clear();
  if(state.destructionMeleeNpc) {
    world.removeNpc(*state.destructionMeleeNpc);state.destructionMeleeNpc=nullptr;
    }
  if(!state.destructionMeleeArena) {
    if(const auto* anchor=world.findPoint("NW_XARDAS_TOWER_VALLEY_091",false)) {
      player.setPosition(anchor->pos);player.updateTransform();
      }
    if(!chooseRagdollArena(world,player)) {arenaAbort("destruction melee needs native flat outdoor ground");return;}
    state.destructionMeleeArena=true;
    }
  const auto source=context.ragdollArenaTarget-Vec3(0,80,0);
  std::string visual=kind=="barrel"?"NW_HARBOUR_BARREL_01.3DS":"NW_HARBOUR_CRATE_01.3DS";
  if(kind=="fence") {
    visual.clear();
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(body.className=="fence") {visual=body.visual;break;}
    if(visual.empty()) {arenaAbort("destruction melee stock fence mesh is absent");return;}
    }
  if(!destructionAdd(state,context,world,visual,source)) {arenaAbort("destruction melee native mesh insertion failed");return;}
  const auto bodies=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(bodies.begin(),bodies.end(),[&state](const auto& body){return destructionOwns(state,body.id);});
  if(found==bodies.end() || !found->destructible || found->broken) {
    arenaAbort("destruction melee needs intact cache-backed native prop");return;
    }
  auto center=(found->localBounds[0]+found->localBounds[1])*.5f;found->transform.project(center);
  const float extent=(found->localBounds[1].z-found->localBounds[0].z)*.5f;
  auto stand=center-Vec3(0,0,extent+75.f);
  // This upright authored post spans the full strike height. The middle of
  // NW_NATURE_FENCE_01 is a gap between two narrow horizontal rails.
  if(kind=="fence")stand.x+=100.f;
  const auto floor=world.physic()->landRay(Vec3(stand.x,source.y+80,stand.z),180);
  if(!floor.hasCol) {arenaAbort("destruction melee actor has no native floor");return;}
  stand.y=floor.v.y;
  // The arena is an isolated acceptance fixture. It never changes map-vob IDs.
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* other=world.npcById(id);other && other!=&player && (other->position()-source).length()<2300.f) {
      other->clearAiQueue();other->clearState(false);other->stopWalking();other->setExternalControl(true);
      }
  auto* actor=&player;
  if(npc) {
    actor=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),stand);
    if(!actor) {arenaAbort("destruction melee NPC actor insertion failed");return;}
    state.destructionMeleeNpc=actor;
    }
  if(!destructionMeleeEquip(world,*actor,weapon))return;
  actor->setPosition(stand);actor->setDirection(Vec3(0,0,1));actor->updateTransform();
  if(npc) {player.closeWeapon(true);player.setPosition(stand+Vec3(200,0,-100));player.updateTransform();}
  state.destructionMeleeActor=actor;state.destructionMeleeFacing=Vec3(0,0,1);
  state.destructionMeleeStand=stand;state.destructionMeleeRestance=kind=="fence";
  state.destructionMeleeAttempts=state.destructionMeleeHits=0;
  state.destructionCase=std::string(kind)+"-"+std::string(weapon)+(npc?"-npc":"");
  state.destructionStart=world.tickCount();state.destructionTrace=true;
  if(!destructionMeleeProbe(state,world,*actor,*found,"setup")) {
    arenaAbort("destruction melee stance has no real wood contact inside native attack reach");return;
    }
  context.cam=C_Hold;lookFrom(center+Vec3(250,230,-310),center+Vec3(0,35,0));
  const auto* item=weapon=="fist"?nullptr:actor->currentMeleeWeapon();
  event("destruction2_melee_setup","\"case\":"+jsonStr(state.destructionCase)+",\"class\":"+jsonStr(kind)+
    ",\"weapon\":"+jsonStr(weapon)+",\"actor\":"+jsonStr(npc?"npc":"hero")+",\"strength\":100,\"weapon_damage\":"+
    std::to_string(item?item->handle().damage_total:0)+",\"weapon_range_cm\":"+std::to_string(item?item->swordLength():0)+
    ",\"actor_cm\":"+ragdollVector(stand)+",\"bodies\":"+destructionRows(state,world));
  }

void destructionMeleeTick(Destruction& state,const DestructionContext& context,World& world) {
  if(!state.destructionMeleeRunning || !state.destructionMeleeActor)return;
  if(state.destructionMeleeBlows>0 && state.destructionMeleeAttempts>=state.destructionMeleeBlows) {
    if(world.tickCount()>=state.destructionMeleeNext) {state.destructionMeleeRunning=false;context.waitUntil=0;}
    return;
    }
  const auto bodies=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(bodies.begin(),bodies.end(),[&state](const auto& body){return destructionOwns(state,body.id);});
  if(found==bodies.end() || world.tickCount()>=state.destructionMeleeDeadline) {
    state.destructionMeleeRunning=false;context.waitUntil=0;
    arenaAbort("destruction melee timed out before native animation broke its prop");return;
    }
  if(found->broken) {
    state.destructionMeleeRunning=false;context.waitUntil=context.time+1000;
    event("destruction2_melee_complete","\"case\":"+jsonStr(state.destructionCase)+",\"attempts\":"+
      std::to_string(state.destructionMeleeAttempts)+",\"hits\":"+std::to_string(state.destructionMeleeHits)+
      ",\"bodies\":"+destructionRows(state,world)+",\"fragments\":"+destructionFragments(state,world));return;
    }
  if(world.tickCount()<state.destructionMeleeNext)return;
  auto& actor=*state.destructionMeleeActor;
  // Each fence sample starts outside its post. Forward fist root motion would
  // otherwise carry repeated attacks into the open rail gap after two swings.
  // The animation and its impact run freely between these setup boundaries.
  if(state.destructionMeleeRestance) {
    actor.stopWalking();actor.clearSpeed();actor.setPosition(state.destructionMeleeStand);
    actor.setDirection(state.destructionMeleeFacing);actor.updateTransform();
    if(!destructionMeleeProbe(state,world,actor,*found,"before-animation")) {
      state.destructionMeleeRunning=false;arenaAbort("destruction melee reset stance lost native wood contact");return;
      }
    }
  const bool accepted=actor.doAttack(Npc::Anim::Attack,BS_HIT);
  if(accepted)++state.destructionMeleeAttempts;
  state.destructionMeleeNext=world.tickCount()+(accepted?2200:100);
  event("destruction2_melee_attempt","\"case\":"+jsonStr(state.destructionCase)+",\"accepted\":"+
    (accepted?"true":"false")+",\"ordinal\":"+std::to_string(state.destructionMeleeAttempts)+
    ",\"actor_cm\":"+ragdollVector(actor.position())+",\"bodies\":"+destructionRows(state,world));
  }

std::string destructionRoutineRows(const Npc& npc) {
  std::string result="[";
  npc.forEachRoutine([&](const Npc::RoutineInfo& routine) {
    if(result.size()>1)result+=',';
    result+="{\"start\":"+std::to_string(routine.start.toInt())+",\"end\":"+std::to_string(routine.end.toInt())+
      ",\"state\":"+std::to_string(routine.state.ptr)+",\"point\":"+jsonStr(routine.point)+"}";
    });
  return result+"]";
  }

void destructionFarmSetup(Destruction& state,const DestructionContext& context,World& world,Npc& player,uint32_t zenId,bool npc) {
  state.destructionMeleeRunning=false;state.destructionTrace=false;
  state.destructionFixtures.clear();state.destructionIds.clear();
  if(state.destructionMeleeNpc) {
    world.removeNpc(*state.destructionMeleeNpc);state.destructionMeleeNpc=nullptr;
    }
  const auto bodies=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(bodies.begin(),bodies.end(),[&](const auto& body){return body.zenId==zenId;});
  if(found==bodies.end() || found->className!="fence" || !found->destructible || found->broken) {
    arenaAbort("destruction farm needs an intact cache-backed stock map fence");return;
    }
  auto center=(found->localBounds[0]+found->localBounds[1])*.5f;found->transform.project(center);
  const bool thinZ=found->localBounds[1].z-found->localBounds[0].z<found->localBounds[1].x-found->localBounds[0].x;
  auto axis=(found->localBounds[0]+found->localBounds[1])*.5f+(thinZ?Vec3(0,0,1):Vec3(1,0,0));
  found->transform.project(axis);auto normal=axis-center;normal.y=0;normal=Vec3::normalize(normal);
  const float depth=(thinZ?found->localBounds[1].z-found->localBounds[0].z:found->localBounds[1].x-found->localBounds[0].x)*.5f;
  bool supported=false;Vec3 start,end;
  for(const auto sign:{1.f,-1.f}) {
    start=center-normal*(depth+75.f)*sign;end=center+normal*(depth+180.f)*sign;
    const auto first=world.physic()->landRay(start+Vec3(0,80,0),350);
    const auto last=world.physic()->landRay(end+Vec3(0,80,0),350);
    if(!first.hasCol || !last.hasCol || std::abs(first.v.y-last.v.y)>100.f)continue;
    start.y=first.v.y;end.y=last.v.y;normal*=sign;supported=true;break;
    }
  if(!supported) {arenaAbort("destruction farm fence lacks native walkable ground on both sides");return;}
  auto* actor=&player;
  if(npc) {
    actor=world.findNpcByInstance(world.script().findSymbolIndex("BAU_952_VINO"));
    if(!actor) {arenaAbort("destruction farm NPC actor Vino is absent");return;}
    }
  if(!destructionMeleeEquip(world,*actor,"1h"))return;
  actor->setPosition(start);actor->setDirection(normal);actor->updateTransform();
  if(npc) {player.closeWeapon(true);player.setPosition(start-normal*800.f);player.updateTransform();}
  state.destructionIds.push_back(found->id);state.destructionMeleeActor=actor;
  state.destructionMeleeRestance=false;
  state.destructionMeleeFacing=normal;
  state.destructionMeleeAttempts=state.destructionMeleeHits=0;
  state.destructionFarmZen=zenId;state.destructionFarmStart=start;state.destructionFarmEnd=end;
  state.destructionCase=npc?"farm-fence-1h-npc":"farm-fence-1h";
  state.destructionTrace=true;state.destructionStart=world.tickCount();
  auto* maleth=world.findNpcByInstance(world.script().findSymbolIndex("BAU_954_MALETH"));
  if(!maleth) {arenaAbort("destruction farm routine NPC Maleth is absent");return;}
  state.destructionFarmNpc=maleth;state.destructionFarmNpcStart=maleth->position();state.destructionFarmNpcTravel=0;
  const auto side=Vec3(-normal.z,0,normal.x);
  context.cam=C_Hold;lookFrom(center-normal*310.f+side*290.f+Vec3(0,170,0),center+Vec3(0,-10,0));
  event("destruction2_farm_setup","\"zen_id\":"+std::to_string(zenId)+",\"visual\":"+jsonStr(found->visual)+
    ",\"from_map\":true,\"start_cm\":"+ragdollVector(start)+",\"goal_cm\":"+ragdollVector(end)+
    ",\"actor\":"+jsonStr(symName(world,actor))+
    ",\"intact_passable\":"+(player.testMove(end)?"true":"false")+",\"npc\":"+jsonStr(symName(world,maleth))+
    ",\"npc_cm\":"+ragdollVector(maleth->position())+",\"npc_routines\":"+destructionRoutineRows(*maleth)+
    ",\"bodies\":"+destructionRows(state,world));
  }

void destructionFarmSetupNext(Destruction& state,const DestructionContext& context,World& world,Npc& player) {
  const auto bodies=world.physic()->movableVobSnapshot();
  const DynamicWorld::MovableVobSnapshot* nearest=nullptr;
  float distance=1800.f;
  for(const auto& body:bodies) {
    if(body.zenId==uint32_t(-1) || body.className!="fence" || !body.destructible || body.broken)continue;
    const float cm=(movablePosition(body.transform)-state.destructionFarmStart).length();
    if(cm<distance || (nearest && cm==distance && body.zenId<nearest->zenId)) {nearest=&body;distance=cm;}
    }
  if(!nearest) {arenaAbort("destruction farm needs another intact native fence within 18 m");return;}
  destructionFarmSetup(state,context,world,player,nearest->zenId);
  }

void destructionFarmNpcWalk(Destruction& state,World& world,std::string_view phase) {
  auto* npc=state.destructionFarmNpc;
  if(!npc) {arenaAbort("destruction farm NPC route requires farm setup");return;}
  const auto* start=world.findWayPoint("NW_FARM1_FIELD_08");
  if(!start) {arenaAbort("destruction farm authored route start absent");return;}
  const WayPoint* goal=nullptr;
  for(const auto& conn:start->connections())
    if(conn.point && !conn.point->underWater && (conn.point->pos-start->pos).length()>=250.f &&
       (conn.point->pos-start->pos).length()<=1400.f) {goal=conn.point;break;}
  if(!goal) {arenaAbort("destruction farm authored nearby route has no connected endpoint");return;}
  if(auto* player=world.player())player->closeWeapon(true);
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearGoTo();npc->stopWalking();npc->closeWeapon(true);
  npc->setExternalControl(false);npc->setProcessPolicy(NpcProcessPolicy::AiNormal);npc->setWalkMode(WalkBit::WM_Walk);
  npc->setPosition(start->pos);npc->setDirection(goal->pos-start->pos);npc->updateTransform();
  state.destructionFarmNpcStart=npc->position();state.destructionFarmNpcTravel=0;state.destructionFarmNpcGoal=goal;
  state.destructionFarmNpcBest=(npc->position()-goal->pos).length();
  npc->aiPush(AiQueue::aiGoToPoint(*goal));
  event("destruction2_farm_npc_begin","\"phase\":"+jsonStr(phase)+",\"npc\":"+jsonStr(symName(world,npc))+",\"start_waypoint\":"+jsonStr(start->name)+
    ",\"goal_waypoint\":"+jsonStr(goal->name)+",\"start_cm\":"+ragdollVector(npc->position())+
    ",\"goal_cm\":"+ragdollVector(goal->pos)+",\"routines\":"+destructionRoutineRows(*npc));
  }

void destructionFarmCheckpoint(Destruction& state,World& world,Npc& player,std::string_view phase) {
  auto* npc=state.destructionFarmNpc;
  event("destruction2_farm_checkpoint","\"phase\":"+jsonStr(phase)+",\"zen_id\":"+std::to_string(state.destructionFarmZen)+
    ",\"start_cm\":"+ragdollVector(state.destructionFarmStart)+",\"goal_cm\":"+ragdollVector(state.destructionFarmEnd)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"broken_passable\":"+(player.testMove(state.destructionFarmStart)?"true":"false")+
    ",\"npc_cm\":"+(npc?ragdollVector(npc->position()):"null")+",\"npc_travel_cm\":"+ragdollNumber(state.destructionFarmNpcTravel)+
    ",\"npc_goal_distance_cm\":"+(npc && state.destructionFarmNpcGoal?ragdollNumber((npc->position()-state.destructionFarmNpcGoal->pos).length()):"null")+
    ",\"npc_best_goal_distance_cm\":"+(std::isfinite(state.destructionFarmNpcBest)?ragdollNumber(state.destructionFarmNpcBest):"null")+
    ",\"npc_routines\":"+(npc?destructionRoutineRows(*npc):"[]")+",\"bodies\":"+destructionRows(state,world)+",\"fragments\":"+destructionFragments(state,world));
  }

void destructionSessionSnapshot(World& world,std::string_view label) {
  std::string bodies="[",fragments="[";
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(body.zenId==uint32_t(-1) || (body.className!="crate" && body.className!="barrel" && body.className!="fence"))continue;
    if(bodies.size()>1)bodies+=',';
    bodies+="{\"zen_id\":"+std::to_string(body.zenId)+",\"class\":"+jsonStr(body.className)+",\"visual\":"+
      jsonStr(body.visual)+",\"broken\":"+(body.broken?"true":"false")+",\"health\":"+ragdollNumber(body.health)+
      ",\"max_health\":"+ragdollNumber(body.maxHealth)+",\"transform\":"+ragdollMatrix(body.transform)+"}";
    }
  for(const auto& part:world.physic()->fragmentSnapshot()) {
    if(part.zenId==uint32_t(-1))continue;
    if(fragments.size()>1)fragments+=',';
    fragments+="{\"zen_id\":"+std::to_string(part.zenId)+",\"piece\":"+std::to_string(part.piece)+
      ",\"sleeping\":"+(part.sleeping?"true":"false")+",\"permanent\":"+(part.permanent?"true":"false")+
      ",\"asleep_since_min\":"+std::to_string(part.asleepSince)+",\"transform\":"+ragdollMatrix(part.transform)+
      ",\"position_cm\":"+ragdollVector(movablePosition(part.transform))+",\"velocity_cm_s\":"+
      ragdollVector(part.velocity)+",\"angular_velocity\":"+ragdollVector(part.angularVelocity)+"}";
    }
  event("destruction2_snapshot","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"game_minutes\":"+std::to_string(world.time().toInt()/60000)+",\"enabled\":"+
    (world.physic()->destructionEnabled()?"true":"false")+",\"bodies\":"+bodies+"],\"fragments\":"+fragments+"]");
  }

void destructionAge(World& world,int64_t minutes,bool youngest) {
  int64_t asleep=-1;
  for(const auto& part:world.physic()->fragmentSnapshot())
    if(part.zenId!=uint32_t(-1) && !part.permanent && part.sleeping && part.asleepSince>=0)
      asleep=asleep<0?part.asleepSince:youngest?std::max(asleep,part.asleepSince):std::min(asleep,part.asleepSince);
  if(asleep<0 || minutes<0) {arenaAbort("destruction age requires sleeping map debris and nonnegative minutes");return;}
  Gothic::inst().gameSession()->setTime(gtime(int64_t(0),int64_t(0),asleep+minutes));
  event("destruction2_age","\"anchor\":"+jsonStr(youngest?"youngest":"oldest")+
    ",\"asleep_since_min\":"+std::to_string(asleep)+",\"age_min\":"+std::to_string(minutes)+
    ",\"game_minutes\":"+std::to_string(world.time().toInt()/60000));
  }

bool destructionCommand(Destruction& state,const DestructionContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  const std::string& c = a[0];
  if(c=="destruction_traces" && a.size()>=2) {
    std::string command=a[1],value,error;
    for(size_t i=2;i<a.size();++i)command+=' '+a[i];
    if(!TestBench::destructionTracesDebug(w,pl,command,value,error)) {arenaAbort(error);return true;}
    event("destruction_traces_debug","\"command\":"+jsonStr(command)+",\"sample\":"+value);
    }
  else if(c=="destruction" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","destruction",a[1]=="on");
    w.physic()->setDestructionEnabled(a[1]=="on");
    }
  else if(c=="destruction" && a.size()==3 && a[1]=="setup") destructionSetup(state,context,w,pl,a[2]);
  else if(c=="destruction" && a.size()==2 && a[1]=="begin") destructionBegin(state,context,w);
  else if(c=="destruction" && a.size()==2 && a[1]=="checkpoint") destructionCheckpoint(state,context,w);
  else if(c=="destruction" && a.size()==3 && a[1]=="support" && (a[2]=="on" || a[2]=="off")) {
    state.destructionSupportTrace=a[2]=="on";
    state.destructionSupportNext=0;
    }
  else if(c=="destruction" && a.size()==3 && a[1]=="support" && a[2]=="sample") destructionSupportSample(state,context,w);
  else if(c=="destruction2" && (a.size()==4 || a.size()==5) && a[1]=="setup")
    destructionMeleeSetup(state,context,w,pl,a[2],a[3],a.size()==5 && a[4]=="npc");
  else if(c=="destruction2" && (a.size()==2 || a.size()==3) && a[1]=="attack") {
    if(!state.destructionMeleeActor || state.destructionIds.empty()) {arenaAbort("destruction melee setup required");return true;}
    // `attack <n>`: n blows and no more, whether the prop broke or not (a take with destruction off)
    state.destructionMeleeBlows=a.size()==3 ? unsigned(num(a,2,0)) : 0;
    state.destructionMeleeRunning=true;state.destructionMeleeNext=w.tickCount();
    state.destructionMeleeDeadline=w.tickCount()+90000;context.waitUntil=context.time+90034;
    }
  else if(c=="destruction2" && (a.size()==2 || a.size()==3 || (a.size()==4 && a[3]=="npc")) && a[1]=="farm")
    destructionFarmSetup(state,context,w,pl,uint32_t(num(a,2,19355)),a.size()==4);
  else if(c=="destruction2" && a.size()==2 && a[1]=="farm-nearby") destructionFarmSetupNext(state,context,w,pl);
  else if(c=="destruction2" && a.size()==2 && a[1]=="farm-walk") {
    pl.closeWeapon(true);pl.stopWalking();pl.clearAiQueue();pl.setDirection(state.destructionMeleeFacing);pl.updateTransform();
    exec("hold forward 2800",w,pl,ctrl);context.waitUntil=context.time+2834;
    }
  else if(c=="destruction2" && a.size()==2 && a[1]=="farm-ready") {
    if(!destructionMeleeEquip(w,pl,"1h"))return true;
    pl.setPosition(state.destructionFarmStart);
    pl.setDirection(state.destructionMeleeFacing);pl.updateTransform();
    }
  else if(c=="destruction2" && a.size()==3 && a[1]=="farm-npc-walk") destructionFarmNpcWalk(state,w,a[2]);
  else if(c=="destruction2" && a.size()==3 && a[1]=="farm-checkpoint") destructionFarmCheckpoint(state,w,pl,a[2]);
  else if(c=="destruction2" && a.size()==3 && a[1]=="snapshot") destructionSessionSnapshot(w,a[2]);
  else if(c=="destruction2" && a.size()==4 && a[1]=="age" && (a[3]=="oldest" || a[3]=="youngest"))
    destructionAge(w,int64_t(num(a,2,0)),a[3]=="youngest");
  else if(c=="destruction2" && a.size()==3 && a[1]=="save") {
    try {destructionSessionSnapshot(w,"saved-"+a[2]);saveMovableSession(w,a[2]);}
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="destruction2" && a.size()==2 && a[1]=="far") {
    const auto parts=w.physic()->fragmentSnapshot();
    if(parts.empty()) {arenaAbort("destruction far requires map debris");return true;}
    const auto origin=movablePosition(parts.front().transform);
    std::vector<const WayPoint*> candidates;
    w.findWayPoint(origin,[&](const WayPoint& point) {
      const auto distance=(point.pos-origin).length();
      if(!point.underWater && distance>=3600.f && distance<=10000.f)candidates.push_back(&point);
      return false;
      });
    std::sort(candidates.begin(),candidates.end(),[&](const auto* first,const auto* second) {
      const auto a=(first->pos-origin).quadLength(),b=(second->pos-origin).quadLength();
      return a==b?first->name<second->name:a<b;
      });
    Vec3 destination;std::string waypoint;float minimum=0;
    for(const auto* point:candidates) {
      const auto floor=w.physic()->landRay(point->pos+Vec3(0,200,0),800);
      if(!floor.hasCol || (floor.n.length()>0 && floor.n.y<.6f))continue;
      float distance=std::numeric_limits<float>::infinity();
      for(const auto& part:parts)distance=std::min(distance,(floor.v-movablePosition(part.transform)).length());
      if(distance<3500.f)continue;
      destination=floor.v;waypoint=point->name;minimum=distance;break;
      }
    if(waypoint.empty()) {arenaAbort("destruction far needs a supported distant authored waypoint");return true;}
    pl.closeWeapon(true);pl.clearAiQueue();pl.stopWalking();pl.setPosition(destination);pl.updateTransform();
    event("destruction2_far","\"debris_cm\":"+ragdollVector(origin)+",\"hero_cm\":"+ragdollVector(destination)+
      ",\"waypoint\":"+jsonStr(waypoint)+",\"minimum_debris_distance_cm\":"+ragdollNumber(minimum)+
      ",\"distance_cm\":"+ragdollNumber((destination-origin).length()));
    }
  else return false;
  return true;
  }

}
