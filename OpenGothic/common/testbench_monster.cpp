#include "testbench_internal.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <Tempest/Pixmap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "appcadence.h"
#include "game/gamescript.h"
#include "game/serialize.h"
#include "gothic.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "resources.h"
#include "world/aiqueue.h"
#include "world/movablevobs.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

// Monster fixtures use the same MDH capsule and ordinary AI/animation movement
// as the game. Only preparation places actors; measurements never move them.
void monsterCharacterError(std::string_view reason) {
  event("monster_character_error","\"reason\":"+jsonStr(reason));arenaAbort(reason);
  }

Npc* monsterCharacterActor(World& world,std::string_view instance,const Vec3& at) {
  auto* npc=world.addNpc(world.script().findSymbolIndex(instance),at);
  if(!npc || npc->isHuman() || npc->isDown() || !npc->visualSkeleton()) {
    monsterCharacterError("monster fixture instance unavailable: "+std::string(instance));return nullptr;
    }
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->stopWalking();
  npc->setExternalControl(true);npc->setProcessPolicy(NpcProcessPolicy::AiNormal);npc->setWalkMode(WalkBit::WM_Run);
  npc->setPosition(at);npc->updateTransform();
  const auto capsule=npc->navigationCapsule();const auto& guild=world.script().guildVal();const auto g=npc->guild();
  const auto& collision=npc->visualSkeleton()->bboxCol;const float mdhHeight=collision[1].y-collision[0].y;
  event("monster_character_body","\"instance\":"+jsonStr(instance)+",\"npc_id\":"+std::to_string(world.npcId(npc))+
    ",\"guild\":"+std::to_string(g)+",\"radius_cm\":"+ragdollNumber(capsule.x)+",\"height_cm\":"+ragdollNumber(mdhHeight)+
    ",\"mdh_collision_height_cm\":"+ragdollNumber(mdhHeight)+",\"cv_height_cm\":"+ragdollNumber(mdhHeight)+
    ",\"legacy_query_height_cm\":"+ragdollNumber(capsule.y)+
    ",\"step_height_cm\":"+ragdollNumber(guild.step_height[g]>0?float(guild.step_height[g]):50.f)+
    ",\"slide_angle_deg\":"+ragdollNumber(float(guild.slide_angle[g]))+",\"mass_kg\":"+ragdollNumber(npc->physicsMass())+
    ",\"water_knee_cm\":"+ragdollNumber(float(guild.water_depth_knee[g]))+
    ",\"water_chest_cm\":"+ragdollNumber(float(guild.water_depth_chest[g]))+
    ",\"has_swim_animations\":"+(npc->hasSwimAnimations()?"true":"false"));
  return npc;
  }

void monsterCharacterClear(MonsterCharacter& state,const MonsterCharacterContext& context,World& world) {
  auto& route=state.monsterCharacterRoute;
  route.active=false;
  if(route.npc) {
    auto* npc=route.npc;route.npc=nullptr;
    for(auto& actor:state.monsterCharacterExtras)if(actor.npc==npc)actor.npc=nullptr;
    world.removeNpc(*npc);
    }
  for(auto& actor:state.monsterCharacterExtras)if(actor.npc) {
    auto* npc=actor.npc;actor.npc=nullptr;world.removeNpc(*npc);
    }
  state.monsterCharacterExtras.clear();state.monsterCharacterProps.clear();
  state.monsterCharacterBarrel=MonsterCharacter::MonsterCharacterProp();
  state.monsterCharacterMeasuring=false;state.monsterCharacterCrowd=false;state.monsterCharacterPack=false;
  context.character.characterProp.reset();context.character.characterPropId=0;context.character.characterPropTravel=0;
  state.monsterCharacterLedgeBody=DynamicWorld::Item();state.monsterCharacterLedgeShape.reset();
  route=MonsterCharacter::MonsterCharacterRoute();
  }

void monsterCharacterLedge(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player,std::string_view species,float height,float bottom) {
  const bool wolf=species=="wolf",troll=species=="troll";
  if((!wolf && !troll) || (wolf && height!=20.f && height!=40.f && height!=55.f) ||
     (troll && height!=40.f && height!=80.f) ||
     (bottom!=0.f && !(wolf && height==40.f && bottom==-100.f))) {
    monsterCharacterError("ledge expects wolf 20|40|55 or troll 40|80 centimetres");return;
    }
  monsterCharacterClear(state,context,world);
  Vec3 anchor;
  if(!characterFloor(world,Vec3(38674.5156f,4308.43604f,11976.749f),anchor)) {
    monsterCharacterError("isolated ledge lacks its verified stock anchor");return;
    }
  // One analytic riser, with a 6 m running approach and 15 m landing. The
  // elevated support excludes stock rocks/actors while remaining part of the
  // real World queried by both legacy MoveAlgo and CharacterVirtual.
  anchor.y+=10000.f;
  asset::WorldMesh mesh;mesh.materials.resize(1);mesh.materials[0].group=asset::MaterialGroup::STONE;
  const auto quad=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
    const auto index=uint32_t(mesh.vertices.size());
    for(const auto& point:{a,b,c,d})mesh.vertices.push_back({point.x,point.y,point.z});
    mesh.polygons.vertex_indices.insert(mesh.polygons.vertex_indices.end(),
      {index,index+2,index+1,index,index+3,index+2});
    mesh.polygons.material_indices.insert(mesh.polygons.material_indices.end(),{0,0});
    };
  // Gothic source winding is reversed by PhysicVbo.
  quad({-1200,0,-900},{-1200,0,900},{1500,0,900},{1500,0,-900});
  quad({0,height,-900},{0,height,900},{1500,height,900},{1500,height,-900});
  quad({0,bottom,-900},{0,bottom,900},{0,height,900},{0,height,-900});
  state.monsterCharacterLedgeShape.reset(PhysicMeshShape::load(PackedMesh(mesh,PackedMesh::PK_Physic)));
  auto matrix=Matrix4x4::mkIdentity();matrix.translate(anchor);
  state.monsterCharacterLedgeBody=world.physic()->addStaticObj(state.monsterCharacterLedgeShape.get(),matrix,"BENCH_MONSTER_SINGLE_LEDGE");
  if(state.monsterCharacterLedgeBody.isEmpty()) {monsterCharacterError("isolated ledge collision creation failed");return;}
  auto& route=state.monsterCharacterRoute;
  route.name=std::string(species)+"_ledge_"+std::to_string(unsigned(height));route.isolatedLedge=true;
  if(bottom<0.f)route.name+="_buried";
  route.start=anchor+Vec3(-600,0,0);route.goal=anchor+Vec3(800,height,0);route.tolerance=60;
  for(const auto& point:{route.start,anchor+Vec3(200,height,0),route.goal}) {
    const auto floor=world.physic()->landRay(point+Vec3(0,20,0),40);
    if(!floor.hasCol || std::abs(floor.v.y-point.y)>.1f) {
      monsterCharacterError("isolated ledge floor proof disagrees with authored surface");return;
      }
    }
  const auto face=world.physic()->ray(anchor+Vec3(-30,height*.5f,0),anchor+Vec3(30,height*.5f,0));
  if(!face.hasCol || std::abs(face.v.x-anchor.x)>.1f) {
    monsterCharacterError("isolated ledge lacks its single vertical face");return;
    }
  player.setInteraction(nullptr,true);player.clearAiQueue();player.clearGoTo();player.stopWalking();
  player.setPosition(anchor+Vec3(-600,0,500));player.updateTransform();
  route.npc=monsterCharacterActor(world,wolf?"Wolf":"Troll",route.start);if(!route.npc)return;
  route.npc->setDirection(route.goal-route.start);route.npc->updateTransform();
  route.npc->setCharacterVirtualDiagnosticsEnabled(true);
  context.subject=route.npc;state.monsterCharacterHeroStart=player.position();
  event("monster_character_route_setup","\"route\":"+jsonStr(route.name)+",\"npc_id\":"+std::to_string(world.npcId(route.npc))+
    ",\"waypoint\":\"verified_wolf_lunge+100m\",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"arrival_radius_cm\":60,\"arrival_height_tolerance_cm\":5"+
    ",\"ledge_height_cm\":"+ragdollNumber(height)+",\"face_bottom_cm\":"+ragdollNumber(bottom)+
    ",\"running_approach_cm\":600,\"landing_length_cm\":1500"+
    ",\"native_action\":\"Npc::goToPosition\",\"synthetic_geometry\":true,\"floor_probes\":3,\"face_proof\":true");
  }

