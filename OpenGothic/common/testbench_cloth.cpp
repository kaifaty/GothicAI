#include "testbench_internal.h"
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

#include "camera.h"
#include "gothic.h"
#include "game/gamescript.h"
#include "game/gamesession.h"
#include "game/serialize.h"
#include "physics/cloth.h"
#include "world/bullet.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

// These fixtures only observe authored cloth and launch ordinary Bullet objects. No
// synthetic impulse, replacement cloth or NPC target participates in acceptance.
std::string clothRows(ClothContext ctx,World& world,bool geometry=false) {
  auto& state=ctx.cloth;
  std::string rows="[";
  for(const auto& body:world.physic()->cloth().snapshots()) {
    if(rows.size()>1)rows+=",";
    rows+="{\"id\":"+std::to_string(body.id)+",\"zen_id\":"+std::to_string(body.zenId)+
      ",\"visual\":"+jsonStr(body.visual)+",\"center_cm\":"+ragdollVector(body.center)+
      ",\"active\":"+(body.active?"true":"false")+",\"exposed\":"+(body.exposed?"true":"false")+
      ",\"visible\":"+(body.visible?"true":"false")+",\"simulation_steps\":"+std::to_string(body.simulationSteps)+
      ",\"vertices\":"+std::to_string(body.vertices)+
      ",\"rendered_vertices\":"+std::to_string(body.worldPositionsCm.size())+
      ",\"pinned\":"+std::to_string(body.pinned)+",\"weight\":"+ragdollNumber(body.simulationWeight)+
      ",\"anchor_error_cm\":"+ragdollNumber(body.maxAnchorErrorCm)+
      ",\"max_speed_cm_s\":"+ragdollNumber(body.maxSpeedCmPerSec)+
      ",\"max_speed_ever_cm_s\":"+ragdollNumber(body.maxSpeedEverCmPerSec)+
      ",\"contact_vertices\":"+std::to_string(body.contactVertices)+
      ",\"max_speed_vertex\":"+std::to_string(body.maxSpeedVertex)+
      ",\"max_speed_source_cm\":"+ragdollVector(body.maxSpeedSourceCm)+
      ",\"max_speed_world_cm\":"+ragdollVector(body.maxSpeedWorldCm)+
      ",\"max_speed_contact\":"+(body.maxSpeedContact?"true":"false")+
      ",\"max_speed_contact_body\":"+jsonStr(body.maxSpeedContactBody)+
      ",\"max_speed_contact_normal\":"+ragdollVector(body.maxSpeedContactNormal)+
      ",\"max_speed_ever_vertex\":"+std::to_string(body.maxSpeedEverVertex)+
      ",\"max_speed_ever_source_cm\":"+ragdollVector(body.maxSpeedEverSourceCm)+
      ",\"max_speed_ever_world_cm\":"+ragdollVector(body.maxSpeedEverWorldCm)+
      ",\"max_speed_ever_contact\":"+(body.maxSpeedEverContact?"true":"false")+
      ",\"max_speed_ever_contact_body\":"+jsonStr(body.maxSpeedEverContactBody)+
      ",\"max_speed_ever_contact_normal\":"+ragdollVector(body.maxSpeedEverContactNormal)+
      ",\"transition_blend_delta_cm\":"+ragdollNumber(body.transitionMaxDeltaCm)+
      ",\"spell_hits\":"+std::to_string(body.projectileHits)+",\"arrow_passes\":"+std::to_string(body.arrowPasses)+
      ",\"recoveries\":"+std::to_string(body.recoveryCount);
    rows+=",\"contact_bodies\":[";
    for(size_t i=0;i<body.contactBodies.size();++i) {
      if(i) rows+=",";
      rows+=jsonStr(body.contactBodies[i]);
      }
    rows+="]";
    rows+=",\"wind_occluders\":[";
    for(size_t i=0;i<body.windOccluders.size();++i) {
      if(i) rows+=",";
      const auto& roof=body.windOccluders[i];
      rows+="{\"body\":"+jsonStr(roof.body)+",\"point_cm\":"+ragdollVector(roof.pointCm)+
            ",\"distance_cm\":"+ragdollNumber(roof.distanceCm)+"}";
      }
    rows+="]";
    if(geometry && body.zenId==state.clothZen) {
      rows+=",\"rendered_world_vertices_cm\":[";
      for(size_t i=0;i<body.worldPositionsCm.size();++i) {
        if(i)rows+=",";
        rows+=ragdollVector(body.worldPositionsCm[i]);
        }
      rows+="]";
      }
    rows+="}";
    }
  return rows+"]";
  }

void clothFields(ClothContext ctx,World& world) {
  auto& state=ctx.cloth;
  if(!state.clothTrace || state.clothMeasuring || state.clothSample==world.tickCount())return;
  state.clothSample=world.tickCount();
  float frameDelta=0;
  bool changed=false;
  for(const auto& body:world.physic()->cloth().snapshots()) {
    auto& old=state.clothPrevious[body.id];
    if(body.zenId==state.clothZen && old.size()==body.worldPositionsCm.size())
      for(size_t i=0;i<old.size();++i)frameDelta=std::max(frameDelta,(old[i]-body.worldPositionsCm[i]).length());
    if(const auto i=state.clothPreviousActive.find(body.id);i!=state.clothPreviousActive.end())
      changed|=body.zenId==state.clothZen && i->second!=body.active;
    state.clothPreviousActive[body.id]=body.active;old=body.worldPositionsCm;
    }
  event("cloth_sample","\"case\":"+jsonStr(state.clothCase)+",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"hero_cm\":"+ragdollVector(world.player()->position())+
    ",\"hero_target_distance_cm\":"+ragdollNumber((world.player()->position()-state.clothCenter).length())+
    ",\"target_max_frame_delta_cm\":"+ragdollNumber(frameDelta)+
    ",\"target_active_changed\":"+(changed?"true":"false")+",\"cloth\":"+clothRows(ctx,world,changed));
  }

void clothCheckpoint(ClothContext ctx,World& world,std::string_view label) {
  auto& state=ctx.cloth;
  event("cloth_checkpoint","\"case\":"+jsonStr(state.clothCase)+",\"label\":"+jsonStr(label)+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+",\"enabled\":"+
    (world.physic()->clothEnabled()?"true":"false")+",\"wind_scale\":"+
    ragdollNumber(world.physic()->cloth().windScale())+",\"cloth\":"+clothRows(ctx,world,true));
  }

bool clothStandAt(ClothContext ctx,World& world,Npc& player,const Vec3& target,float distance) {
  auto& state=ctx.cloth;
  for(float angle:{0.f,.4f,-.4f,.8f,-.8f,1.6f,-1.6f,3.14f}) {
    const float ca=std::cos(angle),sa=std::sin(angle);
    const auto base=state.clothDirection;
    const Vec3 direction(base.x*ca-base.z*sa,0,base.x*sa+base.z*ca);
    float horizontal=distance;
    Vec3 point;
    bool supported=true;
    // Resolve the horizontal distance against the native floor, retaining the
    // requested 3D observer radius when a banner is well above street level.
    for(int pass=0;pass<4;++pass) {
      const auto floor=world.physic()->landRay(target+direction*horizontal+Vec3(0,6000,0),12000);
      if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-target.y)>=distance ||
         world.physic()->waterRay(floor.v+Vec3(0,5,0),1).hasCol) {supported=false;break;}
      point=floor.v+Vec3(0,1,0);
      horizontal=std::sqrt(std::max(0.f,distance*distance-(point.y-target.y)*(point.y-target.y)));
      }
    if(!supported || std::abs((point-target).length()-distance)>1.f)continue;
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.setPosition(point);
    player.setDirection(target-point);player.updateTransform();
    state.clothDirection=direction;
    state.clothHeldPosition=point;state.clothHold=true;
    return true;
    }
  return false;
  }

