#include "testbench_internal.h"
#include "game/navigationeyeworld.h"
#include "testbench.h"

#include <algorithm>
#include <cmath>

#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "gothic.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

bool itemWakeTableItems(World& world,Npc& player,bool sword) {
  const auto context=itemWakeContext();
  auto& state=context.itemWake;
  const auto symbol=world.script().findSymbolIndex(sword?"ITFO_BEER":"ITFO_APPLE");
  if(symbol==size_t(-1)) {arenaAbort("item wake preparation: tabletop item instance unavailable");return false;}
  Item definition(world,symbol,Item::T_Inventory);
  const auto* itemMesh=Resources::loadMesh(definition.handle().visual);
  if(!itemMesh) {arenaAbort("item wake preparation: tabletop item mesh unavailable");return false;}
  const float itemHeight=itemMesh->bbox()[1].y-itemMesh->bbox()[0].y;
  Vec3 center=state.itemWakeCenter;
  float top=center.y+80.f;
  if(sword) {
    center=(state.itemWakeBladePoints[0]+state.itemWakeBladePoints[2])*.5f;
    top=std::min({state.itemWakeBladePoints[0].y,state.itemWakeBladePoints[1].y,state.itemWakeBladePoints[2].y})-.75f*itemHeight;
    }
  const auto axis=sword?Vec3::normalize(state.itemWakeBladePoints[2]-state.itemWakeBladePoints[0]):Vec3(0,0,1);
  std::string tableVisual;
  std::vector<std::pair<float,std::string>> tables;
  std::unordered_set<std::string> seen;
  for(const auto& row:world.physic()->movableVobSnapshot()) {
    if(row.className!="table" || !seen.insert(row.visual).second)continue;
    const auto* mesh=Resources::loadMesh(row.visual);
    if(!mesh)continue;
    const auto extent=mesh->bbox()[1]-mesh->bbox()[0];
    const float depth=std::min(extent.x,extent.z);
    if(depth>=60.f)tables.emplace_back(depth,row.visual);
    }
  std::sort(tables.begin(),tables.end());
  Matrix4x4 matrix;
  DynamicWorld::RayLandResult supports[3];
  Vec3 normals[3];
  const unsigned count=sword?3u:1u;
  const auto itemExtent=itemMesh->bbox()[1]-itemMesh->bbox()[0];
  const float margin=.5f*std::max(itemExtent.x,itemExtent.z)+2.f;
  const Vec3 tangent(-axis.z,0,axis.x);
  const auto longAxis=sword?axis:tangent;
  size_t unsupported=0,blocked=0;
  for(const auto& candidate:tables) {
    const auto* table=Resources::loadMesh(candidate.second);
    const auto* bounds=table->bbox();
    const auto extent=bounds[1]-bounds[0];
    state.itemWakeTable=std::make_unique<PhysicMesh>(*table,*world.physic(),false);
    if(state.itemWakeTable->isEmpty())continue;
    state.itemWakeTable->setPayloadPtr(state.itemWakeTable.get());
    for(const Vec3 offset:{Vec3(),Vec3(.25f,0,0),Vec3(-.25f,0,0),Vec3(0,0,.25f),Vec3(0,0,-.25f),
                          Vec3(.25f,0,.25f),Vec3(.25f,0,-.25f),Vec3(-.25f,0,.25f),Vec3(-.25f,0,-.25f)}) {
      matrix=Matrix4x4::mkIdentity();
      matrix.rotateOY(std::atan2(-longAxis.z,longAxis.x)*180.f/float(M_PI)+(extent.x>=extent.z?0.f:90.f));
      auto local=(bounds[0]+bounds[1])*.5f+Vec3(offset.x*extent.x,0,offset.z*extent.z);local.y=bounds[1].y;
      matrix.project(local);
      matrix.set(3,0,center.x-local.x);matrix.set(3,1,top-local.y);matrix.set(3,2,center.z-local.z);
      if(sword) {
        const float ahead=std::max(0.f,.5f*candidate.first-margin-3.f);
        const float outward=std::max(0.f,.5f*std::max(extent.x,extent.z)-.2f*state.itemWakeSwordLength-margin-3.f);
        matrix.set(3,0,matrix.at(3,0)+tangent.x*ahead+axis.x*outward);
        matrix.set(3,2,matrix.at(3,2)+tangent.z*ahead+axis.z*outward);
        }
      state.itemWakeTable->setObjMatrix(matrix);
      const auto first=sword?state.itemWakeBladePoints[0]:center;
      const auto probe=world.physic()->landRay(Vec3(first.x,top+30.f,first.z),extent.y+60.f);
      if(!probe.hasCol || probe.uptr!=state.itemWakeTable.get()) {++unsupported;continue;}
      matrix.set(3,1,matrix.at(3,1)+top-probe.v.y);
      state.itemWakeTable->setObjMatrix(matrix);
      bool supported=true;
      for(unsigned i=0;i<count && supported;++i) {
        const auto target=sword?state.itemWakeBladePoints[i]:center;
        const Vec3 from(target.x,top+30.f,target.z);
        supports[i]=world.physic()->landRay(from,60.f);
        const auto hit=navigationRay(*world.physic(),from,from-Vec3(0,60.f,0),&player);
        supported=supports[i].hasCol && supports[i].uptr==state.itemWakeTable.get() &&
                  std::abs(supports[i].v.y-top)<=3.f && hit.hasCol && hit.normal.y>=.98f &&
                  (hit.point-supports[i].v).length()<.1f;
        normals[i]=hit.normal;
        for(const Vec3 edge:{Vec3(margin,0,0),Vec3(-margin,0,0),Vec3(0,0,margin),Vec3(0,0,-margin)}) {
          if(!supported)break;
          const auto side=world.physic()->landRay(from+edge,60.f);
          supported=side.hasCol && side.uptr==state.itemWakeTable.get() && std::abs(side.v.y-supports[i].v.y)<=3.f;
          }
        }
      if(!supported) {++unsupported;continue;}
      // A supported tabletop must not stop the authored stroke before it reaches the Items.
      for(size_t i=0;i+1<state.itemWakeBladePath.size() && supported;i+=2) {
        for(float t:{0.f,.5f,1.f}) {
          if(!supported)break;
          const auto from=i>0 ? state.itemWakeBladePath[i-2]+(state.itemWakeBladePath[i]-state.itemWakeBladePath[i-2])*t : state.itemWakeBladePath[i];
          const auto to=i>0 ? state.itemWakeBladePath[i-1]+(state.itemWakeBladePath[i+1]-state.itemWakeBladePath[i-1])*t : state.itemWakeBladePath[i+1];
          const auto hit=world.physic()->sweep(from,to,2.f);
          supported=!hit.hasCol || hit.uptr!=state.itemWakeTable.get();
          }
        }
      if(!supported) {++blocked;continue;}
      tableVisual=candidate.second;break;
      }
    if(!tableVisual.empty())break;
    state.itemWakeTable.reset();
    }
  if(tableVisual.empty()) {
    arenaAbort("item wake preparation: no usable tabletop, support_rejects="+std::to_string(unsupported)+
               ", obstruction_rejects="+std::to_string(blocked));return false;
    }
  state.itemWakeTable->setPayloadPtr(nullptr);
  asset::VirtualObject data;data.type=asset::VirtualObjectType::zCVob;data.id=uint32_t(-1);
  data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
  data.visual=std::make_shared<asset::VisualMesh>();data.visual->type=asset::VisualType::MESH;data.visual->name=tableVisual;
  state.itemWakeTableVisual=std::make_unique<ObjVisual>();state.itemWakeTableVisual->setVisual(data,world,false,false);
  state.itemWakeTableVisual->setObjMatrix(matrix);
  std::string supportRows="[";
  for(unsigned i=0;i<count;++i) {
    const auto& support=supports[i];
    const Vec3 target=support.v;
    if(i) supportRows+=',';
    supportRows+="{\"point_cm\":"+ragdollVector(support.v)+",\"normal\":"+ragdollVector(normals[i])+"}";
    auto pose=Matrix4x4::mkIdentity();pose.translate(target.x,support.v.y+100.f,target.z);
    auto* item=world.addItemDyn(symbol,pose,world.npcId(&player));
    if(!item || !item->isDynamic()) {arenaAbort("item wake preparation: tabletop item has no body");return false;}
    const auto bodies=world.physic()->waterBodySnapshot();
    const auto body=std::find_if(bodies.begin(),bodies.end(),[&](const auto& row){return row.item==item;});
    if(body==bodies.end()) {arenaAbort("item wake preparation: tabletop item has no native bounds");return false;}
    const auto nativeCenter=(body->collisionBounds[0]+body->collisionBounds[1])*.5f;
    const Vec3 delta(target.x-nativeCenter.x,support.v.y+.05f-body->collisionBounds[0].y,target.z-nativeCenter.z);
    const auto at=item->position()+delta;
    pose.set(3,0,at.x);pose.set(3,1,at.y);pose.set(3,2,at.z);
    item->setObjMatrix(pose);
    ItemWake::ItemWakeSample sample;sample.item=item;sample.name=sword?"ITFO_BEER":"ITFO_APPLE";
    sample.initial=at;sample.boundsOrigin=at;sample.floorY=support.v.y;
    sample.collisionBounds[0]=body->collisionBounds[0]+delta;sample.collisionBounds[1]=body->collisionBounds[1]+delta;
    state.itemWakeItems.push_back(sample);
    }
  state.itemWakePreparing=true;
  event("item_wake_table","\"case\":"+jsonStr(state.itemWakeCase)+",\"visual\":"+jsonStr(tableVisual)+
    ",\"top_cm\":"+ragdollNumber(top)+",\"items\":"+std::to_string(state.itemWakeItems.size())+
    ",\"blade_step_cm\":"+ragdollNumber(state.itemWakeBladeSpeed)+",\"support_rejects\":"+std::to_string(unsupported)+
    ",\"obstruction_rejects\":"+std::to_string(blocked)+",\"support\":"+supportRows+"]"+
    ",\"native_support_verified\":true"+(sword?",\"dry_blade_table_clear\":true,\"dry_blade_samples\":"+
      std::to_string(state.itemWakeBladePath.size()/2):""));
  return true;
  }