bool monsterCharacterOpenLane(World& world,const Vec3& center,const Vec3& direction,float halfLength,float radius,float height,
                              Vec3& start,Vec3& goal,float maximumStep=25.f) {
  const Vec3 side(-direction.z,0,direction.x);
  if(!characterFloor(world,center-direction*halfLength,start) || !characterFloor(world,center+direction*halfLength,goal))return false;
  for(float across:{-radius,0.f,radius}) {
    Vec3 previous;
    for(unsigned sample=0;sample<=20;++sample) {
      Vec3 floor;
      if(!characterFloor(world,start+(goal-start)*(float(sample)/20.f)+side*across,floor))return false;
      if(sample && std::abs(floor.y-previous.y)>maximumStep)return false;
      if(sample)for(float y:{25.f,height*.5f,height-10.f})
        if(world.physic()->ray(previous+Vec3(0,y,0),floor+Vec3(0,y,0)).hasCol)return false;
      if(std::abs(floor.y-(start.y+(goal.y-start.y)*(float(sample)/20.f)))>50.f)return false;
      previous=floor;
      }
    }
  return true;
  }

MonsterCharacter::MonsterCharacterExtra& monsterCharacterExtra(MonsterCharacter& state,World& world,Npc* npc,std::string_view role) {
  state.monsterCharacterExtras.emplace_back();auto& actor=state.monsterCharacterExtras.back();
  actor.npc=npc;actor.role=role;actor.start=npc->position();
  actor.lastPosition=actor.start;actor.lastVisual=npc->characterVisualPosition();
  actor.progressPosition=actor.start;actor.lastSupported=actor.start;actor.progressTime=world.tickCount();
  return actor;
  }

void monsterCharacterPoint(MonsterCharacter::MonsterCharacterExtra& actor,const Vec3& at) {
  actor.points.emplace_back(at,"FP_MONSTER_VIDEO_"+actor.role+"_"+std::to_string(actor.points.size()));
  actor.points.back().groundPos=at;
  }

bool monsterCharacterProp(MonsterCharacter& state,World& world,std::string_view role,const Vec3& at) {
  const std::string visual=role=="crate"?"NW_HARBOUR_CRATE_01.3DS":"NW_HARBOUR_BARREL_01.3DS";
  const auto* mesh=Resources::loadMesh(visual);const auto* rule=MovableVobs::classifyVisual(visual);
  if(!mesh || !rule) {monsterCharacterError("stock monster prop mesh/class unavailable: "+std::string(role));return false;}
  Vec3 floor;if(!characterFloor(world,at,floor)) {monsterCharacterError("monster prop has no stock support");return false;}
  const auto* bounds=mesh->bbox();auto matrix=Matrix4x4::mkIdentity();
  matrix.translate(floor.x-(bounds[0].x+bounds[1].x)*.5f,floor.y-bounds[0].y+.2f,floor.z-(bounds[0].z+bounds[1].z)*.5f);
  asset::VirtualObject data;data.type=asset::VirtualObjectType::zCVob;data.id=uint32_t(-1);
  data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
  data.visual=std::make_shared<asset::VisualMesh>();data.visual->type=asset::VisualType::MESH;data.visual->name=visual;
  const auto before=world.physic()->movableVobSnapshot();
  auto fixture=std::make_unique<MovableFixture>();fixture->visual.setVisual(data,world,false,false);fixture->visual.setObjMatrix(matrix);
  fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
  if(fixture->body.isEmpty()) {monsterCharacterError("stock monster prop body unavailable");return false;}
  fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& m){view->setObjMatrix(m);});
  fixture->visual.setupFracture(world,fixture->body);
  uintptr_t id=0;
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;})) {id=body.id;break;}
  if(!id) {monsterCharacterError("stock monster prop snapshot unavailable");return false;}
  event("monster_character_prop_setup","\"role\":"+jsonStr(role)+",\"mass_kg\":"+ragdollNumber(fixture->body.mass())+
    ",\"position_cm\":"+ragdollVector(movablePosition(matrix))+",\"blocks_walking\":"+(rule->blocksWalking?"true":"false"));
  state.monsterCharacterProps.push_back({std::move(fixture),std::string(role),movablePosition(matrix),movablePosition(matrix),id});return true;
  }

void monsterCharacterSampleProps(MonsterCharacter& state,const MonsterCharacterContext& context,World& world) {
  if(state.monsterCharacterProps.empty() && !state.monsterCharacterBarrel.id)return;
  const auto bodies=world.physic()->movableVobSnapshot();
  const auto fragments=world.physic()->fragmentSnapshot();
  auto sample=[&](MonsterCharacter::MonsterCharacterProp& prop) {
    for(const auto& body:bodies)if(body.id==prop.id) {
      // Native snapshots retain the last intact matrix after fracture. Item's
      // body pointer has been released then, so Item::matrix() is identity.
      prop.lastIntact=movablePosition(body.transform);prop.broken=body.broken;
      prop.travel=std::max(prop.travel,(prop.lastIntact-prop.start).length());
      break;
      }
    prop.fragments=unsigned(std::count_if(fragments.begin(),fragments.end(),[&](const auto& part){return part.parentId==prop.id;}));
    };
  for(auto& prop:state.monsterCharacterProps)sample(prop);
  if(state.monsterCharacterBarrel.id) {
    sample(state.monsterCharacterBarrel);
    context.character.characterPropTravel=state.monsterCharacterBarrel.travel;
    }
  }

void monsterCharacterPropEnd(const MonsterCharacter::MonsterCharacterProp& prop,std::string_view route) {
  event("monster_character_prop_end","\"route\":"+jsonStr(route)+",\"role\":"+jsonStr(prop.role)+
    ",\"max_displacement_cm\":"+ragdollNumber(prop.travel)+",\"initial_cm\":"+ragdollVector(prop.start)+
    ",\"broken\":"+(prop.broken?"true":"false")+",\"last_intact_cm\":"+ragdollVector(prop.lastIntact)+
    ",\"fragment_count\":"+std::to_string(prop.fragments)+",\"end_cm\":"+(prop.broken?"null":ragdollVector(prop.lastIntact)));
  }

void monsterCharacterRemoveAmbient(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,const Npc& player,const Vec3& center,float radius,std::string_view site) {
  std::vector<Npc*> ambient;
  for(uint32_t id=0;id<world.npcCount();++id)if(auto* npc=world.npcById(id);npc && npc!=&player && !npc->isHuman() &&
      npc!=state.monsterCharacterRoute.npc && (npc->position()-center).length()<=radius &&
      std::none_of(state.monsterCharacterExtras.begin(),state.monsterCharacterExtras.end(),[&](const auto& actor){return actor.npc==npc;}))
    ambient.push_back(npc);
  for(auto* npc:ambient) {
    if(context.subject==npc)context.subject=nullptr;
    world.removeNpc(*npc);
    }
  event("monster_character_ambient_removed","\"site\":"+jsonStr(site)+",\"center_cm\":"+ragdollVector(center)+
    ",\"radius_cm\":"+ragdollNumber(radius)+",\"nonhuman_count\":"+std::to_string(ambient.size()));
  }

