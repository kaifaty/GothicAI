#include "testbench_internal.h"
#include "game/navigationeyeworld.h"
#include "testbench.h"

#include <Tempest/MemWriter>
#include <Tempest/Pixmap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "game/serialize.h"
#include "gothic.h"
#include "camera.h"
#include "graphics/mesh/protomesh.h"
#include "world/aiqueue.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "game/gamesession.h"
#include "world/bullet.h"
#include "world/movablevobs.h"

using namespace Tempest;

namespace testbench_detail {

void ownMeshWalk(World& world,Npc& player) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  if(state.ownMeshZen==uint32_t(-1)) {arenaAbort("own-mesh requires setup");return;}
  auto start=state.ownMeshAxis+state.ownMeshApproach*270.f;
  const auto floor=world.physic()->landRay(start+Vec3(0,200,0),700.f);
  if(!floor.hasCol || floor.n.y<.8f) {arenaAbort("own-mesh approach has no supported ground");return;}
  start.y=floor.v.y;
  player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.clearSpeed();
  player.setPosition(start);player.setDirection(-state.ownMeshApproach);player.updateTransform();
  state.ownMeshHeroStart=start;state.ownMeshHeroHistory.clear();state.ownMeshWalking=true;state.ownMeshMaxCrossTrack=0;
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(body.zenId==state.ownMeshZen)state.ownMeshBodyStart=movablePosition(body.transform);
  }

void ownMeshSetup(World& world,Npc& player,uint32_t zenId,const Vec3& approach) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  const auto all=world.physic()->movableVobSnapshot();
  const auto found=std::find_if(all.begin(),all.end(),[zenId](const auto& body){return body.zenId==zenId;});
  if(found==all.end() || !found->visual.starts_with("NW_HARBOUR_BARREL_01") || approach.length()<.9f) {
    arenaAbort("own-mesh requires the authored NW_HARBOUR_BARREL_01 and a horizontal approach");return;
    }
  state.ownMeshZen=zenId;state.ownMeshApproach=Vec3::normalize(approach);
  state.ownMeshAxis=(found->localBounds[0]+found->localBounds[1])*.5f;
  found->origin.project(state.ownMeshAxis);
  std::string corridor="[";
  for(float distance=270.f;distance>=90.f;distance-=30.f) {
    auto point=state.ownMeshAxis+state.ownMeshApproach*distance;
    const auto floor=world.physic()->landRay(point+Vec3(0,200,0),700.f);
    if(!floor.hasCol || floor.n.y<.8f) {arenaAbort("own-mesh straight corridor has no supported ground");return;}
    if(corridor.size()>1)corridor+=',';
    corridor+=ragdollVector(floor.v);
    }
  ownMeshWalk(world,player);state.ownMeshWalking=false;
  context.cam=C_Hold;
  const auto camera=state.ownMeshAxis+Vec3(-.25879f,0,.96593f)*430.f+Vec3(0,160,0);
  lookFrom(camera,state.ownMeshAxis);
  event("own_mesh_setup","\"zen_id\":"+std::to_string(zenId)+",\"visual\":"+jsonStr(found->visual)+
    ",\"origin\":"+ragdollMatrix(found->origin)+",\"axis_half_height_cm\":"+ragdollVector(state.ownMeshAxis)+
    ",\"approach_direction\":"+ragdollVector(state.ownMeshApproach)+
    ",\"approach_floor_cm\":"+corridor+"]"+
    ",\"hero_start_cm\":"+ragdollVector(state.ownMeshHeroStart)+",\"camera_cm\":"+ragdollVector(camera));
  }