// The observer stays beyond CharacterVirtual's three-metre item range. Setup
// lets native Item bodies settle and retire before a contact is introduced.
void itemWakeFields(World& world) {
  const auto context=itemWakeContext();
  auto& state=context.itemWake;
  if(state.itemWakeCalibrationEnd) {
    auto& npc=*state.itemWakeActors.front();
    if(!state.itemWakeCalibrationSample) {
      if(world.tickCount()<state.itemWakeStart)return;
      npc.setPosition(state.itemWakeSwordRoot);npc.setDirection({0,0,1});npc.updateTransform();
      if(!npc.setAnim(Npc::Anim::AttackL)) {arenaAbort("item wake preparation: delayed NPC sword calibration refused");return;}
      }
    const auto* skeleton=npc.visualSkeleton();
    const auto hand=skeleton?skeleton->findNode("ZS_RIGHTHAND"):size_t(-1);
    if(!skeleton || hand>=skeleton->nodes.size()) {arenaAbort("item wake preparation: sword hand unavailable");return;}
    const auto& matrix=npc.animationBone(hand);
    Vec3 points[3];
    for(unsigned i=0;i<3;++i) {points[i]=Vec3(-state.itemWakeSwordLength*(.55f+.2f*float(i)),0,0);matrix.project(points[i]);}
    const auto* sequence=state.itemWakeSwordSequence;
    const float frame=npc.animProgress(sequence->name)*float(sequence->data->numFrames);
    // This fixture's T_1HATTACKL native window is frames 0..9 (0..400 ms).
    // Sample frames 2..6: blending can leave the blade steep at the optimal tag alone.
    if(npc.isAttackAnim() && frame<float(state.itemWakeSwordOptimalFrame+4)) {
      Vec3 grip,tip(-state.itemWakeSwordLength,0,0);matrix.project(grip);matrix.project(tip);
      state.itemWakeBladePath.push_back(grip);state.itemWakeBladePath.push_back(tip);
      }
    if(state.itemWakeCalibrationSample && npc.isAttackAnim() &&
       frame>=float(state.itemWakeSwordOptimalFrame) && frame<float(state.itemWakeSwordOptimalFrame+4)) {
      const float speed=(points[2]-state.itemWakePreviousTip).length();
      const float span=std::abs(points[2].y-points[0].y);
      event("item_wake_sword_calibration_sample","\"frame\":"+ragdollNumber(frame)+",\"root_cm\":"+ragdollVector(npc.position())+
        ",\"points_cm\":["+ragdollVector(points[0])+","+ragdollVector(points[1])+","+ragdollVector(points[2])+
        "],\"vertical_span_cm\":"+ragdollNumber(span)+",\"step_cm\":"+ragdollNumber(speed));
      if(speed>=1.f && span<12.f && (state.itemWakeBladeSpeed<1.f ||
          span<std::abs(state.itemWakeBladePoints[2].y-state.itemWakeBladePoints[0].y))) {
        state.itemWakeBladeSpeed=speed;
        for(unsigned i=0;i<3;++i)state.itemWakeBladePoints[i]=points[i];
        state.itemWakeBladePathEnd=state.itemWakeBladePath.size();
        }
      }
    state.itemWakePreviousTip=points[2];state.itemWakeCalibrationSample=true;
    if(world.tickCount()<state.itemWakeCalibrationEnd)return;
    state.itemWakeCalibrationEnd=0;
    if(state.itemWakeBladeSpeed<1.f) {arenaAbort("item wake preparation: NPC sword had no horizontal stroke inside the sampled active interval");return;}
    state.itemWakeBladePath.resize(state.itemWakeBladePathEnd);
    npc.setAnim(Npc::Anim::Idle);
    npc.setPosition(state.itemWakeSwordRoot);npc.setDirection({0,0,1});npc.updateTransform();
    if(!itemWakeTableItems(world,*world.player(),true))return;
    }
  if((!state.itemWakeRunning && !state.itemWakePreparing) || state.itemWakeSample==world.tickCount())return;
  if(state.itemWakeRunning && state.itemWakeWindow && world.tickCount()-state.itemWakeStart>state.itemWakeWindow)return;
  state.itemWakeSample=world.tickCount();
  const auto bodies=world.physic()->waterBodySnapshot();
  for(auto& sample:state.itemWakeItems) {
    const auto at=sample.item->position();
    const bool finite=std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z);
    if(!finite) {sample.fell=true;sample.below=true;sample.preparationDive|=state.itemWakePreparing;continue;}
    if(const auto native=std::find_if(bodies.begin(),bodies.end(),[&](const auto& body){return body.item==sample.item;});
       native!=bodies.end()) {
      sample.collisionBounds[0]=native->collisionBounds[0];sample.collisionBounds[1]=native->collisionBounds[1];
      sample.boundsOrigin=at;
      }
    else {
      sample.collisionBounds[0]+=at-sample.boundsOrigin;sample.collisionBounds[1]+=at-sample.boundsOrigin;
      sample.boundsOrigin=at;
      }
    if(state.itemWakePreparing) {
      sample.preparationDive|=sample.collisionBounds[0].y<sample.floorY-100.f;
      continue;
      }
    const bool dynamic=sample.item->isDynamic();
    sample.bodyCreated|=dynamic && !sample.hadBody;sample.hadBody=dynamic;
    sample.travel=std::max(sample.travel,(at-sample.initial).length());
    const auto center=(sample.collisionBounds[0]+sample.collisionBounds[1])*.5f;
    const auto floor=world.physic()->landRay(Vec3(center.x,std::max(sample.floorY+500.f,sample.collisionBounds[1].y+20.f),center.z),10000.f);
    if(floor.hasCol)sample.penetration=std::max(sample.penetration,floor.v.y-sample.collisionBounds[0].y);
    sample.fell|=!floor.hasCol || sample.penetration>5.f;
    // This conservative threshold is above the actual world's lower bound.
    sample.below|=at.y<sample.floorY-10000.f;
    }
  if(state.itemWakePreparing)return;
  if(state.itemWakeCase=="sword") {
    const auto& npc=*state.itemWakeActors.front();
    const auto hand=npc.visualSkeleton()->findNode("ZS_RIGHTHAND");
    const auto& matrix=npc.animationBone(hand);
    Vec3 grip,tip(-state.itemWakeSwordLength,0,0);matrix.project(grip);matrix.project(tip);
    const float frame=npc.animProgress(state.itemWakeSwordSequence->name)*float(state.itemWakeSwordSequence->data->numFrames);
    if(frame>=float(state.itemWakeSwordOptimalFrame) && frame<float(state.itemWakeSwordOptimalFrame+4)) {
      std::string crossed="[";
      for(unsigned i=0;i<3;++i) {
        const auto& bounds=state.itemWakeItems[i].collisionBounds;
        for(float t:{0.f,.5f,1.f}) {
          const auto a=state.itemWakeCalibrationSample?state.itemWakePreviousGrip+(grip-state.itemWakePreviousGrip)*t:grip;
          const auto b=state.itemWakeCalibrationSample?state.itemWakePreviousTip+(tip-state.itemWakePreviousTip)*t:tip;
          float enter=0,exit=1;
          for(unsigned axis=0;axis<3 && enter<=exit;++axis) {
            const float from=axis==0?a.x:axis==1?a.y:a.z,to=axis==0?b.x:axis==1?b.y:b.z;
            const float low=(axis==0?bounds[0].x:axis==1?bounds[0].y:bounds[0].z)-2.f;
            const float high=(axis==0?bounds[1].x:axis==1?bounds[1].y:bounds[1].z)+2.f;
            if(std::abs(to-from)<1.e-5f) {if(from<low || from>high)exit=-1;}
            else {
              const float u=(low-from)/(to-from),v=(high-from)/(to-from);
              enter=std::max(enter,std::min(u,v));exit=std::min(exit,std::max(u,v));
              }
            }
          state.itemWakeBladeCrossed[i]|=enter<=exit;
          }
        if(i)crossed+=',';
        crossed+=state.itemWakeBladeCrossed[i]?"true":"false";
        }
      event("item_wake_sword_path","\"frame\":"+ragdollNumber(frame)+",\"root_cm\":"+ragdollVector(npc.position())+
        ",\"grip_cm\":"+ragdollVector(grip)+",\"tip_cm\":"+ragdollVector(tip)+",\"crossed_item_bounds\":"+crossed+"]");
      }
    state.itemWakePreviousGrip=grip;state.itemWakePreviousTip=tip;state.itemWakeCalibrationSample=true;
    }
  if((state.itemWakeCase=="debris" || state.itemWakeCase=="event-debris") && world.tickCount()-state.itemWakeStart>=200) {
    const auto parts=world.physic()->fragmentSnapshot();
    if(std::none_of(parts.begin(),parts.end(),[](const auto& part){return destructionOwns(itemWakeContext().destruction,part.parentId);})) {
      arenaAbort("item wake preparation: crate break created no native debris");return;
      }
    }
  if(state.itemWakeCase=="pile-far") {
    const float infinity=std::numeric_limits<float>::infinity();
    Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);size_t parts=0;
    for(const auto& body:bodies) {
      if(!body.ragdoll || std::none_of(state.itemWakeActors.begin(),state.itemWakeActors.end(),
        [&](const auto* npc){return npc->ragdollBody()==body.ragdoll;}))continue;
      ++parts;
      low.x=std::min(low.x,body.collisionBounds[0].x);low.y=std::min(low.y,body.collisionBounds[0].y);low.z=std::min(low.z,body.collisionBounds[0].z);
      high.x=std::max(high.x,body.collisionBounds[1].x);high.y=std::max(high.y,body.collisionBounds[1].y);high.z=std::max(high.z,body.collisionBounds[1].z);
      }
    if(!parts) {arenaAbort("item wake preparation: far pile lost its native ragdolls");return;}
    for(const auto& sample:state.itemWakeItems) {
      const auto* box=sample.collisionBounds;
      const Vec3 gap(std::max({low.x-box[1].x,box[0].x-high.x,0.f}),
                     std::max({low.y-box[1].y,box[0].y-high.y,0.f}),
                     std::max({low.z-box[1].z,box[0].z-high.z,0.f}));
      state.itemWakeMinimumGap=std::min(state.itemWakeMinimumGap,gap.length());
      state.itemWakeQueryOverlaps+=box[0].x<=high.x+50.f && box[1].x>=low.x-50.f &&
        box[0].y<=high.y+50.f && box[1].y>=low.y-50.f && box[0].z<=high.z+50.f && box[1].z>=low.z-50.f;
      }
    }
  if(state.itemWakeCase=="pile" || state.itemWakeCase=="pile-far" || state.itemWakeCase=="arrow" ||
     state.itemWakeCase=="sword" || state.itemWakeCase.starts_with("event-")) {
    const auto step=world.physic()->simulationStep();const auto advanced=step-state.itemWakeLastNativeStep;
    state.itemWakeLastNativeStep=step;
    if(advanced>1) {arenaAbort("item wake preparation: measurement frame advanced multiple native steps");return;}
    if(advanced==0)++state.itemWakeZeroFrames;
    else if(state.itemWakeCase!="pile-far" || state.itemWakePhysics.size()<720) {
      if(state.itemWakePhysics.size()==20000) {arenaAbort("item wake physics samples overflow");return;}
      if(state.itemWakePhysics.empty())state.itemWakeFirstPhysicsTick=world.tickCount();
      state.itemWakeLastPhysicsTick=world.tickCount();
      state.itemWakePhysics.push_back(world.physic()->physicsStats().stepMilliseconds);
      }
    }
  if((state.itemWakeCase=="pile" || state.itemWakeCase=="pile-far") && world.tickCount()>=state.itemWakePulse) {
    state.itemWakePulse=world.tickCount()+100;
    const float sign=(world.tickCount()/500)%2 ? 1.f : -1.f;
    for(auto* npc:state.itemWakeActors)npc->addRagdollImpulse({10.f*sign,8.f,5.f*sign});
    }
  }