void monsterCharacterPack(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player) {
  monsterCharacterClear(state,context,world);auto& route=state.monsterCharacterRoute;route.name="wolf_pack";
  std::vector<const WayPoint*> points;
  if(const auto* known=world.findPoint("FP_ROAM_NW_BIGFARM_LAKE_MONSTER_02_01"))points.push_back(known);
  std::vector<Vec3> ring;Vec3 center;std::string waypoint;float roughness=0,heightRange=0;unsigned stoneSupports=0;
  for(const auto* point:points) {
    if(!characterFloor(world,point->pos,center))continue;
    float angle=0,rough=0;Vec3 downhill;
    if(!slopeAt(world,center,angle,downhill,rough) || angle>25)continue;
    for(float radius:{230.f,300.f,370.f}) {
      ring.clear();float low=1e9f,high=-1e9f;bool valid=true;unsigned stone=0;
      for(unsigned i=0;i<12;++i) {
        const float yaw=float(i)*float(M_PI)/6.f;Vec3 floor;
        if(!characterFloor(world,center+Vec3(std::cos(yaw)*radius,0,std::sin(yaw)*radius),floor) ||
           std::abs(floor.y-center.y)>180) {valid=false;break;}
        const auto support=world.physic()->landRay(floor+Vec3(0,15,0),30);
        if(!support.hasCol || support.n.y<std::cos(30.f*float(M_PI)/180.f) ||
           world.physic()->waterRay(floor+Vec3(0,1,0),0).hasCol) {valid=false;break;}
        stone+=support.mat==asset::MaterialGroup::STONE;
        if(!ring.empty() && !characterLane(world,ring.back(),floor,35)) {valid=false;break;}
        low=std::min(low,floor.y);high=std::max(high,floor.y);ring.push_back(floor);
        }
      if(!valid || ring.size()!=12 || stone<4 || !characterLane(world,ring.back(),ring.front(),35) || high-low<30 || high-low>220)continue;
      // Real terrain height variation keeps the ring visibly uneven. Stock
      // forest surfaces provide the rock/ground, with no synthetic nav mesh.
      waypoint=point->name;roughness=rough;heightRange=high-low;stoneSupports=stone;break;
      }
    if(!waypoint.empty())break;
    }
  if(waypoint.empty()) {monsterCharacterError("wolf pack lacks a supported uneven stock terrain ring");return;}
  monsterCharacterRemoveAmbient(state,context,world,player,center,3000,"wolf_pack");
  characterFreezeNear(world,player,center,center);player.stopWalking();player.clearAiQueue();player.setPosition(center);player.updateTransform();
  for(unsigned i=0;i<3;++i) {
    const auto index=i*4;auto* npc=monsterCharacterActor(world,"Wolf",ring[index]);if(!npc)return;
    npc->setDirection(ring[(index+11)%12]-ring[index]);npc->updateTransform();
    auto& actor=monsterCharacterExtra(state,world,npc,"wolf"+std::to_string(i+1));actor.loop=true;
    for(unsigned j=1;j<=12;++j)monsterCharacterPoint(actor,ring[(index+24-j)%12]);
    if(i==0)route.npc=npc;
    }
  route.start=ring.front();route.goal=ring[6];context.subject=route.npc;state.monsterCharacterPack=true;state.monsterCharacterHeroStart=player.position();
  event("monster_character_pack_setup","\"route\":\"wolf_pack\",\"actors\":3,\"waypoint\":"+jsonStr(waypoint)+
    ",\"hero_cm\":"+ragdollVector(center)+",\"ring_height_range_cm\":"+ragdollNumber(heightRange)+
    ",\"roughness_cm\":"+ragdollNumber(roughness)+",\"stone_supports\":"+std::to_string(stoneSupports)+
    ",\"ring_supports\":12,\"native_action\":\"AI_GoToPoint\"");
  }

void monsterCharacterCliff(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player,bool higher) {
  monsterCharacterClear(state,context,world);auto& route=state.monsterCharacterRoute;
  route.name=higher?"molerat_cliff_46":"molerat_cliff_36";
  const std::string waypoint=higher?"NW_XARDAS_TOWER_VALLEY_02":"NW_XARDAS_TOWER_VALLEY_03_C";
  const auto* point=world.findPoint(waypoint);
  Vec3 center,downhill;float angle=0,rough=0;
  if(!point || !characterFloor(world,point->groundPos,center,2000) || !slopeAt(world,center,angle,downhill,rough) ||
     !characterFloor(world,center-rotY(Vec3(1,0,0),higher?135.f:90.f)*100.f,route.start,2000) ||
     !characterFloor(world,center+rotY(Vec3(1,0,0),higher?135.f:90.f)*500.f,route.goal,2000)) {
    monsterCharacterError("known cliff waypoint has no supported ledge endpoints: "+waypoint);return;
    }
  monsterCharacterRemoveAmbient(state,context,world,player,center,3000,"cliff");
  characterFreezeNear(world,player,route.start,route.goal);npcCharacterPlaceHero(world,player,route.start,route.goal);
  route.npc=monsterCharacterActor(world,"Molerat",route.start);if(!route.npc)return;
  route.npc->setDirection(route.goal-route.start);route.npc->updateTransform();context.subject=route.npc;
  route.destination=WayPoint(route.goal,"FP_MONSTER_CHARACTER_LEDGE");route.destination.groundPos=route.goal;
  state.monsterCharacterHeroStart=player.position();
  const float limit=float(world.script().guildVal().slide_angle[route.npc->guild()]);
  const auto support=world.physic()->landRay(center+Vec3(0,15,0),30);
  event("monster_character_cliff_setup","\"route\":"+jsonStr(route.name)+",\"waypoint\":"+jsonStr(waypoint)+
    ",\"angle_deg\":"+ragdollNumber(angle)+",\"guild_slide_angle_deg\":"+ragdollNumber(limit)+
    ",\"support_walkable\":"+(support.hasCol && support.n.y>=std::cos(limit*float(M_PI)/180.f)?"true":"false")+
    ",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal));
  }

void monsterCharacterCrowd(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player) {
  monsterCharacterClear(state,context,world);
  if(!chooseRagdollArena(world,player)) {monsterCharacterError("monster performance lacks twenty supported stock lanes");return;}
  const auto center=context.ragdollArenaTarget-Vec3(0,80,0);
  monsterCharacterRemoveAmbient(state,context,world,player,center+Vec3(0,0,-950),2500,"performance");
  characterFreezeNear(world,player,center,center);player.stopWalking();player.clearAiQueue();player.setPosition(center+Vec3(0,0,-950));player.updateTransform();
  const char* species[]={"Wolf","Molerat","Scavenger"};
  for(size_t i=0;i<20;++i) {
    const auto at=context.ragdollArenaPlaces[i];Vec3 end;bool found=false;
    for(const auto& direction:{Vec3(1,0,0),Vec3(-1,0,0),Vec3(0,0,1),Vec3(0,0,-1)}) {
      if(characterFloor(world,at+direction*140.f,end) && characterLane(world,at,end,20)) {found=true;break;}
      }
    if(!found) {monsterCharacterError("monster performance lane has unsupported endpoint");return;}
    auto* npc=monsterCharacterActor(world,species[i%3],at);if(!npc)return;
    npc->setDirection(end-at);npc->updateTransform();
    auto& actor=monsterCharacterExtra(state,world,npc,"crowd"+std::to_string(i+1));actor.loop=true;
    monsterCharacterPoint(actor,end);monsterCharacterPoint(actor,at);
    }
  state.monsterCharacterHeroStart=player.position();
  state.monsterCharacterCrowd=true;context.subject=nullptr;context.cam=C_Hold;context.camAuto=false;
  lookFrom(center+Vec3(0,1200,1300),center+Vec3(0,40,0));
  event("monster_character_crowd_setup","\"actors\":20,\"wolves\":7,\"molerats\":7,\"scavengers\":6,\"radius_cm\":2000,\"hero_cm\":"+
    ragdollVector(player.position())+",\"waypoint\":"+jsonStr(context.ragdollArenaWaypoint));
  }

bool monsterCharacterSlope(World& world,Vec3& start,Vec3& goal,std::string& waypoint) {
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& wp){if(!wp.underWater)points.push_back(&wp);});
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  for(const auto* point:points) {
    Vec3 center;if(!characterFloor(world,point->pos,center))continue;
    float angle=0,rough=0;Vec3 downhill;
    if(!slopeAt(world,center,angle,downhill,rough) || angle<12 || angle>25 || rough>4)continue;
    if(!monsterCharacterOpenLane(world,center,-downhill,400,20,85,start,goal,25))continue;
    if(goal.y-start.y<120)continue;
    waypoint=point->name;
    event("monster_character_surface","\"route\":\"wolf_slope\",\"angle_deg\":"+ragdollNumber(angle)+
      ",\"roughness_cm\":"+ragdollNumber(rough));return true;
    }
  return false;
  }

