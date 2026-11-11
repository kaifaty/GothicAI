#include "testbench_internal.h"

#include <Tempest/MemWriter>
#include <Tempest/Pixmap>
#include <cstring>
#include <fstream>
#include "game/serialize.h"
#include "world/aiqueue.h"
#include "graphics/mesh/submesh/packedmesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "appcadence.h"
#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "gothic.h"
#include "graphics/mesh/animationsolver.h"
#include "graphics/mesh/skeleton.h"
#include "physics/charactervirtualdiagnostics.h"
#include "resources.h"
#include "world/movablevobs.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/triggers/movetrigger.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

const char* characterMoveFrameColumns = ",diagnostics_valid,move_sequence,move_dt_ms,requested_x_cm,requested_y_cm,requested_z_cm,accepted_dx_cm,accepted_dy_cm,accepted_dz_cm,advancement_ratio,steep_contact,steep_normal_x,steep_normal_y,steep_normal_z,move_blocked";

void npcCharacterStopMotionTrace(Character& state,World* world) {
  if(world)
    for(const auto& actor:state.npcCharacterTraceActors)
      if(world->npcId(actor.npc)!=uint32_t(-1))actor.npc->setCharacterVirtualDiagnosticsEnabled(false);
  state.npcCharacterTraceActors.clear();
  }


// Stage 5 fixtures select stock geometry before driving real player keys. No
// per-frame teleport is used on a route, and a missing fixture is an error.
bool characterFloor(World& world,const Vec3& at,Vec3& floor,float depth) {
  const auto hit=world.physic()->landRay(at+Vec3(0,180,0),depth);
  // C_Object floors (including city stair meshes) intentionally return a zero
  // normal through the old land query. Height sampling below verifies the path.
  if(!hit.hasCol || (hit.n.length()>0 && hit.n.y<.35f)) return false;
  floor=hit.v;return true;
  }


bool characterLane(World& world,const Vec3& start,const Vec3& end,float maximumStep) {
  const auto delta=end-start;
  Vec3 previous=start;
  for(unsigned i=0;i<=12;++i) {
    Vec3 floor;
    if(!characterFloor(world,start+delta*(float(i)/12.f),floor) || std::abs(floor.y-previous.y)>maximumStep) return false;
    if(i && world.physic()->ray(previous+Vec3(0,110,0),floor+Vec3(0,110,0)).hasCol) return false;
    previous=floor;
    }
  return true;
  }


bool characterStairsProfile(World& world,const Vec3& low,const Vec3& high,unsigned& risers,float& maximumRiser,
                            float minimumRiser,float minimumRise) {
  risers=0;maximumRiser=0;unsigned plateaus=0;
  Vec3 previous=low;
  for(unsigned i=1;i<=40;++i) {
    Vec3 floor;
    if(!characterFloor(world,low+(high-low)*(float(i)/40.f),floor))return false;
    const float rise=floor.y-previous.y;
    if(rise< -2.f || rise>32.f)return false;
    if(rise>=minimumRiser) {++risers;maximumRiser=std::max(maximumRiser,rise);}
    if(std::abs(rise)<2.f)++plateaus;
    previous=floor;
    }
  // Several discrete risers and level treads distinguish stairs from one
  // furniture top or a sloping ramp picked by the broad IK roughness probe.
  return risers>=3 && plateaus>=8 && high.y-low.y>=minimumRise;
  }


void characterFreezeNear(World& world,Npc& player,const Vec3& start,const Vec3& goal) {
  size_t count=0;
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player &&
       ((npc->position()-start).length()<2200.f || (npc->position()-goal).length()<2200.f)) {
      // Match the existing `freeze` bench command before the teleport's first
      // native tick, so automatic important dialogs cannot consume player keys.
      npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);++count;
      }
  event("character_fixture_freeze","\"npcs\":"+std::to_string(count)+",\"radius_cm\":2200");
  }


void characterIdleGuards(World& world,const Npc& player) {
  size_t count=0;
  // A loaded archive does not retain external control. Guard the full ten
  // second idle window against nearby creatures, without relying on one
  // archive's NPC indices or changing the measured human's state or pose.
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && !npc->isHuman() &&
       (npc->position()-player.position()).length()<10000.f) {
      npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);++count;
      }
  event("character_idle_guards","\"creatures\":"+std::to_string(count)+",\"radius_cm\":10000");
  }


bool prepareCharacterRoute(Character& state,const CharacterContext& context,World& world,Npc& player,const std::string& name,bool abortMissing,std::string_view onlyWaypoint) {
  Character::CharacterRoute route;route.name=name;
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    if(!point.underWater && (onlyWaypoint.empty() || point.name==onlyWaypoint)) points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  auto* physics=world.physic();
  const auto& guild=world.script().guildVal();
  const float legacySlideAngle=float(guild.slide_angle[player.guild()]);
  const float legacySlideEnd=float(guild.slide_angle2[player.guild()]);
  const float steepMinimum=std::max(40.f,legacySlideAngle+1.f);
  const float steepMaximum=std::min(65.f,89.f-legacySlideEnd);
  const auto city=[](const WayPoint& point) {return point.name.starts_with("NW_CITY_");};
  const auto harbour=[](const WayPoint& point) {
    return point.name.starts_with("NW_CITY_") && (point.name.find("HABOUR")!=std::string::npos ||
      point.name.find("HARBOUR")!=std::string::npos || point.name.find("HARBOR")!=std::string::npos);
    };
  if(name=="port_edge")
    std::stable_partition(points.begin(),points.end(),[](const auto* point){return point->name=="NW_CITY_HABOUR_POOR_AREA_PATH_01_03";});
  bool found=false;
  if(name=="stairs_up" || name=="stairs_down" || name=="steep_slope") {
    const bool stairs=name!="steep_slope";
    for(const auto* point:points) {
      if(stairs && (!city(*point) || point->name.find("BED")!=std::string::npos)) continue;
      const std::vector<float> offsets=stairs?std::vector<float>{0.f,-80.f,80.f}:std::vector<float>{0.f,-120.f,120.f,-240.f,240.f};
      for(const float x:offsets)for(const float z:offsets) {
        if(found)continue;
        Vec3 center;
        if(!characterFloor(world,point->position()+Vec3(x,0,z),center))continue;
        float angle=0,rough=0;Vec3 downhill;
        if(!slopeAt(world,center,angle,downhill,rough)) continue;
        if(stairs ? !(angle>=18 && angle<=40 && rough>=6 && rough<=25) : !(angle>=steepMinimum && angle<=steepMaximum && rough<6)) continue;
        Vec3 low,high;
        if(!characterFloor(world,center+downhill*100.f,low) || !characterFloor(world,center-downhill*100.f,high)) continue;
        unsigned risers=0;float maximumRiser=0;
        if(stairs && (!characterLane(world,low,high,32) || !characterStairsProfile(world,low,high,risers,maximumRiser)))continue;
        if(!stairs && high.y-low.y<100) continue;
        if(!stairs) {
          const auto surface=physics->landRay(center+Vec3(0,20,0),50);
          const float k=float(M_PI)/180.f;
          if(!surface.hasCol || !(std::sin(legacySlideEnd*k)<surface.n.y && surface.n.y<std::cos(legacySlideAngle*k)))continue;
          }
        route.waypoint=point->name;
        // Enter the steep route on its measured surface and face downhill.
        // Starting at its low foot instead only tests walking into a cliff.
        route.start=stairs ? (name=="stairs_down" ? high : low) : center;
        route.goal=stairs ? (name=="stairs_down" ? low : high) : low;
        route.tolerance=55;found=true;
        event("character_route_surface","\"route\":"+jsonStr(name)+",\"angle_deg\":"+ragdollNumber(angle)+
          ",\"roughness_cm\":"+ragdollNumber(rough)+",\"riser_count\":"+std::to_string(risers)+
          ",\"maximum_riser_cm\":"+ragdollNumber(maximumRiser)+",\"legacy_slide_angle_deg\":"+ragdollNumber(legacySlideAngle)+
          ",\"legacy_slide_end_deg\":"+ragdollNumber(legacySlideEnd)+",\"sample_cm\":"+ragdollVector(center));
        }
      if(found)break;
      }
    }
  else if(name=="port_edge") {
    for(const auto* point:points) {
      if(!harbour(*point)) continue;
      for(float x:{0.f,-80.f,80.f,-160.f,160.f})for(float z:{0.f,-80.f,80.f,-160.f,160.f}) {
        if(found)continue;
        Vec3 center;if(!characterFloor(world,point->position()+Vec3(x,0,z),center)) continue;
        for(unsigned i=0;i<24 && !found;++i) {
          const float angle=float(i)*float(M_PI)/12.f;
          const Vec3 along(std::cos(angle),0,std::sin(angle)),side(-along.z,0,along.x);
          Vec3 end;
          if(!characterFloor(world,center+along*300.f,end) || !characterLane(world,center,end,20) || std::abs(end.y-center.y)>30) continue;
          bool nearEdge=true;
          for(float distance:{0.f,150.f,300.f}) {
            Vec3 floor;
            if(!characterFloor(world,center+along*distance,floor)) {nearEdge=false;break;}
            // A missing floor beyond a wall is not an exposed harbour edge.
            const auto from=floor+Vec3(0,80,0),to=from+side*95.f;
            const auto edge=physics->landRay(to,3000);
            if(physics->ray(from,to).hasCol || !edge.hasCol || floor.y-edge.v.y<=120.f) {nearEdge=false;break;}
            for(float width:{-25.f,25.f}) {
              Vec3 support;
              if(!characterFloor(world,floor+side*width,support) || std::abs(support.y-floor.y)>20.f)nearEdge=false;
              }
            if(!nearEdge)break;
            }
          if(!nearEdge) continue;
          route.waypoint=point->name;route.start=center;route.goal=end;route.tolerance=70;found=true;
          }
        }
      if(found)break;
      }
    }
  else if(name=="doorway") {
    for(const auto* point:points) {
      if(!city(*point))continue;
      Vec3 center;if(!characterFloor(world,point->position(),center)) continue;
      for(unsigned i=0;i<12 && !found;++i) {
        const float angle=float(i)*float(M_PI)/12.f;
        const Vec3 direction(std::cos(angle),0,std::sin(angle)),side(-direction.z,0,direction.x);
        for(float shift:{-160.f,-80.f,0.f,80.f,160.f}) {
          const auto doorway=center+direction*shift;
          const auto left=physics->ray(doorway+Vec3(0,110,0),doorway+side*130.f+Vec3(0,110,0));
          const auto right=physics->ray(doorway+Vec3(0,110,0),doorway-side*130.f+Vec3(0,110,0));
          if(!left.hasCol || !right.hasCol)continue;
          const float width=(left.v-right.v).length();
          if(width<95 || width>220 || (left.v-doorway-Vec3(0,110,0)).length()<42 ||
             (right.v-doorway-Vec3(0,110,0)).length()<42)continue;
          Vec3 start,end;
          if(!characterFloor(world,doorway-direction*180.f,start) || !characterFloor(world,doorway+direction*180.f,end) ||
             !characterLane(world,start,end,25) || std::abs(end.y-start.y)>25)continue;
          // A doorway opens out on at least one side; a narrow street is not this case.
          if(physics->ray(start+Vec3(0,110,0),start+side*150.f+Vec3(0,110,0)).hasCol &&
             physics->ray(start+Vec3(0,110,0),start-side*150.f+Vec3(0,110,0)).hasCol)continue;
          route.waypoint=point->name;route.start=start;route.goal=end;route.tolerance=75;found=true;break;
          }
        }
      if(found)break;
      }
    }
  else if(name=="tavern") {
    if(!context.movableReady)prepareMovableFixture(world,player);
    if(context.movableReady) {
      route.start=context.movableWalkStart;route.goal=context.movableWalkEnd;
      route.waypoint=std::string(symName(world,context.noiseHost));route.tolerance=55;found=true;
      }
    }
  else if(name=="jump_ledge") {
    for(const auto* point:points) {
      if(!city(*point)) continue;
      Vec3 start;if(!characterFloor(world,point->position(),start))continue;
      for(unsigned i=0;i<24 && !found;++i) {
        const float angle=float(i)*float(M_PI)/12.f;
        const Vec3 direction(std::cos(angle),0,std::sin(angle));
        for(float distance:{150.f,200.f,250.f}) {
          const auto landing=physics->landRay(start+direction*distance+Vec3(0,210,0),250);
          if(!landing.hasCol || landing.n.y<.8f || landing.v.y-start.y<55 || landing.v.y-start.y>110)continue;
          if(physics->ray(start+Vec3(0,180,0),landing.v+Vec3(0,180,0)).hasCol)continue;
          Vec3 beyond;if(!characterFloor(world,landing.v+direction*80.f,beyond) || std::abs(beyond.y-landing.v.y)>15)continue;
          route.waypoint=point->name;route.start=start;route.goal=beyond;route.tolerance=75;found=true;break;
          }
        }
      if(found)break;
      }
    }
  else if(name=="water_entry") {
    // The city waterfront is a quay above sea level. Walking off it exercises
    // walking -> legacy falling -> swimming, including the actual handoff.
    for(const auto* point:points) {
      if(!harbour(*point) && point->name.find("WATER")==std::string::npos &&
         point->name.find("BEACH")==std::string::npos && point->name.find("COAST")==std::string::npos)continue;
      Vec3 dry;if(!characterFloor(world,point->position(),dry) || physics->waterRay(dry,15).hasCol)continue;
      for(unsigned i=0;i<24 && !found;++i) {
        const float angle=float(i)*float(M_PI)/12.f;
        const Vec3 direction(std::cos(angle),0,std::sin(angle));
        for(float distance:{200.f,350.f,500.f,700.f,1000.f,1400.f}) {
          const auto bottom=physics->landRay(dry+direction*distance+Vec3(0,100,0),3000);
          if(!bottom.hasCol)continue;
          const auto liquid=physics->waterRay(bottom.v+Vec3(0,5,0),0);
          if(!liquid.hasCol || liquid.wdepth-bottom.v.y<160 || liquid.wdepth>dry.y+30 || dry.y-liquid.wdepth>1000)continue;
          if(physics->ray(dry+Vec3(0,120,0),Vec3(bottom.v.x,dry.y+120,bottom.v.z)).hasCol)continue;
          route.waypoint=point->name;route.start=dry;route.goal={bottom.v.x,liquid.wdepth,bottom.v.z};
          route.tolerance=100;found=true;break;
          }
        }
      if(found)break;
      }
    }
  if(!found) {
    event("character_error","\"route\":"+jsonStr(name)+",\"reason\":\"no stock geometry satisfies fixture preconditions\"");
    if(abortMissing)arenaAbort("character route preparation failed: "+name);
    return false;
    }
  route.direction=route.goal-route.start;route.direction.y=0;route.direction=Vec3::normalize(route.direction);
  route.prepared=true;route.lastSupported=route.start;
  characterFreezeNear(world,player,route.start,route.goal);
  player.stopWalking();player.clearAiQueue();player.setPosition(route.start);player.setDirection(route.direction);player.updateTransform();
  context.hasPrev=false;state.characterRoute=route;
  context.cam=C_Follow;context.camDist=240;context.camHeight=70;context.camAngle=110;context.camAuto=true;
  event("character_route_setup","\"route\":"+jsonStr(name)+",\"waypoint\":"+jsonStr(route.waypoint)+
    ",\"start_cm\":"+ragdollVector(route.start)+",\"goal_cm\":"+ragdollVector(route.goal)+
    ",\"tolerance_cm\":"+ragdollNumber(route.tolerance)+",\"yaw_deg\":"+ragdollNumber(player.rotation()));
  return true;
  }


void characterSnapshot(World& world,Npc& player,std::string_view operation) {
  event("character_snapshot","\"operation\":"+jsonStr(operation)+",\"world\":"+jsonStr(world.name())+
    ",\"root_cm\":"+ragdollVector(player.position())+",\"visual_root_cm\":"+ragdollVector(player.characterVisualPosition())+",\"cv_enabled\":"+
    (Gothic::physicsFlags().characterVirtual?"true":"false")+
    ",\"cv_active\":"+(player.characterVirtualActive()?"true":"false"));
  }