bool clothObserverFloor(World& world,const Vec3& position,Vec3& supported) {
  const auto floor=world.physic()->landRay(position+Vec3(0,10000,0),20000);
  if(!floor.hasCol || floor.n.y<.8f || world.physic()->waterRay(floor.v+Vec3(0,5,0),1).hasCol)
    return false;
  supported=floor.v+Vec3(0,1,0);
  // A native roof or yard is a valid fixture support, provided the hero fits.
  return !world.physic()->ray(supported+Vec3(0,5,0),supported+Vec3(0,180,0)).hasCol;
  }

bool clothNearbyObserver(World& world,const Vec3& target,Vec3& supported) {
  float score=std::numeric_limits<float>::max();
  // High ship flags cannot be observed from street level within the production
  // 40 m sphere. Search native yards and decks as well as ordinary ground.
  for(float radius:{0.f,100.f,200.f,350.f,500.f,750.f,1000.f,1500.f,2000.f,2500.f,3000.f}) {
    const int steps=radius==0.f?1:48;
    for(int step=0;step<steps;++step) {
      const float angle=float(step)*2.f*float(M_PI)/float(steps);
      Vec3 candidate;
      if(!clothObserverFloor(world,target+Vec3(std::cos(angle)*radius,0,std::sin(angle)*radius),candidate))continue;
      const auto distance=(candidate-target).length();
      if(distance>3400.f)continue;
      // Prefer a little room around a native support over balancing on a mast.
      unsigned room=0;
      for(const auto offset:{Vec3(25,0,0),Vec3(-25,0,0),Vec3(0,0,25),Vec3(0,0,-25)}) {
        const auto nearby=world.physic()->landRay(candidate+offset+Vec3(0,30,0),60);
        room+=nearby.hasCol && nearby.n.y>.8f && std::abs(nearby.v.y-candidate.y)<10.f;
        }
      const float cost=std::abs(distance-1800.f)+float(4-room)*500.f;
      if(cost<score) {score=cost;supported=candidate;}
      }
    }
  return std::isfinite(score) && score<std::numeric_limits<float>::max();
  }

std::string clothUpper(std::string text) {
  for(auto& c:text)c=char(std::toupper(static_cast<unsigned char>(c)));
  return text;
  }

bool clothCamera(ClothContext ctx,World& world,std::string_view group,bool required=true,Vec3* targetOut=nullptr);

float clothCameraDistance(const std::vector<Vec3>& points,const Vec3& target,const Vec3& direction) {
  const auto* camera=Gothic::inst().camera();
  const float halfY=(camera?camera->fovY():67.5f)*float(M_PI)/360.f;
  const auto projection=camera?camera->projective():Matrix4x4();
  const float aspect=camera?std::abs(projection.at(1,1)/projection.at(0,0)):4.f/3.f;
  const float tanY=std::tan(halfY),tanX=tanY*aspect;
  auto right=Vec3::crossProduct(direction,Vec3(0,1,0));
  if(right.length()<.01f)right=Vec3(1,0,0);
  else right=Vec3::normalize(right);
  const auto up=Vec3::normalize(Vec3::crossProduct(right,direction));
  float distance=650.f;
  for(const auto& point:points) {
    const auto delta=point-target;
    const float depth=Vec3::dotProduct(delta,direction);
    // Fit the actual projected geometry, with room for flutter at all edges.
    distance=std::max(distance,depth+1.2f*(std::abs(Vec3::dotProduct(delta,right))+25.f)/tanX);
    distance=std::max(distance,depth+1.2f*(std::abs(Vec3::dotProduct(delta,up))+25.f)/tanY);
    }
  return distance;
  }