bool monsterCharacterCourtyard(World& world,Npc& player,const Npc&,Vec3& start,Vec3& goal,std::string& waypoint) {
  if(!characterFloor(world,Vec3(8727.44434f,128.25412f,-745.565796f),start) ||
     !characterFloor(world,Vec3(8727.44434f,128.269653f,54.4342041f),goal))return false;
  waypoint="KNOWN_CORAGON_COURTYARD";
  event("monster_character_courtyard_setup","\"outdoor\":true,\"known_site\":true,\"lane_width_cm\":700");
  characterFreezeNear(world,player,start,goal);return true;
  }

bool monsterCharacterWater(World& world,float chest,Vec3& dry,Vec3& wet,std::string& waypoint) {
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& wp) {
    if(wp.name.find("BEACH")!=std::string::npos || wp.name.find("WATER")!=std::string::npos ||
       wp.name.find("LAKE")!=std::string::npos || wp.name.find("COAST")!=std::string::npos ||
       wp.name.find("HABOUR")!=std::string::npos)points.push_back(&wp);
    });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  for(const auto* point:points) {
    const auto ground=world.physic()->landRay(point->pos+Vec3(0,300,0),2500);
    if(!ground.hasCol || world.physic()->waterRay(ground.v,1).hasCol)continue;
    for(unsigned azimuth=0;azimuth<24;++azimuth) {
      const float angle=float(azimuth)*float(M_PI)/12.f;const Vec3 along(std::cos(angle),0,std::sin(angle));
      Vec3 previous=ground.v;
      for(float distance=25;distance<=1600;distance+=25) {
        const auto at=ground.v+along*distance;
        const auto floor=world.physic()->landRay(Vec3(at.x,ground.v.y+300,at.z),2500);
        if(!floor.hasCol || std::abs(floor.v.y-previous.y)>20 ||
           world.physic()->ray(previous+Vec3(0,40,0),floor.v+Vec3(0,40,0)).hasCol)break;
        previous=floor.v;
        const auto water=world.physic()->waterRay(floor.v+Vec3(0,.1f,0),0);
        if(!water.hasCol || water.wdepth-floor.v.y<chest+25)continue;
        dry=ground.v;wet=Vec3(at.x,water.wdepth-chest,at.z);waypoint=point->name;
        event("monster_character_water_surface","\"surface_y_cm\":"+ragdollNumber(water.wdepth)+
          ",\"depth_cm\":"+ragdollNumber(water.wdepth-floor.v.y)+",\"chest_cm\":"+ragdollNumber(chest));return true;
        }
      }
    }
  return false;
  }

bool monsterCharacterChaseGoal(World& world,const Npc& npc,const Npc& player,const Vec3& start,const Vec3& obstacleEnd,
                               Vec3& hero,bool stairs=false) {
  const auto& guild=world.script().guildVal();const auto g=npc.guild();
  const float step=guild.step_height[g]>0?float(guild.step_height[g]):50.f;
  const auto shape=npc.navigationCapsule();const float height=npc.visualSkeleton()->bboxCol[1].y-npc.visualSkeleton()->bboxCol[0].y;
  const float nativeStop=npc.bodyObservationTarget(player).attackRangeCm;
  const auto along=Vec3::normalize(Vec3(obstacleEnd.x-start.x,0,obstacleEnd.z-start.z));
  const Vec3 across(-along.z,0,along.x);const float obstacleDistance=std::hypot(obstacleEnd.x-start.x,obstacleEnd.z-start.z);
  unsigned attempts=0,noFloor=0,stepRejected=0,wallRejected=0,slopeRejected=0;
  for(float extension=nativeStop+50.f;extension<=nativeStop+800.f;extension+=25.f)for(float yaw:{0.f,10.f,-10.f,20.f,-20.f}) {
    const float angle=yaw*float(M_PI)/180.f;
    const auto candidate=obstacleEnd+(along*std::cos(angle)+across*std::sin(angle))*extension;
    const auto delta=candidate-start;const float forward=Vec3::dotProduct(delta,along);
    // The actual straight native chase must still cross the selected stairs,
    // rather than walking around them to a laterally displaced observer.
    const float crossingSide=std::abs(Vec3::dotProduct(delta,across))*obstacleDistance/forward;
    if(crossingSide>45.f || extension*std::cos(angle)<nativeStop+40.f)continue;
    ++attempts;bool valid=true;Vec3 centerEnd;
    const auto direction=Vec3::normalize(Vec3(delta.x,0,delta.z));const Vec3 side(-direction.z,0,direction.x);
    const unsigned samples=unsigned(std::ceil(std::hypot(delta.x,delta.z)/15.f));
    for(float width:{0.f,-shape.x,shape.x}) {
      Vec3 previous=start+side*width;
      if(!characterFloor(world,previous,previous)) {++noFloor;valid=false;break;}
      for(unsigned sample=1;sample<=samples;++sample) {
        const float fraction=float(sample)/float(samples);
        const Vec3 probe(start.x+delta.x*fraction+side.x*width,previous.y,start.z+delta.z*fraction+side.z*width);
        Vec3 floor;
        // Follow the previously sampled support height. A long uphill segment
        // can rise beyond characterFloor's +180 cm window from its first point.
        if(!characterFloor(world,probe,floor)) {++noFloor;valid=false;break;}
        if(std::abs(floor.y-previous.y)>step) {++stepRejected;valid=false;break;}
        const float head=std::max(step+5.f,height-5.f);
        if(world.physic()->ray(previous+Vec3(0,head,0),floor+Vec3(0,head,0)).hasCol) {
          ++wallRejected;valid=false;break;
          }
        // Stair tread/riser seams can return a riser normal. Their distinct
        // risers and tread heights were checked by characterStairsProfile;
        // applying a smooth-slope normal limit here rejects valid stairs.
        if(!stairs) {
          const auto support=world.physic()->landRay(floor+Vec3(0,15,0),30.f);
          const float normalLength=support.n.length();
          if(support.hasCol && normalLength>0.f &&
             support.n.y/normalLength<std::cos(float(guild.slide_angle[g])*float(M_PI)/180.f)) {
            ++slopeRejected;valid=false;break;
            }
          }
        previous=floor;
        }
      if(!valid)break;
      if(width==0)centerEnd=previous;
      }
    if(!valid)continue;
    hero=centerEnd;
    event("monster_character_chase_destination","\"found\":true,\"attempts\":"+std::to_string(attempts)+
      ",\"native_stop_cm\":"+ragdollNumber(nativeStop)+",\"extension_cm\":"+ragdollNumber(extension)+
      ",\"yaw_offset_deg\":"+ragdollNumber(yaw)+",\"crossing_side_cm\":"+ragdollNumber(crossingSide)+
      ",\"obstacle_end_cm\":"+ragdollVector(obstacleEnd)+",\"hero_cm\":"+ragdollVector(hero));return true;
    }
  event("monster_character_chase_destination","\"found\":false,\"attempts\":"+std::to_string(attempts)+
    ",\"native_stop_cm\":"+ragdollNumber(nativeStop)+",\"no_floor\":"+std::to_string(noFloor)+
    ",\"step_rejected\":"+std::to_string(stepRejected)+",\"wall_rejected\":"+std::to_string(wallRejected)+
    ",\"slope_rejected\":"+std::to_string(slopeRejected)+",\"obstacle_end_cm\":"+ragdollVector(obstacleEnd));
  return false;
  }