void characterRouteEnd(Character& state,World& world,Npc& player) {
  auto& route=state.characterRoute;
  if(!route.active) {arenaAbort("character route end without a begun route");return;}
  route.active=false;
  event("character_route_end","\"route\":"+jsonStr(route.name)+",\"success\":"+(route.reached?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string((route.reached?route.arrived:world.tickCount())-route.began)+
    ",\"window_ms\":"+std::to_string(world.tickCount()-route.began)+",\"start_cm\":"+ragdollVector(route.start)+
    ",\"goal_cm\":"+ragdollVector(route.goal)+",\"end_cm\":"+ragdollVector(player.position())+
    ",\"fall_count\":"+std::to_string(route.fallCount)+",\"saw_slide\":"+(route.sawSlide?"true":"false")+",\"saw_swim\":"+(route.sawSwim?"true":"false"));
  }


Matrix4x4 characterSupportMatrix(Character& state,World& world) {
  if(state.characterMover)return state.characterMover->transform();
  if(state.characterPlatform)return state.characterPlatformMatrix;
  for(const auto& row:world.physic()->movableVobSnapshot())
    if(row.id==state.characterPropId)return row.transform;
  return Matrix4x4::mkIdentity();
  }


void characterRideSample(Character& state,World& world,const Npc& rider) {
  const auto matrix=characterSupportMatrix(state,world);
  auto expected=state.characterRideLocalStart;matrix.project(expected);
  const auto error=rider.position()-expected;
  auto local=rider.position();auto inverse=matrix;inverse.inverse();inverse.project(local);
  state.characterRideMaximumDrift=std::max(state.characterRideMaximumDrift,error.length());
  state.characterRideMaximumHorizontalDrift=std::max(state.characterRideMaximumHorizontalDrift,std::hypot(error.x,error.z));
  state.characterRideMaximumLocalDrift=std::max(state.characterRideMaximumLocalDrift,(local-state.characterRideLocalStart).length());
  event("character_ride_sample","\"support_kind\":"+jsonStr(state.characterMover?"mover":state.characterPlatform?"kinematic":"dynamic")+
    ",\"rider\":"+jsonStr(state.characterRideNpc?"npc":"hero")+",\"rotation\":"+(state.characterRideRotating?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string(world.tickCount()-state.characterRideBegan)+
    ",\"support_cm\":"+ragdollVector(movablePosition(matrix))+",\"rider_cm\":"+ragdollVector(rider.position())+
    ",\"expected_cm\":"+ragdollVector(expected)+",\"error_cm\":"+ragdollVector(error)+
    ",\"local_cm\":"+ragdollVector(local)+",\"angle_deg\":"+ragdollNumber(state.characterRideAngle)+
    ",\"ground_velocity_cm_s\":"+ragdollVector(rider.characterVirtualGroundVelocity())+
    ",\"cv_active\":"+(rider.characterVirtualActive()?"true":"false")+
    ",\"falling\":"+(rider.isFalling()?"true":"false")+",\"in_air\":"+(rider.isInAir()?"true":"false"));
  }


void characterFields(Character& state,const CharacterContext& context,World& world,Npc& player,uint64_t dt,PlayerControl& ctrl) {
  auto& route=state.characterRoute;
  if(!route.active && !state.characterMeasuring && state.characterPropRole.empty() && !state.characterRiding)return;
  const auto now=std::chrono::steady_clock::now();
  const float wall=state.characterHasWall?std::chrono::duration<float,std::milli>(now-state.characterLastWall).count():0;
  state.characterHasWall=true;state.characterLastWall=now;
  const auto& character=state.characterRiding && state.characterRideNpc?*state.characterRideNpc:player;
  const auto position=character.position();
  bool move=false;
  for(const auto& hold:context.holds)
    move|=hold.a==KeyCodec::Forward || hold.a==KeyCodec::Back || hold.a==KeyCodec::Left || hold.a==KeyCodec::Right;
  bool normal=false;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  normal=AppCadence::normalClock();
#endif
  const std::string rideLabel=std::string(state.characterMover?"ride_mover":state.characterPlatform?
    state.characterRideRotating?"ride_rotation":"ride_kinematic":"ride_dynamic")+(state.characterRideNpc?"_npc":"");
  const std::string label=state.characterMeasuring?"performance":route.active?route.name:state.characterRiding?rideLabel:state.characterPropRole;
  if(!normal || wall>0)state.characterFrames.push_back({label,state.characterFrame++,world.tickCount(),dt,position,
    character.characterVirtualGroundVelocity(),character.characterVisualPosition(),move,character.isInAir(),character.isFalling(),character.isSwim(),
    state.characterRideNpc?bool(Gothic::settingsGetI("GAME","npc_character_virtual")):Gothic::physicsFlags().characterVirtual,
    character.characterVirtualActive(),context.rec && context.recVideo,normal,
    world.physic()->physicsStats().stepMilliseconds,wall});
  if(state.characterMeasuring) {
    const float sign=((world.tickCount()-state.characterMeasureStart)/700)%2==0?1.f:-1.f;
    player.setDirection(state.characterRoute.direction*sign);
    }
  if(route.active) {
    route.sawSwim|=player.isSwim();route.sawSlide|=player.isSlide();
    if(!player.isInAir() && !player.isFalling() && !player.isSwim())route.lastSupported=position;
    const bool edgeFall=route.name!="jump_ledge" && route.name!="water_entry" && player.isFalling() &&
      route.lastSupported.y-position.y>100.f;
    if(edgeFall && !route.falling) {
      ++route.fallCount;
      event("character_fall","\"route\":"+jsonStr(route.name)+",\"root_cm\":"+ragdollVector(position)+
        ",\"support_cm\":"+ragdollVector(route.lastSupported));
      }
    route.falling=edgeFall;
    const auto offset=position-route.goal;
    const float remaining=-Vec3::dotProduct(offset,route.direction);
    const float side=std::abs(offset.x*route.direction.z-offset.z*route.direction.x);
    const bool height=route.name=="water_entry"?route.sawSwim:std::abs(position.y-route.goal.y)<60.f;
    if(!route.reached && remaining<=route.tolerance && side<route.tolerance && height) {
      route.reached=true;route.arrived=world.tickCount();
      std::erase_if(context.holds,[&](const Hold& hold) {
        const bool moving=hold.a==KeyCodec::Forward || hold.a==KeyCodec::Back || hold.a==KeyCodec::Left || hold.a==KeyCodec::Right;
        if(moving)release(ctrl,hold.a);
        return moving;
        });
      event("character_route_arrive","\"route\":"+jsonStr(route.name)+",\"elapsed_ms\":"+
        std::to_string(route.arrived-route.began)+",\"root_cm\":"+ragdollVector(position));
      }
    }
  if(state.characterRideMoving)characterRideSample(state,world,character);
  if(state.characterItem)
    state.characterPropTravel=std::max(state.characterPropTravel,(state.characterItem->position()-state.characterPropInitial).length());
  if(state.characterPropId)
    for(const auto& body:world.physic()->movableVobSnapshot())if(body.id==state.characterPropId)
      state.characterPropTravel=std::max(state.characterPropTravel,(movablePosition(body.transform)-state.characterPropInitial).length());
  }


void characterPropCheckpoint(Character& state,World& world,Npc& player) {
  if(state.characterPropRole.empty()) {arenaAbort("character prop checkpoint needs a prop");return;}
  Vec3 position,velocity;float mass=0;bool contact=false,blocks=false;
  if(state.characterItem) {position=state.characterItem->position();velocity=state.characterItem->physicsVelocity();mass=state.characterItem->physicsMass();contact=state.characterItem->physicsTouchedByNpc();}
  else for(const auto& body:world.physic()->movableVobSnapshot()) if(body.id==state.characterPropId) {
    position=movablePosition(body.transform);velocity=body.velocity;mass=body.mass;contact=body.touchedByNpc;blocks=body.blocksWalking;break;
    }
  float nearPlane=0,farPlane=0;bool crossed=false;
  for(const auto& body:world.physic()->movableVobSnapshot())if(body.id==state.characterPropId) {
    nearPlane=std::numeric_limits<float>::infinity();farPlane=-nearPlane;
    for(unsigned corner=0;corner<8;++corner) {
      Vec3 point((corner&1)?body.localBounds[1].x:body.localBounds[0].x,
                 (corner&2)?body.localBounds[1].y:body.localBounds[0].y,
                 (corner&4)?body.localBounds[1].z:body.localBounds[0].z);
      body.transform.project(point);
      const float projected=Vec3::dotProduct(point,state.characterPropDirection);
      nearPlane=std::min(nearPlane,projected);farPlane=std::max(farPlane,projected);
      }
    crossed=Vec3::dotProduct(player.position(),state.characterPropDirection)>farPlane+25.f;
    break;
    }
  event("character_prop_checkpoint","\"role\":"+jsonStr(state.characterPropRole)+",\"id\":"+
    std::to_string(state.characterPropId)+",\"elapsed_ms\":"+std::to_string(world.tickCount()-state.characterPropBegan)+",\"mass_kg\":"+ragdollNumber(mass)+",\"position_cm\":"+ragdollVector(position)+
    ",\"velocity_cm_s\":"+ragdollVector(velocity)+",\"max_displacement_cm\":"+ragdollNumber(state.characterPropTravel)+
    ",\"initial_cm\":"+ragdollVector(state.characterPropInitial)+",\"hero_cm\":"+ragdollVector(player.position())+
    ",\"hero_initial_cm\":"+ragdollVector(state.characterPropHeroInitial)+",\"native_contact\":"+(contact?"true":"false")+
    ",\"passed_through\":"+(crossed?"true":"false")+",\"near_plane_cm\":"+ragdollNumber(nearPlane)+
    ",\"far_plane_cm\":"+ragdollNumber(farPlane)+",\"blocks_walking\":"+(blocks?"true":"false"));
  }


void prepareCharacterProp(Character& state,const CharacterContext& context,World& world,Npc& player,const std::string& role) {
  if(state.characterItem) {world.removeItem(*state.characterItem);state.characterItem=nullptr;}
  state.characterProp.reset();state.characterPropId=0;state.characterPropTravel=0;state.characterPropRole=role;
  if(!context.movableReady) prepareMovableFixture(world,player);
  if(!context.movableReady)return;
  // The general tavern stand adds a floor dish at this lane's midpoint. Remove
  // those temporary stand props so no overlap can propel a test item by itself.
  context.movableFixtures.clear();
  const auto direction=Vec3::normalize(context.movableWalkEnd-context.movableWalkStart);
  const auto center=(context.movableWalkStart+context.movableWalkEnd)*.5f;
  if(role=="item" || role=="mug") {
    const auto symbol=world.script().findSymbolIndex(role=="mug"?"ITFO_BEER":"ItPo_Health_01");
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(center+Vec3(0,10,0));
    state.characterItem=world.addItemDyn(symbol,matrix,size_t(-1));
    if(!state.characterItem) {arenaAbort("character real Item insertion failed");return;}
    state.characterItem->setPhysicsEnable(world);
    state.characterPropInitial=state.characterItem->position();
    }
  else {
    std::string visual;
    for(const auto& body:world.physic()->movableVobSnapshot())if(body.className==role) {visual=body.visual;break;}
    if(visual.empty() && role=="barrel") visual="NW_HARBOUR_BARREL_01.3DS";
    const auto* mesh=Resources::loadMesh(visual);const auto* rule=MovableVobs::classifyVisual(visual);
    if(!mesh || !rule) {arenaAbort("character stock prop mesh/class unavailable: "+role);return;}
    const auto* bounds=mesh->bbox();
    asset::VirtualObject data;data.type=asset::VirtualObjectType::zCVob;data.id=uint32_t(-1);
    data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
    data.position={center.x-(bounds[0].x+bounds[1].x)*.5f,center.y-bounds[0].y+.2f,center.z-(bounds[0].z+bounds[1].z)*.5f};
    data.visual=std::make_shared<asset::VisualMesh>();data.visual->type=asset::VisualType::MESH;data.visual->name=visual;
    auto fixture=std::make_unique<MovableFixture>();fixture->visual.setVisual(data,world,false,false);
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(data.position.x,data.position.y,data.position.z);fixture->visual.setObjMatrix(matrix);
    const auto before=world.physic()->movableVobSnapshot();
    fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
    if(fixture->body.isEmpty()) {arenaAbort("character native prop insertion failed");return;}
    fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& matrix){view->setObjMatrix(matrix);});
    state.characterProp=std::move(fixture);state.characterPropInitial=movablePosition(matrix);
    for(const auto& body:world.physic()->movableVobSnapshot())
      if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;}))state.characterPropId=body.id;
    }
  characterFreezeNear(world,player,context.movableWalkStart,context.movableWalkEnd);
  player.stopWalking();player.clearAiQueue();player.setPosition(context.movableWalkStart);player.setDirection(direction);player.updateTransform();
  state.characterPropHeroInitial=player.position();state.characterPropDirection=direction;state.characterPropBegan=world.tickCount();state.characterHasWall=false;
  event("character_prop_setup","\"role\":"+jsonStr(role)+",\"initial_cm\":"+ragdollVector(state.characterPropInitial)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"direction\":"+ragdollVector(direction));
  }


void characterRideCheckpoint(Character& state,World& world,Npc& player,bool setup) {
  if(!state.characterRiding) {arenaAbort("character ride checkpoint needs a support body");return;}
  const auto& rider=state.characterRideNpc?*state.characterRideNpc:player;
  const auto matrix=characterSupportMatrix(state,world);
  const auto body=movablePosition(matrix);
  auto expected=state.characterRideLocalStart;matrix.project(expected);
  auto local=rider.position();auto inverse=matrix;inverse.inverse();inverse.project(local);
  const auto error=rider.position()-expected;
  event(setup?"character_ride_setup":"character_ride_checkpoint","\"support_kind\":"+jsonStr(state.characterMover?"mover":state.characterPlatform?"kinematic":"dynamic")+
    ",\"support_cm\":"+ragdollVector(body)+",\"support_initial_cm\":"+ragdollVector(state.characterRideStart)+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"hero_initial_cm\":"+ragdollVector(state.characterRideHeroStart)+
    ",\"rider\":"+jsonStr(state.characterRideNpc?"npc":"hero")+",\"rider_cm\":"+ragdollVector(rider.position())+
    ",\"local_initial_cm\":"+ragdollVector(state.characterRideLocalStart)+",\"local_cm\":"+ragdollVector(local)+
    ",\"expected_cm\":"+ragdollVector(expected)+",\"error_cm\":"+ragdollVector(error)+
    ",\"support_displacement_cm\":"+ragdollNumber((body-state.characterRideStart).length())+
    ",\"angle_deg\":"+ragdollNumber(state.characterRideAngle)+",\"elapsed_ms\":"+std::to_string(world.tickCount()-state.characterRideBegan)+
    ",\"max_drift_cm\":"+ragdollNumber(state.characterRideMaximumDrift)+
    ",\"max_horizontal_drift_cm\":"+ragdollNumber(state.characterRideMaximumHorizontalDrift)+
    ",\"max_local_drift_cm\":"+ragdollNumber(state.characterRideMaximumLocalDrift)+
    ",\"mass_kg\":"+ragdollNumber(state.characterProp?state.characterProp->body.mass():0.f)+
    ",\"support_velocity_cm_s\":"+ragdollVector(state.characterProp?state.characterProp->body.velocity():Vec3())+
    ",\"ground_velocity_cm_s\":"+ragdollVector(rider.characterVirtualGroundVelocity())+
    ",\"falling\":"+(rider.isFalling()?"true":"false")+",\"in_air\":"+(rider.isInAir()?"true":"false"));
  }


// NPC stands use real AI_GoToPoint actions. Only fixture preparation moves an
// actor directly; route frames are ordinary animation-driven MoveAlgo ticks.
std::string npcCharacterBodyJson(const CharacterVirtualDiagnostics::Body& body,
                                const std::vector<DynamicWorld::MovableVobSnapshot>& movable) {
  std::string visual=body.label,kind;
  if(body.pointerType==DynamicWorld::P_Item)
    kind="Item";
  else if(body.pointerType==DynamicWorld::P_Interactive) {
    if(const auto* rule=MovableVobs::classifyVisual(visual))kind=rule->className;
    else kind="Interactive";
    }
  else if(body.pointerType==DynamicWorld::P_MovableVob) {
    const auto id=reinterpret_cast<uintptr_t>(body.payload);
    for(const auto& entry:movable)if(entry.id==id) {visual=entry.visual;kind=entry.className;break;}
    }
  if(kind.empty()) {
    if(const auto* rule=MovableVobs::classifyVisual(visual))kind=rule->className;
    else if(body.layer==DynamicWorld::C_Landscape)kind="landscape";
    else kind="vob";
    }
  return "{\"present\":"+std::string(body.present?"true":"false")+",\"id\":"+std::to_string(body.id)+
    ",\"layer\":"+std::to_string(body.layer)+",\"pointer_type\":"+std::to_string(body.pointerType)+
    ",\"motion_type\":"+std::to_string(body.motionType)+",\"mass_kg\":"+ragdollNumber(body.mass)+
    ",\"label\":"+jsonStr(body.label)+",\"visual\":"+jsonStr(visual)+",\"class\":"+jsonStr(kind)+
    ",\"position_cm\":"+ragdollVector(body.position)+",\"linear_velocity_cm_s\":"+ragdollVector(body.linearVelocity)+
    ",\"angular_velocity_rad_s\":"+ragdollVector(body.angularVelocity)+",\"point_velocity_cm_s\":"+ragdollVector(body.pointVelocity)+"}";
  }