void itemWakeResetWindow(World& world,uint64_t window) {
  const auto context=itemWakeContext();
  auto& state=context.itemWake;
  state.itemWakeInitiallyBodyless=0;
  for(auto& sample:state.itemWakeItems) {
    sample.initial=sample.item->position();sample.travel=0;sample.penetration=0;
    sample.bodyCreated=false;sample.hadBody=sample.item->isDynamic();sample.fell=false;sample.below=false;
    state.itemWakeInitiallyBodyless+=!sample.hadBody;
    }
  state.itemWakePhysics.clear();state.itemWakeSample=uint64_t(-1);
  state.itemWakeStart=world.tickCount();state.itemWakeWindow=window;
  state.itemWakeLastNativeStep=world.physic()->simulationStep();state.itemWakeZeroFrames=0;
  state.itemWakeFirstPhysicsTick=0;state.itemWakeLastPhysicsTick=0;
  state.itemWakeMinimumGap=std::numeric_limits<float>::infinity();state.itemWakeQueryOverlaps=0;
  state.itemWakeRunning=true;
  }

void itemWakeSetup(World& world,Npc& player,std::string_view name) {
  const auto context=itemWakeContext();
  auto& state=context.itemWake;
  if(name!="ragdoll" && name!="debris" && name!="item" && name!="pile" && name!="pile-far" && name!="event" &&
     name!="arrow" && name!="sword") {
    arenaAbort("item wake setup expects ragdoll, debris, item, pile, pile-far, event, arrow or sword");return;
    }
  state.itemWakeRunning=false;state.itemWakePreparing=false;
  for(auto& sample:state.itemWakeItems)world.removeItem(*sample.item);
  state.itemWakeItems.clear();
  if(state.itemWakeMover)world.removeItem(*state.itemWakeMover);
  state.itemWakeMover=nullptr;
  for(auto* npc:state.itemWakeActors)world.removeNpc(*npc);
  state.itemWakeActors.clear();
  state.itemWakeTable.reset();state.itemWakeTableVisual.reset();
  state.itemWakeCalibrationEnd=0;state.itemWakeCalibrationSample=false;state.itemWakeBladeSpeed=0;
  state.itemWakeSwordSequence=nullptr;
  state.itemWakeBladePath.clear();state.itemWakeBladePathEnd=0;
  std::fill(std::begin(state.itemWakeBladeCrossed),std::end(state.itemWakeBladeCrossed),false);
  context.destruction.destructionFixtures.clear();context.destruction.destructionIds.clear();
  if(!state.itemWakeSite) {
    if(const auto* anchor=world.findPoint("NW_XARDAS_TOWER_VALLEY_091",false)) {
      player.setPosition(anchor->pos);player.updateTransform();
      }
    if(!chooseRagdollArena(world,player)) {arenaAbort("item wake preparation: fixture needs supported outdoor ground");return;}
    state.itemWakeCenter=context.ragdollArenaTarget-Vec3(0,80,0);
    std::vector<Npc*> nearby;
    for(uint32_t id=0;id<world.npcCount();++id)
      if(auto* npc=world.npcById(id);npc && npc!=&player &&
         (npc->position()-state.itemWakeCenter).length()<2000.f)nearby.push_back(npc);
    for(auto* npc:nearby)world.removeNpc(*npc);
    std::vector<Item*> stock;
    world.detectItem(state.itemWakeCenter,2000.f,[&](Item& item){stock.push_back(&item);});
    for(auto* item:stock)world.removeItem(*item);
    event("item_wake_arena_clear","\"stock_items_removed\":"+std::to_string(stock.size()));
    state.itemWakeSite=true;
    }
  player.clearAiQueue();player.clearGoTo();player.stopWalking();player.closeWeapon(true);
  auto hero=state.itemWakeCenter+Vec3(0,0,-1500);
  const auto ground=world.physic()->landRay(hero+Vec3(0,300,0),900);
  if(!ground.hasCol) {arenaAbort("item wake preparation: observer has no supported floor");return;}
  hero.y=ground.v.y;player.setPosition(hero);player.updateTransform();
  context.cam=C_Hold;lookFrom(state.itemWakeCenter+Vec3(0,650,850),state.itemWakeCenter);
  state.itemWakeCase=name;state.itemWakePhysics.clear();state.itemWakeSample=uint64_t(-1);
  if(name=="arrow") {
    itemWakeTableItems(world,player,false);
    return;
    }
  if(name=="sword") {
    Gothic::settingsSetI("GAME","melee_blade_shadow",1);
    const auto symbol=world.script().findSymbolIndex("BDT_1013_BANDIT_L");
    const auto sword=world.script().findSymbolIndex("ITMW_1H_COMMON_01");
    const auto at=state.itemWakeCenter+Vec3(0,0,-150);
    const auto floor=world.physic()->landRay(at+Vec3(0,200,0),500.f);
    if(symbol==size_t(-1) || sword==size_t(-1) || !floor.hasCol || floor.n.y<.98f) {
      arenaAbort("item wake preparation: sword actor lacks native symbols or supported stance");return;
      }
    auto* npc=world.addNpc(symbol,floor.v);
    if(!npc) {arenaAbort("item wake preparation: sword actor insertion failed");return;}
    state.itemWakeActors.push_back(npc);
    npc->clearAiQueue();npc->clearState(true);npc->clearGoTo();npc->stopWalking();npc->setExternalControl(true);
    npc->handle().attribute[ATR_STRENGTH]=100;npc->setTalentSkill(TALENT_1H,2);
    npc->setToFightMode(sword);npc->setTarget(nullptr);npc->setDirection({0,0,1});npc->updateTransform();
    const auto* weapon=npc->currentMeleeWeapon();
    const auto* mesh=weapon?Resources::loadMesh(weapon->handle().visual):nullptr;
    const auto* sequence=npc->setAnimAngGet(Npc::Anim::AttackL);
    if(!mesh || !sequence) {arenaAbort("item wake preparation: native NPC sword attack unavailable");return;}
    const auto& data=*sequence->data;
    bool optimalFound=false;
    for(const auto& event:data.events) {
      if(event.type!=asset::MdsEventType::OPTIMAL_FRAME || event.frames.empty())continue;
      const auto clamped=data.optimalFrame(event.frames.front());
      state.itemWakeSwordOptimalFrame=sequence->reverse?data.numFrames-1-clamped:clamped;
      optimalFound=true;break;
      }
    if(!optimalFound) {arenaAbort("item wake preparation: NPC sword attack has no authored optimal frame");return;}
    state.itemWakeSwordSequence=sequence;
    state.itemWakeSwordLength=-mesh->bbox()[0].x;
    state.itemWakeSwordRoot=floor.v;
    npc->setAnim(Npc::Anim::Idle);
    state.itemWakeStart=world.tickCount()+uint64_t(sequence->totalTime())+400;
    state.itemWakeCalibrationEnd=state.itemWakeStart+uint64_t(sequence->totalTime())+400;
    event("item_wake_sword_calibration","\"npc\":"+jsonStr(TestBench::who(npc))+
      ",\"animation\":"+jsonStr(sequence->name)+",\"optimal_frame\":"+std::to_string(state.itemWakeSwordOptimalFrame)+
      ",\"length_cm\":"+ragdollNumber(state.itemWakeSwordLength)+",\"root_cm\":"+ragdollVector(state.itemWakeSwordRoot));
    return;
    }
  const char* names[]{"ITMI_GOLD","ITFO_BEER","ITMW_1H_COMMON_01","ITFO_APPLE"};
  const unsigned count=(name=="pile" || name=="pile-far" || name=="event")?50:4;
  size_t initialOverlaps=0;float maximumReleaseHeight=0,gridSpacing=0;
  for(unsigned i=0;i<count;++i) {
    const auto symbol=world.script().findSymbolIndex(names[i%4]);
    if(symbol==size_t(-1)) {arenaAbort("item wake preparation: native item instance unavailable");return;}
    const auto offset=count==50 ? Vec3() : Vec3(0,0,(float(i)-1.5f)*48.f);
    const auto at=state.itemWakeCenter+offset;
    const auto floor=world.physic()->landRay(at+Vec3(0,100,0),300);
    if(!floor.hasCol || floor.n.y<.98f) {arenaAbort("item wake preparation: item lacks level native support");return;}
    auto pose=Matrix4x4::mkIdentity();
    if(Gothic::physicsFlags().itemHulls) {pose.rotateOX(20);pose.rotateOY(37);}
    pose.set(3,0,at.x);pose.set(3,1,floor.v.y+100.f);pose.set(3,2,at.z);
    auto* item=world.addItemDyn(symbol,pose,world.npcId(&player));
    if(!item || !item->isDynamic()) {arenaAbort("item wake preparation: item has no native body");return;}
    const auto after=world.physic()->waterBodySnapshot();
    const auto native=std::find_if(after.begin(),after.end(),[&](const auto& body){return body.item==item;});
    if(native==after.end()) {arenaAbort("item wake preparation: item has no native bounds");return;}
    if(count==50) {
      const auto extent=native->collisionBounds[1]-native->collisionBounds[0];
      gridSpacing=std::max(gridSpacing,std::max(extent.x,extent.z)+5.f);
      ItemWake::ItemWakeSample sample;sample.item=item;sample.name=names[i%4];
      sample.collisionBounds[0]=native->collisionBounds[0];sample.collisionBounds[1]=native->collisionBounds[1];
      item->setPhysicsDisable();
      state.itemWakeItems.push_back(sample);
      continue;
      }
    const auto center=(native->collisionBounds[0]+native->collisionBounds[1])*.5f;
    auto displacement=Vec3(at.x-center.x,floor.v.y+100.f-native->collisionBounds[0].y,at.z-center.z);
    const auto low=native->collisionBounds[0]+displacement,high=native->collisionBounds[1]+displacement;
    for(const auto& previous:state.itemWakeItems)
      if(low.x<previous.collisionBounds[1].x && previous.collisionBounds[0].x<high.x &&
         low.z<previous.collisionBounds[1].z && previous.collisionBounds[0].z<high.z)
        displacement.y=std::max(displacement.y,previous.collisionBounds[1].y+10.f-native->collisionBounds[0].y);
    maximumReleaseHeight=std::max(maximumReleaseHeight,native->collisionBounds[0].y+displacement.y-floor.v.y);
    const auto release=item->position()+displacement;
    auto releasePose=item->transform();
    releasePose.set(3,0,release.x);releasePose.set(3,1,release.y);releasePose.set(3,2,release.z);
    item->setObjMatrix(releasePose);
    ItemWake::ItemWakeSample sample;sample.item=item;sample.name=names[i%4];sample.initial=release;
    sample.collisionBounds[0]=native->collisionBounds[0]+displacement;
    sample.collisionBounds[1]=native->collisionBounds[1]+displacement;
    sample.boundsOrigin=release;sample.floorY=floor.v.y;
    for(size_t other=0;other<state.itemWakeItems.size();++other) {
      const auto* a=sample.collisionBounds;const auto* b=state.itemWakeItems[other].collisionBounds;
      if(a[0].x<b[1].x && b[0].x<a[1].x && a[0].y<b[1].y && b[0].y<a[1].y &&
         a[0].z<b[1].z && b[0].z<a[1].z)++initialOverlaps;
      }
    state.itemWakeItems.push_back(sample);
    }
  if(count==50) {
    for(size_t i=0;i<state.itemWakeItems.size();++i) {
      auto& sample=state.itemWakeItems[i];
      const auto at=state.itemWakeCenter+Vec3((float(i%10)-4.f)*gridSpacing,0,(float(i/10)-2.f)*gridSpacing);
      const auto floor=world.physic()->landRay(at+Vec3(0,500,0),1000.f);
      if(!floor.hasCol || floor.n.y<.98f) {arenaAbort("item wake preparation: sleeping grid lacks level native support");return;}
      const auto center=(sample.collisionBounds[0]+sample.collisionBounds[1])*.5f;
      const auto delta=Vec3(at.x-center.x,floor.v.y+.05f-sample.collisionBounds[0].y,at.z-center.z);
      const auto position=sample.item->position()+delta;
      auto pose=sample.item->transform();
      pose.set(3,0,position.x);pose.set(3,1,position.y);pose.set(3,2,position.z);
      sample.item->setObjMatrix(pose);
      if(!sample.item->wakePhysics() || !sample.item->physicsSleeping() || sample.item->physicsVelocity().quadLength()!=0.f) {
        arenaAbort("item wake preparation: grid item was not created sleeping at rest");return;
        }
      sample.initial=position;sample.boundsOrigin=position;sample.floorY=floor.v.y;
      sample.collisionBounds[0]+=delta;sample.collisionBounds[1]+=delta;
      for(size_t other=0;other<i;++other) {
        const auto* a=sample.collisionBounds;const auto* b=state.itemWakeItems[other].collisionBounds;
        if(a[0].x<b[1].x && b[0].x<a[1].x && a[0].y<b[1].y && b[0].y<a[1].y &&
           a[0].z<b[1].z && b[0].z<a[1].z)++initialOverlaps;
        }
      }
    }
  if(initialOverlaps) {arenaAbort("item wake preparation: initial item AABBs overlap");return;}
  state.itemWakePreparing=true;
  event("item_wake_setup","\"case\":"+jsonStr(name)+",\"enabled\":"+
    (Gothic::settingsGetI("GAME","item_wake")?"true":"false")+
    ",\"item_hulls\":"+(Gothic::physicsFlags().itemHulls?"true":"false")+
    ",\"items\":"+std::to_string(count)+",\"initial_item_overlaps\":"+std::to_string(initialOverlaps)+
    ",\"maximum_release_height_cm\":"+ragdollNumber(maximumReleaseHeight)+
    ",\"grid_spacing_cm\":"+ragdollNumber(gridSpacing)+",\"hero_distance_cm\":"+
    ragdollNumber((player.position()-state.itemWakeCenter).length()));
  }