bool clothFrame(World& world,const Cloth::Snapshot& body,Vec3& cameraEye,Vec3& cameraTarget,
                Vec3& cameraNormal,float& span) {
  const auto& reference=body.referenceWorldPositionsCm.empty()?body.worldPositionsCm:body.referenceWorldPositionsCm;
  if(reference.empty())return false;
  float referenceLow=reference.front().y,referenceHigh=reference.front().y;
  for(const auto& point:reference) {referenceLow=std::min(referenceLow,point.y);referenceHigh=std::max(referenceHigh,point.y);}
  // Only a short original bottom strip can be buried. An EARTH material on an
  // upper storey or awning must not erase the top of a city banner.
  const float burialCeiling=referenceLow+std::min(75.f,(referenceHigh-referenceLow)*.2f);
  const auto earthFloor=[&](const Vec3& point)->std::optional<float> {
    auto from=point+Vec3(0,200,0);
    const float bottom=point.y-20.f;
    for(int pass=0;pass<8 && from.y>bottom;++pass) {
      const auto hit=world.physic()->landRay(from,from.y-bottom);
      if(!hit.hasCol)return {};
      if(hit.mat==asset::MaterialGroup::EARTH && hit.n.y>.7f)return hit.v.y;
      from.y=hit.v.y-.5f;
      }
    return {};
    };
  std::vector<bool> sourceBuried(reference.size(),false),currentBuried(body.worldPositionsCm.size(),false);
  size_t buriedSource=0,buriedCurrent=0;
  float maximumBurial=0;
  Vec3 buriedLow,buriedHigh;
  for(size_t i=0;i<reference.size();++i) {
    if(reference[i].y>burialCeiling)continue;
    const auto floor=earthFloor(reference[i]);
    if(!floor || *floor>burialCeiling || reference[i].y>=*floor-2.f)continue;
    sourceBuried[i]=true;
    if(!buriedSource)buriedLow=buriedHigh=reference[i];
    ++buriedSource;maximumBurial=std::max(maximumBurial,*floor-reference[i].y);
    buriedLow.x=std::min(buriedLow.x,reference[i].x);buriedLow.y=std::min(buriedLow.y,reference[i].y);buriedLow.z=std::min(buriedLow.z,reference[i].z);
    buriedHigh.x=std::max(buriedHigh.x,reference[i].x);buriedHigh.y=std::max(buriedHigh.y,reference[i].y);buriedHigh.z=std::max(buriedHigh.z,reference[i].z);
    if(i<body.worldPositionsCm.size()) {
      const auto currentFloor=earthFloor(body.worldPositionsCm[i]);
      if(currentFloor && *currentFloor<=burialCeiling && body.worldPositionsCm[i].y<*currentFloor-2.f) {currentBuried[i]=true;++buriedCurrent;}
      }
    }
  std::vector<Vec3> fit;
  for(size_t i=0;i<reference.size();++i)if(!sourceBuried[i])fit.push_back(reference[i]);
  for(size_t i=0;i<body.worldPositionsCm.size();++i)if(!currentBuried[i])fit.push_back(body.worldPositionsCm[i]);
  if(fit.empty())return false;
  Vec3 bounds[2]={fit.front(),fit.front()};
  for(const auto& point:fit) {
    bounds[0].x=std::min(bounds[0].x,point.x);bounds[0].y=std::min(bounds[0].y,point.y);bounds[0].z=std::min(bounds[0].z,point.z);
    bounds[1].x=std::max(bounds[1].x,point.x);bounds[1].y=std::max(bounds[1].y,point.y);bounds[1].z=std::max(bounds[1].z,point.z);
    }
  float area=0;
  for(size_t i=0;i+2<body.indices.size();i+=3) {
    const auto a=body.indices[i],b=body.indices[i+1],c=body.indices[i+2];
    if(a>=reference.size() || b>=reference.size() || c>=reference.size())continue;
    const auto normal=Vec3::crossProduct(reference[b]-reference[a],reference[c]-reference[a]);
    const auto next=normal.length();
    if(next>area && (std::abs(normal.y)<next*.9f || area==0.f)) {area=next;cameraNormal=Vec3::normalize(normal);}
    }
  if(area<.01f)return false;
  cameraTarget=(bounds[0]+bounds[1])*.5f;
  span=(bounds[1]-bounds[0]).length();
  const auto hiddenByCloth=[&](const Vec3& from,const Vec3& to,const std::vector<Vec3>& pose) {
    const auto direction=to-from;
    for(size_t i=0;i+2<body.indices.size();i+=3) {
      const auto a=body.indices[i],b=body.indices[i+1],c=body.indices[i+2];
      if(a>=pose.size() || b>=pose.size() || c>=pose.size())continue;
      const auto edge1=pose[b]-pose[a],edge2=pose[c]-pose[a];
      const auto cross=Vec3::crossProduct(direction,edge2);
      const float determinant=Vec3::dotProduct(edge1,cross);
      if(std::abs(determinant)<1.e-6f)continue;
      const auto offset=from-pose[a];
      const float u=Vec3::dotProduct(offset,cross)/determinant;
      if(u<0.f || u>1.f)continue;
      const auto q=Vec3::crossProduct(offset,edge1);
      const float v=Vec3::dotProduct(direction,q)/determinant;
      if(v<0.f || u+v>1.f)continue;
      const float distance=Vec3::dotProduct(edge2,q)/determinant;
      if(distance>=0.f && distance<.9999f)return true;
      }
    return false;
    };
  bool cameraClear=false;
  size_t candidates=0,tested=0,hidden=0,attachments=0,selfRigidHits=0,bestBlocked=size_t(-1);
  std::string firstBlockedLabel;
  Vec3 firstBlockedPosition={},firstBlockedTarget={};
  float selectedAngle=0,selectedElevation=0;
  // A cloth's rear face can be hidden by its own front face. The authored
  // below-ground section also cannot be photographed; neither masks world walls.
  for(float elevation:{.55f,.8f,1.1f,0.f,-.3f,-.55f}) {
    // High ship cloth hangs underneath its yard/stay. A view from below can
    // show the fabric without putting the attachment in front of it.
    if(elevation<=0.f && bounds[0].y<1000.f)continue;
    for(float angle:{0.f,15.f,-15.f,30.f,-30.f,60.f,-60.f,180.f,195.f,165.f,210.f,150.f}) {
      ++candidates;
      const auto normal=rotY(cameraNormal,angle);
      const auto direction=Vec3::normalize(normal+Vec3(0,elevation,0));
      const float cameraDistance=clothCameraDistance(fit,cameraTarget,direction);
      const auto eye=cameraTarget+direction*cameraDistance;
      if(bounds[1].y<1000.f && eye.y<bounds[1].y+100.f)continue;
      size_t blocked=0,candidateTested=0,candidateHidden=0,candidateAttachments=0,candidateSelfHits=0;
      std::string candidateBlockedLabel;
      Vec3 candidateBlockedPosition={},candidateBlockedTarget={};
      const auto checkPose=[&](const std::vector<Vec3>& pose,const std::vector<bool>& buried) {
        for(size_t vertex=0;vertex<pose.size();++vertex) {
          if(buried[vertex])continue;
          // Authored pins can lie inside their beam or stone mount. They
          // remain in the frame bounds; only the free cloth needs a clear ray.
          if(std::any_of(body.anchorsCm.begin(),body.anchorsCm.end(),[&](const Vec3& anchor) {
               return (pose[vertex]-anchor).quadLength()<.0625f;
               })) {++candidateAttachments;continue;}
          const auto inward=cameraTarget-pose[vertex];
          const float length=inward.length();
          const auto target=pose[vertex]+(length>2.f?inward*(2.f/length):inward);
          const auto end=target-Vec3::normalize(target-eye)*2.f;
          if(hiddenByCloth(eye,end,pose)) {++candidateHidden;continue;}
          ++candidateTested;
          std::string label;
          const auto hit=world.physic()->rayClothCamera(eye,end,body.visual,bounds,candidateSelfHits,&label);
          if(!hit.hasCol)continue;
          bool attachment=false;
          if(hit.mat!=asset::MaterialGroup::EARTH && !hit.vob && (hit.v-target).length()<=20.f) {
            for(const auto& anchor:body.anchorsCm)
              if((target-anchor).length()<=25.f && (hit.v-anchor).length()<=25.f) {attachment=true;break;}
            }
          if(attachment)++candidateAttachments;
          else {
            if(!blocked) {candidateBlockedLabel=label;candidateBlockedPosition=hit.v;candidateBlockedTarget=target;}
            ++blocked;
            }
          }
        };
      checkPose(reference,sourceBuried);checkPose(body.worldPositionsCm,currentBuried);
      const auto ground=world.physic()->landRay(eye+Vec3(0,500,0),1000);
      const bool groundAccepted=!ground.hasCol || eye.y-ground.v.y>=180.f;
      if(blocked<bestBlocked) {
        bestBlocked=blocked;tested=candidateTested;hidden=candidateHidden;attachments=candidateAttachments;selfRigidHits=candidateSelfHits;
        firstBlockedLabel=candidateBlockedLabel;firstBlockedPosition=candidateBlockedPosition;firstBlockedTarget=candidateBlockedTarget;
        }
      if(blocked || !groundAccepted || !candidateTested)continue;
      cameraEye=eye;cameraNormal=normal;cameraClear=true;
      tested=candidateTested;hidden=candidateHidden;attachments=candidateAttachments;selfRigidHits=candidateSelfHits;
      selectedAngle=angle;selectedElevation=elevation;break;
      }
    if(cameraClear)break;
    }
  event("cloth_camera_frame","\"zen_id\":"+std::to_string(body.zenId)+",\"visual\":"+jsonStr(body.visual)+
    ",\"reference_vertices\":"+std::to_string(reference.size())+",\"current_vertices\":"+std::to_string(body.worldPositionsCm.size())+
    ",\"buried_reference_vertices\":"+std::to_string(buriedSource)+",\"buried_current_vertices\":"+std::to_string(buriedCurrent)+
    ",\"buried_bounds_min_cm\":"+(buriedSource?ragdollVector(buriedLow):"null")+
    ",\"buried_bounds_max_cm\":"+(buriedSource?ragdollVector(buriedHigh):"null")+
    ",\"maximum_original_burial_cm\":"+ragdollNumber(maximumBurial)+",\"fit_vertices\":"+std::to_string(fit.size())+
    ",\"burial_strip_ceiling_cm\":"+ragdollNumber(burialCeiling)+",\"self_rigid_hits\":"+std::to_string(selfRigidHits)+
    ",\"tested_visible_vertices\":"+std::to_string(tested)+",\"self_hidden_vertices\":"+std::to_string(hidden)+
    ",\"attachment_allowances\":"+std::to_string(attachments)+",\"minimum_blocked_vertices\":"+std::to_string(bestBlocked)+
    ",\"candidates\":"+std::to_string(candidates)+",\"clear\":"+(cameraClear?"true":"false")+
    ",\"azimuth_deg\":"+ragdollNumber(selectedAngle)+",\"elevation\":"+ragdollNumber(selectedElevation)+
    ",\"first_blocked_label\":"+jsonStr(firstBlockedLabel)+
    ",\"first_blocked_position_cm\":"+(bestBlocked?ragdollVector(firstBlockedPosition):"null")+
    ",\"first_blocked_target_cm\":"+(bestBlocked?ragdollVector(firstBlockedTarget):"null")+
    ",\"first_blocked_distance_cm\":"+(bestBlocked?ragdollNumber((firstBlockedPosition-firstBlockedTarget).length()):"null")+
    ",\"bounds_min_cm\":"+ragdollVector(bounds[0])+",\"bounds_max_cm\":"+ragdollVector(bounds[1]));
  return cameraClear;
  }