std::string npcCharacterPoseJson(const CharacterVirtualDiagnostics::Pose& pose,
                                const std::vector<DynamicWorld::MovableVobSnapshot>& movable) {
  std::string contacts="[";
  const auto flag=[](bool value){return value?"true":"false";};
  for(const auto& c:pose.contacts) {
    if(contacts.size()>1)contacts+=',';
    contacts+="{\"body\":"+npcCharacterBodyJson(c.body,movable)+",\"subshape\":"+std::to_string(c.subShape)+
      ",\"position_cm\":"+ragdollVector(c.position)+",\"normal\":"+ragdollVector(c.contactNormal)+
      ",\"surface_normal\":"+ragdollVector(c.surfaceNormal)+",\"point_velocity_cm_s\":"+ragdollVector(c.pointVelocity)+
      ",\"distance_cm\":"+ragdollNumber(c.distance)+",\"supporting_volume_distance_cm\":"+ragdollNumber(c.supportingVolumeDistance)+
      ",\"had_collision\":"+flag(c.hadCollision)+",\"discarded\":"+flag(c.discarded)+",\"sensor\":"+flag(c.sensor)+
      ",\"can_push_character\":"+flag(c.canPushCharacter)+",\"within_supporting_volume\":"+flag(c.withinSupportingVolume)+
      ",\"walkable_slope\":"+flag(c.walkableSlope)+",\"support_allowed_by_policy\":"+flag(c.supportAllowedByPolicy)+
      ",\"support_eligible\":"+flag(c.supportEligible)+"}";
    }
  return "{\"position_cm\":"+ragdollVector(pose.position)+",\"velocity_cm_s\":"+ragdollVector(pose.velocity)+
    ",\"ground_state\":"+std::to_string(pose.groundState)+",\"ground\":"+npcCharacterBodyJson(pose.ground,movable)+
    ",\"ground_subshape\":"+std::to_string(pose.groundSubShape)+",\"ground_position_cm\":"+ragdollVector(pose.groundPosition)+
    ",\"ground_normal\":"+ragdollVector(pose.groundNormal)+",\"ground_velocity_cm_s\":"+ragdollVector(pose.groundVelocity)+
    ",\"contacts\":"+contacts+"]}";
  }


std::string npcCharacterCollisionJson(World& world,const CharacterVirtualDiagnostics::NpcCollision& collision) {
  // Resolve a borrowed neighbour immediately; historical samples retain JSON
  // values, never pointers to actors that may despawn later in the frame.
  const auto id=world.npcId(collision.npc);
  const auto* blocker=id!=uint32_t(-1)?world.npcById(id):nullptr;
  return "{\"npc_id\":"+(blocker?std::to_string(id):"null")+",\"npc\":"+jsonStr(blocker?symName(world,blocker):"none")+
    ",\"position_cm\":"+ragdollVector(collision.position)+",\"normal\":"+ragdollVector(collision.normal)+
    ",\"allowed_depth_cm\":"+ragdollNumber(collision.allowedDepth)+",\"rejected_depth_cm\":"+ragdollNumber(collision.rejectedDepth)+
    ",\"tested_position_cm\":"+ragdollVector(collision.testedPosition)+"}";
  }


std::string npcCharacterMoveJson(World& world,const CharacterVirtualDiagnostics::NpcMove& move) {
  const auto flag=[](bool value){return value?"true":"false";};
  return "{\"initial_cm\":"+ragdollVector(move.initial)+",\"requested_cm\":"+ragdollVector(move.requested)+
    ",\"clipped_request_cm\":"+ragdollVector(move.clippedRequest)+",\"native_position_cm\":"+ragdollVector(move.nativePosition)+
    ",\"accepted_cm\":"+ragdollVector(move.accepted)+",\"request_clipped\":"+flag(move.requestClipped)+
    ",\"resolved_clipped\":"+flag(move.resolvedClipped)+",\"blocked\":"+flag(move.blocked)+
    ",\"sequence\":"+std::to_string(move.sequence)+",\"dt_ms\":"+std::to_string(move.dt)+
    ",\"advancement_ratio\":"+ragdollNumber(move.advancementRatio)+",\"steep_contact\":"+flag(move.steepContact)+
    ",\"steep_contact_normal\":"+ragdollVector(move.steepContactNormal)+
    ",\"request_collision\":"+npcCharacterCollisionJson(world,move.requestCollision)+
    ",\"resolved_collision\":"+npcCharacterCollisionJson(world,move.resolvedCollision)+"}";
  }


CharacterMoveFrame characterMoveFrame(const Npc& npc) {
  CharacterVirtualDiagnostics diagnostic;
  CharacterMoveFrame frame;
  frame.valid=npc.characterVirtualDiagnostics(diagnostic);
  if(!frame.valid)return frame;
  const auto& move=diagnostic.npcMove;
  frame.sequence=move.sequence;frame.dt=move.dt;frame.requested=move.requested;
  frame.acceptedDelta=move.accepted-move.initial;frame.advancementRatio=move.advancementRatio;
  frame.steepContact=move.steepContact;frame.steepNormal=move.steepContactNormal;frame.blocked=move.blocked;
  return frame;
  }


bool npcCharacterNeedsMovableLookup(const CharacterVirtualDiagnostics::Pose& pose) {
  if(pose.ground.pointerType==DynamicWorld::P_MovableVob)return true;
  return std::any_of(pose.contacts.begin(),pose.contacts.end(),[](const auto& contact) {
    return contact.body.pointerType==DynamicWorld::P_MovableVob;
    });
  }


std::string npcCharacterSample(World& world,const Npc& npc,float physicalJump,float visualJump,uint64_t frame,uint64_t dt) {
  const auto goal=npc.navigationGoal();
  const auto observed=npc.bodyObservationState();
  const auto goalId=world.npcId(goal.npc);
  const auto* target=goalId!=uint32_t(-1)?world.npcById(goalId):nullptr;
  CharacterVirtualDiagnostics d;
  const bool valid=npc.characterVirtualDiagnostics(d);
  std::string row="\"tick\":"+std::to_string(world.tickCount())+",\"frame\":"+std::to_string(frame)+
    ",\"frame_dt_ms\":"+std::to_string(dt)+",\"npc_id\":"+std::to_string(world.npcId(&npc))+",\"npc\":"+jsonStr(symName(world,&npc))+
    ",\"root_cm\":"+ragdollVector(npc.position())+",\"visual_root_cm\":"+ragdollVector(npc.characterVisualPosition())+
    ",\"root_motion_model_cm\":"+ragdollVector(npc.animMoveSpeed(dt))+
    ",\"physical_dy_cm\":"+ragdollNumber(physicalJump)+",\"visual_dy_cm\":"+ragdollNumber(visualJump)+
    ",\"yaw_deg\":"+ragdollNumber(npc.rotation())+",\"body_state\":"+std::to_string(unsigned(npc.bodyState()))+
    ",\"body_state_masked\":"+std::to_string(unsigned(npc.bodyStateMasked()))+",\"state\":"+jsonStr(stateName(world,npc))+
    ",\"in_air\":"+(npc.isInAir()?"true":"false")+",\"falling\":"+(npc.isFalling()?"true":"false")+
    ",\"interactive\":"+(npc.interactive()?"true":"false")+",\"external_control\":"+(npc.isExternalControl()?"true":"false")+
    ",\"process_policy\":"+std::to_string(int(npc.processPolicy()))+
    ",\"ai_wait_ms\":"+std::to_string(observed.aiWaitMs)+",\"ani_wait_ms\":"+std::to_string(observed.aniWaitMs)+
    ",\"output_wait_ms\":"+std::to_string(observed.outputWaitMs)+",\"fai_wait_ms\":"+std::to_string(observed.faiWaitMs)+
    ",\"animations\":"+jsonStr(npc.animNames())+",\"ai_queue_empty\":"+(npc.isAiQueueEmpty()?"true":"false")+
    ",\"ai_busy\":"+(npc.isAiBusy()?"true":"false")+",\"move_hint\":"+std::to_string(unsigned(npc.moveHint()))+
    ",\"cv_enabled\":"+((npc.isPlayer() ? Gothic::physicsFlags().characterVirtual :
                                         Gothic::physicsFlags().npcCharacterVirtual)?"true":"false")+
    ",\"cv_active\":"+(npc.characterVirtualActive()?"true":"false")+",\"diagnostics_valid\":"+(valid?"true":"false")+
    ",\"nav_goal\":{\"active\":"+(goal.active?"true":"false")+",\"kind\":"+jsonStr(goal.kind)+",\"name\":"+jsonStr(goal.name)+
    ",\"position_cm\":"+ragdollVector(goal.position)+",\"npc_id\":"+(target?std::to_string(goalId):"null")+
    ",\"npc\":"+jsonStr(target?symName(world,target):"none")+"}";
  if(valid) {
    // Look up movable visual/class names only when a sampled native body needs
    // them. The trace writes referenced contacts, not the world's full catalog.
    std::vector<DynamicWorld::MovableVobSnapshot> movable;
    if(npcCharacterNeedsMovableLookup(d.before) || npcCharacterNeedsMovableLookup(d.afterExtendedUpdate) ||
       npcCharacterNeedsMovableLookup(d.afterStairs))movable=world.physic()->movableVobSnapshot();
    row+=",\"dt_ms\":"+std::to_string(d.dt)+",\"requested_cm\":"+ragdollVector(d.requested)+
      ",\"ground_normal_fallbacks\":"+std::to_string(d.groundNormalFallbacks)+
      ",\"requested_velocity_cm_s\":"+ragdollVector(d.requestedVelocity)+
      ",\"stair_desired_velocity_cm_s\":"+ragdollVector(d.stairDesiredVelocity)+
      ",\"max_slope_rad\":"+ragdollNumber(d.maxSlopeRadians)+",\"stair_height_cm\":"+ragdollNumber(d.stairHeight)+
      ",\"stair_desired_distance_cm\":"+ragdollNumber(d.stairDesiredDistance)+
      ",\"stair_achieved_distance_cm\":"+ragdollNumber(d.stairAchievedDistance)+
      ",\"stair_supported\":"+(d.stairSupported?"true":"false")+",\"stair_can_walk\":"+(d.stairCanWalk?"true":"false")+
      ",\"stair_blocked_by_policy\":"+(d.stairBlockedByPolicy?"true":"false")+
      ",\"stair_progress_blocked\":"+(d.stairProgressBlocked?"true":"false")+
      ",\"stair_attempted\":"+(d.stairAttempted?"true":"false")+",\"stair_succeeded\":"+(d.stairSucceeded?"true":"false")+
      ",\"stair_probe\":{\"reason\":"+jsonStr(d.stairProbe.reason)+
      ",\"up_cm\":"+ragdollVector(d.stairProbe.upPosition)+",\"forward_cm\":"+ragdollVector(d.stairProbe.forwardPosition)+
      ",\"final_cm\":"+ragdollVector(d.stairProbe.finalPosition)+",\"test_cm\":"+ragdollVector(d.stairProbe.testPosition)+
      ",\"test_final_cm\":"+ragdollVector(d.stairProbe.testFinalPosition)+
      ",\"landing_normal\":"+ragdollVector(d.stairProbe.landingNormal)+
      ",\"test_landing_normal\":"+ragdollVector(d.stairProbe.testLandingNormal)+
      ",\"up_fraction\":"+ragdollNumber(d.stairProbe.upFraction)+
      ",\"landing_fraction\":"+ragdollNumber(d.stairProbe.landingFraction)+
      ",\"test_landing_fraction\":"+ragdollNumber(d.stairProbe.testLandingFraction)+"}"+
      ",\"stair_obstacle_id\":"+std::to_string(d.stairObstacleId)+",\"npc_move\":"+npcCharacterMoveJson(world,d.npcMove)+
      ",\"before\":"+npcCharacterPoseJson(d.before,movable)+
      ",\"after_extended_update\":"+npcCharacterPoseJson(d.afterExtendedUpdate,movable)+
      ",\"after_stairs\":"+npcCharacterPoseJson(d.afterStairs,movable);
    }
  return row;
  }


void npcCharacterSupportTrace(Character& state,const CharacterContext& context,World& world,const Npc& npc,float physicalJump,float visualJump) {
  if(!npcCharacterTrace)return;
  auto& route=state.npcCharacterRoute;
  auto row="\"route\":"+jsonStr(route.name)+","+
    npcCharacterSample(world,npc,physicalJump,visualJump,state.npcCharacterFrames.size()-1,context.frameMs);
  route.supportHistory.push_back(std::move(row));
  if(route.supportHistory.size()>6)route.supportHistory.pop_front();
  // This threshold selects diagnostic output only; it never changes motion.
  if(std::abs(physicalJump)<=10.f && std::abs(visualJump)<=10.f)return;
  const int count=int(route.supportHistory.size());
  for(int i=0;i<count;++i)
    event("npc_character_support_trace","\"spike_frame\":"+std::to_string(state.npcCharacterFrames.size()-1)+
      ",\"relative_frame\":"+std::to_string(i-count+1)+","+route.supportHistory[size_t(i)]);
  }


void npcCharacterMotionTrace(Character& state,const CharacterContext& context,World& world,uint64_t dt) {
  for(auto& actor:state.npcCharacterTraceActors) {
    const auto& npc=*actor.npc;
    const auto root=npc.position(),visual=npc.characterVisualPosition();
    const float physicalJump=actor.hasPrevious?root.y-actor.lastRoot.y:0.f;
    const float visualJump=actor.hasPrevious?visual.y-actor.lastVisual.y:0.f;
    event("npc_character_motion_trace","\"window\":"+jsonStr(state.npcCharacterObserving?state.npcCharacterWindow:std::string())+","+
      npcCharacterSample(world,npc,physicalJump,visualJump,context.step,dt));
    actor.lastRoot=root;actor.lastVisual=visual;actor.hasPrevious=true;
    }
  }


void npcCharacterSnapshot(World& world,Npc& player,std::string_view label) {
  std::string rows="[";
  for(uint32_t id=0;id<world.npcCount();++id) {
    const auto* npc=world.npcById(id);
    if(!npc || npc==&player || !npc->isHuman() || npc->isDown() ||
       (npc->position()-player.position()).length()>2300.f)continue;
    if(rows.size()>1)rows+=',';
    rows+="{\"npc_id\":"+std::to_string(id)+",\"npc\":"+jsonStr(symName(world,npc))+
      ",\"root_cm\":"+ragdollVector(npc->position())+",\"visual_root_cm\":"+ragdollVector(npc->characterVisualPosition())+
      ",\"cv_active\":"+(npc->characterVirtualActive()?"true":"false")+
      ",\"interactive\":"+(npc->interactive()?"true":"false")+"}";
    }
  event("npc_character_snapshot","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false")+
    ",\"hero_cm\":"+ragdollVector(player.position())+",\"actors\":"+rows+"]");
  }


Npc* npcCharacterActor(World& world,const Vec3& position) {
  auto* npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),position);
  if(!npc) {arenaAbort("NPC CharacterVirtual fixture actor insertion failed");return nullptr;}
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->closeWeapon(true);
  // Keep native queued actions and movement; suppress source-save routines
  // and perception callbacks between scripted fixture legs.
  npc->setExternalControl(true);npc->setProcessPolicy(NpcProcessPolicy::AiNormal);npc->setWalkMode(WalkBit::WM_Run);
  npc->aiPush(AiQueue::aiWait(60000));npc->setPosition(position);npc->updateTransform();
  return npc;
  }