void ownMeshArrow(World& world,Npc& player) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  if(state.ownMeshZen==uint32_t(-1)) {arenaAbort("own-mesh arrow requires setup");return;}
  const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
  if(symbol==size_t(-1)) {arenaAbort("own-mesh arrow instance unavailable");return;}
  const auto target=state.ownMeshAxis,from=target+state.ownMeshApproach*270.f;
  const float speed=DynamicWorld::bulletSpeed;
  const auto direction=Vec3::normalize(Archery::launch(from,target,speed)-from);
  Item arrow(world,symbol,Item::T_Inventory);
  auto& bullet=world.shootArrow(arrow,from,direction,speed);
  bullet.setOrigin(&player);bullet.setPrecise(1.f);
  state.ownMeshArrowHits=0;
  event("own_mesh_arrow","\"zen_id\":"+std::to_string(state.ownMeshZen)+",\"from_cm\":"+
    ragdollVector(from)+",\"target_cm\":"+ragdollVector(target)+",\"speed_cm_ms\":"+ragdollNumber(speed));
  }

void ownMeshClearance(World& world,Npc& player) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  if(state.ownMeshZen==uint32_t(-1)) {arenaAbort("own-mesh clearance requires setup");return;}
  auto bodies=world.physic()->movableVobContacts(100.f);
  // With MOVABLE_VOBS=0 there is no native dynamic hull to overlap. Keep the
  // same line/camera guards against the original source collision mesh.
  if(!world.physic()->movableVobsEnabled()) {
    for(const auto& source:world.physic()->movableVobSnapshot())if(source.zenId==state.ownMeshZen) {
      DynamicWorld::MovableVobContacts body;
      body.zenId=source.zenId;body.visual=source.visual;body.disposition=source.disposition;
      body.hullMin=body.hullMax=state.ownMeshAxis;
      for(unsigned corner=0;corner<8;++corner) {
        Vec3 point(source.localBounds[corner&1].x,source.localBounds[(corner>>1)&1].y,source.localBounds[(corner>>2)&1].z);
        source.origin.project(point);
        body.hullMin=Vec3(std::min(body.hullMin.x,point.x),std::min(body.hullMin.y,point.y),std::min(body.hullMin.z,point.z));
        body.hullMax=Vec3(std::max(body.hullMax.x,point.x),std::max(body.hullMax.y,point.y),std::max(body.hullMax.z,point.z));
        }
      bodies.push_back(std::move(body));break;
      }
    }
  for(const auto& body:bodies) {
    if(body.zenId!=state.ownMeshZen)continue;
    std::string contacts="[";float gap=100.f;
    for(const auto& hit:body.contacts) {
      const bool floor=hit.category==DynamicWorld::C_Landscape && hit.below;
      if(!floor)gap=std::min(gap,-hit.depth);
      if(contacts.size()>1)contacts+=',';
      contacts+="{\"other\":"+jsonStr(hit.other)+",\"zen_id\":"+std::to_string(hit.otherZenId)+
        ",\"category\":"+std::to_string(hit.category)+",\"floor_support\":"+(floor?"true":"false")+
        ",\"depth_cm\":"+ragdollNumber(hit.depth)+",\"point_cm\":"+ragdollVector(hit.point)+"}";
      }
    event("own_mesh_clearance","\"zen_id\":"+std::to_string(body.zenId)+
      ",\"enabled\":"+(body.enabled?"true":"false")+",\"disposition\":"+jsonStr(body.disposition)+
      ",\"margin_cm\":100,\"minimum_non_floor_gap_cm\":"+(body.enabled?ragdollNumber(gap):"null")+",\"contacts\":"+contacts+"]");
    if(gap<=0.f || (world.physic()->movableVobsEnabled() && !body.enabled)) {
      arenaAbort("own-mesh barrel must retain a positive gap and remain movable when enabled");return;
      }
    const auto extent=(body.hullMax-body.hullMin)*.5f;
    const auto axis=state.ownMeshAxis;
    const auto* camera=Gothic::inst().camera();bool framed=camera!=nullptr,visible=true;
    Vec3 ndcMin(1e9f,1e9f,1e9f),ndcMax(-1e9f,-1e9f,-1e9f);
    const auto eye=axis+Vec3(-.25879f,0,.96593f)*430.f+Vec3(0,160,0);
    for(unsigned corner=0;corner<8;++corner) {
      const Vec3 point((corner&1)?body.hullMax.x:body.hullMin.x,(corner&2)?body.hullMax.y:body.hullMin.y,
                       (corner&4)?body.hullMax.z:body.hullMin.z);
      float x=point.x,y=point.y,z=point.z,w=1;
      if(camera)camera->viewProj().project(x,y,z,w);
      if(w<=0)framed=false;
      else {
        x/=w;y/=w;z/=w;
        framed=framed && std::abs(x)<.95f && std::abs(y)<.95f && z>=0.f && z<=1.f;
        ndcMin=Vec3(std::min(ndcMin.x,x),std::min(ndcMin.y,y),std::min(ndcMin.z,z));
        ndcMax=Vec3(std::max(ndcMax.x,x),std::max(ndcMax.y,y),std::max(ndcMax.z,z));
        }
      const auto hit=world.physic()->ray(eye,point);
      const auto own=world.physic()->rayMovableVob(eye,point);
      visible=visible && (!hit.hasCol || (own && own->zenId==state.ownMeshZen));
      }
    event("own_mesh_camera","\"zen_id\":"+std::to_string(body.zenId)+",\"eye_cm\":"+ragdollVector(eye)+
      ",\"framed\":"+(framed?"true":"false")+",\"visible\":"+(visible?"true":"false")+
      ",\"ndc_min\":"+ragdollVector(ndcMin)+",\"ndc_max\":"+ragdollVector(ndcMax));
    if(!framed || !visible) {arenaAbort("own-mesh camera must see the complete barrel from the free side");return;}
    for(unsigned line=0;line<3;++line) {
      const auto direction=line==2?-state.ownMeshApproach:state.ownMeshApproach;
      float edge=std::numeric_limits<float>::infinity();
      if(std::abs(direction.x)>.001f)edge=std::min(edge,extent.x/std::abs(direction.x));
      if(std::abs(direction.z)>.001f)edge=std::min(edge,extent.z/std::abs(direction.z));
      for(float y:{axis.y,body.hullMin.y+40.f,body.hullMin.y+110.f,body.hullMin.y+170.f}) {
        if(line==1 && y!=axis.y)continue;
        auto from=axis+direction*(edge+2.f);from.y=y;
        const auto to=from+direction*200.f;
        const auto hit=navigationRay(*world.physic(),from,to,&player);
        const auto visual=world.physic()->ray(from,to);
        event("own_mesh_line","\"zen_id\":"+std::to_string(body.zenId)+",\"line\":"+
          jsonStr(line==0?"hero_approach":line==1?"arrow":"push")+",\"from_cm\":"+ragdollVector(from)+
          ",\"to_cm\":"+ragdollVector(to)+",\"length_cm\":200,\"physical_hit\":"+(hit.hasCol?"true":"false")+
          ",\"physical_hit_distance_cm\":"+ragdollNumber(hit.hasCol?(hit.point-from).length():200.f)+
          ",\"physical_kind\":"+std::to_string(unsigned(hit.kind))+",\"visual_hit\":"+(visual.hasCol?"true":"false")+
          ",\"visual_hit_distance_cm\":"+ragdollNumber(visual.hasCol?(visual.v-from).length():200.f));
        if(hit.hasCol || visual.hasCol) {arenaAbort("own-mesh approach, arrow and push require clear 200 cm lines");return;}
        }
      }
    return;
    }
  arenaAbort("own-mesh clearance body disappeared");
  }