void clothSetup(ClothContext ctx,World& world,Npc& player,std::string selector,std::string label) {
  auto& state=ctx.cloth;
  const auto all=world.physic()->cloth().snapshots();
  if(all.empty()) {arenaAbort("cloth fixture world has no authored cloth");return;}
  auto found=all.end();
  Vec3 denseObserver;
  bool dense=false,denseSupported=false;
  state.clothObserverSupport="native-floor";
  if(selector=="densest") {
    size_t maximum=0;float nearest=std::numeric_limits<float>::max();
    // Search observer positions and count the unchanged real 3D radius.
    // Cluster centroids include the gaps between cloth objects, where a larger
    // number may be active than when standing directly beside one flag.
    std::vector<Vec3> centers;
    for(const auto& body:all) {
      centers.push_back(body.center);
      Vec3 center;size_t count=0;
      for(const auto& other:all)if((other.center-body.center).length()<7000.f) {center+=other.center;++count;}
      if(count)centers.push_back(center/float(count));
      }
    for(const auto& center:centers) {
      Vec3 floor;
      const bool hasFloor=clothNearbyObserver(world,center,floor);
      for(bool onFloor:{true,false}) {
        if(onFloor && !hasFloor)continue;
        // Static flight is a test-bench observer only, equivalent to Marvin
        // flying up the mast. Production always keeps its unchanged 3D radius.
        const auto observer=onFloor?floor:center-Vec3(0,100,0);
        size_t count=0;float minimum=std::numeric_limits<float>::max();auto target=all.end();
        for(auto i=all.begin();i!=all.end();++i) {
          const auto distance=(i->center-observer).length();
          count+=distance<Cloth::radiusCm;
          if(distance<minimum) {minimum=distance;target=i;}
          }
        if(count>maximum || (count==maximum && onFloor && !denseSupported) ||
           (count==maximum && onFloor==denseSupported && minimum<nearest)) {
          maximum=count;nearest=minimum;found=target;denseObserver=observer;dense=true;denseSupported=onFloor;
          }
        }
      }
    }
  else if(selector.find_first_not_of("0123456789")==std::string::npos) {
    const auto zen=uint32_t(std::strtoull(selector.c_str(),nullptr,10));
    found=std::find_if(all.begin(),all.end(),[&](const auto& body){return body.zenId==zen;});
    }
  else {
    selector=clothUpper(std::move(selector));
    found=std::find_if(all.begin(),all.end(),[&](const auto& body){return clothUpper(body.visual)==selector;});
    if(found==all.end())
      found=std::find_if(all.begin(),all.end(),[&](const auto& body){return clothUpper(body.visual).find(selector)!=std::string::npos;});
    }
  if(found==all.end()) {arenaAbort("cloth fixture requires an authored cloth id or visual selector");return;}
  Vec3 cameraEye,cameraTarget,cameraNormal;float span=0;
  bool cameraClear=false;
  if(dense && clothUpper(found->visual).find("NW_SHIP_")!=std::string::npos) {
    // Density selection is independent of the camera. A nearest furled sail can
    // sit against its yard, whereas the complete ship has a clear broadside.
    cameraClear=clothCamera(ctx,world,"harbour",false,&cameraTarget);
    if(cameraClear) {
      cameraEye=ctx.camPos;span=(cameraEye-cameraTarget).length()/1.6f;
      cameraNormal=Vec3::normalize(cameraEye-cameraTarget);
      }
    }
  else cameraClear=clothFrame(world,*found,cameraEye,cameraTarget,cameraNormal,span);
  if(!cameraClear && dense) {
    // Retain the exact observer and active body count. Only the photographed
    // cloth target changes if another member of the cluster is unobstructed.
    for(auto candidate=all.begin();candidate!=all.end();++candidate) {
      if((candidate->center-denseObserver).length()>=Cloth::radiusCm)continue;
      if(!clothFrame(world,*candidate,cameraEye,cameraTarget,cameraNormal,span))continue;
      found=candidate;cameraClear=true;break;
      }
    }
  if(!cameraClear) {arenaAbort("cloth fixture has no unobstructed fully framed camera side");return;}
  state.clothZen=found->zenId;state.clothCase=std::move(label);state.clothCenter=found->center;
  state.clothDirection=Vec3(cameraNormal.x,0,cameraNormal.z);
  if(state.clothDirection.length()<.01f)state.clothDirection=Vec3(1,0,0);
  else state.clothDirection=Vec3::normalize(state.clothDirection);
  bool supported=false;
  if(dense) {
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.setPosition(denseObserver);
    player.setDirection(found->center-denseObserver);player.updateTransform();
    state.clothHeldPosition=denseObserver;state.clothHold=true;supported=true;
    state.clothObserverSupport=denseSupported?"native-floor":"fixture-static-flight";
    }
  else supported=clothStandAt(ctx,world,player,found->center,1800.f) || clothStandAt(ctx,world,player,found->center,3200.f);
  if(!supported) {
    Vec3 observer;
    if(clothNearbyObserver(world,found->center,observer)) {
      player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.setPosition(observer);
      player.setDirection(found->center-observer);player.updateTransform();
      state.clothHeldPosition=observer;state.clothHold=true;supported=true;
      }
    }
  if(!supported) {
    const auto observer=found->center+state.clothDirection*1800.f;
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();player.setPosition(observer);
    player.setDirection(found->center-observer);player.updateTransform();
    state.clothHeldPosition=observer;state.clothHold=true;
    state.clothObserverSupport="fixture-static-flight";
    }
  ctx.subject=nullptr;ctx.cam=C_Hold;lookFrom(cameraEye,cameraTarget);
  state.clothPrevious.clear();state.clothPreviousActive.clear();state.clothTrace=true;
  world.physic()->cloth().resetStats();
  size_t nearby=0;for(const auto& body:all)nearby+=(body.center-player.position()).length()<Cloth::radiusCm;
  event("cloth_setup","\"case\":"+jsonStr(state.clothCase)+",\"world\":"+jsonStr(world.name())+
    ",\"selector\":"+jsonStr(selector)+",\"zen_id\":"+std::to_string(found->zenId)+",\"visual\":"+jsonStr(found->visual)+
    ",\"center_cm\":"+ragdollVector(found->center)+",\"hero_cm\":"+ragdollVector(player.position())+
    ",\"observer_support\":"+jsonStr(state.clothObserverSupport)+
    ",\"nearby_cloth_count\":"+std::to_string(nearby)+",\"camera_cm\":"+ragdollVector(cameraEye)+
    ",\"camera_target_cm\":"+ragdollVector(cameraTarget)+",\"camera_face_normal\":"+ragdollVector(cameraNormal)+
    ",\"camera_frame_span_cm\":"+ragdollNumber(span));
  }