void npcCharacterPlaceHero(World& world,Npc& player,const Vec3& start,const Vec3& goal) {
  auto along=goal-start;along.y=0;
  along=along.length()>0?Vec3::normalize(along):Vec3(1,0,0);
  const Vec3 side(-along.z,0,along.x);
  Vec3 hero;bool found=false;
  for(float distance:{400.f,-400.f,600.f,-600.f,800.f,-800.f}) {
    const auto at=start+side*distance;
    if(!characterFloor(world,at,hero) || std::abs(hero.y-start.y)>150)continue;
    if(world.physic()->ray(hero+Vec3(0,100,0),hero+along*40.f+Vec3(0,100,0)).hasCol)continue;
    found=true;break;
    }
  if(!found)hero=start-along*250.f;
  player.stopWalking();player.clearAiQueue();player.setPosition(hero);player.setDirection(start-hero);player.updateTransform();
  }


void characterIdleSlopeSample(Character& state,const CharacterContext& context,World& world,Npc& player,std::string_view label) {
  auto& fixture=state.characterIdleSlope;
  auto* actor=fixture.npc?state.npcCharacterRoute.npc:&player;
  if(!fixture.prepared || !actor) {arenaAbort("character idle slope sample needs a prepared fixture");return;}
  const auto position=actor->position();
  const auto displacement=position-fixture.start,interval=position-fixture.previous;
  event("character_idle_slope_sample","\"label\":"+jsonStr(label)+",\"actor\":"+jsonStr(fixture.npc?"npc":"hero")+
    ",\"angle_deg\":"+ragdollNumber(fixture.angle)+",\"elapsed_ms\":"+std::to_string(world.tickCount()-fixture.began)+
    ",\"interval_ms\":"+std::to_string(world.tickCount()-fixture.previousTick)+
    ",\"start_cm\":"+ragdollVector(fixture.start)+",\"displacement_cm\":"+ragdollVector(displacement)+
    ",\"displacement_length_cm\":"+ragdollNumber(displacement.length())+
    ",\"interval_displacement_cm\":"+ragdollVector(interval)+",\"interval_displacement_length_cm\":"+ragdollNumber(interval.length())+
    ","+npcCharacterSample(world,*actor,0,0,context.step,context.frameMs));
  fixture.previous=position;fixture.previousTick=world.tickCount();
  }


void characterIdleSlopePrepare(Character& state,const CharacterContext& context,World& world,Npc& player,float requestedAngle,bool npc) {
  if(!std::isfinite(requestedAngle) || requestedAngle<5.f || requestedAngle>65.f) {
    arenaAbort("character idle slope angle must be between 5 and 65 degrees");return;
    }
  npcCharacterStopMotionTrace(state,&world);
  auto& route=state.npcCharacterRoute;
  if(route.npc) {
    if(context.subject==route.npc)context.subject=nullptr;
    world.removeNpc(*route.npc);
    }
  route=Character::NpcCharacterRoute();state.characterIdleSlope=Character::CharacterIdleSlope();
  state.characterRoute.active=false;
  auto* actor=&player;
  if(npc) {
    route.npc=npcCharacterActor(world,player.position()+Vec3(400,0,0));
    if(!route.npc)return;
    actor=route.npc;
    }
  const auto capsule=actor->navigationCapsule();
  const float radius=capsule.x;
  const auto* bounds=actor->bBoxCol();
  const float height=bounds?std::max(bounds[1].y-bounds[0].y,2.f*radius):capsule.y;
  // Match the native capsule's 1 cm padding. Placing the logical feet directly
  // on a sloping triangle would insert its lower sphere into the ground and
  // count the first depenetration as idle movement. This is an analytic tangent
  // pose, with no simulation tick, settling period or discarded measurement.
  constexpr float padding=1.f;
  const float footprint=std::max(60.f,2.f*(radius+padding));
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {if(!point.underWater)points.push_back(&point);});
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  auto* physics=world.physic();
  Vec3 floor,normal;std::string waypoint;float measuredAngle=0,maximumPlaneError=0,maximumNormalAngle=0;
  const float radians=float(M_PI)/180.f;
  // The waypoint order and offsets are fixed, so separate ON/OFF processes
  // select exactly the same authored landscape triangle.
  for(const auto* point:points) {
    for(float x:{0.f,-80.f,80.f,-160.f,160.f,-240.f,240.f,-400.f,400.f})
      for(float z:{0.f,-80.f,80.f,-160.f,160.f,-240.f,240.f,-400.f,400.f}) {
        if(!waypoint.empty())continue;
        const auto center=physics->landRay(point->position()+Vec3(x,300,z),1200.f);
        // landRay only exposes normals for C_Landscape, which is static stock
        // geometry; zero-normal object tops and movable props are excluded.
        if(!center.hasCol || center.n.length()<.99f || center.n.y<=.25f)continue;
        const auto n=Vec3::normalize(center.n);
        const float angle=std::acos(std::clamp(n.y,-1.f,1.f))/radians;
        const float angleTolerance=requestedAngle>=45.f && requestedAngle<50.f?.75f:2.f;
        if(std::abs(angle-requestedAngle)>angleTolerance)continue;
        float planeError=0,normalAngle=0;bool planar=true;
        for(int ix=-2;ix<=2 && planar;++ix)for(int iz=-2;iz<=2;++iz) {
          const auto offset=Vec3(float(ix)*footprint*.5f,0,float(iz)*footprint*.5f);
          const float y=center.v.y-Vec3::dotProduct(n,offset)/n.y;
          const auto hit=physics->landRay(Vec3(center.v.x+offset.x,y+30.f,center.v.z+offset.z),60.f);
          if(!hit.hasCol || hit.n.length()<.99f) {planar=false;break;}
          const float error=std::abs(Vec3::dotProduct(hit.v-center.v,n));
          const float normalError=std::acos(std::clamp(Vec3::dotProduct(n,Vec3::normalize(hit.n)),-1.f,1.f))/radians;
          if(error>.25f || normalError>1.f) {planar=false;break;}
          planeError=std::max(planeError,error);normalAngle=std::max(normalAngle,normalError);
          }
        if(!planar)continue;
        const auto feet=center.v+Vec3(0,(radius+padding)/n.y-radius-padding,0);
        if(physics->ray(feet+Vec3(0,radius+padding,0),feet+Vec3(0,height+padding,0)).hasCol)continue;
        for(unsigned direction=0;direction<8 && planar;++direction) {
          const float yaw=float(direction)*float(M_PI)/4.f;
          const Vec3 delta(std::cos(yaw)*(radius+padding),0,std::sin(yaw)*(radius+padding));
          for(float y:{radius+padding,height-radius})
            if(physics->ray(feet+Vec3(0,y,0),feet+Vec3(0,y,0)+delta).hasCol) {planar=false;break;}
          }
        if(!planar)continue;
        floor=center.v;normal=n;waypoint=point->name;measuredAngle=angle;
        maximumPlaneError=planeError;maximumNormalAngle=normalAngle;
        }
    if(!waypoint.empty())break;
    }
  if(waypoint.empty()) {arenaAbort("no static planar landscape footprint for idle slope "+std::to_string(requestedAngle));return;}
  auto downhill=Vec3(normal.x,0,normal.z);downhill=Vec3::normalize(downhill);
  // Jolt also translates the shape upward by its padding.
  const float verticalOffset=(radius+padding)/normal.y-radius-padding;
  const auto start=floor+Vec3(0,verticalOffset,0);
  characterFreezeNear(world,player,floor,floor);
  actor->setInteraction(nullptr,true);actor->clearAiQueue();actor->stopWalking();actor->clearSpeed();
  actor->setPosition(start);actor->setDirection(downhill);actor->updateTransform();
  actor->setCharacterVirtualDiagnosticsEnabled(true);
  state.npcCharacterTraceActors.push_back({actor,start,actor->characterVisualPosition(),false});
  if(npc) {
    actor->aiPush(AiQueue::aiWait(60000));
    npcCharacterPlaceHero(world,player,start,start+downhill*200.f);
    context.subject=actor;
    } else context.subject=nullptr;
  auto& fixture=state.characterIdleSlope;
  fixture.prepared=true;fixture.npc=npc;fixture.angle=measuredAngle;
  fixture.began=world.tickCount();fixture.previousTick=world.tickCount();fixture.start=start;fixture.previous=start;
  context.hasPrev=false;
  context.cam=C_Follow;context.camDist=300;context.camHeight=100;context.camAngle=110;context.camAuto=true;
  event("character_idle_slope_setup","\"actor\":"+jsonStr(npc?"npc":"hero")+",\"waypoint\":"+jsonStr(waypoint)+
    ",\"requested_angle_deg\":"+ragdollNumber(requestedAngle)+",\"angle_deg\":"+ragdollNumber(measuredAngle)+
    ",\"floor_cm\":"+ragdollVector(floor)+",\"normal\":"+ragdollVector(normal)+",\"start_cm\":"+ragdollVector(start)+
    ",\"capsule_radius_cm\":"+ragdollNumber(radius)+",\"capsule_height_cm\":"+ragdollNumber(height)+
    ",\"character_padding_cm\":"+ragdollNumber(padding)+",\"vertical_offset_cm\":"+ragdollNumber(verticalOffset)+
    ",\"footprint_half_width_cm\":"+ragdollNumber(footprint)+",\"footprint_samples\":25,\"warmup_ms\":0"+
    ",\"maximum_plane_error_cm\":"+ragdollNumber(maximumPlaneError)+",\"maximum_normal_angle_deg\":"+ragdollNumber(maximumNormalAngle));
  characterIdleSlopeSample(state,context,world,player,"initial");
  }


// Idle regressions use ordinary MoveAlgo ticks, including a real AI wait for
// the NPC. Coordinates change only here; sampling never pins the actor in place.
void characterIdlePrepare(Character& state,const CharacterContext& context,World& world,Npc& player,PlayerControl& ctrl,std::string_view role,std::string_view site) {
  if(role!="hero" && role!="npc") {arenaAbort("character idle requires hero or npc");return;}
  Vec3 start(13881.523f,1672.283f,-15549.843f);
  float yaw=0;
  std::string waypoint="NW_FARM1_PATH_01";
  if(site=="market") {
    start=Vec3(6718.370605f,270.145355f,-6029.716797f);yaw=-89.73f;waypoint="market_void";
    }
  else if(site=="steeper") {
    const auto& guild=world.script().guildVal();
    const float maximum=std::min({32.f,float(guild.slide_angle[player.guild()])-2.f,float(guild.slide_angle[GIL_BDT])-2.f});
    std::vector<const WayPoint*> points;
    world.forEachWayPoint([&](const WayPoint& point) {if(!point.underWater)points.push_back(&point);});
    std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
    bool found=false;
    for(const auto* point:points) {
      for(float x:{0.f,-120.f,120.f,-240.f,240.f})for(float z:{0.f,-120.f,120.f,-240.f,240.f}) {
        if(found)continue;
        Vec3 floor;if(!characterFloor(world,point->position()+Vec3(x,0,z),floor))continue;
        const auto hit=world.physic()->landRay(floor+Vec3(0,20,0),50);
        if(!hit.hasCol || hit.n.y<=0)continue;
        const float normalAngle=std::acos(std::clamp(hit.n.y,0.f,1.f))*180.f/float(M_PI);
        if(normalAngle<18.f || normalAngle>maximum)continue;
        float angle=0,rough=0;Vec3 downhill;
        if(!slopeAt(world,floor,angle,downhill,rough) || angle<18.f || angle>maximum || rough>3.f)continue;
        Vec3 low,high;
        if(!characterFloor(world,floor+downhill*80.f,low) || !characterFloor(world,floor-downhill*80.f,high))continue;
        if(!characterLane(world,low,high,35.f))continue;
        start=floor;waypoint=point->name;found=true;
        }
      if(found)break;
      }
    if(!found) {arenaAbort("character idle steeper slope lacks stock walkable geometry");return;}
    }
  else if(site!="gentle") {arenaAbort("unknown character idle site");return;}
  const auto floor=world.physic()->landRay(start+Vec3(0,20,0),80);
  if(!floor.hasCol || std::abs(floor.v.y-start.y)>20.f) {arenaAbort("character idle start has no expected floor");return;}
  npcCharacterStopMotionTrace(state,&world);
  auto& idle=state.characterIdle;
  context.subject=nullptr;
  if(idle.fixtureNpc) {world.removeNpc(*idle.fixtureNpc);idle.fixtureNpc=nullptr;}
  idle.actor=nullptr;
  ctrl.clearInput();context.holds.clear();context.taps.clear();
  state.characterRoute=Character::CharacterRoute();state.npcCharacterRoute.active=false;
  characterFreezeNear(world,player,start,start);
  player.setInteraction(nullptr,true);player.clearAiQueue();player.clearGoTo();player.clearSpeed();player.stopWalking();player.closeWeapon(true);
  if(role=="npc") {
    npcCharacterPlaceHero(world,player,start,start+Vec3(1,0,0));
    idle.fixtureNpc=world.addNpc(world.script().findSymbolIndex("BDT_1013_BANDIT_L"),start);
    if(!idle.fixtureNpc) {arenaAbort("character idle NPC insertion failed");return;}
    auto& npc=*idle.fixtureNpc;
    npc.setInteraction(nullptr,true);npc.clearAiQueue();npc.clearState(false);npc.clearGoTo();npc.clearSpeed();npc.stopWalking();npc.closeWeapon(true);
    npc.setExternalControl(true);npc.setProcessPolicy(NpcProcessPolicy::AiNormal);npc.aiPush(AiQueue::aiWait(60000));
    idle.actor=&npc;context.subject=&npc;
    }
  else idle.actor=&player;
  idle.actor->setPosition(start);idle.actor->setDirection(yaw);idle.actor->updateTransform();
  idle.site=site;idle.role=role;idle.initial=idle.actor->position();idle.began=world.tickCount();
  idle.actor->setCharacterVirtualDiagnosticsEnabled(true);
  state.npcCharacterTraceActors.push_back({idle.actor,idle.actor->position(),idle.actor->characterVisualPosition(),false});
  context.hasPrev=false;context.cam=C_Follow;context.camDist=350;context.camHeight=110;context.camAngle=110;context.camAuto=true;
  event("character_idle_setup","\"role\":"+jsonStr(role)+",\"site\":"+jsonStr(site)+",\"waypoint\":"+jsonStr(waypoint)+
    ",\"root_cm\":"+ragdollVector(start)+",\"floor_cm\":"+ragdollVector(floor.v)+",\"floor_normal\":"+ragdollVector(floor.n)+
    ",\"slope_deg\":"+ragdollNumber(std::acos(std::clamp(floor.n.y,0.f,1.f))*180.f/float(M_PI))+
    ",\"yaw_deg\":"+ragdollNumber(yaw)+",\"hero_cm\":"+ragdollVector(player.position())+
    ",\"hero_distance_cm\":"+ragdollNumber((player.position()-idle.actor->position()).length()));
  }


// Give and draw a weapon to the prepared idle actor without the draw animation,
// so the hero and an NPC enter the same armed stance on the same frame.
void characterIdleWeapon(Character& state,World& world,std::string_view kind) {
  auto& idle=state.characterIdle;
  if(!idle.actor) {arenaAbort("character idle weapon needs a prepared actor");return;}
  auto& npc=*idle.actor;
  npc.closeWeapon(true);
  const auto item=[&](std::string_view name) {
    const size_t id=world.script().findSymbolIndex(name);
    if(id==size_t(-1)) arenaAbort("character idle weapon: no item "+std::string(name));
    return id;
    };
  if(kind=="fist") npc.setToFistMode();
  else if(kind=="1h" || kind=="2h") {
    const size_t id=item(kind=="1h" ? "ItMw_1h_Common_01" : "ItMw_Zweihaender1");
    if(id==size_t(-1))return;
    npc.setToFightMode(id);
    }
  else if(kind=="bow") {
    const size_t id=item("ItRw_Bow_L_01"),arrows=item("ItRw_Arrow");
    if(id==size_t(-1) || arrows==size_t(-1))return;
    npc.addItem(arrows,50);
    npc.setToBowMode(id);
    }
  else {arenaAbort("character idle weapon expects fist|1h|2h|bow");return;}
  // An NPC held by an AI wait does not re-pick its stand animation by itself.
  npc.setAnim(Npc::Anim::Idle);
  event("character_idle_weapon","\"role\":"+jsonStr(idle.role)+",\"weapon\":"+jsonStr(kind)+
    ",\"weapon_state\":"+std::to_string(int(npc.weaponState()))+",\"body_state\":"+std::to_string(uint32_t(npc.bodyStateMasked())));
  }