bool monsterCharacterStairs(World& world,Npc& npc,Npc& player,Vec3& start,Vec3& goal,std::string& waypoint,bool stoneOnly=false) {
  std::vector<const WayPoint*> points;
  if(!stoneOnly) {
    if(const auto* point=world.findPoint("NW_CITY_TO_FARM2_09"))points.push_back(point);
    } else world.forEachWayPoint([&](const WayPoint& wp) {
    if(!wp.underWater && wp.name.starts_with("NW_CITY_") && wp.name.find("BED")==std::string::npos &&
       (!stoneOnly || wp.name.find("DITCH")==std::string::npos))points.push_back(&wp);
    });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  if(stoneOnly)std::stable_partition(points.begin(),points.end(),[](const auto* wp){return wp->name.find("STAIR")!=std::string::npos;});
  std::stable_partition(points.begin(),points.end(),[](const auto* wp){return wp->name=="NW_CITY_TO_FOREST_STAIRS01";});
  if(stoneOnly)std::stable_partition(points.begin(),points.end(),[](const auto* point) {
    return point->name.find("_TO_")==std::string::npos && point->name.find("FOREST")==std::string::npos &&
           point->name.find("DITCH")==std::string::npos;
    });
  const auto offsets=stoneOnly?std::vector<float>{0.f,-80.f,80.f,-160.f,160.f,-240.f,240.f,-400.f,400.f,-600.f,600.f}:std::vector<float>{0.f,-80.f,80.f};
  for(const auto* point:points)for(float x:offsets)for(float z:offsets) {
    Vec3 center;if(!characterFloor(world,point->pos+Vec3(x,0,z),center))continue;
    if(stoneOnly) {
      const auto support=world.physic()->landRay(center+Vec3(0,15,0),30);
      if(!support.hasCol || support.mat!=asset::MaterialGroup::STONE)continue;
      }
    float angle=0,rough=0;Vec3 downhill;
    if(!slopeAt(world,center,angle,downhill,rough) || angle<(stoneOnly?10.f:18.f) || angle>40 ||
       rough<(stoneOnly?2.f:6.f) || rough>25)continue;
    Vec3 low,high;
    if(!characterFloor(world,center+downhill*100.f,low) || !characterFloor(world,center-downhill*100.f,high))continue;
    unsigned risers=0;float maximumRiser=0;
    if(!characterStairsProfile(world,low,high,risers,maximumRiser,stoneOnly?4.f:6.f,stoneOnly?40.f:45.f))continue;
    Vec3 hero;if(!monsterCharacterChaseGoal(world,npc,player,low,high,hero,true))continue;
    start=low;goal=hero;waypoint=point->name;
    event("monster_character_surface","\"route\":"+jsonStr(stoneOnly?"wolf_terrain":"wolf_stairs")+",\"waypoint\":"+jsonStr(waypoint)+
      ",\"stone_surface\":"+(stoneOnly?"true":"false")+
      ",\"angle_deg\":"+ragdollNumber(angle)+",\"roughness_cm\":"+ragdollNumber(rough)+
      ",\"riser_count\":"+std::to_string(risers)+",\"maximum_riser_cm\":"+ragdollNumber(maximumRiser));return true;
    }
  return false;
  }

void monsterCharacterPrepare(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player,const std::string& name) {
  monsterCharacterClear(state,context,world);auto& route=state.monsterCharacterRoute;route.name=name;
  route.npc=monsterCharacterActor(world,name=="troll_barrel"?"Troll":"Wolf",player.position());if(!route.npc)return;
  std::string waypoint;
  if(name=="wolf_terrain") {
    if(!prepareCharacterRoute(context.character,context.characterContext,world,player,"stairs_up",true,"NW_CITY_TO_FOREST_STAIRS01"))return;
    const auto& stairs=context.character.characterRoute;
    auto direction=stairs.goal-stairs.start;direction.y=0;
    if(direction.length()<1) {monsterCharacterError("known stairs lack a forward direction");return;}
    direction=Vec3::normalize(direction);
    auto floor=[&](const Vec3& at,Vec3& result) {
      const auto hit=world.physic()->landRay(at+Vec3(0,500,0),1000);
      if(!hit.hasCol || std::abs(hit.v.y-at.y)>180)return false;
      result=hit.v;return true;
      };
    Vec3 escape;
    if(!floor(stairs.start-direction*150.f,route.start) || !floor(stairs.goal,route.goal) ||
       !floor(stairs.goal+direction*150.f,escape)) {
      monsterCharacterError("known stairs lack supported chase and hero escape endpoints");return;
      }
    waypoint=stairs.waypoint;route.chase=true;route.heroRun=true;
    route.heroDestination=WayPoint(escape,"FP_MONSTER_VIDEO_HERO_ESCAPE");route.heroDestination.groundPos=escape;
    event("monster_character_surface","\"route\":\"wolf_terrain\",\"waypoint\":"+jsonStr(waypoint)+
      ",\"known_stairs\":true,\"hero_escape_cm\":"+ragdollVector(escape));
    } else if(name=="wolf_stairs") {
    route.npc->setCharacterVirtualDiagnosticsEnabled(true);
    if(!monsterCharacterStairs(world,*route.npc,player,route.start,route.goal,waypoint)) {
      monsterCharacterError("wolf stairs lack a supported native chase continuation");return;
      }
    route.chase=true;
    } else if(name=="wolf_slope") {
    if(!monsterCharacterSlope(world,route.start,route.goal,waypoint)) {monsterCharacterError("wolf slope lacks a supported stock lane");return;}
    route.chase=true;
    } else if(name=="troll_barrel") {
    if(!monsterCharacterCourtyard(world,player,*route.npc,route.start,route.goal,waypoint)) {monsterCharacterError("troll lacks a body-size outdoor courtyard lane");return;}
    } else if(name=="wolf_water") {
    // Read the wolf's actual guild instead of assuming human swimming depth.
    const float chest=float(world.script().guildVal().water_depth_chest[route.npc->guild()]);
    const bool canSwim=route.npc->hasSwimAnimations();
    if(!canSwim || !monsterCharacterWater(world,chest,route.start,route.goal,waypoint)) {
      monsterCharacterError("wolf water requires a continuous walkable stock bank and native swim animation");return;
      }
    } else {monsterCharacterError("unknown monster route: "+name);return;}
  characterFreezeNear(world,player,route.start,route.goal);
  if(route.chase) {
    if(name=="wolf_slope") {
      Vec3 hero;
      if(!monsterCharacterChaseGoal(world,*route.npc,player,route.start,route.goal,hero)) {
        monsterCharacterError("wolf slope has no supported hero destination beyond its obstacle");return;
        }
      route.goal=hero;
      }
    player.stopWalking();player.clearAiQueue();player.setPosition(route.goal);player.updateTransform();
    } else npcCharacterPlaceHero(world,player,route.start,route.goal);
  if(name=="troll_barrel") {
    Vec3 hero;
    if(!characterFloor(world,Vec3(9127.44434f,128.25412f,-745.565796f),hero)) {monsterCharacterError("courtyard hero has no floor");return;}
    player.setPosition(hero);player.updateTransform();
    }
  route.npc->setPosition(route.start);route.npc->setDirection(route.goal-route.start);route.npc->updateTransform();context.subject=route.npc;
  state.monsterCharacterHeroStart=player.position();
  if(route.chase)route.tolerance=route.npc->bodyObservationTarget(player).attackRangeCm+30.f;
  route.destination=WayPoint(route.goal,"FP_MONSTER_CHARACTER_GOAL");route.destination.groundPos=route.goal;
  if(name=="troll_barrel") {
    // Reuse the stock barrel mesh and production dynamic-vob constructor.
    const std::string visual="NW_HARBOUR_BARREL_01.3DS";const auto* mesh=Resources::loadMesh(visual);
    const auto* rule=MovableVobs::classifyVisual(visual);
    if(!mesh || !rule) {monsterCharacterError("stock barrel mesh/class unavailable");return;}
    const auto center=route.start+(route.goal-route.start)*.35f;const auto* bounds=mesh->bbox();
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(center.x-(bounds[0].x+bounds[1].x)*.5f,
      center.y-bounds[0].y+.2f,center.z-(bounds[0].z+bounds[1].z)*.5f);
    asset::VirtualObject data;data.type=asset::VirtualObjectType::zCVob;data.id=uint32_t(-1);
    data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
    data.visual=std::make_shared<asset::VisualMesh>();data.visual->type=asset::VisualType::MESH;data.visual->name=visual;
    const auto before=world.physic()->movableVobSnapshot();
    auto fixture=std::make_unique<MovableFixture>();fixture->visual.setVisual(data,world,false,false);fixture->visual.setObjMatrix(matrix);
    fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
    if(fixture->body.isEmpty()) {monsterCharacterError("stock barrel native body unavailable");return;}
    fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& m){view->setObjMatrix(m);});
    fixture->visual.setupFracture(world,fixture->body);
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;})) {context.character.characterPropId=body.id;break;}
    if(!context.character.characterPropId) {monsterCharacterError("stock barrel snapshot unavailable");return;}
    context.character.characterProp=std::move(fixture);context.character.characterPropInitial=movablePosition(matrix);context.character.characterPropTravel=0;
    auto& barrel=state.monsterCharacterBarrel;barrel.role="barrel";barrel.id=context.character.characterPropId;
    barrel.start=context.character.characterPropInitial;barrel.lastIntact=barrel.start;
    event("monster_character_barrel","\"mass_kg\":"+ragdollNumber(context.character.characterProp->body.mass())+
      ",\"position_cm\":"+ragdollVector(context.character.characterPropInitial)+",\"blocks_walking\":"+(rule->blocksWalking?"true":"false"));
    if(!monsterCharacterProp(state,world,"crate",route.start+(route.goal-route.start)*.68f))return;
    }
  event("monster_character_route_setup","\"route\":"+jsonStr(name)+",\"npc_id\":"+std::to_string(world.npcId(route.npc))+
    ",\"waypoint\":"+jsonStr(waypoint)+",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"arrival_radius_cm\":"+ragdollNumber(route.tolerance)+
    ",\"native_action\":"+jsonStr(route.chase?"AI_GoToNpc":"AI_GoToPoint"));
  }