bool itemWakeBreakCrate(World& world,Npc& player) {
  const auto context=itemWakeContext();
  auto& state=context.itemWake;
  const std::string visual="NW_HARBOUR_CRATE_01.3DS";
  auto support=state.itemWakeCenter+Vec3(0,220,0);float floorY=state.itemWakeCenter.y;
  const bool groundEvent=state.itemWakeCase=="event-debris";
  if(groundEvent) {
    const auto* mesh=Resources::loadMesh(visual);
    if(!mesh) {arenaAbort("item wake preparation: event crate mesh unavailable");return false;}
    float left=std::numeric_limits<float>::infinity();
    for(const auto& sample:state.itemWakeItems)left=std::min(left,sample.collisionBounds[0].x);
    support.x=left-(mesh->bbox()[1].x-mesh->bbox()[0].x)*.5f-20.f;
    support.y=state.itemWakeCenter.y;
    Vec3 failed;
    if(!destructionSupported(world,visual,support,support,failed)) {
      arenaAbort("item wake preparation: beside-grid crate lacks supported footprint");return false;
      }
    floorY=support.y;
    }
  if(!destructionAdd(world,visual,support)) {
    arenaAbort("item wake preparation: crate insertion failed");return false;
    }
  const auto bodies=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(bodies.begin(),bodies.end(),[](const auto& body){return destructionOwns(itemWakeContext().destruction,body.id);});
  if(found==bodies.end()) {arenaAbort("item wake preparation: crate has no native snapshot");return false;}
  auto target=(found->localBounds[0]+found->localBounds[1])*.5f;found->transform.project(target);
  if(groundEvent) {
    const auto snapshots=world.physic()->waterBodySnapshot();
    const auto native=std::find_if(snapshots.begin(),snapshots.end(),[&](const auto& body){return body.parentId==found->id;});
    if(native==snapshots.end()) {arenaAbort("item wake preparation: event crate lacks native bounds");return false;}
    float itemLeft=std::numeric_limits<float>::infinity();
    for(const auto& sample:state.itemWakeItems)itemLeft=std::min(itemLeft,sample.collisionBounds[0].x);
    const float gap=itemLeft-native->collisionBounds[1].x,offset=native->collisionBounds[0].y-floorY;
    if(gap<=0.f || std::abs(offset)>5.f) {arenaAbort("item wake preparation: event crate overlaps items or lacks ground contact");return false;}
    event("item_wake_event_crate","\"support_cm\":"+ragdollVector(support)+
      ",\"bottom_floor_offset_cm\":"+ragdollNumber(offset)+",\"item_grid_gap_cm\":"+ragdollNumber(gap));
    }
  const auto affected=world.physic()->meleeMovableVobs(target+Vec3(-180,0,0),Vec3(1,0,0),300.f,
    1000.f,1000.f,FractureRule::Melee::OneHand,&player);
  if(affected!=1) {arenaAbort("item wake preparation: crate native break strike missed");return false;}
  return true;
  }