void ownMeshFields(World& world,Npc& player) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  if(!state.ownMeshWalking)return;
  const Vec3 side(-state.ownMeshApproach.z,0,state.ownMeshApproach.x);
  const float crossTrack=Vec3::dotProduct(player.position()-state.ownMeshHeroStart,side);
  state.ownMeshMaxCrossTrack=std::max(state.ownMeshMaxCrossTrack,std::abs(crossTrack));
  // Steer the held forward input back to the straight approach line. This
  // prevents a slanted barrel facet from turning the test into a walk around
  // it; all positions and velocities still come from the native controller.
  player.setDirection(Vec3::normalize(-state.ownMeshApproach-side*(crossTrack*.1f)));
  state.ownMeshHeroHistory.emplace_back(world.tickCount(),player.position());
  while(state.ownMeshHeroHistory.size()>1 &&
        state.ownMeshHeroHistory[1].first+500<world.tickCount())state.ownMeshHeroHistory.pop_front();
  }

void ownMeshCheckpoint(World& world,Npc& player) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  if(state.ownMeshZen==uint32_t(-1)) {arenaAbort("own-mesh checkpoint requires setup");return;}
  ownMeshFields(world,player);
  const auto hero=player.position(),delta=hero-state.ownMeshAxis;
  const auto& history=state.ownMeshHeroHistory;
  const uint64_t elapsed=history.empty()?0:world.tickCount()-history.front().first;
  const auto movement=history.empty()?Vec3():hero-history.front().second;
  const float speed=elapsed?Vec3(movement.x,0,movement.z).length()*1000.f/float(elapsed):0.f;
  for(const auto& body:world.physic()->movableVobSnapshot()) {
    if(body.zenId!=state.ownMeshZen)continue;
    const auto position=movablePosition(body.transform);
    event("own_mesh_checkpoint","\"zen_id\":"+std::to_string(body.zenId)+
      ",\"enabled\":"+(body.enabled?"true":"false")+",\"disposition\":"+jsonStr(body.disposition)+
      ",\"hero_start_cm\":"+ragdollVector(state.ownMeshHeroStart)+",\"hero_position_cm\":"+ragdollVector(hero)+
      ",\"hero_axis_distance_cm\":"+ragdollNumber(std::hypot(delta.x,delta.z))+
      ",\"max_cross_track_cm\":"+ragdollNumber(state.ownMeshMaxCrossTrack)+
      ",\"final_sample_ms\":"+std::to_string(elapsed)+",\"final_speed_cm_s\":"+ragdollNumber(speed)+
      ",\"body_start_cm\":"+ragdollVector(state.ownMeshBodyStart)+",\"body_position_cm\":"+ragdollVector(position)+
      ",\"body_displacement_cm\":"+ragdollNumber((position-state.ownMeshBodyStart).length())+
      ",\"body_transform\":"+ragdollMatrix(body.transform)+",\"npc_contact\":"+(body.touchedByNpc?"true":"false")+
      ",\"mass_kg\":"+ragdollNumber(body.mass)+",\"hull_volume_m3\":"+ragdollNumber(body.hullVolume)+
      ",\"material_group\":"+std::to_string(unsigned(body.material))+",\"density_kg_m3\":"+ragdollNumber(body.density)+
      ",\"mass_multiplier\":"+ragdollNumber(body.massMultiplier)+",\"arrow_hits\":"+std::to_string(state.ownMeshArrowHits));
    state.ownMeshWalking=false;return;
    }
  arenaAbort("own-mesh authored body disappeared");
  }