void monsterCharacterBegin(MonsterCharacter& state,World& world) {
  auto& route=state.monsterCharacterRoute;
  if(!route.npc) {monsterCharacterError("monster begin requires fixture actor");return;}
  if(!state.monsterCharacterCrowd)route.npc->setCharacterVirtualDiagnosticsEnabled(true);
  route.active=true;route.reached=false;route.stalled=false;route.falling=false;route.wasAirborne=false;route.began=world.tickCount();
  route.arrived=0;route.progressTime=route.began;route.stalls=0;route.falls=0;route.cvFrames=0;route.swimFrames=0;
  route.maxPhysicalJump=0;route.maxVisualJump=0;route.lastPosition=route.npc->position();
  route.lastVisual=route.npc->characterVisualPosition();route.progressPosition=route.lastPosition;route.lastSupported=route.lastPosition;
  for(auto& actor:state.monsterCharacterExtras)if(actor.npc) {
    if(!state.monsterCharacterCrowd)actor.npc->setCharacterVirtualDiagnosticsEnabled(true);
    actor.npc->clearAiQueue();actor.npc->clearGoTo();actor.npc->stopWalking();
    actor.start=actor.npc->position();actor.lastPosition=actor.start;actor.lastVisual=actor.npc->characterVisualPosition();
    actor.progressPosition=actor.start;actor.lastSupported=actor.start;actor.progressTime=world.tickCount();
    actor.next=0;actor.queued=false;actor.stalled=false;actor.falling=false;actor.wasAirborne=false;
    actor.arrived=0;actor.arrivals=0;actor.stalls=0;actor.falls=0;actor.cvFrames=0;actor.maxVisualJump=0;actor.maxPhysicalJump=0;actor.travel=0;
    }
  if(route.heroRun)if(auto* hero=Gothic::inst().player()) {
    hero->clearAiQueue();hero->clearGoTo();hero->setWalkMode(WalkBit::WM_Run);
    hero->aiPush(AiQueue::aiGoToPoint(route.heroDestination));
    }
  if(!route.attack && !state.monsterCharacterPack) {
    route.npc->clearAiQueue();route.npc->stopWalking();
    if(route.isolatedLedge) {
      if(!route.npc->goToPosition(route.goal)) {monsterCharacterError("isolated ledge native running goal was rejected");return;}
      }
    else if(route.chase)route.npc->aiPush(AiQueue::aiGoToNpc(Gothic::inst().player()));
    else route.npc->aiPush(AiQueue::aiGoToPoint(route.destination));
    }
  event("monster_character_route_begin","\"route\":"+jsonStr(route.name)+",\"cv_enabled\":"+
    (Gothic::settingsGetI("GAME","monster_character_virtual")?"true":"false")+",\"start_cm\":"+ragdollVector(route.lastPosition));
  }

void monsterCharacterEnd(MonsterCharacter& state,const MonsterCharacterContext& context,World& world) {
  auto& route=state.monsterCharacterRoute;
  if(!route.active || !route.npc) {monsterCharacterError("monster end without active fixture");return;}
  monsterCharacterSampleProps(state,context,world);
  route.active=false;
  event("monster_character_route_end","\"route\":"+jsonStr(route.name)+",\"success\":"+(route.reached?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string((route.reached?route.arrived:world.tickCount())-route.began)+
    ",\"window_ms\":"+std::to_string(world.tickCount()-route.began)+",\"stalls_over_1s\":"+std::to_string(route.stalls)+
    ",\"max_physical_jump_cm\":"+ragdollNumber(route.maxPhysicalJump)+",\"max_visual_jump_cm\":"+ragdollNumber(route.maxVisualJump)+
    ",\"falls\":"+std::to_string(route.falls)+",\"cv_frames\":"+std::to_string(route.cvFrames)+",\"swim_frames\":"+std::to_string(route.swimFrames)+
    ",\"saw_swim\":"+(route.sawSwim?"true":"false")+",\"left_water\":"+(route.leftWater?"true":"false")+
    ",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal)+",\"end_cm\":"+ragdollVector(route.npc->position())+
    ",\"attack_distance_cm\":"+ragdollNumber(route.attackDistance)+",\"attack_rise_cm\":"+ragdollNumber(route.attackRise)+
    ",\"attack_duration_ms\":"+(route.attackEnded?std::to_string(route.attackEnded-route.began):"null")+
    ",\"charge_collision_callbacks\":"+std::to_string(route.callbacks)+",\"barrel_displacement_cm\":"+ragdollNumber(context.character.characterPropTravel)+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","monster_character_virtual")?"true":"false"));
  for(auto& actor:state.monsterCharacterExtras)if(actor.npc) {
    event("monster_character_actor_end","\"route\":"+jsonStr(route.name)+",\"role\":"+jsonStr(actor.role)+
      ",\"npc_id\":"+std::to_string(world.npcId(actor.npc))+",\"instance\":"+jsonStr(symName(world,actor.npc))+
      ",\"arrivals\":"+std::to_string(actor.arrivals)+",\"elapsed_ms\":"+(actor.arrived?std::to_string(actor.arrived-route.began):"null")+
      ",\"stalls_over_1s\":"+std::to_string(actor.stalls)+",\"max_visual_jump_cm\":"+ragdollNumber(actor.maxVisualJump)+
      ",\"max_physical_jump_cm\":"+ragdollNumber(actor.maxPhysicalJump)+",\"falls\":"+std::to_string(actor.falls)+
      ",\"travel_cm\":"+ragdollNumber(actor.travel)+",\"cv_frames\":"+std::to_string(actor.cvFrames)+
      ",\"start_cm\":"+ragdollVector(actor.start)+",\"end_cm\":"+ragdollVector(actor.npc->position()));
    actor.npc->clearAiQueue();actor.npc->clearGoTo();actor.npc->stopWalking();
    }
  for(const auto& prop:state.monsterCharacterProps)monsterCharacterPropEnd(prop,route.name);
  if(state.monsterCharacterBarrel.id)monsterCharacterPropEnd(state.monsterCharacterBarrel,route.name);
  route.npc->clearAiQueue();route.npc->clearGoTo();route.npc->stopWalking();
  }

// The attack needs three supported roots and one short native attack lane,
// rather than the twenty-place arena used by the performance fixture.
bool chooseMonsterAttackSite(World& world,const Npc&,const Npc&,bool withFly,Vec3& start,Vec3& goal,Vec3& flyAt) {
  // Both native attacks use the verified open lunge lane. The old short valley
  // lane can stop the fieldraider on terrain before its target collision.
  if(!characterFloor(world,Vec3(38674.5156f,4308.43604f,11976.749f),start) ||
     !characterFloor(world,start+Vec3(withFly?350.f:1200.f,0,0),goal) ||
     (withFly && !characterFloor(world,start+Vec3(150,0,-140),flyAt)))return false;
  event("monster_character_attack_site","\"site\":"+jsonStr(withFly?"verified_fieldraider_charge":"verified_wolf_lunge")+
    ",\"known_site\":true");
  return true;
  }

void monsterCharacterAttack(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,Npc& player,const std::string& species) {
  monsterCharacterClear(state,context,world);
  if(species!="wolf" && species!="fieldraider") {monsterCharacterError("unknown monster attack species");return;}
  auto& route=state.monsterCharacterRoute;route.name=species+"_attack";route.attack=true;route.animation="T_FISTATTACKMOVE";
  route.npc=monsterCharacterActor(world,species=="wolf"?"Wolf":"Giant_Bug",player.position());if(!route.npc)return;
  Vec3 flyAt;
  if(!chooseMonsterAttackSite(world,*route.npc,player,species=="fieldraider",route.start,route.goal,flyAt)) {
    monsterCharacterError("monster attack requires supported roots and a clear native attack lane on outdoor city stone near the market");return;
    }
  monsterCharacterRemoveAmbient(state,context,world,player,route.start,3000,"attack");
  characterFreezeNear(world,player,route.start,route.goal);
  player.stopWalking();player.clearAiQueue();player.setPosition(route.goal);player.updateTransform();
  route.npc->setPosition(route.start);route.npc->setDirection(Vec3(1,0,0));route.npc->updateTransform();route.npc->setTarget(&player);route.npc->drawWeaponFist();
  state.monsterCharacterHeroStart=player.position();
  if(species=="fieldraider") {
    auto* fly=monsterCharacterActor(world,"Bloodfly",flyAt);if(!fly)return;
    fly->setDirection(Vec3(1,0,0));fly->updateTransform();monsterCharacterExtra(state,world,fly,"bloodfly");
    const auto& guild=world.script().guildVal();const auto g=fly->guild();
    if(guild.water_depth_knee[g]!=999999 || guild.water_depth_chest[g]!=999999) {
      monsterCharacterError("bloodfly control lacks the production flying exclusion marker");return;
      }
    event("monster_character_bloodfly_setup","\"npc_id\":"+std::to_string(world.npcId(fly))+
      ",\"root_cm\":"+ragdollVector(flyAt)+",\"expected_cv_active\":false");
    }
  context.subject=route.npc;
  event("monster_character_attack_setup","\"route\":"+jsonStr(route.name)+",\"animation\":"+jsonStr(route.animation)+
    ",\"target_cm\":"+ragdollVector(player.position())+",\"collision_required\":"+(species=="fieldraider"?"true":"false"));
  }

void monsterCharacterSnapshot(World& world,Npc& player,std::string_view label) {
  std::string actors="[";
  for(uint32_t id=0;id<world.npcCount();++id)if(const auto* npc=world.npcById(id);npc && npc!=&player && !npc->isHuman() &&
      !npc->isDown() && (npc->position()-player.position()).length()<=2300.f) {
    if(actors.size()>1)actors+=',';
    actors+="{\"npc_id\":"+std::to_string(id)+",\"instance\":"+jsonStr(symName(world,npc))+
      ",\"root_cm\":"+ragdollVector(npc->position())+",\"cv_active\":"+(npc->characterVirtualActive()?"true":"false")+"}";
    }
  event("monster_character_snapshot","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","monster_character_virtual")?"true":"false")+",\"actors\":"+actors+"]");
  }