void itemWakeBegin(World& world,Npc& player) {
  const auto context=itemWakeContext();
  auto& state=context.itemWake;
  if(state.itemWakeItems.empty() || state.itemWakeRunning) {arenaAbort("item wake setup required");return;}
  itemWakeFields(world);
  for(const auto& sample:state.itemWakeItems) {
    const auto center=(sample.collisionBounds[0]+sample.collisionBounds[1])*.5f;
    const auto floor=world.physic()->landRay(Vec3(center.x,std::max(sample.floorY+500.f,sample.collisionBounds[1].y+20.f),center.z),10000.f);
    const float offset=floor.hasCol?sample.collisionBounds[0].y-floor.v.y:std::numeric_limits<float>::infinity();
    if(state.itemWakeCase=="sword" && sample.item->isDynamic() && sample.item->physicsSleeping())
      sample.item->setPhysicsDisable();
    if(sample.item->isDynamic() || sample.preparationDive || !floor.hasCol || std::abs(offset)>5.f) {
      arenaAbort("item wake preparation: invalid resting "+sample.name+", dynamic="+std::to_string(sample.item->isDynamic())+
        ", dived="+std::to_string(sample.preparationDive)+", bottom_floor_offset_cm="+ragdollNumber(offset));return;
      }
    }
  state.itemWakePreparing=false;
  const auto center=state.itemWakeCenter;
  const bool far=state.itemWakeCase=="pile-far";
  if(state.itemWakeCase=="arrow") {
    const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
    if(symbol==size_t(-1)) {arenaAbort("item wake preparation: arrow instance unavailable");return;}
    const auto& sample=state.itemWakeItems.front();
    const auto target=(sample.collisionBounds[0]+sample.collisionBounds[1])*.5f;
    const auto from=target+Vec3(-200,0,0);
    const float speed=DynamicWorld::bulletSpeed;
    Item arrow(world,symbol,Item::T_Inventory);
    auto& bullet=world.shootArrow(arrow,from,Vec3::normalize(Archery::launch(from,target,speed)-from),speed);
    bullet.setOrigin(&player);bullet.setPrecise(1.f);
    event("item_wake_arrow","\"from_cm\":"+ragdollVector(from)+",\"target_cm\":"+ragdollVector(target));
    }
  else if(state.itemWakeCase=="sword") {
    auto& npc=*state.itemWakeActors.front();
    npc.setPosition(state.itemWakeSwordRoot);npc.setDirection({0,0,1});npc.updateTransform();
    state.itemWakeCalibrationSample=false;
    if(!npc.setAnim(Npc::Anim::AttackL)) {arenaAbort("item wake preparation: repeated native NPC sword attack refused");return;}
    event("item_wake_sword","\"npc\":"+jsonStr(TestBench::who(&npc))+
      ",\"animation\":"+jsonStr(npc.animNames())+",\"items\":3,\"root_cm\":"+ragdollVector(npc.position()));
    }
  else if(state.itemWakeCase=="ragdoll" || state.itemWakeCase=="pile" || far || state.itemWakeCase=="event") {
    const unsigned count=(state.itemWakeCase=="pile" || far)?6:1;
    for(unsigned i=0;i<count;++i) {
      const auto at=center+Vec3(count==1?0.f:(float(i%3)-1.f)*120.f,220.f,float(i/3)*120.f-40.f);
      auto* npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),at);
      if(!npc) {arenaAbort("item wake preparation: falling NPC insertion failed");return;}
      npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
      npc->handle().flags=daedalus::NpcFlag(uint32_t(npc->handle().flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
      npc->setLastHitBy(&player);
      if(!npc->setRagdollDown(true)) {arenaAbort("item wake preparation: native NPC death refused");return;}
      npc->prepareRagdoll();
      if(!npc->ragdollBody()) {arenaAbort("item wake preparation: NPC death created no ragdoll");return;}
      npc->addRagdollImpulse({0,-5,8});state.itemWakeActors.push_back(npc);
      }
    if(state.itemWakeCase=="event")state.itemWakeCase="event-ragdoll";
    }
  else if(state.itemWakeCase=="debris") {
    if(!itemWakeBreakCrate(world,player))return;
    }
  else {
    const auto instance=world.script().findSymbolIndex("ITMI_LUTE");
    if(instance==size_t(-1)) {arenaAbort("item wake preparation: rolling item instance unavailable");return;}
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(center+Vec3(-180,30,-24));
    auto* item=world.addItemDyn(instance,matrix,size_t(-1));
    if(!item || item->physicsMass()<=0 || !item->addSpellImpulse({item->physicsMass()*5.f,0,0},&player)) {
      arenaAbort("item wake preparation: rolling native Item launch failed");return;
      }
    state.itemWakeMover=item;
    }
  if(far) {
    const float infinity=std::numeric_limits<float>::infinity();
    Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);
    Vec3 itemLow=low,itemHigh=high;size_t parts=0;
    for(const auto& body:world.physic()->waterBodySnapshot()) {
      if(!body.ragdoll || std::none_of(state.itemWakeActors.begin(),state.itemWakeActors.end(),
        [&](const auto* npc){return npc->ragdollBody()==body.ragdoll;}))continue;
      ++parts;
      low.x=std::min(low.x,body.collisionBounds[0].x);low.z=std::min(low.z,body.collisionBounds[0].z);
      high.x=std::max(high.x,body.collisionBounds[1].x);high.z=std::max(high.z,body.collisionBounds[1].z);
      }
    if(!parts) {arenaAbort("item wake preparation: far pile lacks native ragdoll bounds");return;}
    for(const auto& sample:state.itemWakeItems) {
      itemLow.x=std::min(itemLow.x,sample.collisionBounds[0].x);itemLow.z=std::min(itemLow.z,sample.collisionBounds[0].z);
      itemHigh.x=std::max(itemHigh.x,sample.collisionBounds[1].x);itemHigh.z=std::max(itemHigh.z,sample.collisionBounds[1].z);
      }
    bool supported=false;std::vector<Vec3> places;std::vector<float> floors;Vec3 translation;
    for(const auto offset:{Vec3(high.x+300.f-itemLow.x,0,0),Vec3(low.x-300.f-itemHigh.x,0,0),
                           Vec3(0,0,high.z+300.f-itemLow.z),Vec3(0,0,low.z-300.f-itemHigh.z)}) {
      places.clear();floors.clear();
      for(const auto& sample:state.itemWakeItems) {
        const auto low=sample.collisionBounds[0]+offset,high=sample.collisionBounds[1]+offset;
        const auto center=(low+high)*.5f;
        const auto floor=world.physic()->landRay(Vec3(center.x,std::max(sample.floorY+500.f,high.y+20.f),center.z),1500.f);
        if(!floor.hasCol || floor.n.y<.98f)break;
        const auto at=sample.item->position()+Vec3(offset.x,floor.v.y+.05f-sample.collisionBounds[0].y,offset.z);
        if((at-player.position()).length()<1000.f)break;
        places.push_back(at);floors.push_back(floor.v.y);
        }
      if(places.size()==state.itemWakeItems.size()) {supported=true;translation=offset;break;}
      }
    if(!supported) {arenaAbort("item wake preparation: far item grid has no supported position outside ragdolls");return;}
    for(size_t i=0;i<places.size();++i) {
      auto& sample=state.itemWakeItems[i];const auto delta=places[i]-sample.item->position();
      auto pose=sample.item->transform();
      pose.set(3,0,places[i].x);pose.set(3,1,places[i].y);pose.set(3,2,places[i].z);
      sample.item->setObjMatrix(pose);
      sample.collisionBounds[0]+=delta;sample.collisionBounds[1]+=delta;sample.boundsOrigin=places[i];sample.floorY=floors[i];
      }
    event("item_wake_far_placement","\"translation_cm\":"+ragdollVector(translation)+",\"initial_minimum_axis_gap_cm\":300");
    }
  for(const auto& sample:state.itemWakeItems)
    if((sample.item->position()-player.position()).length()<1000.f) {
      arenaAbort("item wake preparation: items must remain at least ten metres from the hero");return;
      }
  itemWakeResetWindow(world,state.itemWakeCase.starts_with("event-") || state.itemWakeCase=="arrow" || state.itemWakeCase=="sword"?3000:0);
  state.itemWakePulse=world.tickCount()+100;
  event("item_wake_begin","\"case\":"+jsonStr(state.itemWakeCase)+",\"bodyless_items\":"+
    std::to_string(state.itemWakeInitiallyBodyless)+",\"ragdolls\":"+std::to_string(state.itemWakeActors.size()));
  }