bool clothCamera(ClothContext ctx,World& world,std::string_view group,bool required,Vec3* targetOut) {
  if(group!="harbour") {if(required)arenaAbort("cloth camera group expects harbour");return false;}
  const auto all=world.physic()->cloth().snapshots();
  std::vector<Vec3> points;
  size_t objects=0;
  for(const auto& body:all)if(clothUpper(body.visual).find("NW_SHIP_")!=std::string::npos) {
    points.insert(points.end(),body.worldPositionsCm.begin(),body.worldPositionsCm.end());++objects;
    }
  if(points.empty()) {if(required)arenaAbort("harbour camera requires the authored ship cloth");return false;}
  Vec3 low=points.front(),high=low;
  for(const auto& point:points) {
    low.x=std::min(low.x,point.x);low.y=std::min(low.y,point.y);low.z=std::min(low.z,point.z);
    high.x=std::max(high.x,point.x);high.y=std::max(high.y,point.y);high.z=std::max(high.z,point.z);
    }
  const auto target=(low+high)*.5f;
  Vec3 eye;bool clear=false;
  for(float angle:{3.14f,0.f,2.74f,.4f,3.54f,-.4f,2.34f,.8f}) {
    const auto direction=Vec3::normalize(Vec3(std::cos(angle),.3f,std::sin(angle)));
    const auto distance=clothCameraDistance(points,target,direction);
    eye=target+direction*distance;
    bool visible=true;
    for(const auto& body:all)if(clothUpper(body.visual).find("NW_SHIP_")!=std::string::npos) {
      const auto delta=body.center-eye;
      visible&=!world.physic()->ray(eye,eye+delta*.97f).hasCol;
      }
    if(visible) {clear=true;break;}
    }
  if(!clear) {if(required)arenaAbort("harbour camera has no unobstructed view of every authored cloth object");return false;}
  ctx.subject=nullptr;ctx.cam=C_Hold;lookFrom(eye,target);
  if(targetOut)*targetOut=target;
  event("cloth_camera","\"group\":"+jsonStr(group)+",\"objects\":"+std::to_string(objects)+
    ",\"camera_cm\":"+ragdollVector(eye)+",\"target_cm\":"+ragdollVector(target)+
    ",\"bounds_min_cm\":"+ragdollVector(low)+",\"bounds_max_cm\":"+ragdollVector(high));
  return true;
  }

void clothCameraRange(ClothContext ctx,World& world,float range) {
  auto& state=ctx.cloth;
  if(ctx.cam!=C_Hold || Gothic::inst().camera()==nullptr || state.clothZen==uint32_t(-1) ||
     !std::isfinite(range) || range<=0.f) {
    arenaAbort("cloth camera-range requires held selected cloth and a positive finite range");return;
    }
  const auto all=world.physic()->cloth().snapshots();
  const auto body=std::find_if(all.begin(),all.end(),[&](const auto& value){return value.zenId==state.clothZen;});
  if(body==all.end() || body->worldPositionsCm.empty()) {arenaAbort("cloth camera-range selected cloth is absent");return;}
  auto side=ctx.camPos-state.clothCenter;side.y=0.f;
  const float previousRange=side.length();
  if(previousRange<.01f || range<previousRange) {arenaAbort("cloth camera-range must increase its existing horizontal range");return;}
  const auto eye=state.clothCenter+side*(range/previousRange)+Vec3(0,ctx.camPos.y-state.clothCenter.y,0);
  const auto& reference=body->referenceWorldPositionsCm.empty()?body->worldPositionsCm:body->referenceWorldPositionsCm;
  Vec3 bounds[2]={reference.front(),reference.front()};
  const auto addBounds=[&](const auto& pose) {
    for(const auto& point:pose) {
      bounds[0].x=std::min(bounds[0].x,point.x);bounds[0].y=std::min(bounds[0].y,point.y);bounds[0].z=std::min(bounds[0].z,point.z);
      bounds[1].x=std::max(bounds[1].x,point.x);bounds[1].y=std::max(bounds[1].y,point.y);bounds[1].z=std::max(bounds[1].z,point.z);
      }
    };
  addBounds(reference);addBounds(body->worldPositionsCm);
  lookFrom(eye,state.clothCenter);
  size_t tested=0,blocked=0,attachments=0,selfHits=0;bool framed=true;
  const auto checkPose=[&](const auto& pose) {
    for(const auto& point:pose) {
      framed&=arenaProjected(point);
      if(std::any_of(body->anchorsCm.begin(),body->anchorsCm.end(),[&](const Vec3& anchor) {
           return (point-anchor).quadLength()<.0625f;
           })) {++attachments;continue;}
      const auto inward=state.clothCenter-point;
      const float length=inward.length();
      const auto target=point+(length>2.f?inward*(2.f/length):inward);
      const auto end=target-Vec3::normalize(target-eye)*2.f;
      ++tested;
      const auto hit=world.physic()->rayClothCamera(eye,end,body->visual,bounds,selfHits);
      if(!hit.hasCol)continue;
      bool attachment=false;
      if(hit.mat!=asset::MaterialGroup::EARTH && !hit.vob && (hit.v-target).length()<=20.f)
        attachment=std::any_of(body->anchorsCm.begin(),body->anchorsCm.end(),[&](const Vec3& anchor) {
          return (target-anchor).length()<=25.f && (hit.v-anchor).length()<=25.f;
          });
      if(attachment)++attachments;else ++blocked;
      }
    };
  checkPose(reference);checkPose(body->worldPositionsCm);
  const bool clear=framed && tested>0 && blocked==0;
  event("cloth_camera_range","\"horizontal_range_cm\":"+ragdollNumber(range)+
    ",\"previous_horizontal_range_cm\":"+ragdollNumber(previousRange)+",\"camera_cm\":"+ragdollVector(eye)+
    ",\"target_cm\":"+ragdollVector(state.clothCenter)+",\"pitch_degrees\":"+ragdollNumber(ctx.camHeldAngles.x)+
    ",\"yaw_degrees\":"+ragdollNumber(ctx.camHeldAngles.y)+",\"tested_vertices\":"+std::to_string(tested)+
    ",\"blocked_vertices\":"+std::to_string(blocked)+",\"attachment_allowances\":"+std::to_string(attachments)+
    ",\"self_rigid_hits\":"+std::to_string(selfHits)+",\"fully_framed\":"+(framed?"true":"false")+
    ",\"clear\":"+(clear?"true":"false"));
  if(!clear)arenaAbort("cloth camera-range has no unobstructed fully framed view");
  }