void characterIdleSnapshot(Character& state,const CharacterContext& context,World& world,std::string_view label,bool begin) {
  auto& idle=state.characterIdle;
  if(!idle.actor) {arenaAbort("character idle snapshot needs a prepared actor");return;}
  if(begin) {idle.initial=idle.actor->position();idle.began=world.tickCount();}
  const auto delta=idle.actor->position()-idle.initial;
  event("character_idle_snapshot","\"label\":"+jsonStr(label)+",\"role\":"+jsonStr(idle.role)+",\"site\":"+jsonStr(idle.site)+
    ",\"elapsed_ms\":"+std::to_string(world.tickCount()-idle.began)+",\"initial_root_cm\":"+ragdollVector(idle.initial)+
    ",\"drift_cm\":"+ragdollNumber(delta.length())+",\"horizontal_drift_cm\":"+ragdollNumber(std::hypot(delta.x,delta.z))+","+
    npcCharacterSample(world,*idle.actor,0,0,context.step,context.frameMs));
  }


void npcCharacterPrepare(Character& state,const CharacterContext& context,World& world,Npc& player,const std::string& name) {
  auto& route=state.npcCharacterRoute;
  if(route.npc) {world.removeNpc(*route.npc);route.npc=nullptr;}
  if(state.characterItem) {world.removeItem(*state.characterItem);state.characterItem=nullptr;}
  route=Character::NpcCharacterRoute();route.name=name;
  if(name=="stairs") {
    if(!prepareCharacterRoute(state,context,world,player,"stairs_up",true,"NW_CITY_TO_FOREST_STAIRS01"))return;
    } else if(name=="doorway" || name=="tavern") {
    if(!prepareCharacterRoute(state,context,world,player,name))return;
    } else if(name=="market_void") {
    // The target deliberately has no floor. Drive the ordinary AI action
    // toward it; retaining a real supported position is the expected result.
    state.characterRoute=Character::CharacterRoute();
    state.characterRoute.start=Vec3(6718.370605f,270.145355f,-6029.716797f);
    state.characterRoute.goal=Vec3(6754.f,262.f,-6115.f);
    state.characterRoute.waypoint="market_void";
    const auto floor=world.physic()->landRay(state.characterRoute.start+Vec3(0,20,0),80);
    if(!floor.hasCol || std::abs(floor.v.y-state.characterRoute.start.y)>20.f) {
      arenaAbort("NPC market void start has no expected floor");return;
      }
    characterFreezeNear(world,player,state.characterRoute.start,state.characterRoute.goal);
    } else {arenaAbort("unknown NPC CharacterVirtual route: "+name);return;}
  route.start=state.characterRoute.start;route.goal=state.characterRoute.goal;
  route.destination=WayPoint(route.goal,"FP_NPC_CHARACTER_GOAL");route.destination.groundPos=route.goal;
  npcCharacterPlaceHero(world,player,route.start,route.goal);
  route.npc=npcCharacterActor(world,route.start);if(!route.npc)return;
  route.npc->setCharacterVirtualDiagnosticsEnabled(npcCharacterTrace);
  if(name=="market_void")route.npc->setDirection(-89.73f);
  else route.npc->setDirection(route.goal-route.start);
  route.npc->updateTransform();context.subject=route.npc;
  if(name=="tavern") {
    // Use an actual dropped Item in the free lane beside authored furniture.
    // Remove only the general stand's temporary props so ON/OFF starts agree.
    context.movableFixtures.clear();
    auto matrix=Matrix4x4::mkIdentity();matrix.translate((route.start+route.goal)*.5f);
    state.characterItem=world.addItemDyn(world.script().findSymbolIndex("ITFO_BEER"),matrix,size_t(-1));
    if(!state.characterItem) {arenaAbort("NPC tavern real beer mug insertion failed");return;}
    // Item proxies are centred boxes with at least 15 cm half-height. Keep
    // both the proxy and real visual bounds above the floor before settling.
    const auto* bounds=state.characterItem->bBox();
    if(!bounds) {arenaAbort("NPC tavern beer mug has no native visual bounds");return;}
    const float height=std::max({15.f,(bounds[1].y-bounds[0].y)*.5f,-bounds[0].y})+2.f;
    matrix.translate(0,height,0);state.characterItem->setObjMatrix(matrix);
    state.characterItem->setPhysicsEnable(world);
    state.characterPropInitial=state.characterItem->position();state.characterPropTravel=0;
    route.mugMassKg=state.characterItem->physicsMass();route.mugContact=false;
    event("npc_character_mug_setup","\"position_cm\":"+ragdollVector(state.characterPropInitial)+
      ",\"floor_cm\":"+ragdollVector((route.start+route.goal)*.5f)+",\"bounds_min_cm\":"+ragdollVector(bounds[0])+
      ",\"bounds_max_cm\":"+ragdollVector(bounds[1])+",\"mass_kg\":"+ragdollNumber(state.characterItem->physicsMass()));
    }
  context.cam=C_Follow;context.camDist=350;context.camHeight=110;context.camAngle=110;context.camAuto=true;context.hasPrev=false;
  event("npc_character_route_setup","\"route\":"+jsonStr(name)+",\"npc_id\":"+std::to_string(world.npcId(route.npc))+
    ",\"waypoint\":"+jsonStr(state.characterRoute.waypoint)+",\"start_cm\":"+ragdollVector(route.start)+
    ",\"goal_cm\":"+ragdollVector(route.goal)+",\"hero_cm\":"+ragdollVector(player.position()));
  }