void monsterCharacterSave(MonsterCharacter& state,World& world,Npc& player) {
  auto& route=state.monsterCharacterRoute;
  if(!route.npc || !route.npc->characterVirtualActive()) {monsterCharacterError("monster save requires an active ON controller");return;}
  try {
    route.npc->clearAiQueue();route.npc->clearGoTo();route.npc->stopWalking();
    route.savedPosition=route.npc->position();monsterCharacterSnapshot(world,player,"saved");
    std::vector<uint8_t> session;
    {MemWriter writer(session);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
    Gothic::inst().gameSession()->save(out,"monster character",preview);}
    std::ofstream file(outDir+"/session-monster.sav",std::ios::binary);
    file.write(reinterpret_cast<const char*>(session.data()),std::streamsize(session.size()));
    if(!file)throw std::runtime_error("cannot write monster session archive");
    std::vector<uint8_t> bytes;const auto id=world.npcId(route.npc);
    {MemWriter writer(bytes);Serialize out(writer);out.setContext(&world);out.setVersion(Serialize::Current);route.npc->save(out,id,"/npc/");}
    {MemReader reader(bytes);Serialize in(reader);in.setContext(&world);in.setVersion(Serialize::Current);route.npc->load(in,id,"/npc/");route.npc->postValidate();}
    route.npc->setExternalControl(true);route.npc->updateAnimation(0,true);
    const float delta=(route.npc->position()-route.savedPosition).length();
    event("monster_character_archive","\"operation\":\"native_npc_restore\",\"position_delta_cm\":"+ragdollNumber(delta)+
      ",\"limit_cm\":0.5,\"session\":\"session-monster.sav\",\"session_bytes\":"+std::to_string(session.size())+
      ",\"npc_archive_bytes\":"+std::to_string(bytes.size()));
    if(delta>.5f) {monsterCharacterError("monster NPC archive moved more than 0.5 cm");return;}
    route.restoreSteps=2;
    } catch(const std::exception& e) {monsterCharacterError(e.what());}
  }