void clothSaveSession(ClothContext ctx,World& world,const std::string& label) {
  if(label.empty() || label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
    throw std::runtime_error("invalid cloth save label");
  std::vector<uint8_t> bytes;
  {MemWriter writer(bytes);Serialize out(writer);Pixmap preview(1,1,TextureFormat::RGBA8);std::memset(preview.data(),0,4);
  Gothic::inst().gameSession()->save(out,"cloth "+label,preview);}
  const auto path="session-cloth-"+label+".sav";
  std::ofstream file(outDir+"/"+path,std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!file)throw std::runtime_error("cannot write complete cloth GameSession archive");
  event("cloth_session_save","\"archive\":"+jsonStr(path)+",\"bytes\":"+std::to_string(bytes.size())+
    ",\"world\":"+jsonStr(world.name())+",\"cloth\":"+clothRows(ctx,world));
  }

void clothInitial(ClothContext ctx,World& world,std::string_view label) {
  bool rest=true;size_t count=0;
  for(const auto& body:world.physic()->cloth().snapshots()) {
    ++count;
    rest&=!body.active && body.maxSpeedCmPerSec==0.f && body.maxSpeedEverCmPerSec==0.f &&
      body.projectileHits==0 && body.arrowPasses==0 && body.recoveryCount==0 && body.maxAnchorErrorCm<=.5f;
    }
  event("cloth_session_initial","\"label\":"+jsonStr(label)+",\"world\":"+jsonStr(world.name())+
    ",\"cloth_count\":"+std::to_string(count)+",\"rest\":"+(rest?"true":"false")+",\"cloth\":"+clothRows(ctx,world));
  if(!count || !rest)arenaAbort("cloth cold load did not start from transient rest");
  }

void clothShoot(ClothContext ctx,World& world,Npc& player,std::string_view kind) {
  auto& state=ctx.cloth;
  const auto all=world.physic()->cloth().snapshots();
  const auto found=std::find_if(all.begin(),all.end(),[&](const auto& body){return body.zenId==state.clothZen;});
  if(found==all.end() || !found->active) {arenaAbort("cloth projectile requires its active authored target");return;}
  float bestArea=0;Vec3 target,normal;
  for(size_t i=0;i+2<found->indices.size();i+=3) {
    const auto a=found->indices[i],b=found->indices[i+1],c=found->indices[i+2];
    if(a>=found->worldPositionsCm.size() || b>=found->worldPositionsCm.size() || c>=found->worldPositionsCm.size())continue;
    const auto p=found->worldPositionsCm[a],q=found->worldPositionsCm[b],r=found->worldPositionsCm[c];
    const auto cross=Vec3::crossProduct(q-p,r-p);
    const auto area=cross.length();
    const auto center=(p+q+r)/3.f;
    bool pinned=false;
    for(const auto& anchor:found->anchorsCm)if((center-anchor).length()<15.f)pinned=true;
    if(pinned || area<=bestArea)continue;
    const auto direction=Vec3::normalize(cross);
    for(float side:{1.f,-1.f}) {
      const auto from=center+direction*(250.f*side);
      const auto hit=world.physic()->ray(from,center-direction*(100.f*side));
      if(hit.hasCol)continue;
      bestArea=area;target=center;normal=direction*side;break;
      }
    }
  if(bestArea<1.f) {arenaAbort("cloth projectile has no exposed native cloth face");return;}
  const bool arrow=kind=="arrow";
  size_t symbol=size_t(-1);
  if(arrow)symbol=world.script().findSymbolIndex("ITRW_ARROW");
  else if(kind=="fireball" || kind=="wind") {
    for(size_t i=0;i<world.script().symbolsCount();++i) {
      const auto* entry=world.script().findSymbol(i);
      if(!entry || entry->type()!=daedalus::DaedalusDataType::INSTANCE)continue;
      auto name=entry->name();for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
      if(!name.starts_with("ITRU_") && !name.starts_with("ITSC_"))continue;
      Item candidate(world,i,Item::T_Inventory);
      if(world.script().physicalSpell(candidate.spellId())==
         (kind=="wind"?GameScript::PhysicalSpell::WindFist:GameScript::PhysicalSpell::Fireball)) {symbol=i;break;}
      }
    }
  if(symbol==size_t(-1)) {arenaAbort("cloth fixture native projectile instance is unavailable");return;}
  Item item(world,symbol,Item::T_Inventory);
  const auto from=target+normal*250.f;
  // shootArrow is the public free-direction entry to WorldObjects::shootBullet;
  // the Item still creates the native spell Bullet, spell VFX and collision path.
  auto& bullet=world.shootArrow(item,from,-normal,arrow?DynamicWorld::bulletSpeed:DynamicWorld::spellSpeed);
  state.clothShotKind=std::string(kind);state.clothShotAt=ctx.time;
  state.clothShotHitsBefore=arrow?found->arrowPasses:found->projectileHits;
  state.clothArrow=arrow?&bullet:nullptr;state.clothShotTarget=target;state.clothShotNormal=normal;
  state.clothArrowDownstream=0;
  bullet.setOrigin(&player);if(arrow)bullet.setPrecise(1.f);
  event("cloth_projectile","\"kind\":"+jsonStr(kind)+",\"zen_id\":"+std::to_string(state.clothZen)+
    ",\"native_spell\":"+(bullet.isSpell()?"true":"false")+",\"spell_id\":"+std::to_string(bullet.spellId())+
    ",\"from_cm\":"+ragdollVector(from)+",\"face_target_cm\":"+ragdollVector(target)+
    ",\"direction\":"+ragdollVector(-normal));
  }

void clothArrowNpc(ClothContext ctx,World& world,Npc& player,std::string_view order) {
  auto& state=ctx.cloth;
  if(order!="behind" && order!="ahead") {arenaAbort("cloth arrow-npc expects behind or ahead");return;}
  const auto all=world.physic()->cloth().snapshots();
  const auto found=std::find_if(all.begin(),all.end(),[&](const auto& body){return body.zenId==state.clothZen;});
  if(found==all.end() || !found->active) {arenaAbort("cloth arrow-npc requires an active authored banner");return;}
  float bestArea=0;Vec3 target,normal;
  for(size_t i=0;i+2<found->indices.size();i+=3) {
    const auto p=found->worldPositionsCm[found->indices[i]],q=found->worldPositionsCm[found->indices[i+1]],r=found->worldPositionsCm[found->indices[i+2]];
    const auto cross=Vec3::crossProduct(q-p,r-p);
    if(cross.length()<=bestArea)continue;
    const auto center=(p+q+r)/3.f;
    for(float side:{1.f,-1.f}) {
      const auto direction=Vec3::normalize(cross)*side;
      if(std::abs(direction.y)>.25f || world.physic()->ray(center+direction*200.f,center-direction*150.f).hasCol)continue;
      target=center;normal=direction;bestArea=cross.length();break;
      }
    }
  if(bestArea<1.f) {arenaAbort("cloth arrow-npc has no unobstructed face and NPC segment");return;}
  if(state.clothArrowNpc==nullptr) {
    const auto symbol=world.script().findSymbolIndex("BAU_904_BAUER");
    if(symbol==size_t(-1)) {arenaAbort("cloth arrow-npc fixture instance unavailable");return;}
    state.clothArrowNpc=world.addNpc(symbol,target);
    if(!state.clothArrowNpc) {arenaAbort("cloth arrow-npc fixture insertion failed");return;}
    ctx.crew.push_back(state.clothArrowNpc);
    }
  auto& npc=*state.clothArrowNpc;
  npc.clearAiQueue();npc.clearState(false);npc.stopWalking();npc.clearSpeed();npc.setExternalControl(true);
  npc.handle().attribute[ATR_HITPOINTS]=npc.handle().attribute[ATR_HITPOINTSMAX]=50000;
  const auto from=target+normal*200.f,to=target-normal*150.f;
  Vec3 expectedImpact;bool prepared=false;
  const float npcSide=order=="behind"?-70.f:70.f;
  for(float height:{100.f,120.f,80.f,140.f,160.f}) {
    npc.setPosition(target+normal*npcSide-Vec3(0,height,0));npc.setDirection(normal);npc.updateTransform();
    float distance=0;
    if(npc.rayHitZone(from,to,distance)==HitZones::None)continue;
    expectedImpact=from+Vec3::normalize(to-from)*distance;
    const float side=Vec3::dotProduct(expectedImpact-target,normal);
    if((order=="behind" && side< -5.f) || (order=="ahead" && side>5.f)) {prepared=true;break;}
    }
  if(!prepared) {arenaAbort("cloth arrow-npc fixture bones do not lie strictly before or after its banner");return;}
  const auto arrowSymbol=world.script().findSymbolIndex("ITRW_ARROW");
  if(arrowSymbol==size_t(-1)) {arenaAbort("cloth arrow-npc native arrow item unavailable");return;}
  Item item(world,arrowSymbol,Item::T_Inventory);
  constexpr uint64_t dt=100;
  // One synchronous native moveBullet call, with gravity compensated in its
  // initial velocity. The native Bullet callback still resolves bones, damage,
  // attachments and stop. Its registered body cannot tick during this call.
  Bullet arrow(world,item,from);
  arrow.setOrigin(&player);arrow.setPrecise(1.f);
  const auto direction=(to-from)/float(dt)+Vec3(0,.5f*DynamicWorld::gravity*float(dt),0);
  arrow.setDirection(direction);
  DynamicWorld::BulletBody step(world.physic(),&arrow);
  step.setPosition(from);step.setDirection(direction);
  const auto before=found->arrowPasses;
  ClothBulletBenchDriver::step(*world.physic(),step,direction,dt);
  const auto afterRows=world.physic()->cloth().snapshots();
  const auto after=std::find_if(afterRows.begin(),afterRows.end(),[&](const auto& body){return body.zenId==state.clothZen;});
  const auto passes=after==afterRows.end()?before:after->arrowPasses;
  const bool nativeHit=arrow.isFinished() && arrow.hitZone()!=HitZones::None && (arrow.impactPoint()-expectedImpact).length()<.1f;
  const bool pass=nativeHit && passes-before==(order=="behind"?1u:0u);
  event("cloth_arrow_npc_verify","\"order\":"+jsonStr(order)+",\"pass\":"+(pass?"true":"false")+
    ",\"move_bullet_calls\":1,\"dt_ms\":"+std::to_string(dt)+",\"native_precise_hit\":"+(nativeHit?"true":"false")+
    ",\"arrow_passes_before\":"+std::to_string(before)+",\"arrow_passes_after\":"+std::to_string(passes)+
    ",\"from_cm\":"+ragdollVector(from)+",\"step_end_cm\":"+ragdollVector(to)+",\"cloth_target_cm\":"+ragdollVector(target)+
    ",\"npc_impact_cm\":"+ragdollVector(arrow.impactPoint())+",\"npc_impact_side_cm\":"+
    ragdollNumber(Vec3::dotProduct(arrow.impactPoint()-target,normal)));
  if(!pass)arenaAbort("cloth single-step arrow/NPC ordering check failed");
  }

bool clothCommand(ClothContext ctx,const std::vector<std::string>& a,World& w,Npc& pl) {
  auto& state=ctx.cloth;
  const auto& c=a[0];
  if(c=="cloth" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","cloth",a[1]=="on");
    event("cloth_toggle","\"enabled\":"+std::string(w.physic()->clothEnabled()?"true":"false"));
    }
  else if(c=="cloth" && a.size()==4 && a[1]=="setup")clothSetup(ctx,w,pl,a[2],a[3]);
  else if(c=="cloth" && a.size()==6 && a[1]=="room-check") {
    if(!isNumber(a[2]) || !isNumber(a[3]) || !isNumber(a[4]) || (a[5]!="indoor" && a[5]!="outdoor")) {
      arenaAbort("cloth room-check expects finite x y z and indoor or outdoor");return true;
      }
    const Vec3 point(num(a,2,0),num(a,3,0),num(a,4,0));
    if(!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
      arenaAbort("cloth room-check expects finite coordinates");return true;
      }
    const bool indoor=w.isIndoor(point),expected=a[5]=="indoor";
    const bool pass=indoor==expected;
    event("cloth_room_check","\"point_cm\":"+ragdollVector(point)+",\"bsp_room\":"+jsonStr(w.roomAt(point))+
      ",\"indoor\":"+(indoor?"true":"false")+",\"expected\":"+jsonStr(a[5])+",\"pass\":"+(pass?"true":"false"));
    if(!pass)arenaAbort("cloth room classification check failed");
    }
  else if(c=="cloth" && a.size()==3 && a[1]=="wind") {
    if(a[2]!="calm" && a[2]!="gust" && a[2]!="natural") {arenaAbort("cloth wind expects calm, gust or natural");return true;}
    w.physic()->cloth().setWindScale(a[2]=="calm"?0.f:(a[2]=="gust"?2.f:1.f));
    event("cloth_wind","\"mode\":"+jsonStr(a[2]));
    }
  else if(c=="cloth" && a.size()==3 && a[1]=="checkpoint")clothCheckpoint(ctx,w,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="shoot")clothShoot(ctx,w,pl,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="arrow-npc")clothArrowNpc(ctx,w,pl,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="save")clothSaveSession(ctx,w,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="camera")clothCamera(ctx,w,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="camera-range") {
    if(!isNumber(a[2])) {arenaAbort("cloth camera-range expects a numeric range in cm");return true;}
    clothCameraRange(ctx,w,num(a,2,0));
    }
  else if(c=="cloth" && a.size()==4 && a[1]=="camera-turn") {
    const float degrees=num(a,2,0),duration=num(a,3,0);
    auto* camera=Gothic::inst().camera();
    if(ctx.cam==C_Turn && ctx.time>=ctx.camTurnStart+ctx.camTurnDuration)ctx.updateCamera(w,pl);
    if(!isNumber(a[2]) || !isNumber(a[3]) || !std::isfinite(degrees) || !std::isfinite(duration) ||
       duration<1.f || duration>60000.f || camera==nullptr || ctx.cam!=C_Hold) {
      arenaAbort("cloth camera-turn requires a held camera, finite degrees and duration from 1 to 60000 ms");return true;
      }
    ctx.camTurnAngles=ctx.camHeldAngles;ctx.camTurnDegrees=degrees;
    ctx.camTurnStart=ctx.time;ctx.camTurnDuration=uint64_t(duration);ctx.cam=C_Turn;
    ctx.waitUntil=ctx.time+ctx.camTurnDuration+1;
    event("cloth_camera_turn","\"degrees\":"+ragdollNumber(degrees)+",\"duration_ms\":"+
      std::to_string(ctx.camTurnDuration)+",\"camera_cm\":"+ragdollVector(ctx.camPos)+
      ",\"pitch_degrees\":"+ragdollNumber(ctx.camTurnAngles.x)+",\"from_yaw_degrees\":"+
      ragdollNumber(ctx.camTurnAngles.y));
    }
  else if(c=="cloth" && a.size()==3 && a[1]=="initial")clothInitial(ctx,w,a[2]);
  else if(c=="cloth" && a.size()==3 && a[1]=="verify") {
    bool pass=true;bool target=false;
    size_t sails=0,flags=0,exposedSails=0,exposedFlags=0;
    for(const auto& body:w.physic()->cloth().snapshots()) {
      if(a[2]=="harbour-wind") {
        if(!body.active)continue;
        if(body.visual.starts_with("NW_SHIP_SAIL_02:")) {
          ++sails;exposedSails+=body.exposed;
          }
        else if(body.visual.starts_with("NW_SHIP_FLAG_01:") || body.visual.starts_with("NW_SHIP_FLAG_02:")) {
          ++flags;exposedFlags+=body.exposed;
          }
        }
      else if(a[2]=="anchors") {
        target|=body.zenId==state.clothZen && body.active && body.pinned>0;
        pass&=std::isfinite(body.maxAnchorErrorCm) && body.maxAnchorErrorCm<=.5f &&
          std::isfinite(body.maxSpeedEverCmPerSec) && body.maxSpeedEverCmPerSec<=Cloth::maxVelocityMS*100.f+.5f && body.recoveryCount==0;
        for(const auto& point:body.worldPositionsCm)
          pass&=std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
        }
      else if(body.zenId==state.clothZen) {
        target=true;
        if(a[2]=="arrow")pass&=state.clothShotKind=="arrow" && body.arrowPasses>state.clothShotHitsBefore && state.clothArrowDownstream>=30.f;
        else if(a[2]=="fireball" || a[2]=="wind")pass&=state.clothShotKind==a[2] && body.projectileHits>state.clothShotHitsBefore;
        else pass=false;
        }
      }
    if(a[2]=="harbour-wind") {
      target=sails==5 && flags==4;
      pass&=exposedSails==sails && exposedFlags==flags;
      }
    pass&=target;
    event("cloth_verify","\"check\":"+jsonStr(a[2])+",\"pass\":"+(pass?"true":"false")+
      ",\"active_sails\":"+std::to_string(sails)+",\"exposed_sails\":"+std::to_string(exposedSails)+
      ",\"active_flags\":"+std::to_string(flags)+",\"exposed_flags\":"+std::to_string(exposedFlags)+
      ",\"arrow_downstream_cm\":"+ragdollNumber(state.clothArrowDownstream)+",\"cloth\":"+clothRows(ctx,w));
    if(!pass)arenaAbort("cloth native "+a[2]+" check failed");
    }
  else if(c=="cloth" && a.size()==3 && a[1]=="distance") {
    if(state.clothZen==uint32_t(-1)) {arenaAbort("cloth boundary requires a selected target");return true;}
    if(!clothStandAt(ctx,w,pl,state.clothCenter,num(a,2,4000))) {
      if(state.clothObserverSupport!="fixture-static-flight") {
        arenaAbort("cloth boundary requires a dry supported observer at the requested radius");return true;
        }
      state.clothHeldPosition=state.clothCenter+state.clothDirection*num(a,2,4000);
      pl.setPosition(state.clothHeldPosition);pl.updateTransform();
      }
    event("cloth_observer","\"hero_cm\":"+ragdollVector(pl.position())+",\"distance_cm\":"+
      ragdollNumber((pl.position()-state.clothCenter).length()));
    }
  else if(c=="cloth" && a.size()==2 && a[1]=="reset-stats")w.physic()->cloth().resetStats();
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="cloth" && a.size()==3 && a[1]=="measure") {
    if(!AppCadence::normalClock() || !AppCadence::arm(a[2]=="on")) {arenaAbort("cloth normal cadence arm failed");return true;}
    state.clothMeasuring=true;state.clothPhysics.clear();state.clothPhysics.reserve(20000);state.clothOverflow=false;
    const auto all=w.physic()->cloth().snapshots();
    const auto active=std::count_if(all.begin(),all.end(),[](const auto& body){return body.active;});
    AppCadence::counts(true,uint32_t(active),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    event("cloth_measure_before","\"mode\":"+jsonStr(a[2])+",\"active_soft_bodies\":"+
      std::to_string(active)+",\"cloth\":"+clothRows(ctx,w));
    }
  else if(c=="cloth" && a.size()==2 && a[1]=="report") {
    state.clothMeasuring=false;
    const auto all=w.physic()->cloth().snapshots();
    const auto active=std::count_if(all.begin(),all.end(),[](const auto& body){return body.active;});
    AppCadence::counts(false,uint32_t(active),0,uint32_t(w.physic()->physicsStats().bodies),0,0);
    std::vector<float> costs;costs.reserve(state.clothPhysics.size());
    if(auto* file=std::fopen((outDir+"/cloth-physics.csv").c_str(),"w")) {
      std::fprintf(file,"sample,t_game_ms,dt_ms,step_ms,native_bodies\n");
      for(size_t i=0;i<state.clothPhysics.size();++i) {
        const auto& row=state.clothPhysics[i];costs.push_back(row.milliseconds);
        std::fprintf(file,"%zu,%llu,%llu,%.9g,%zu\n",i,(unsigned long long)row.time,
          (unsigned long long)row.dt,double(row.milliseconds),row.bodies);
        }
      std::fclose(file);
      }
    else {arenaAbort("cannot write cloth native step timings");return true;}
    std::sort(costs.begin(),costs.end());
    const auto p95=costs.empty()?0.f:costs[std::min(costs.size()-1,size_t(std::ceil(double(costs.size())*.95)-1))];
    event("cloth_measure_after","\"active_soft_bodies\":"+std::to_string(active)+
      ",\"samples\":"+std::to_string(costs.size())+",\"physics_p95_ms\":"+ragdollNumber(p95)+
      ",\"overflow\":"+(state.clothOverflow?"true":"false")+",\"cloth\":"+clothRows(ctx,w));
    }
#else
  else if(c=="cloth" && a.size()>=2 && (a[1]=="measure" || a[1]=="report")) {
    arenaAbort("cloth timing requires a build with OPENGOTHIC_ENABLE_APP_CADENCE=ON");
    }
#endif
  else return false;
  return true;
  }

}