std::string ownMeshChestInventory(World& world,const Npc& player) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  const auto& fixture=state.ownMeshChest;
  std::string rows="[";
  for(const auto& [id,count]:fixture.contents) {
    if(rows.size()>1)rows+=',';
    rows+="{\"class\":"+jsonStr(world.script().findSymbol(id)->name())+
      ",\"chest_before\":"+std::to_string(count)+",\"chest_now\":"+
      std::to_string(fixture.chest->inventory().itemCount(id))+
      ",\"hero_before\":"+std::to_string(fixture.heroBefore.at(id))+
      ",\"hero_now\":"+std::to_string(player.inventory().itemCount(id))+"}";
    }
  return rows+"]";
  }

void ownMeshChestSnapshot(World& world,const Npc& player,std::string_view label) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  const auto& fixture=state.ownMeshChest;
  if(!fixture.chest) {arenaAbort("own-mesh-chest snapshot needs setup");return;}
  const auto& visual=fixture.chest->objVisual();
  const auto& lid=visual.bone(fixture.lid);
  // Midpoint of the original lid's upper front bounding-box edge, in the
  // lid node's local centimetres. Both models measure the same fixed point.
  Vec3 front(0,27.36672f,-58.41916f);
  lid.project(front);
  event("own_mesh_chest_snapshot","\"label\":"+jsonStr(label)+
    ",\"zen_id\":"+std::to_string(fixture.chest->zenId())+
    ",\"mob_id\":"+std::to_string(world.mobsiId(fixture.chest))+
    ",\"mob_cm\":"+ragdollVector(fixture.chest->position())+
    ",\"state\":"+std::to_string(fixture.chest->stateId())+
    ",\"animation\":"+jsonStr(visual.animNames())+
    ",\"lid_node\":\"BIP01 CHEST_BIG_1\",\"lid_world\":"+ragdollMatrix(lid)+
    ",\"front_probe_local_cm\":[0,27.36672,-58.41916],\"front_probe_world_cm\":"+ragdollVector(front)+
    ",\"hero_cm\":"+ragdollVector(player.position())+
    ",\"hero_attached\":"+(player.interactive()==fixture.chest?"true":"false")+
    ",\"hero_gold\":"+std::to_string(player.inventory().itemCount(world.script().goldId()->index()))+
    ",\"hero_inventory\":"+ragdollInventory(player,world)+
    ",\"chest_empty\":"+(fixture.chest->inventory().isEmpty()?"true":"false")+
    ",\"items\":"+ownMeshChestInventory(world,player));
  }