void npcCharacterBegin(Character& state,World& world,std::string_view phase) {
  auto& route=state.npcCharacterRoute;
  if(!route.npc) {arenaAbort("NPC character begin requires its fixture actor");return;}
  if(!phase.empty())route.name=phase;
  route.active=true;route.reached=false;route.arrived=0;route.began=world.tickCount();route.progressTime=route.began;
  route.lastPosition=route.npc->position();route.lastVisual=route.npc->characterVisualPosition();route.progressPosition=route.lastPosition;
  route.maxVisualJump=0;route.maxPhysicalJump=0;route.maxAboveFloor=0;route.stalls=0;route.stalled=false;
  // Route evidence includes steep-contact sliding without changing the native
  // human controller. Diagnostics remain transient and owned by this fixture.
  route.supportHistory.clear();route.npc->setCharacterVirtualDiagnosticsEnabled(true);
  state.npcCharacterHasWall=false;
  event("npc_character_route_begin","\"route\":"+jsonStr(route.name)+",\"npc_id\":"+std::to_string(world.npcId(route.npc))+
    ",\"start_cm\":"+ragdollVector(route.npc->position())+",\"goal_cm\":"+ragdollVector(route.goal)+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false"));
  }


void npcCharacterEnd(Character& state,World& world) {
  auto& route=state.npcCharacterRoute;
  if(!route.active || !route.npc) {arenaAbort("NPC character end without active fixture");return;}
  route.active=false;
  std::string mug;
  if(state.characterItem)
    mug=",\"mug_mass_kg\":"+ragdollNumber(route.mugMassKg)+
      ",\"mug_max_displacement_cm\":"+ragdollNumber(state.characterPropTravel)+
      ",\"mug_native_contact\":"+(route.mugContact?"true":"false");
  event("npc_character_route_end","\"route\":"+jsonStr(route.name)+",\"success\":"+(route.reached?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string((route.reached?route.arrived:world.tickCount())-route.began)+
    ",\"window_ms\":"+std::to_string(world.tickCount()-route.began)+",\"start_cm\":"+ragdollVector(route.start)+
    ",\"goal_cm\":"+ragdollVector(route.goal)+",\"end_cm\":"+ragdollVector(route.npc->position())+
    ",\"stalls\":"+std::to_string(route.stalls)+",\"max_visual_jump_cm\":"+ragdollNumber(route.maxVisualJump)+
    ",\"max_physical_jump_cm\":"+ragdollNumber(route.maxPhysicalJump)+",\"max_above_floor_cm\":"+ragdollNumber(route.maxAboveFloor)+
    ",\"sat\":"+(route.sat?"true":"false")+",\"standing\":"+(route.standing?"true":"false")+
    ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false")+mug);
  }


void characterStairPerformance(World& world,Npc& player) {
  if(!Gothic::physicsFlags().characterVirtual || !Gothic::settingsGetI("GAME","npc_character_virtual")) {
    arenaAbort("stair performance requires both human CharacterVirtual flags on");return;
    }
  std::vector<const Npc*> sources{&player};
  for(uint32_t id=0;id<world.npcCount() && sources.size()<11;++id) {
    const auto* npc=world.npcById(id);
    if(npc && npc!=&player && npc->isHuman() && !npc->isDown() && npc->visualSkeleton() &&
       npc->navigationCapsule().x>0.f && npc->navigationCapsule().y>0.f)
      sources.push_back(npc);
    }
  if(sources.size()!=11 || !player.visualSkeleton()) {
    arenaAbort("stair performance needs one loaded hero and ten loaded live human skeletons");return;
    }
  constexpr float radians=float(M_PI)/180.f, angle=55.f*radians, padding=1.f;
  const float cosine=std::cos(angle), tangent=std::tan(angle);
  asset::WorldMesh plane;
  plane.vertices={{-2000,-2000*tangent,-2000},{-2000,-2000*tangent,2000},
                  {2000,2000*tangent,2000},{2000,2000*tangent,-2000}};
  plane.materials.resize(1);plane.materials[0].group=asset::MaterialGroup::STONE;
  // PhysicVbo reverses Gothic source winding into upward-facing native triangles.
  plane.polygons.vertex_indices={0,2,1,0,3,2};plane.polygons.material_indices={0,0};
  DynamicWorld isolated(nullptr,plane);
  struct Actor {
    DynamicWorld::NpcItem body;
    const Npc* source=nullptr;
    float step=0,slope=0,mass=0;
    };
  std::vector<Actor> actors;actors.reserve(11);
  const auto& guild=world.script().guildVal();
  for(size_t index=0;index<sources.size();++index) {
    const auto* source=sources[index];
    auto body=isolated.addGhostObj(source->visualSkeleton());
    const auto capsule=source->navigationCapsule();
    if(std::abs(body.capsuleRadius()-capsule.x)>.01f || std::abs(body.capsuleHeight()-capsule.y)>.01f) {
      arenaAbort("stair performance human skeleton must retain its actual landscape capsule dimensions");return;
      }
    const float step=std::max(float(guild.step_height[source->guild()]),0.f);
    const float slope=float(guild.slide_angle[source->guild()])*radians;
    if(!(slope>0.f && slope<angle && std::isfinite(slope))) {
      arenaAbort("stair performance requires every human to treat 55 degrees as non-walkable");return;
      }
    const Vec3 initial(0,(capsule.x+padding)/cosine-capsule.x-padding,(float(index)-5.f)*150.f);
    body.setPosition(initial);
    const float mass=source->physicsMass(), stepHeight=step>0.f?step:50.f;
    const auto state=body.moveVirtual({},0,stepHeight,slope,mass);
    if(!state.steep || (state.position-initial).length()>.001f) {
      arenaAbort("stair performance must acquire all eleven analytic steep supports without settling");return;
      }
    actors.push_back({std::move(body),source,stepHeight,slope,mass});
    }
  // Flush only zero-dt contact preparation from the production timer. No
  // positive-dt warmup or measured steps are discarded.
  isolated.tick(0);
  struct Sample { uint64_t frame=0,time=0,dt=0;float physics=0,wall=0;unsigned steep=0,grounded=0; };
  std::vector<Sample> samples;samples.reserve(600);
  uint64_t elapsed=0;
  unsigned minimumSteep=11;
  for(uint64_t frame=0;frame<600;++frame) {
    const uint64_t dt=frame%3==0?16:17;
    const Vec3 request(4.f*float(dt)/17.f,0,0);
    unsigned steep=0,grounded=0;
    const auto began=std::chrono::steady_clock::now();
    // All eleven controllers coexist. Direct wrapper requests keep stressing
    // the uphill stair sweeps even though normal MoveAlgo would enter Slide.
    for(auto& actor:actors) {
      const auto state=actor.body.moveVirtual(request,dt,actor.step,actor.slope,actor.mass);
      steep+=state.steep;grounded+=state.grounded;
      }
    isolated.tick(dt);
    const float wall=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-began).count();
    elapsed+=dt;
    samples.push_back({frame,elapsed,dt,isolated.physicsStats().stepMilliseconds,wall,steep,grounded});
    minimumSteep=std::min(minimumSteep,steep);
    }
  auto* file=std::fopen((outDir+"/character-stair-performance.csv").c_str(),"w");
  if(!file) {arenaAbort("cannot write stair performance samples");return;}
  std::fprintf(file,"frame,t_sim_ms,dt_ms,actors,uphill_requests,steep_actors,grounded_actors,step_ms,batch_wall_ms\n");
  std::vector<float> physics,wall;physics.reserve(samples.size());wall.reserve(samples.size());
  for(const auto& sample:samples) {
    std::fprintf(file,"%llu,%llu,%llu,11,11,%u,%u,%.9g,%.9g\n",(unsigned long long)sample.frame,
                 (unsigned long long)sample.time,(unsigned long long)sample.dt,sample.steep,sample.grounded,
                 double(sample.physics),double(sample.wall));
    physics.push_back(sample.physics);wall.push_back(sample.wall);
    }
  std::fclose(file);std::sort(physics.begin(),physics.end());std::sort(wall.begin(),wall.end());
  const auto p95=size_t(std::ceil(.95*double(samples.size())))-1;
  event("character_stair_performance","\"actors\":11,\"hero\":1,\"npcs\":10,\"samples\":"+std::to_string(samples.size())+
    ",\"simulated_ms\":"+std::to_string(elapsed)+",\"angle_deg\":55,\"minimum_steep_actors\":"+std::to_string(minimumSteep)+
    ",\"p95_step_ms\":"+ragdollNumber(physics[p95])+",\"p95_batch_wall_ms\":"+ragdollNumber(wall[p95])+
    ",\"timing_diagnostics\":false,\"source\":\"eleven independent human NpcItem controllers in isolated DynamicWorld\"");
  // Enable tracing only after formal timing. One extra uphill step per actor
  // proves the measured configuration without charging diagnostics to p95.
  for(size_t index=0;index<actors.size();++index) {
    auto& actor=actors[index];actor.body.setVirtualDiagnosticsEnabled(true);
    const auto state=actor.body.moveVirtual({4,0,0},17,actor.step,actor.slope,actor.mass);
    CharacterVirtualDiagnostics diagnostic;
    if(!actor.body.virtualDiagnostics(diagnostic)) {arenaAbort("stair performance coverage diagnostics are unavailable");return;}
    event("character_stair_performance_actor","\"actor\":"+std::to_string(index)+",\"role\":"+jsonStr(index?"npc":"hero")+
      ",\"source_npc_id\":"+std::to_string(world.npcId(actor.source))+",\"source_npc\":"+jsonStr(symName(world,actor.source))+
      ",\"radius_cm\":"+ragdollNumber(actor.body.capsuleRadius())+",\"height_cm\":"+ragdollNumber(actor.body.capsuleHeight())+
      ",\"mass_kg\":"+ragdollNumber(actor.mass)+",\"step_height_cm\":"+ragdollNumber(actor.step)+
      ",\"max_slope_rad\":"+ragdollNumber(actor.slope)+",\"root_cm\":"+ragdollVector(state.position)+
      ",\"ground_normal\":"+ragdollVector(state.groundNormal)+",\"ground_state\":"+std::to_string(diagnostic.afterStairs.groundState)+
      ",\"steep\":"+(state.steep?"true":"false")+",\"stair_attempted\":"+(diagnostic.stairAttempted?"true":"false")+
      ",\"stair_succeeded\":"+(diagnostic.stairSucceeded?"true":"false")+",\"reason\":"+jsonStr(diagnostic.stairProbe.reason));
    }
  if(minimumSteep!=11 || elapsed!=10000)arenaAbort("stair performance lost its eleven continuous steep-slope actors or ten-second window");
  }


void npcCharacterMarket(Character& state,const CharacterContext& context,World& world,Npc& player) {
  const WayPoint* center=world.findPoint("NW_CITY_MERCHANT_PATH_20");
  if(!center) {arenaAbort("NPC character market fixture has no stock market waypoint");return;}
  if(state.npcCharacterRoute.npc) {world.removeNpc(*state.npcCharacterRoute.npc);state.npcCharacterRoute=Character::NpcCharacterRoute();}
  std::vector<Npc*> actors;
  for(uint32_t id=0;id<world.npcCount();++id) {
    auto* npc=world.npcById(id);
    if(npc && npc!=&player && npc->isHuman() && !npc->isDown() && (npc->position()-center->position()).length()<2000.f)
      actors.push_back(npc);
    }
  characterFreezeNear(world,player,center->position(),center->position());
  // Keep precisely twenty live humans inside the measured radius even when
  // the source save has a larger native market population.
  for(size_t i=20;i<actors.size();++i) {
    actors[i]->setPosition(center->position()+Vec3(3000.f+float(i-20)*100.f,0,0));actors[i]->updateTransform();
    }
  std::vector<std::pair<Vec3,Vec3>> lanes;
  for(float radius:{200.f,400.f,600.f,800.f,1000.f,1200.f,1400.f})for(unsigned step=0;step<24 && lanes.size()<20;++step) {
    const float angle=float(step)*float(M_PI)/12.f;
    Vec3 start,end;
    if(!characterFloor(world,center->position()+Vec3(std::cos(angle)*radius,0,std::sin(angle)*radius),start) ||
       std::abs(start.y-center->position().y)>100)continue;
    if(std::any_of(lanes.begin(),lanes.end(),[&](const auto& lane){return (start-lane.first).length()<120.f;}))continue;
    bool found=false;
    for(unsigned direction=0;direction<8;++direction) {
      const float yaw=float(direction)*float(M_PI)/4.f;
      const Vec3 along(std::cos(yaw),0,std::sin(yaw));
      if(characterFloor(world,start+along*280.f,end) && characterLane(world,start,end,20.f) && std::abs(end.y-start.y)<45 &&
         std::none_of(lanes.begin(),lanes.end(),[&](const auto& lane){return (end-lane.first).length()<100.f || (end-lane.second).length()<100.f;})) {found=true;break;}
      }
    if(found)lanes.emplace_back(start,end);
    }
  if(lanes.size()!=20) {arenaAbort("market fixture lacks twenty supported walking lanes");return;}
  player.stopWalking();player.setPosition(center->position());player.updateTransform();
  state.npcCharacterCrowd.clear();
  for(size_t i=0;i<lanes.size();++i) {
    auto* npc=i<actors.size()?actors[i]:npcCharacterActor(world,lanes[i].first);
    if(!npc)return;
    npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
    npc->setProcessPolicy(NpcProcessPolicy::AiNormal);npc->setWalkMode(WalkBit::WM_Run);
    npc->setPosition(lanes[i].first);npc->setDirection(lanes[i].second-lanes[i].first);npc->updateTransform();
    state.npcCharacterCrowd.emplace_back();auto& walker=state.npcCharacterCrowd.back();walker.npc=npc;
    walker.ends[0]=WayPoint(lanes[i].second,"FP_NPC_CHARACTER_MARKET_B");walker.ends[1]=WayPoint(lanes[i].first,"FP_NPC_CHARACTER_MARKET_A");
    for(auto& point:walker.ends)point.groundPos=point.pos;
    }
  context.subject=nullptr;context.cam=C_World;context.camDist=1000;context.camHeight=800;context.camAngle=45;context.camAuto=false;
  event("npc_character_market_setup","\"actors\":20,\"reused_npcs\":"+std::to_string(std::min(size_t(20),actors.size()))+
    ",\"waypoint\":"+jsonStr(center->name)+",\"hero_cm\":"+ragdollVector(player.position()));
  }


void npcCharacterFields(Character& state,const CharacterContext& context,World& world,Npc& player,uint64_t dt) {
  auto& route=state.npcCharacterRoute;
  if(route.npc && !route.active && route.seat) {
    const auto at=route.npc->position();
    const bool supported=!route.npc->isInAir() && !route.npc->isFalling() && std::abs(at.y-route.start.y)<5.f;
    if(!supported || !route.sawPreparationSample || (at-route.preparationSample).length()>1.f)route.supportSince=world.tickCount();
    route.preparationSample=at;route.sawPreparationSample=true;
    }
  if(!route.active && !state.npcCharacterMeasuring && !state.npcCharacterObserving)return;
  unsigned nearby=0,virtualNearby=0,movingNearby=0,movingVirtualNearby=0;
  for(uint32_t id=0;id<world.npcCount();++id) {
    auto* npc=world.npcById(id);
    if(!npc || npc==&player || !npc->isHuman() || npc->isDown())continue;
    const bool near=(npc->position()-player.position()).quadLength()<2000.f*2000.f;
    nearby+=near;virtualNearby+=near && npc->characterVirtualActive();
    const auto goal=npc->navigationGoal();
    movingNearby+=near && goal.active;movingVirtualNearby+=near && goal.active && npc->characterVirtualActive();
    if(!state.npcCharacterObserving || (npc->position()-player.position()).length()>3000.f)continue;
    if(npc->characterVirtualActive())++state.npcCharacterCvFrames;
    auto& prior=state.npcCharacterGoals[npc];
    const bool same=goal.active && prior.active && prior.kind==goal.kind && prior.name==goal.name && prior.npc==goal.npc &&
      (goal.npc!=nullptr || (goal.position-prior.target).length()<10.f);
    if(prior.active && !same) {
      const auto delta=npc->position()-prior.target;
      if(delta.x*delta.x+delta.z*delta.z<=150.f*150.f && std::abs(delta.y)<=200.f) {
        ++state.npcCharacterArrivals;
        event("npc_character_goal_arrive","\"window\":"+jsonStr(state.npcCharacterWindow)+",\"npc_id\":"+std::to_string(id)+
          ",\"goal\":"+jsonStr(prior.name)+",\"goal_cm\":"+ragdollVector(prior.target)+",\"root_cm\":"+ragdollVector(npc->position()));
        }
      }
    prior.active=goal.active;
    if(goal.active) {prior.target=goal.position;prior.npc=goal.npc;prior.kind=goal.kind;prior.name=goal.name;state.npcCharacterWalkers.insert(npc);}
    }
  if(state.npcCharacterObserving) {++state.npcCharacterObservedFrames;state.npcCharacterMaxNearby=std::max(state.npcCharacterMaxNearby,nearby);}
  if(!route.active && !state.npcCharacterMeasuring)return;
  if(state.npcCharacterMeasuring && !state.npcCharacterCrowd.empty()) {
    unsigned close=0;float maximumDistance=0;
    for(const auto& walker:state.npcCharacterCrowd) {
      const float distance=(walker.npc->position()-player.position()).length();
      close+=distance<2000.f;maximumDistance=std::max(maximumDistance,distance);
      }
    state.npcCharacterCrowdMinNearby=std::min(state.npcCharacterCrowdMinNearby,close);
    state.npcCharacterCrowdMaxNearby=std::max(state.npcCharacterCrowdMaxNearby,close);
    state.npcCharacterCrowdMaximumDistance=std::max(state.npcCharacterCrowdMaximumDistance,maximumDistance);
    }
  const auto now=std::chrono::steady_clock::now();
  const float wall=state.npcCharacterHasWall?std::chrono::duration<float,std::milli>(now-state.npcCharacterLastWall).count():0;
  state.npcCharacterLastWall=now;state.npcCharacterHasWall=true;
  bool normal=false;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  normal=AppCadence::normalClock();
#endif
  const auto* npc=route.active?route.npc:nullptr;
  state.npcCharacterFrames.push_back({state.npcCharacterMeasuring?"performance":route.name,
    uint64_t(state.npcCharacterFrames.size()),world.tickCount(),dt,npc?uint64_t(world.npcId(npc)):uint64_t(-1),
    npc?npc->position():player.position(),npc?npc->characterVisualPosition():player.characterVisualPosition(),
    bool(Gothic::settingsGetI("GAME","npc_character_virtual")),npc && npc->characterVirtualActive(),
    npc && npc->navigationGoal().active,npc && npc->isInAir(),npc && npc->interactive(),context.rec && context.recVideo,normal,
    world.physic()->physicsStats().stepMilliseconds,wall,nearby,virtualNearby,movingNearby,movingVirtualNearby,
    npc?characterMoveFrame(*npc):CharacterMoveFrame()});
  if(!route.active || !npc)return;
  const auto position=npc->position(),visual=npc->characterVisualPosition();
  npcCharacterSupportTrace(state,context,world,*npc,position.y-route.lastPosition.y,visual.y-route.lastVisual.y);
  route.maxVisualJump=std::max(route.maxVisualJump,std::abs(visual.y-route.lastVisual.y));
  route.maxPhysicalJump=std::max(route.maxPhysicalJump,std::abs(position.y-route.lastPosition.y));
  route.lastVisual=visual;route.lastPosition=position;
  if(const auto floor=world.physic()->landRay(position+Vec3(0,20,0),600);floor.hasCol)
    route.maxAboveFloor=std::max(route.maxAboveFloor,position.y-floor.v.y);
  if(state.characterItem) {
    state.characterPropTravel=std::max(state.characterPropTravel,(state.characterItem->position()-state.characterPropInitial).length());
    route.mugContact|=state.characterItem->physicsTouchedByNpc();
    route.mugMassKg=std::max(route.mugMassKg,state.characterItem->physicsMass());
    }
  bool arrived=false;
  if(route.seat) {
    if(npc->interactive()!=nullptr && npc->interactive()!=route.seat) {arenaAbort("NPC native AI_UseMob selected a different BENCH");return;}
    route.sat|=npc->interactive()==route.seat && route.seat->stateId()>=1;
    route.standing=route.sat && npc->interactive()==nullptr && npc->isStanding();
    arrived=route.standing;
    } else {
    const auto delta=position-route.goal;
    arrived=delta.x*delta.x+delta.z*delta.z<=70.f*70.f && std::abs(delta.y)<60;
    }
  if(!route.reached && arrived) {
    route.reached=true;route.arrived=world.tickCount();
    event("npc_character_route_arrive","\"route\":"+jsonStr(route.name)+",\"elapsed_ms\":"+std::to_string(route.arrived-route.began)+
      ",\"root_cm\":"+ragdollVector(position));
    }
  if(!route.reached && !route.seat && npc->navigationGoal().active) {
    if((position-route.progressPosition).length()>=10.f) {route.progressPosition=position;route.progressTime=world.tickCount();route.stalled=false;}
    if(world.tickCount()-route.progressTime>=2000 && !route.stalled) {
      ++route.stalls;route.stalled=true;
      event("npc_character_route_stall","\"route\":"+jsonStr(route.name)+",\"root_cm\":"+ragdollVector(position));
      }
    }
  }


bool prepareCharacterRideSupport(Character& state,World& world,Npc& player,bool kinematic,bool rotating,Vec3& top) {
  // The former fixture moved a tavern table along global X, unrelated to the
  // inspected free walking lane. A support test needs clearance for the whole
  // moving hull and the standing actor, including the elevated kinematic top.
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    if(!point.underWater && point.name.starts_with("NW_CITY_"))points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  Vec3 floor,along;std::string waypoint;
  for(const auto* point:points) {
    Vec3 center;if(!characterFloor(world,point->position(),center))continue;
    for(unsigned step=0;step<8 && waypoint.empty();++step) {
      const float yaw=float(step)*float(M_PI)/4.f;
      const Vec3 direction(std::cos(yaw),0,std::sin(yaw)),side(-direction.z,0,direction.x);
      bool clear=true;
      for(float offset:{-120.f,-60.f,0.f,60.f,120.f}) {
        Vec3 previous;
        for(int distance=-120;distance<=480 && clear;distance+=60) {
          Vec3 ground;
          if(!characterFloor(world,center+direction*float(distance)+side*offset,ground) ||
             std::abs(ground.y-center.y)>3.f) {clear=false;break;}
          if(world.physic()->ray(ground+Vec3(0,5,0),ground+Vec3(0,450,0)).hasCol) {clear=false;break;}
          if(distance>-120)
            for(float height:{10.f,60.f,120.f,200.f,300.f,420.f})
              if(world.physic()->ray(previous+Vec3(0,height,0),ground+Vec3(0,height,0)).hasCol)clear=false;
          previous=ground;
          }
        if(!clear)break;
        }
      if(!clear)continue;
      for(uint32_t id=0;id<world.npcCount();++id) {
        const auto* npc=world.npcById(id);if(!npc || npc==&player)continue;
        const auto delta=npc->position()-(center+direction*180.f);
        if(std::abs(Vec3::dotProduct(delta,direction))<420.f && std::abs(Vec3::dotProduct(delta,side))<180.f &&
           std::abs(delta.y)<300.f) {clear=false;break;}
        }
      if(clear) {floor=center;along=direction;waypoint=point->name;}
      }
    if(!waypoint.empty())break;
    }
  if(waypoint.empty()) {arenaAbort("character support requires a clear level 6m city lane and 4.5m headroom");return false;}
  std::string visual;
  for(const auto& row:world.physic()->movableVobSnapshot())if(row.className=="table" && row.mass>=10.f) {visual=row.visual;break;}
  const auto* mesh=Resources::loadMesh(visual);const auto* rule=MovableVobs::classifyVisual(visual);
  if(!mesh || !rule) {arenaAbort("character support stock table unavailable");return false;}
  const auto* bounds=mesh->bbox();
  state.characterRidePivot=(bounds[0]+bounds[1])*.5f;state.characterRidePivot.y=0;
  auto matrix=Matrix4x4::mkIdentity();
  matrix.translate(floor.x-(bounds[0].x+bounds[1].x)*.5f,floor.y-bounds[0].y+.2f+(kinematic?150.f:0.f),
                   floor.z-(bounds[0].z+bounds[1].z)*.5f);
  top=(bounds[0]+bounds[1])*.5f;top.y=bounds[1].y;
  if(rotating) {
    // A noncentral point proves angular carry; the actor remains inside the
    // real tabletop rather than standing over the rotation axis.
    if(bounds[1].x-bounds[0].x>=bounds[1].z-bounds[0].z)top.x+=std::min(40.f,(bounds[1].x-bounds[0].x)*.2f);
    else top.z+=std::min(40.f,(bounds[1].z-bounds[0].z)*.2f);
    }
  matrix.project(top);
  asset::VirtualObject data;data.type=asset::VirtualObjectType::zCVob;data.id=uint32_t(-1);
  data.show_visual=true;data.cd_dynamic=true;data.cd_static=true;
  data.visual=std::make_shared<asset::VisualMesh>();data.visual->type=asset::VisualType::MESH;data.visual->name=visual;
  if(kinematic) {
    state.characterPlatformVisual=std::make_unique<ObjVisual>();state.characterPlatformVisual->setVisual(data,world,false,false);
    state.characterPlatform=std::make_unique<PhysicMesh>(*mesh,*world.physic(),true);
    state.characterPlatformMatrix=matrix;state.characterPlatform->setObjMatrix(matrix);state.characterPlatformVisual->setObjMatrix(matrix);
    }
  else {
    const auto before=world.physic()->movableVobSnapshot();
    auto fixture=std::make_unique<MovableFixture>();fixture->visual.setVisual(data,world,false,false);fixture->visual.setObjMatrix(matrix);
    fixture->body=world.physic()->addMovableVob(*mesh,visual,matrix,rule->massMultiplier,rule->blocksWalking,rule->className,true);
    if(fixture->body.isEmpty() || fixture->body.mass()<10.f) {arenaAbort("character dynamic support requires at least 10kg");return false;}
    fixture->body.setTransformCallback([view=&fixture->visual](const Matrix4x4& pose){view->setObjMatrix(pose);});
    state.characterProp=std::move(fixture);
    for(const auto& row:world.physic()->movableVobSnapshot())
      if(std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==row.id;}))state.characterPropId=row.id;
    }
  state.characterRideDirection=along;
  event("character_support_site","\"waypoint\":"+jsonStr(waypoint)+",\"floor_cm\":"+ragdollVector(floor)+
    ",\"direction\":"+ragdollVector(along)+",\"visual\":"+jsonStr(visual)+
    ",\"bounds_min_cm\":"+ragdollVector(bounds[0])+",\"bounds_max_cm\":"+ragdollVector(bounds[1]));
  return true;
  }


void writeCharacterMoveFrame(std::FILE* file,const CharacterMoveFrame& move) {
  std::fprintf(file,",%d,%llu,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%.9g,%d\n",
    int(move.valid),(unsigned long long)move.sequence,(unsigned long long)move.dt,
    double(move.requested.x),double(move.requested.y),double(move.requested.z),
    double(move.acceptedDelta.x),double(move.acceptedDelta.y),double(move.acceptedDelta.z),
    double(move.advancementRatio),int(move.steepContact),double(move.steepNormal.x),
    double(move.steepNormal.y),double(move.steepNormal.z),int(move.blocked));
  }