void itemWakeEventDebris(World& world,Npc& player) {
  const auto context=itemWakeContext();
  auto& state=context.itemWake;
  if(state.itemWakeCase!="event-ragdoll" || state.itemWakeRunning || state.itemWakeItems.size()!=50) {
    arenaAbort("item wake event-debris requires the completed one-corpse event with the same fifty items");return;
    }
  state.itemWakeCase="event-debris";
  if(!itemWakeBreakCrate(world,player))return;
  itemWakeResetWindow(world,3000);
  event("item_wake_begin","\"case\":\"event-debris\",\"bodyless_items\":"+
    std::to_string(state.itemWakeInitiallyBodyless)+",\"ragdolls\":"+std::to_string(state.itemWakeActors.size())+
    ",\"same_items_reused\":true");
  }

void itemWakeCheckpoint(World& world) {
  const auto context=itemWakeContext();
  auto& state=context.itemWake;
  itemWakeFields(world);
  if(state.itemWakeCase=="sword" && std::any_of(std::begin(state.itemWakeBladeCrossed),std::end(state.itemWakeBladeCrossed),
      [](bool crossed){return !crossed;})) {
    arenaAbort("item wake preparation: actual sword sweep missed resting Item bounds");return;
    }
  unsigned moved=0,created=0,falls=0,below=0;float penetration=0;std::string items="[";
  for(const auto& sample:state.itemWakeItems) {
    moved+=sample.travel>5.f;created+=sample.bodyCreated;falls+=sample.fell;below+=sample.below;
    penetration=std::max(penetration,sample.penetration);
    if(items.size()>1)items+=',';
    items+="{\"instance\":"+jsonStr(sample.name)+",\"initial_cm\":"+ragdollVector(sample.initial)+
      ",\"position_cm\":"+ragdollVector(sample.item->position())+",\"max_displacement_cm\":"+
      ragdollNumber(sample.travel)+",\"max_floor_penetration_cm\":"+ragdollNumber(sample.penetration)+
      ",\"body_created\":"+(sample.bodyCreated?"true":"false")+"}";
    }
  auto times=state.itemWakePhysics;std::sort(times.begin(),times.end());
  const float p95=times.empty()?0.f:times[size_t(.95*double(times.size()-1))];
  const bool enabled=Gothic::settingsGetI("GAME","item_wake")!=0;
  const bool reportOnly=state.itemWakeCase=="pile",far=state.itemWakeCase=="pile-far",eventCase=state.itemWakeCase.starts_with("event-");
  const auto elapsed=state.itemWakeSample-state.itemWakeStart;
  const auto checkpointElapsed=world.tickCount()-state.itemWakeStart;
  const bool safe=falls==0 && below==0;
  const bool weapon=state.itemWakeCase=="arrow" || state.itemWakeCase=="sword";
  const bool pass=reportOnly || (safe && (far ? times.size()==720 && created==0 && state.itemWakeInitiallyBodyless==50 &&
    state.itemWakeQueryOverlaps==0 && state.itemWakeMinimumGap>50.f : eventCase ?
    checkpointElapsed>=3000 && times.size()>=179 && times.size()<=180 && p95<=2.f :
    enabled ? moved>0 && created>0 && (!weapon || moved==state.itemWakeItems.size()) : moved==0 && created==0));
  event("item_wake_checkpoint","\"case\":"+jsonStr(state.itemWakeCase)+",\"enabled\":"+
    (enabled?"true":"false")+",\"item_hulls\":"+(Gothic::physicsFlags().itemHulls?"true":"false")+
    ",\"report_only\":"+(reportOnly?"true":"false")+",\"initially_bodyless_items\":"+std::to_string(state.itemWakeInitiallyBodyless)+
    ",\"moved_over_5cm\":"+std::to_string(moved)+",\"bodies_created\":"+std::to_string(created)+
    ",\"floor_falls\":"+std::to_string(falls)+",\"below_world\":"+std::to_string(below)+
    ",\"max_floor_penetration_cm\":"+ragdollNumber(penetration)+",\"physics_samples\":"+std::to_string(times.size())+
    ",\"native_step_samples\":"+std::to_string(times.size())+",\"zero_step_frames\":"+std::to_string(state.itemWakeZeroFrames)+
    ",\"physics_first_sample_ms\":"+(times.empty()?"null":std::to_string(state.itemWakeFirstPhysicsTick-state.itemWakeStart))+
    ",\"physics_last_sample_ms\":"+(times.empty()?"null":std::to_string(state.itemWakeLastPhysicsTick-state.itemWakeStart))+
    ",\"window_ms\":"+std::to_string(state.itemWakeWindow)+",\"sampled_elapsed_ms\":"+std::to_string(elapsed)+
    ",\"checkpoint_elapsed_ms\":"+std::to_string(checkpointElapsed)+
    ",\"far_query_overlaps\":"+std::to_string(state.itemWakeQueryOverlaps)+",\"minimum_aggregate_gap_cm\":"+
    (far?ragdollNumber(state.itemWakeMinimumGap):"null")+",\"physics_p95_ms\":"+ragdollNumber(p95)+
    ",\"pass\":"+(pass?"true":"false")+",\"items\":"+items+"]");
  state.itemWakeRunning=false;
  if(!pass)arenaAbort("item wake native contact or physics budget failed");
  }

bool itemWakeCommand(const std::vector<std::string>& a,World& w,Npc& pl) {
  const auto& c=a[0];
  if(c=="item-wake" && a.size()==3 && a[1]=="setup")itemWakeSetup(w,pl,a[2]);
  else if(c=="item-wake" && a.size()==2 && a[1]=="begin")itemWakeBegin(w,pl);
  else if(c=="item-wake" && a.size()==2 && a[1]=="checkpoint")itemWakeCheckpoint(w);
  else if(c=="item-wake" && a.size()==2 && a[1]=="event-debris")itemWakeEventDebris(w,pl);
  else return false;
  return true;
  }

}