void ownMeshChestFields(World& world,const Npc& player) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  auto& fixture=state.ownMeshChest;
  if(!fixture.chest || !fixture.opened)return;
  const auto animation=fixture.chest->objVisual().animNames();
  if(animation.find("T_S0_2_S1")!=std::string::npos)fixture.sawTransition=true;
  if(animation==fixture.lastAnimation)return;
  fixture.lastAnimation=animation;
  ownMeshChestSnapshot(world,player,"animation");
  }

void ownMeshChestSetup(World& world,Npc& player,const Vec3& near,uint32_t zenId,bool reload) {
  const auto context=ownMeshContext();
  auto& state=context.ownMesh;
  auto& fixture=state.ownMeshChest;
  fixture=OwnMesh::OwnMeshChest{};
  for(uint32_t id=0;auto* mob=world.mobsiById(id);++id) {
    if(mob->zenId()!=zenId || (mob->position()-near).length()>5.f)continue;
    const auto* mesh=mob->objVisual().protoMesh();
    if(!mob->isContainer() || !mesh || !mesh->fname.starts_with("CHESTBIG_NW_NORMAL_OPEN") ||
       mesh->findNode("BIP01 CHEST_BIG_1")==size_t(-1) || mesh->findNode("ZS_POS0")==size_t(-1))continue;
    fixture.chest=mob;fixture.lid=mesh->findNode("BIP01 CHEST_BIG_1");break;
    }
  if(!fixture.chest) {arenaAbort("own-mesh-chest could not find the exact authored CHESTBIG_NW_NORMAL_OPEN");return;}
  auto& chest=*fixture.chest;
  if(reload) {
    // Observe the complete loaded session before moving its hero or changing
    // container contents. The caller compares this snapshot with the save.
    ownMeshChestSnapshot(world,player,"loaded");
    const bool pass=chest.stateId()==1 && chest.inventory().isEmpty() &&
      chest.objVisual().animNames().find("S_S1")!=std::string::npos;
    event("own_mesh_chest_loaded","\"pass\":"+std::string(pass?"true":"false"));
    if(!pass)arenaAbort("own-mesh-chest loaded session lost open state or taken contents");
    return;
    }
  if(chest.isLocked() || !chest.isAvailable()) {arenaAbort("own-mesh-chest needs the free unlocked authored chest");return;}
  const bool seeded=chest.inventory().isEmpty();
  if(seeded)chest.inventory().addItem("ITMI_GOLD",1,world);
  for(auto item=chest.inventory().iterator(Inventory::T_Inventory);item.isValid();++item)
    fixture.contents[item->clsId()]+=item.count();
  if(fixture.contents.empty()) {arenaAbort("own-mesh-chest has no transferable contents");return;}
  for(const auto& [id,count]:fixture.contents)fixture.heroBefore[id]=player.inventory().itemCount(id);
  player.setInteraction(nullptr,true);player.clearAiQueue();player.clearState(false);player.stopWalking();player.closeWeapon(true);
  const auto socket=chest.nearestPoint(player);
  auto away=socket-chest.position();away.y=0;
  if(away.length()<1.f) {arenaAbort("own-mesh-chest has no horizontal use socket");return;}
  away=Vec3::normalize(away);
  characterFreezeNear(world,player,socket,socket);
  Vec3 start;bool prepared=false;
  for(float distance:{65.f,90.f,35.f,0.f}) {
    auto at=socket+away*distance;
    const auto floor=world.physic()->landRay(Vec3(at.x,socket.y+80.f,at.z),250.f);
    const bool supported=floor.hasCol && floor.n.y>=.8f && std::abs(floor.v.y-chest.position().y)<=80.f;
    if(!supported) {
      event("own_mesh_chest_approach","\"socket_cm\":"+ragdollVector(socket)+
        ",\"distance_cm\":"+ragdollNumber(distance)+",\"floor_cm\":"+ragdollVector(floor.v)+
        ",\"supported\":false");
      continue;
      }
    player.setPosition(floor.v);player.setDirection(chest.position()-floor.v);player.updateTransform();
    const bool collision=player.hasCollision();
    const float useDistance=(player.centerPosition()-socket).length();
    const bool clear=!world.physic()->ray(floor.v+Vec3(0,100,0),socket+Vec3(0,100,0)).hasCol;
    event("own_mesh_chest_approach","\"socket_cm\":"+ragdollVector(socket)+
      ",\"distance_cm\":"+ragdollNumber(distance)+",\"floor_cm\":"+ragdollVector(floor.v)+
      ",\"supported\":true,\"collision\":"+(collision?"true":"false")+
      ",\"use_distance_cm\":"+ragdollNumber(useDistance)+",\"clear\":"+(clear?"true":"false"));
    if(collision || useDistance>155.f || !clear)continue;
    start=floor.v;prepared=true;break;
    }
  if(!prepared) {arenaAbort("own-mesh-chest has no supported collision-free ordinary interaction approach");return;}
  const Vec3 side(-away.z,0,away.x),target=chest.position()+Vec3(0,65,0);
  const auto camera=target+away*150.f+side*100.f+Vec3(0,100,0);
  context.cam=C_Hold;context.camAuto=false;lookFrom(camera,target);
  event("own_mesh_chest_setup","\"zen_id\":"+std::to_string(zenId)+
    ",\"mob_id\":"+std::to_string(world.mobsiId(&chest))+
    ",\"visual\":"+jsonStr(chest.objVisual().protoMesh()->fname)+
    ",\"world\":"+jsonStr(world.name())+",\"mob_cm\":"+ragdollVector(chest.position())+
    ",\"socket_cm\":"+ragdollVector(socket)+",\"hero_start_cm\":"+ragdollVector(start)+
    ",\"camera_cm\":"+ragdollVector(camera)+",\"camera_target_cm\":"+ragdollVector(target)+
    ",\"seeded_gold\":"+(seeded?"true":"false")+",\"items\":"+ownMeshChestInventory(world,player));
  ownMeshChestSnapshot(world,player,"closed");
  }