bool characterCommand(Character& state,const CharacterContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  const std::string& c = a[0];
  if(c=="npccharacter" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","npc_character_virtual",a[1]=="on");
    event("npc_character_toggle","\"enabled\":"+std::string(a[1]=="on"?"true":"false"));
    }
  else if(c=="npccharacter" && a.size()>=3 && a[1]=="trace") {
    if(a.size()==3 && a[2]=="off") {
      const auto count=state.npcCharacterTraceActors.size();npcCharacterStopMotionTrace(state,&w);
      event("npc_character_trace_end","\"actors\":"+std::to_string(count));
      return true;
      }
    std::vector<Npc*> selected;
    for(size_t i=2;i<a.size();++i) {
      const bool numeric=!a[i].empty() && std::all_of(a[i].begin(),a[i].end(),[](char ch){return ch>='0' && ch<='9';});
      auto* npc=findNpc(w,pl,numeric?"id:"+a[i]:a[i]);
      if(!npc) {arenaAbort("NPC character trace has no existing actor: "+a[i]);return true;}
      if(std::find(selected.begin(),selected.end(),npc)==selected.end())selected.push_back(npc);
      }
    npcCharacterStopMotionTrace(state,&w);
    std::string actors="[";
    for(auto* npc:selected) {
      npc->setCharacterVirtualDiagnosticsEnabled(true);
      state.npcCharacterTraceActors.push_back({npc,npc->position(),npc->characterVisualPosition(),false});
      if(actors.size()>1)actors+=',';
      actors+="{\"npc_id\":"+std::to_string(w.npcId(npc))+",\"npc\":"+jsonStr(symName(w,npc))+
        ",\"root_cm\":"+ragdollVector(npc->position())+"}";
      }
    event("npc_character_trace_begin","\"actors\":"+actors+"]");
    }
  else if(c=="npccharacter" && a.size()==3 && a[1]=="route") {
    npcCharacterPrepare(state,context,w,pl,a[2]);
    if(state.npcCharacterRoute.npc)exec("watch watched",w,pl,ctrl);
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="begin") {
    auto& route=state.npcCharacterRoute;
    if(!route.npc) {arenaAbort("NPC character begin needs a prepared route");return true;}
    if(state.characterItem) {
      const auto position=state.characterItem->position(),velocity=state.characterItem->physicsVelocity();
      auto along=route.goal-route.start;along.y=0;along=Vec3::normalize(along);
      const auto delta=position-(route.start+route.goal)*.5f;
      const float side=std::abs(delta.x*along.z-delta.z*along.x),forward=std::abs(Vec3::dotProduct(delta,along));
      const bool ready=side<45.f && forward<60.f && velocity.length()<5.f && delta.y>=0 && delta.y<60.f;
      event("npc_character_mug_prebegin","\"position_cm\":"+ragdollVector(position)+",\"velocity_cm_s\":"+ragdollVector(velocity)+
        ",\"settling_displacement_cm\":"+ragdollNumber((position-state.characterPropInitial).length())+
        ",\"lane_side_cm\":"+ragdollNumber(side)+",\"lane_forward_cm\":"+ragdollNumber(forward)+",\"ready\":"+(ready?"true":"false"));
      if(!ready) {arenaAbort("NPC tavern mug did not settle inside its route lane");return true;}
      state.characterPropInitial=position;state.characterPropTravel=0;
      }
    route.npc->clearAiQueue();route.npc->stopWalking();route.npc->setPosition(route.start);route.npc->updateTransform();
    npcCharacterBegin(state,w);
    route.npc->aiPush(AiQueue::aiGoToPoint(route.destination));route.npc->aiPush(AiQueue::aiWait(60000));
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="end") npcCharacterEnd(state,w);
  else if(c=="npccharacter" && (a.size()==2 || a.size()==3) && a[1]=="seat") {
    auto& route=state.npcCharacterRoute;
    if(route.npc) {w.removeNpc(*route.npc);route.npc=nullptr;}
    if(state.characterItem) {w.removeItem(*state.characterItem);state.characterItem=nullptr;}
    route=Character::NpcCharacterRoute();route.name="seat";
    const WayPoint* requestedAnchor=a.size()==3?w.findPoint(a[2],false):nullptr;
    if(a.size()==3 && !requestedAnchor) {arenaAbort("NPC character seat requested an unknown exact waypoint");return true;}
    struct Candidate {Interactive* mob=nullptr;const WayPoint* waypoint=nullptr;uint32_t id=0;unsigned region=0;float distance=0;};
    std::vector<Candidate> candidates;
    Npc* coragon=nullptr;
    for(uint32_t id=0;id<w.npcCount();++id)
      if(auto* npc=w.npcById(id);npc && symName(w,npc)=="VLK_420_CORAGON") {coragon=npc;break;}
    for(uint32_t id=0;;++id) {
      auto* mob=w.mobsiById(id);if(!mob)break;
      if(mob->schemeName()!="BENCH")continue;
      const auto* waypoint=w.findWayPoint(mob->position());
      if(requestedAnchor && (!waypoint || waypoint->name!=requestedAnchor->name))continue;
      unsigned region=4;
      if(waypoint) {
        const auto& name=waypoint->name;
        const bool harbour=name.starts_with("NW_CITY_") &&
          (name.find("HABOUR")!=std::string::npos || name.find("HARBOUR")!=std::string::npos || name.find("HARBOR")!=std::string::npos);
        region=harbour?0:(name.starts_with("NW_BIGFARM") || name.starts_with("NW_FARM")?1:name.starts_with("NW_CITY_")?3:2);
        }
      candidates.push_back({mob,waypoint,id,region,waypoint?(waypoint->position()-mob->position()).length():1e10f});
      }
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b) {
      if(a.region!=b.region)return a.region<b.region;
      if(a.distance!=b.distance)return a.distance<b.distance;
      return a.id<b.id;
      });
    // An explicit waypoint pins one authored BENCH by geometry. Occupancy or
    // failed native preconditions must not silently select a different seat.
    if(requestedAnchor && candidates.size()>1)candidates.resize(1);
    event("npc_character_seat_candidates","\"count\":"+std::to_string(candidates.size())+
      ",\"requested_waypoint\":"+jsonStr(requestedAnchor?requestedAnchor->name:std::string()));
    Vec3 socket,camera,focus;std::string anchorName;bool selected=false;
    uint32_t selectedId=uint32_t(-1);
    for(const auto& candidate:candidates) {
      auto* mob=candidate.mob;
      const auto candidateEvent=[&](std::string_view reason) {
        event("npc_character_seat_candidate","\"mob_id\":"+std::to_string(candidate.id)+",\"scheme\":"+jsonStr(mob->schemeName())+
          ",\"vob\":"+jsonStr(mob->vobNameStr())+",\"mob_cm\":"+ragdollVector(mob->position())+
          ",\"waypoint\":"+jsonStr(candidate.waypoint?candidate.waypoint->name:std::string())+
          ",\"available\":"+(mob->isAvailable()?"true":"false")+",\"state\":"+std::to_string(mob->stateId())+
          ",\"reason\":"+jsonStr(reason));
        };
      if(!mob->isAvailable() || mob->stateId()>0) {candidateEvent("occupied_or_active");continue;}
      if(coragon && (mob->position()-coragon->position()).length()<1500.f) {candidateEvent("coragon_bar_excluded");continue;}
      if(!candidate.waypoint || candidate.distance>1000.f) {candidateEvent("no_nearby_authored_waypoint");continue;}
      const auto reference=w.physic()->landRay(candidate.waypoint->position()+Vec3(0,80,0),200.f);
      if(!reference.hasCol) {candidateEvent("waypoint_has_no_supported_floor");continue;}
      const float floorReference=reference.v.y;
      if(std::abs(mob->position().y-floorReference)>200.f) {candidateEvent("floor_in_different_storey");continue;}
      if(!route.npc)route.npc=npcCharacterActor(w,reference.v);
      if(!route.npc)return true;
      route.npc->setPosition(reference.v);route.npc->updateTransform();
      socket=mob->nearestPoint(*route.npc);
      bool prepared=false;
      // A native animation socket is not an approach point. Find ordinary
      // supported floor for a real GoToPoint, within UseMob's 165 cm range.
      for(float distance:{240.f,280.f,320.f})for(unsigned step=0;step<24 && !prepared;++step) {
        const float yaw=float(step)*float(M_PI)/12.f;
        auto at=socket+Vec3(std::cos(yaw)*distance,0,std::sin(yaw)*distance);
        const auto floor=w.physic()->landRay(Vec3(at.x,floorReference+50.f,at.z),150.f);
        if(!floor.hasCol || std::abs(floor.v.y-floorReference)>20.f)continue;
        at.y=floor.v.y;route.npc->setPosition(at);route.npc->updateTransform();
        if(route.npc->hasCollision() || w.availableMob(*route.npc,"BENCH")!=mob)continue;
        auto toward=socket-at;toward.y=0;toward=Vec3::normalize(toward);
        const auto approachEnd=socket-toward*120.f;
        auto previous=at;bool lane=true;
        for(unsigned sample=1;sample<=12;++sample) {
          auto point=at+(approachEnd-at)*(float(sample)/12.f);
          const auto support=w.physic()->landRay(Vec3(point.x,floorReference+50.f,point.z),150.f);
          if(!support.hasCol || std::abs(support.v.y-at.y)>20.f) {lane=false;break;}
          point.y=support.v.y;
          if(w.physic()->ray(previous+Vec3(0,100,0),point+Vec3(0,100,0)).hasCol) {lane=false;break;}
          previous=point;
          }
        if(!lane)continue;
        route.npc->setPosition(previous);route.npc->updateTransform();
        const auto usePosition=mob->nearestPoint(*route.npc);
        if(route.npc->hasCollision() || (route.npc->centerPosition()-usePosition).length()>140.f ||
           w.availableMob(*route.npc,"BENCH")!=mob)continue;
        route.destination=WayPoint(previous,"FP_NPC_CHARACTER_SEAT_APPROACH");route.destination.groundPos=previous;
        route.start=at;prepared=true;
        }
      if(!prepared) {candidateEvent("no_supported_collision_free_approach");continue;}
      // Check one fixed camera against both the approach and the actual
      // seated/standing socket area, rather than hiding room geometry.
      focus=(route.start+socket)*.5f;focus.y=floorReference+130.f;
      const Vec3 targets[]{route.start+Vec3(0,10,0),route.start+Vec3(0,130,0),
                           route.destination.position()+Vec3(0,10,0),route.destination.position()+Vec3(0,130,0),
                           Vec3(socket.x,floorReference+50.f,socket.z),socket+Vec3(0,60,0),socket+Vec3(0,150,0)};
      bool visible=false;
      for(float distance:{450.f,550.f,350.f})for(float height:{170.f,140.f,220.f})for(unsigned step=0;step<48 && !visible;++step) {
        const float yaw=float(step)*float(M_PI)/24.f;
        const Vec3 at(focus.x+std::cos(yaw)*distance,floorReference+height,focus.z+std::sin(yaw)*distance);
        bool clear=true;
        for(const auto& target:targets)if(hidden(w,at,target)) {clear=false;break;}
        if(clear) {camera=at;visible=true;}
        }
      if(!visible) {candidateEvent("no_clear_fixed_view");continue;}
      route.seat=mob;route.goal=socket;anchorName=candidate.waypoint->name;selectedId=candidate.id;
      selected=true;candidateEvent("selected");break;
      }
    if(!selected) {arenaAbort("no free authored BENCH has a supported native approach and clear fixed view");return true;}
    characterFreezeNear(w,pl,route.start,route.goal);
    route.npc->stopWalking();route.npc->setPosition(route.start);route.npc->setDirection(route.goal-route.start);route.npc->updateTransform();
    route.npc->setExternalControl(true);route.npc->aiPush(AiQueue::aiWait(60000));
    route.preparationSample=route.start;route.sawPreparationSample=true;route.supportSince=w.tickCount();
    context.subject=route.npc;npcCharacterPlaceHero(w,pl,route.start,route.goal);
    context.cam=C_Hold;context.camAuto=false;lookFrom(camera,focus);
    event("npc_character_seat_setup","\"scheme\":"+jsonStr(route.seat->schemeName())+",\"vob\":"+jsonStr(route.seat->vobNameStr())+
      ",\"mob_id\":"+std::to_string(selectedId)+",\"anchor\":"+jsonStr(anchorName)+",\"mob_cm\":"+ragdollVector(route.seat->position())+
      ",\"socket_cm\":"+ragdollVector(socket)+",\"start_cm\":"+ragdollVector(route.start)+
      ",\"approach_cm\":"+ragdollVector(route.destination.position())+",\"camera_cm\":"+ragdollVector(camera)+
      ",\"camera_target_cm\":"+ragdollVector(focus)+",\"camera_clear\":true");
    exec("watch watched",w,pl,ctrl);
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="sit") {
    auto& route=state.npcCharacterRoute;
    if(!route.npc || !route.seat) {arenaAbort("NPC character sit needs its BENCH fixture");return true;}
    const bool stable=route.sawPreparationSample && w.tickCount()-route.supportSince>=300 &&
      !route.npc->isInAir() && !route.npc->isFalling() && (route.npc->position()-route.start).length()<5.f;
    event("npc_character_seat_prebegin","\"ready\":"+std::string(stable?"true":"false")+
      ",\"root_cm\":"+ragdollVector(route.npc->position())+",\"supported_ms\":"+std::to_string(w.tickCount()-route.supportSince)+
      ",\"selected_vob\":"+jsonStr(route.seat->vobNameStr()));
    if(!stable) {arenaAbort("NPC character seat approach did not settle supported before sit");return true;}
    npcCharacterBegin(state,w);
    route.npc->clearAiQueue();
    // Scripted routines normally approach a grounded waypoint before UseMob.
    // A direct far UseMob chases the elevated animation socket to a 20 cm
    // position-goal, which can send the legacy actor onto adjacent furniture.
    route.npc->aiPush(AiQueue::aiGoToPoint(route.destination));
    route.npc->aiPush(AiQueue::aiUseMob(route.seat->schemeName(),1));route.npc->aiPush(AiQueue::aiWait(60000));
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="stand") {
    auto& route=state.npcCharacterRoute;
    if(!route.npc || !route.seat || !route.sat) {arenaAbort("NPC character did not reach real sitting state before stand");return true;}
    route.npc->clearAiQueue();route.npc->aiPush(AiQueue::aiUseMob(route.seat->schemeName(),-1));route.npc->aiPush(AiQueue::aiWait(60000));
    event("npc_character_stand","\"root_cm\":"+ragdollVector(route.npc->position()));
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="market") npcCharacterMarket(state,context,w,pl);
  else if(c=="npccharacter" && a.size()>=3 && a[1]=="observe" && a[2]=="begin") {
    state.npcCharacterObserving=true;state.npcCharacterWindow=a.size()>3?a[3]:"window";
    state.npcCharacterGoals.clear();state.npcCharacterWalkers.clear();state.npcCharacterArrivals=0;
    state.npcCharacterObservedFrames=0;state.npcCharacterCvFrames=0;state.npcCharacterMaxNearby=0;state.npcCharacterObserveStart=w.tickCount();
    event("npc_character_observe_begin","\"window\":"+jsonStr(state.npcCharacterWindow)+",\"radius_cm\":3000,\"humans_only\":true,\"cv_enabled\":"+
      (Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false"));
    }
  else if(c=="npccharacter" && a.size()==3 && a[1]=="observe" && a[2]=="end") {
    state.npcCharacterObserving=false;
    event("npc_character_observe_end","\"window\":"+jsonStr(state.npcCharacterWindow)+",\"window_ms\":"+std::to_string(w.tickCount()-state.npcCharacterObserveStart)+
      ",\"radius_cm\":3000,\"humans_only\":true,\"unique_walkers\":"+std::to_string(state.npcCharacterWalkers.size())+
      ",\"reached_goals\":"+std::to_string(state.npcCharacterArrivals)+",\"frames\":"+std::to_string(state.npcCharacterObservedFrames)+
      ",\"cv_actor_frames\":"+std::to_string(state.npcCharacterCvFrames)+",\"max_nearby_humans\":"+std::to_string(state.npcCharacterMaxNearby)+
      ",\"cv_enabled\":"+(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false"));
    }
  else if(c=="npccharacter" && a.size()==3 && a[1]=="snapshot") npcCharacterSnapshot(w,pl,a[2]);
  else if(c=="npccharacter" && a.size()==3 && a[1]=="distance") {
    const auto* npc=state.npcCharacterRoute.npc;
    if(!npc) {arenaAbort("NPC character distance needs a fixture actor");return true;}
    pl.setPosition(npc->centerPosition()+Vec3(num(a,2,400),0,0)-(pl.centerPosition()-pl.position()));pl.updateTransform();
    event("npc_character_distance","\"distance_cm\":"+ragdollNumber((npc->centerPosition()-pl.centerPosition()).length())+
      ",\"npc_cm\":"+ragdollVector(npc->position()));
    }
  else if(c=="npccharacter" && a.size()==2 && (a[1]=="teleport" || a[1]=="setposition")) {
    auto& route=state.npcCharacterRoute;
    if(!route.npc) {arenaAbort("NPC character relocation needs its fixture actor");return true;}
    route.npc->setInteraction(nullptr,true);route.npc->clearAiQueue();route.npc->stopWalking();
    const auto target=a[1]=="teleport"?route.start:route.goal;
    if(a[1]=="teleport") {
      route.destination=WayPoint(target,"FP_NPC_CHARACTER_TELEPORT");route.destination.groundPos=target;
      route.npc->aiPush(AiQueue::aiTeleport(route.destination));
      } else {route.npc->setPosition(target);route.npc->updateTransform();}
    route.npc->aiPush(AiQueue::aiWait(60000));
    event("npc_character_relocate","\"kind\":"+jsonStr(a[1])+",\"target_cm\":"+ragdollVector(target));
    }
  else if(c=="npccharacter" && a.size()==3 && a[1]=="save") {
    if(a[2].empty() || a[2].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) {
      arenaAbort("invalid NPC character save label");return true;
      }
    try {
      npcCharacterSnapshot(w,pl,"saved");std::vector<uint8_t> bytes;
      {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
      Gothic::inst().gameSession()->save(out,"npc character "+a[2],preview);}
      std::ofstream file(outDir+"/session-"+a[2]+".sav",std::ios::binary);
      file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
      if(!file)throw std::runtime_error("cannot write complete NPC CharacterVirtual session save");
      event("npc_character_save","\"archive\":"+jsonStr("session-"+a[2]+".sav")+",\"bytes\":"+std::to_string(bytes.size()));
      } catch(const std::exception& error) {arenaAbort(error.what());}
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="npccharacter" && a.size()==3 && a[1]=="measure") {
    if(!AppCadence::normalClock() || !AppCadence::arm(a[2]=="on")) {arenaAbort("NPC character normal cadence arm failed");return true;}
    state.npcCharacterMeasuring=true;state.npcCharacterHasWall=false;
    state.npcCharacterCrowdMinNearby=20;state.npcCharacterCrowdMaxNearby=0;state.npcCharacterCrowdMaximumDistance=0;
    AppCadence::counts(true,uint32_t(state.npcCharacterCrowd.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("npc_character_measure_begin","\"cv_enabled\":"+std::string(Gothic::settingsGetI("GAME","npc_character_virtual")?"true":"false"));
    }
  else if(c=="npccharacter" && a.size()==2 && a[1]=="report") {
    state.npcCharacterMeasuring=false;
    AppCadence::counts(false,uint32_t(state.npcCharacterCrowd.size()),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("npc_character_measure_end","\"frames\":"+std::to_string(state.npcCharacterFrames.size())+
      ",\"fixture_actors\":"+std::to_string(state.npcCharacterCrowd.size())+",\"fixture_min_nearby\":"+std::to_string(state.npcCharacterCrowdMinNearby)+
      ",\"fixture_max_nearby\":"+std::to_string(state.npcCharacterCrowdMaxNearby)+",\"fixture_max_distance_cm\":"+ragdollNumber(state.npcCharacterCrowdMaximumDistance));
    }
#endif
  else if(c=="character" && (a.size()==3 || a.size()==4) && a[1]=="idle" && (a[2]=="hero" || a[2]=="npc"))
    characterIdlePrepare(state,context,w,pl,ctrl,a[2],a.size()==4?a[3]:"gentle");
  else if(c=="character" && a.size()==3 && a[1]=="idle" && a[2]=="begin") characterIdleSnapshot(state,context,w,"begin",true);
  else if(c=="character" && a.size()==4 && a[1]=="idle" && a[2]=="weapon") characterIdleWeapon(state,w,a[3]);
  else if(c=="character" && a.size()==4 && a[1]=="idle" && a[2]=="snapshot") characterIdleSnapshot(state,context,w,a[3]);
  else if(c=="character" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","character_virtual",a[1]=="on");
    event("character_toggle","\"enabled\":"+std::string(a[1]=="on"?"true":"false"));
    }
  else if(c=="character" && a.size()==2 && a[1]=="probe") {
    size_t prepared=0;
    for(const std::string route:{"stairs_up","stairs_down","steep_slope","port_edge","doorway","tavern","jump_ledge","water_entry"})
      prepared+=prepareCharacterRoute(state,context,w,pl,route,false);
    event("character_probe_complete","\"prepared\":"+std::to_string(prepared)+",\"expected\":8");
    }
  else if(c=="character" && a.size()==3 && a[1]=="route") prepareCharacterRoute(state,context,w,pl,a[2]);
  else if(c=="character" && a.size()==2 && a[1]=="stair-performance") characterStairPerformance(w,pl);
  else if(c=="character" && a.size()==4 && a[1]=="idle-slope") {
    if(a[3]!="hero" && a[3]!="npc") {arenaAbort("character idle-slope expects hero or npc");return true;}
    characterIdleSlopePrepare(state,context,w,pl,num(a,2,0),a[3]=="npc");
    }
  else if(c=="character" && a.size()==3 && a[1]=="idle-slope-sample") characterIdleSlopeSample(state,context,w,pl,a[2]);
  else if(c=="character" && a.size()==6 && a[1]=="approach") {
    auto& route=state.characterRoute;
    if(!route.prepared || route.active) {arenaAbort("character approach needs a prepared route");return true;}
    const Vec3 side(-route.direction.z,0,route.direction.x);
    const auto shifted=route.start+side*num(a,3,0)+route.direction*num(a,4,0);
    Vec3 floor;
    if(!characterFloor(w,shifted,floor)) {arenaAbort("character approach has no floor");return true;}
    route.start=floor;route.name+='_'+a[2];
    pl.setPosition(route.start);pl.setDirection(route.direction);
    const float yaw=(pl.rotation()+num(a,5,0))*float(M_PI)/180.f;
    pl.setDirection(Vec3(std::cos(yaw),0,std::sin(yaw)));pl.updateTransform();
    event("character_approach","\"route\":"+jsonStr(route.name)+",\"start_cm\":"+ragdollVector(route.start)+
      ",\"goal_cm\":"+ragdollVector(route.goal)+",\"yaw_deg\":"+ragdollNumber(pl.rotation()));
    }
  else if(c=="character" && a.size()==2 && a[1]=="begin") {
    auto& route=state.characterRoute;
    if(!route.prepared || route.active) {arenaAbort("character begin needs a prepared route");return true;}
    route.active=true;route.reached=false;route.sawSwim=false;route.sawSlide=false;route.fallCount=0;route.falling=false;
    // Idle settling can already slide on the steep route. Start held input
    // from the same prepared coordinate in both modes after the camera settles.
    pl.setPosition(route.start);pl.updateTransform();
    route.began=w.tickCount();route.lastSupported=route.start;
    state.characterHasWall=false;
    event("character_route_begin","\"route\":"+jsonStr(route.name)+",\"start_cm\":"+ragdollVector(route.start)+
      ",\"visual_start_cm\":"+ragdollVector(pl.characterVisualPosition())+
      ",\"goal_cm\":"+ragdollVector(route.goal)+",\"tolerance_cm\":"+ragdollNumber(route.tolerance)+
      ",\"cv_enabled\":"+(Gothic::physicsFlags().characterVirtual?"true":"false"));
    }
  else if(c=="character" && a.size()==2 && a[1]=="end") characterRouteEnd(state,w,pl);
  else if(c=="character" && a.size()==3 && a[1]=="snapshot") characterSnapshot(w,pl,a[2]);
  else if(c=="character" && a.size()==3 && a[1]=="save") {
    if(a[2].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) {
      arenaAbort("invalid character save label");return true;
      }
    try {
      characterSnapshot(w,pl,"saved");
      std::vector<uint8_t> bytes;
      {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
      Gothic::inst().gameSession()->save(out,"character "+a[2],preview);}
      std::ofstream file(outDir+"/session-"+a[2]+".sav",std::ios::binary);
      file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
      if(!file)throw std::runtime_error("cannot write complete CharacterVirtual save");
      event("character_save","\"archive\":"+jsonStr("session-"+a[2]+".sav")+",\"bytes\":"+std::to_string(bytes.size()));
      } catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="character" && a.size()==3 && a[1]=="prop") prepareCharacterProp(state,context,w,pl,a[2]);
  else if(c=="character" && a.size()==2 && a[1]=="prop-checkpoint") characterPropCheckpoint(state,w,pl);
  else if(c=="character" && a.size()==2 && a[1]=="cleanup") {
    if(state.characterItem)w.removeItem(*state.characterItem);
    state.characterItem=nullptr;state.characterProp.reset();state.characterPropId=0;state.characterPropRole.clear();
    state.characterPlatform.reset();state.characterPlatformVisual.reset();state.characterMover=nullptr;
    if(state.characterRideNpc) {
      if(context.subject==state.characterRideNpc)context.subject=nullptr;
      w.removeNpc(*state.characterRideNpc);state.characterRideNpc=nullptr;
      }
    state.characterRiding=false;state.characterRideMoving=false;state.characterRideRotating=false;
    }
  else if(c=="character" && (a.size()==3 || a.size()==4) && a[1]=="ride") {
    if(a.size()==4 && a[3]!="npc") {arenaAbort("character ride rider must be npc");return true;}
    if(state.characterRideNpc || state.characterPlatform || state.characterProp) {arenaAbort("character ride needs cleanup before another support");return true;}
    state.characterRiding=false;state.characterRideMoving=false;state.characterMover=nullptr;
    state.characterRideRotating=a[2]=="rotation";
    state.characterRideDirection=Vec3(1,0,0);
    state.characterRideAngle=0;state.characterRideTravel=0;
    state.characterRideMaximumDrift=0;state.characterRideMaximumHorizontalDrift=0;state.characterRideMaximumLocalDrift=0;
    Vec3 top;
    if(a[2]=="mover") {
      std::vector<MoveTrigger*> movers;
      w.detectMover([&](MoveTrigger& mover){movers.push_back(&mover);});
      std::sort(movers.begin(),movers.end(),[](const auto* a,const auto* b){return a->name()<b->name();});
      bool selected=false;
      for(auto* mover:movers) {
        const auto* mesh=Resources::loadMesh(mover->visualNameView());
        if(!mesh || mover->keyFrameCount()<2)continue;
        const auto* bounds=mesh->bbox();
        auto localTop=(bounds[0]+bounds[1])*.5f;localTop.y=bounds[1].y;
        auto supported=localTop;mover->transform().project(supported);
        const auto ground=w.physic()->landRay(supported+Vec3(0,25,0),60);
        bool eligible=bounds[1].x-bounds[0].x>=100 && bounds[1].z-bounds[0].z>=100 &&
          ground.hasCol && std::abs(ground.v.y-supported.y)<15;
        // The legacy query deliberately clears C_Object normals. Compare five
        // actual surface heights instead of treating that zero normal as slope.
        for(const auto delta:{Vec3(-35,0,0),Vec3(35,0,0),Vec3(0,0,-35),Vec3(0,0,35)}) {
          const auto corner=w.physic()->landRay(supported+delta+Vec3(0,25,0),60);
          eligible&=corner.hasCol && std::abs(corner.v.y-supported.y)<15;
          }
        event("character_mover_candidate","\"name\":"+jsonStr(mover->name())+",\"visual\":"+jsonStr(mover->visualNameView())+
          ",\"frames\":"+std::to_string(mover->keyFrameCount())+",\"standable\":"+(eligible?"true":"false")+
          ",\"root_cm\":"+ragdollVector(mover->position())+",\"top_cm\":"+ragdollVector(supported));
        if(eligible && !selected) {state.characterMover=mover;top=ground.v;selected=true;}
        }
      if(!selected) {event("character_mover_unavailable","\"movers\":"+std::to_string(movers.size())+
        ",\"reason\":\"no mover has an exposed supported horizontal top at least 1m wide\"");return true;}
      }
    else if(a[2]=="dynamic" || a[2]=="kinematic" || a[2]=="rotation") {
      if(!prepareCharacterRideSupport(state,w,pl,a[2]!="dynamic",state.characterRideRotating,top))return true;
      }
    else {arenaAbort("unknown character ride support: "+a[2]);return true;}
    characterFreezeNear(w,pl,top,top+state.characterRideDirection*400.f);
    if(a.size()==4) {
      npcCharacterPlaceHero(w,pl,top,top+state.characterRideDirection*400.f);
      state.characterRideNpc=npcCharacterActor(w,top+Vec3(0,1,0));if(!state.characterRideNpc)return true;
      context.subject=state.characterRideNpc;
      }
    else {pl.stopWalking();pl.clearAiQueue();pl.setPosition(top+Vec3(0,1,0));pl.updateTransform();}
    const auto& rider=state.characterRideNpc?*state.characterRideNpc:pl;
    state.characterRideMatrixStart=characterSupportMatrix(state,w);state.characterRideStart=movablePosition(state.characterRideMatrixStart);
    auto inverse=state.characterRideMatrixStart;inverse.inverse();state.characterRideLocalStart=rider.position();inverse.project(state.characterRideLocalStart);
    state.characterRideBegan=w.tickCount();state.characterPropRole.clear();
    state.characterRideHeroStart=pl.position();state.characterHasWall=false;state.characterRiding=true;
    characterRideCheckpoint(state,w,pl,true);
    }
  else if(c=="character" && a.size()==2 && a[1]=="ride-start") {
    if(!state.characterRiding)return true; // explicit unavailable mover event already identifies missing coverage
    // The setup already selects Idle through stopWalking(). Animation blending
    // and native contact recovery can still settle during its 700ms wait; begin
    // the carry measurement from the actual pose immediately before movement.
    const auto& rider=state.characterRideNpc?*state.characterRideNpc:pl;
    state.characterRideMatrixStart=characterSupportMatrix(state,w);state.characterRideStart=movablePosition(state.characterRideMatrixStart);
    auto inverse=state.characterRideMatrixStart;inverse.inverse();state.characterRideLocalStart=rider.position();inverse.project(state.characterRideLocalStart);
    state.characterRideBegan=w.tickCount();
    state.characterRideMaximumDrift=0;state.characterRideMaximumHorizontalDrift=0;state.characterRideMaximumLocalDrift=0;
    state.characterRideHeroStart=pl.position();
    characterRideCheckpoint(state,w,pl);
    state.characterRideMoving=true;
    if(state.characterMover)w.triggerEvent(TriggerEvent(std::string(state.characterMover->name()),"CHARACTER_BENCH",TriggerEvent::T_Trigger));
    else if(state.characterProp) {
      const auto impulse=state.characterRideDirection*(5.f*state.characterProp->body.mass());
      const bool accepted=state.characterProp->body.addImpulse(impulse,&pl);
      event("character_ride_impulse","\"mass_kg\":"+ragdollNumber(state.characterProp->body.mass())+
        ",\"impulse_ns\":"+ragdollVector(impulse)+",\"accepted\":"+(accepted?"true":"false")+
        ",\"velocity_cm_s\":"+ragdollVector(state.characterProp->body.velocity()));
      }
    }
  else if(c=="character" && a.size()==2 && a[1]=="ride-checkpoint") {
    if(state.characterRiding)characterRideCheckpoint(state,w,pl);
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="character" && a.size()==3 && a[1]=="measure") {
    if(!AppCadence::normalClock() || !AppCadence::arm(a[2]=="on")) {arenaAbort("character normal cadence arm failed");return true;}
    state.characterMeasuring=true;state.characterHasWall=false;state.characterMeasureStart=w.tickCount();
    AppCadence::counts(true,1,0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("character_measure_begin","\"cv_enabled\":"+std::string(Gothic::physicsFlags().characterVirtual?"true":"false"));
    }
  else if(c=="character" && a.size()==2 && a[1]=="report") {
    state.characterMeasuring=false;
    AppCadence::counts(false,1,0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("character_measure_end","\"frames\":"+std::to_string(state.characterFrames.size()));
    }
#endif
  else return false;
  return true;
  }

}