void monsterCharacterFields(MonsterCharacter& state,const MonsterCharacterContext& context,World& world,uint64_t dt) {
  auto& route=state.monsterCharacterRoute;
  if(route.active || state.monsterCharacterCrowd)for(auto& actor:state.monsterCharacterExtras)if(actor.npc) {
    auto& npc=*actor.npc;const auto at=npc.position(),visual=npc.characterVisualPosition();
    if(!state.monsterCharacterCrowd)state.monsterCharacterFrames.push_back({route.name+"_"+actor.role,npc.animNames(),world.tickCount(),dt,
      world.npcId(&npc),at,visual,bool(Gothic::settingsGetI("GAME","monster_character_virtual")),npc.characterVirtualActive(),
      npc.navigationGoal().active,npc.isInAir(),npc.isFalling(),npc.isSwim(),npc.rotation(),characterMoveFrame(npc)});
    actor.maxPhysicalJump=std::max(actor.maxPhysicalJump,std::abs(at.y-actor.lastPosition.y));
    actor.maxVisualJump=std::max(actor.maxVisualJump,std::abs(visual.y-actor.lastVisual.y));
    actor.travel+=std::hypot(at.x-actor.lastPosition.x,at.z-actor.lastPosition.z);
    actor.cvFrames+=npc.characterVirtualActive();actor.lastPosition=at;actor.lastVisual=visual;
    const bool air=npc.isInAir() || npc.isFalling();
    if(!npc.isSwim() && (air || actor.wasAirborne) && actor.lastSupported.y-at.y>100 && !actor.falling) {
      ++actor.falls;actor.falling=true;
      }
    if(!air) {actor.falling=false;if(!npc.isSwim())actor.lastSupported=at;}
    actor.wasAirborne=air;
    if(npc.navigationGoal().active) {
      if(std::hypot(at.x-actor.progressPosition.x,at.z-actor.progressPosition.z)>=5) {
        actor.progressPosition=at;actor.progressTime=world.tickCount();actor.stalled=false;
        }
      if(world.tickCount()-actor.progressTime>1000 && !actor.stalled) {++actor.stalls;actor.stalled=true;}
      }
    if(!actor.points.empty() && actor.next<actor.points.size()) {
      const auto& target=actor.points[actor.next];
      if(actor.queued && (at-target.pos).length()<90 && npc.isAiQueueEmpty()) {
        ++actor.arrivals;actor.arrived=world.tickCount();actor.queued=false;++actor.next;
        if(actor.loop)actor.next%=actor.points.size();
        }
      if(!actor.queued && actor.next<actor.points.size()) {
        npc.aiPush(AiQueue::aiGoToPoint(actor.points[actor.next]));actor.queued=true;
        actor.progressPosition=at;actor.progressTime=world.tickCount();actor.stalled=false;
        }
      }
    }
  if(state.monsterCharacterMeasuring
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
     && !AppCadence::done()
#endif
     ) {
    unsigned nearby=0,virtualNearby=0,moving=0;float maximumDistance=0;
    const auto* player=Gothic::inst().player();
    for(const auto& actor:state.monsterCharacterExtras)if(actor.npc && player) {
      const float distance=(actor.npc->position()-player->position()).length();
      nearby+=distance<2000;virtualNearby+=distance<2000 && actor.npc->characterVirtualActive();
      moving+=actor.npc->navigationGoal().active;maximumDistance=std::max(maximumDistance,distance);
      }
    state.monsterCharacterMinNearby=std::min(state.monsterCharacterMinNearby,nearby);
    state.monsterCharacterMaxNearby=std::max(state.monsterCharacterMaxNearby,nearby);
    state.monsterCharacterMaximumDistance=std::max(state.monsterCharacterMaximumDistance,maximumDistance);
    const auto now=std::chrono::steady_clock::now();
    const float wall=state.monsterCharacterHasWall?std::chrono::duration<float,std::milli>(now-state.monsterCharacterLastWall).count():0;
    state.monsterCharacterLastWall=now;state.monsterCharacterHasWall=true;bool normal=false;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
    normal=AppCadence::normalClock();
#endif
    state.monsterCharacterPerformance.push_back({world.tickCount(),dt,world.physic()->physicsStats().stepMilliseconds,wall,
      maximumDistance,nearby,virtualNearby,moving,context.rec && context.recVideo,normal});
    }
  if(route.active)monsterCharacterSampleProps(state,context,world);
  auto* npc=route.npc;if(!npc)return;
  if(route.restoreSteps) {
    const float delta=(npc->position()-route.savedPosition).length();
    event("monster_character_archive","\"operation\":\"native_restore_step\",\"step\":"+std::to_string(3-route.restoreSteps)+
      ",\"position_delta_cm\":"+ragdollNumber(delta)+",\"limit_cm\":0.5,\"cv_active\":"+(npc->characterVirtualActive()?"true":"false"));
    --route.restoreSteps;if(delta>.5f)monsterCharacterError("monster native post-load step moved more than 0.5 cm");
    }
  if(!route.active)return;
  const auto at=npc->position(),visual=npc->characterVisualPosition();const auto animation=npc->animNames();
  state.monsterCharacterFrames.push_back({route.name,animation,world.tickCount(),dt,world.npcId(npc),at,visual,
    bool(Gothic::settingsGetI("GAME","monster_character_virtual")),npc->characterVirtualActive(),npc->navigationGoal().active,
    npc->isInAir(),npc->isFalling(),npc->isSwim(),npc->rotation(),characterMoveFrame(*npc)});
  if(route.name=="wolf_stairs") {
    CharacterVirtualDiagnostics d;
    const bool valid=npc->characterVirtualDiagnostics(d);
    event("monster_character_stair_step","\"route\":"+jsonStr(route.name)+",\"tick\":"+std::to_string(world.tickCount())+
      ",\"dt_ms\":"+std::to_string(dt)+",\"body_state\":"+std::to_string(unsigned(npc->bodyStateMasked()))+
      ",\"attack_state\":"+(npc->bodyStateMasked()==BS_HIT?"true":"false")+
      ",\"root_cm\":"+ragdollVector(at)+",\"visual_root_cm\":"+ragdollVector(visual)+
      ",\"physical_dy_cm\":"+ragdollNumber(at.y-route.lastPosition.y)+
      ",\"visual_dy_cm\":"+ragdollNumber(visual.y-route.lastVisual.y)+
      ",\"visual_offset_cm\":"+ragdollNumber(visual.y-at.y)+",\"animation\":"+jsonStr(animation)+
      ",\"in_air\":"+(npc->isInAir()?"true":"false")+",\"diagnostics_valid\":"+(valid?"true":"false")+
      ",\"requested_cm\":"+ragdollVector(d.requested)+",\"stair_height_cm\":"+ragdollNumber(d.stairHeight)+
      ",\"cv_before_cm\":"+ragdollVector(d.before.position)+
      ",\"cv_extended_cm\":"+ragdollVector(d.afterExtendedUpdate.position)+
      ",\"cv_stairs_cm\":"+ragdollVector(d.afterStairs.position)+
      ",\"stair_attempted\":"+(d.stairAttempted?"true":"false")+
      ",\"stair_succeeded\":"+(d.stairSucceeded?"true":"false")+",\"stair_reason\":"+jsonStr(d.stairProbe.reason));
    }
  route.cvFrames+=npc->characterVirtualActive();route.swimFrames+=npc->isSwim();route.sawSwim|=npc->isSwim();
  route.maxPhysicalJump=std::max(route.maxPhysicalJump,std::abs(at.y-route.lastPosition.y));
  route.maxVisualJump=std::max(route.maxVisualJump,std::abs(visual.y-route.lastVisual.y));
  route.lastPosition=at;route.lastVisual=visual;
  const bool airborne=npc->isInAir() || npc->isFalling();
  // Short drops stay InAir; the Falling state is reserved for larger falls.
  // Test the landing before replacing its previous support height as well.
  if(!route.attack && !npc->isSwim() && (airborne || route.wasAirborne) &&
     route.lastSupported.y-at.y>100.f && !route.falling) {
    ++route.falls;route.falling=true;
    }
  if(!airborne) {
    route.falling=false;
    if(!npc->isSwim())route.lastSupported=at;
    }
  route.wasAirborne=airborne;
  bool arrived=false;
  if(route.attack) {
    if(!route.attackEnded) {
      const auto travel=at-route.attackStart;
      route.attackDistance=std::max(route.attackDistance,std::hypot(travel.x,travel.z));route.attackRise=std::max(route.attackRise,travel.y);
      if(animation.find(route.animation)==std::string::npos)route.attackEnded=world.tickCount();
      }
    arrived=route.attackEnded && (route.name!="fieldraider_attack" || route.callbacks>0);
    } else {
    const auto* hero=Gothic::inst().player();
    const auto delta=at-(route.heroRun && hero?hero->position():route.goal);
    arrived=std::hypot(delta.x,delta.z)<=route.tolerance && std::abs(delta.y)<80;
    if(route.isolatedLedge)arrived&=std::abs(delta.y)<5.f && !npc->isInAir() && !npc->isFalling();
    if(route.name=="wolf_water")arrived&=npc->isSwim();
    if(route.name=="wolf_water_exit") {route.leftWater=route.sawSwim && !npc->isSwim() && !npc->isInAir();arrived&=route.leftWater;}
    }
  if(state.monsterCharacterPack)arrived=std::all_of(state.monsterCharacterExtras.begin(),state.monsterCharacterExtras.end(),
    [](const auto& actor){return actor.arrivals>=3;});
  if(arrived && !route.reached) {
    route.reached=true;route.arrived=world.tickCount();
    if(route.name=="wolf_terrain" || route.name=="wolf_stairs" || route.isolatedLedge) {
      if(route.heroRun)if(auto* hero=Gothic::inst().player()) {hero->clearAiQueue();hero->clearGoTo();hero->stopWalking();}
      // The test ends the authored chase after a measured catch. Keep native
      // idle physics running for the rest of the movie without a second goal.
      npc->clearAiQueue();npc->clearGoTo();npc->stopWalking();
      event("monster_character_native_goal_stop","\"route\":"+jsonStr(route.name)+",\"elapsed_ms\":"+
        std::to_string(route.arrived-route.began)+",\"root_cm\":"+ragdollVector(at)+",\"native_idle\":true");
      }
    }
  if(!route.attack && !route.reached && (npc->navigationGoal().active || route.isolatedLedge)) {
    if(std::hypot(at.x-route.progressPosition.x,at.z-route.progressPosition.z)>=5.f) {
      route.progressPosition=at;route.progressTime=world.tickCount();route.stalled=false;
      }
    if(world.tickCount()-route.progressTime>1000 && !route.stalled) {++route.stalls;route.stalled=true;}
    }
  }

}