bool ownMeshCommand(const OwnMeshContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  auto& state=context.ownMesh;
  const auto& c=a[0];
  if(c=="own-mesh-chest" && a.size()==6 && (a[1]=="setup" || a[1]=="reload"))
    ownMeshChestSetup(w,pl,Vec3(num(a,2,0),num(a,3,0),num(a,4,0)),uint32_t(std::stoul(a[5])),a[1]=="reload");
  else if(c=="own-mesh-chest" && a.size()==2 && a[1]=="open") {
    auto& fixture=state.ownMeshChest;
    if(!fixture.chest) {arenaAbort("own-mesh-chest open needs setup");return true;}
    ctrl.clearInput();ctrl.clearFocus();ctrl.tickFocus();
    const bool selected=ctrl.focus().interactive==fixture.chest;
    event("own_mesh_chest_focus","\"pass\":"+std::string(selected?"true":"false")+
      ",\"selected_mob_id\":"+std::to_string(w.mobsiId(ctrl.focus().interactive))+
      ",\"expected_mob_id\":"+std::to_string(w.mobsiId(fixture.chest))+
      ",\"hero_cm\":"+ragdollVector(pl.position()));
    if(!selected) {arenaAbort("own-mesh-chest ordinary focus did not select the exact authored chest");return true;}
    fixture.opened=ctrl.interact(*fixture.chest) && pl.interactive()==fixture.chest;
    event("own_mesh_chest_open","\"pass\":"+std::string(fixture.opened?"true":"false")+
      ",\"native_path\":\"PlayerControl::interact/InventoryMenu::open/Npc::setInteraction\"");
    if(!fixture.opened)arenaAbort("own-mesh-chest native player interaction did not attach");
    }
  else if(c=="own-mesh-chest" && a.size()==3 && a[1]=="snapshot")ownMeshChestSnapshot(w,pl,a[2]);
  else if(c=="own-mesh-chest" && a.size()==2 && a[1]=="take") {
    auto& fixture=state.ownMeshChest;
    if(!fixture.chest || !fixture.opened || fixture.chest->stateId()!=1 ||
       fixture.chest->objVisual().animNames().find("S_S1")==std::string::npos || !fixture.sawTransition) {
      arenaAbort("own-mesh-chest take needs native T_S0_2_S1 followed by S_S1");return true;
      }
    // Exactly the transfer called by InventoryMenu::onTakeStuff for a chest;
    // this preserves native inventory removal and the player's taking deed.
    for(const auto& [id,count]:fixture.contents)pl.addItem(id,*fixture.chest,count);
    fixture.taken=true;
    ownMeshChestSnapshot(w,pl,"taken");
    }
  else if(c=="own-mesh-chest" && a.size()==2 && a[1]=="check") {
    const auto& fixture=state.ownMeshChest;
    bool pass=fixture.chest && fixture.opened && fixture.sawTransition && fixture.taken &&
      fixture.chest->stateId()==1 && fixture.chest->inventory().isEmpty() &&
      fixture.chest->objVisual().animNames().find("S_S1")!=std::string::npos;
    for(const auto& [id,count]:fixture.contents)
      pass=pass && pl.inventory().itemCount(id)==fixture.heroBefore.at(id)+count;
    event("own_mesh_chest_check","\"pass\":"+std::string(pass?"true":"false")+
      ",\"saw_transition\":"+(fixture.sawTransition?"true":"false")+
      ",\"items\":"+ownMeshChestInventory(w,pl));
    if(!pass)arenaAbort("own-mesh-chest did not preserve native open state and exact item gain");
    }
  else if(c=="own-mesh-chest" && a.size()==3 && a[1]=="save") {
    if(!state.ownMeshChest.chest || a[2].empty() ||
       a[2].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) {
      arenaAbort("own-mesh-chest save needs a chest fixture and a simple label");return true;
      }
    try {
      ownMeshChestSnapshot(w,pl,"saved");std::vector<uint8_t> bytes;
      {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
      Gothic::inst().gameSession()->save(out,"own mesh chest "+a[2],preview);}
      std::ofstream file(outDir+"/session-"+a[2]+".sav",std::ios::binary);
      file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
      if(!file)throw std::runtime_error("cannot write complete own-mesh-chest session save");
      event("own_mesh_chest_save","\"archive\":"+jsonStr("session-"+a[2]+".sav")+",\"bytes\":"+std::to_string(bytes.size()));
      } catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="own-mesh" && a.size()==5 && a[1]=="setup")
    ownMeshSetup(w,pl,uint32_t(std::stoul(a[2])),Vec3(num(a,3,0),0,num(a,4,0)));
  else if(c=="own-mesh" && a.size()==2 && a[1]=="arrow")ownMeshArrow(w,pl);
  else if(c=="own-mesh" && a.size()==2 && a[1]=="clearance")ownMeshClearance(w,pl);
  else if(c=="own-mesh" && a.size()==2 && a[1]=="walk")ownMeshWalk(w,pl);
  else if(c=="own-mesh" && a.size()==2 && a[1]=="checkpoint")ownMeshCheckpoint(w,pl);
  else return false;
  return true;
  }

}
