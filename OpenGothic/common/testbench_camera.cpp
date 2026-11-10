#include "testbench.h"
#include "testbench_internal.h"

#include <Tempest/Pixmap>
#include <Tempest/SoundDevice>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "camera.h"
#include "game/bodyarena.h"
#include "game/bodyobservations.h"
#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "gothic.h"
#include "graphics/drawcommands.h"
#include "graphics/mesh/footik.h"
#include "graphics/mesh/skeleton.h"
#include "world/objects/npc.h"
#include "world/world.h"

// resources.cpp: texture residency of a deterministic bench (`deterministic on`)
extern bool benchTexturesFrozen;

using namespace Tempest;

namespace testbench_detail {

Vec3 ankle(const Npc& pl, size_t leg) {
  auto& p = FootIk::playerProbe;
  if(p.valid && pl.isPlayer())
    return p.leg[leg].final[2];
  return pl.position() + Vec3(0,10,0);
  }

Vec3 camOrigin(const CameraContext& ctx,World& w, const Npc& pl, float angle) {
  const Vec3  p = pl.position();
  Vec3        dir;
  if(ctx.camera.cam==C_World) {
    const float a = angle*float(M_PI)/180.f;
    dir = {std::cos(a),0,std::sin(a)};
    } else {
    const Vec3  f    = facing(pl);
    const Vec3  side = {-f.z,0,f.x};
    const float b    = angle*float(M_PI)/180.f;
    dir = f*std::cos(b) + side*std::sin(b);
    }
  Vec3 org = p + dir*ctx.camera.camDist + Vec3(0,ctx.camera.camHeight,0);
  // keep the camera above the ground
  const float g = ctx.camera.camLift ? groundY(w,org.x,org.z,org.y+200) : NAN;
  if(!std::isnan(g) && org.y<g+25)
    org.y = g+25;
  return org;
  }

int hiddenCount(const CameraContext& ctx,World& w, const Npc& pl, const Vec3& org) {
  const Vec3 p = pl.position();
  int n = 0;
  n += hidden(w,org,ankle(pl,0)+Vec3(0,5,0)) ? 1 : 0;
  n += hidden(w,org,ankle(pl,1)+Vec3(0,5,0)) ? 1 : 0;
  n += hidden(w,org,p+Vec3(0,ctx.camera.camLook,0)) ? 1 : 0;
  return n;
  }

// side views first
void pickCamAngle(const CameraContext& ctx,World& w, const Npc& pl) {
  static const float angles[] = {90,270,60,120,240,300,30,150,210,330,0,180};
  float best = ctx.camera.camAngle;
  int   bn   = 100;
  for(float a:angles) {
    const int n = hiddenCount(ctx,w,pl,camOrigin(ctx,w,pl,a));
    if(n<bn) {
      bn   = n;
      best = a;
      }
    if(n==0)
      break;
    }
  ctx.camera.camAngle = best;
  }

void lookFrom(const CameraContext& ctx,const Vec3& org, const Vec3& tgt) {
  auto c = Gothic::inst().camera();
  if(c==nullptr)
    return;
  const Vec3  d   = tgt-org;
  const float lxz = std::sqrt(d.x*d.x+d.z*d.z);
  const float yaw = std::atan2(d.z,d.x)*180.f/float(M_PI);
  const float pit = std::atan2(d.y,lxz)*180.f/float(M_PI);
  c->setMarvinMode(Camera::M_Freeze);
  c->setPosition(org);
  c->setSpin(PointF(-pit,yaw));
  c->setAngles(PointF(-pit,yaw));
  if(ctx.camera.cutPending) {
    c->markCut();
    ctx.camera.cutPending = false;
    }
  ctx.camera.camPos = org;
  ctx.camera.camHeldAngles = PointF(-pit,yaw);
  }

// a shot of the hero and camNpc; false if the npc is not there. camNpc `arena`: the arena's primary
// in place of the hero and its opponent
bool npcShot(const CameraContext& ctx,World& w, Npc& player, Vec3& org, Vec3& tgt) {
  Npc* npc = nullptr;
  Npc* own = &player;
  if(ctx.camera.camNpc=="arena") {
    const auto actors = BodyArena::actors();
    if(actors.size()<2)
      return false;
    own = actors[0];
    npc = actors[1];
    } else {
    npc = findNpc(w,player,ctx.camera.camNpc);  // of several (wolves) the nearest living one
    if(npc==&player)
      return false;
    }
  if(npc==nullptr)
    return false;
  Npc& pl = *own;
  const Vec3 hero  = pl.mapHeadBone();
  const Vec3 other = npc->mapHeadBone();
  Vec3 d = other-hero;
  d.y = 0;
  const float len = d.length();
  d = len>1 ? d/len : facing(pl);
  const Vec3 side = {-d.z,0,d.x};
  const Vec3 up   = {0,1,0};
  if(ctx.camera.cam==C_Shoulder) {
    // behind the hero, off his shoulder (camAngle: +1 or -1 picks the shoulder), at the npc's face
    org = hero - d*ctx.camera.camDist + side*(40.f*ctx.camera.camAngle) + up*ctx.camera.camHeight;
    tgt = other - up*8.f;
    }
  else if(ctx.camera.cam==C_NpcFace) {
    // in front of the npc's face (where it looks), turned camAngle degrees towards the hero's side
    const Vec3 f  = facing(*npc);
    const Vec3 c0 = rotY(f, ctx.camera.camAngle), c1 = rotY(f,-ctx.camera.camAngle);
    const Vec3 c  = (c0.x*-d.x + c0.z*-d.z) >= (c1.x*-d.x + c1.z*-d.z) ? c0 : c1;  // the turn towards the hero
    org = other + c*ctx.camera.camDist + up*ctx.camera.camHeight;
    tgt = other - up*8.f;
    }
  else {
    const Vec3  mid  = (hero+other)*0.5f;
    const float dist = std::max(ctx.camera.camDist,len*1.2f);
    org = mid + side*(dist*(ctx.camera.camAngle<0 ? -1.f : 1.f)) + up*ctx.camera.camHeight;
    tgt = mid - up*15.f;
    }
  const float g = groundY(w,org.x,org.z,org.y+200);
  if(!std::isnan(g) && org.y<g+25)
    org.y = g+25;
  return true;
  }

void updateCamera(const CameraContext& ctx,World& w, Npc& pl) {
  ctx.camera.occl = 0;
  if(ctx.camera.cam==C_Turn) {
    if(auto* camera=Gothic::inst().camera()) {
      const float progress=std::min(1.f,float(ctx.time-ctx.camera.camTurnStart)/float(ctx.camera.camTurnDuration));
      const float eased=progress*progress*(3.f-2.f*progress);
      const PointF angles(ctx.camera.camTurnAngles.x,ctx.camera.camTurnAngles.y+ctx.camera.camTurnDegrees*eased);
      camera->setPosition(ctx.camera.camPos);
      camera->setSpin(angles);camera->setAngles(angles);
      ctx.camera.camHeldAngles=angles;
      if(progress>=1.f)ctx.camera.cam=C_Hold;
      }
    return;
    }
  if(ctx.camera.cam==C_Water) {
    lookFrom(ctx,ctx.camera.camPos,ctx.water.cameraTarget);
    return;
    }
  if(ctx.camera.cam==C_RagdollArena) {
    lookFrom(ctx,ctx.camera.camPos,ctx.ragdoll.ragdollArenaTarget);
    return;
    }
  if(ctx.camera.cam==C_Game || ctx.camera.cam==C_Hold)
    return;
  if(ctx.camera.cam==C_Arrow) {
    Vec3 org, tgt;
    arrowShot(w,pl,org,tgt);
    if(!ctx.camera.smValid) {
      ctx.camera.smOrg   = org;
      ctx.camera.smTgt   = tgt;
      ctx.camera.smValid = true;
      } else {
      ctx.camera.smOrg += (org-ctx.camera.smOrg)*ctx.camera.camSmooth;
      ctx.camera.smTgt += (tgt-ctx.camera.smTgt)*ctx.camera.camSmooth;
      }
    lookFrom(ctx,ctx.camera.smOrg,ctx.camera.smTgt);
    return;
    }
  if(ctx.camera.cam==C_Shoulder || ctx.camera.cam==C_NpcFace || ctx.camera.cam==C_Two) {
    // shots of the hero himself, also while the camera watches a subject (gaze)
    Npc* hero = Gothic::inst().player();
    Vec3 org, tgt;
    if(hero==nullptr || !npcShot(ctx,w,*hero,org,tgt))
      return;
    if(!ctx.camera.smValid) {
      ctx.camera.smOrg   = org;
      ctx.camera.smTgt   = tgt;
      ctx.camera.smValid = true;
      } else {
      ctx.camera.smOrg += (org-ctx.camera.smOrg)*ctx.camera.camSmooth;
      ctx.camera.smTgt += (tgt-ctx.camera.smTgt)*ctx.camera.camSmooth;
      }
    lookFrom(ctx,ctx.camera.smOrg,ctx.camera.smTgt);
    if(ctx.camera.clip>0)
      if(auto cam = Gothic::inst().camera())
        cam->setNearClip(std::max(10.f, (ctx.camera.smOrg-ctx.camera.smTgt).length() - ctx.camera.clip));
    return;
    }
  Vec3 tgt = pl.position() + Vec3(0,ctx.camera.camLook,0);
  Vec3 org = ctx.camera.camPos;
  if(ctx.camera.cam==C_Follow || ctx.camera.cam==C_World) {
    org = camOrigin(ctx,w,pl,ctx.camera.camAngle);
    // a body down sinks its position under the ground (85 cm for the hero knocked down): look at the ground over it
    const Vec3  at = pl.position();
    const float g  = groundY(w,at.x,at.z,at.y+100.f);
    if(!std::isnan(g) && g>at.y) {
      tgt.y += g-at.y;
      org.y  = std::max(org.y,g+ctx.camera.camHeight);
      }
    }
  if(ctx.camera.cam==C_Face) {
    // the mouth (a bit above the head bone), seen from the front or at an angle
    const Vec3  f    = facing(pl);
    const Vec3  side = {-f.z,0,f.x};
    const float b    = ctx.camera.camAngle*float(M_PI)/180.f;
    tgt = pl.mapHeadBone() + Vec3(0,ctx.camera.camLook,0);
    org = tgt + (f*std::cos(b) + side*std::sin(b))*ctx.camera.camDist + Vec3(0,ctx.camera.camHeight,0);
    }
  lookFrom(ctx,org,tgt);
  if(ctx.camera.clip>0)
    if(auto cam = Gothic::inst().camera())
      cam->setNearClip(std::max(10.f, (org-tgt).length() - ctx.camera.clip));

  for(size_t i=0; i<2; ++i)
    if(hidden(w,org,ankle(pl,i)+Vec3(0,5,0)))
      ctx.camera.occl |= uint8_t(1u<<i);
  ctx.camera.occlTime = ctx.camera.occl!=0 ? ctx.camera.occlTime+1 : 0;
  if(ctx.camera.camAuto && ctx.camera.cam==C_Follow && ctx.camera.occlTime>Fps/3) {
    pickCamAngle(ctx,w,pl);
    ctx.camera.occlTime = 0;
    }
  }

void standAround(World& w, Npc& s, Npc& pl, float dist, float angle) {
  const float a   = (s.rotation()+angle)*float(M_PI)/180.f;
  Vec3        pos = s.position() + Vec3(std::cos(a),0,std::sin(a))*dist;
  const float g   = groundY(w,pos.x,pos.z,pos.y+100);
  if(std::isnan(g))
    return; // inside a rock: it would fall through the world
  pos.y = g;
  pl.setPosition(pos);
  const Vec3 d = s.position()-pos;
  pl.setDirection(Vec3(d.x,0,d.z));
  pl.updateTransform();
  }

// the hero stands around the watched npc, facing it
void placeHero(const CameraContext& ctx,World& w, Npc& pl) {
  Npc& s = ctx.subjectOf(pl);
  if(&s==&pl || ctx.camera.placeDist<=0)
    return;
  standAround(w,s,pl,ctx.camera.placeDist,ctx.camera.placeAngle);
  }

bool frameRagdollArena(const CameraContext& ctx,size_t expected) {
  auto* camera=Gothic::inst().camera();
  if(camera==nullptr || ctx.crew.size()!=expected) return false;
  const float infinity=std::numeric_limits<float>::infinity();
  Vec3 low{infinity,infinity,infinity},high{-infinity,-infinity,-infinity};
  auto add=[&](const Vec3& point) {
    low.x=std::min(low.x,point.x); low.y=std::min(low.y,point.y); low.z=std::min(low.z,point.z);
    high.x=std::max(high.x,point.x); high.y=std::max(high.y,point.y); high.z=std::max(high.z,point.z);
    };
  for(const auto* npc:ctx.crew) {
    const auto* skeleton=npc->visualSkeleton(); const auto bounds=npc->bounds();
    if(skeleton==nullptr || bounds.rConservative<=0 || !std::isfinite(bounds.rConservative)) return false;
    // The full proof pads each axis by50cm; a radial proxy needs the diagonal.
    ctx.ragdoll.ragdollArenaRadii.push_back(bounds.rConservative+std::sqrt(3.f)*50.f);
    const auto object=npc->transform();
    for(int c=0;c<8;++c) {
      Vec3 p{bounds.bbox[c&1].x,bounds.bbox[(c>>1)&1].y,bounds.bbox[(c>>2)&1].z}; object.project(p); add(p);
      }
    for(const auto& node:skeleton->nodes) {
      const auto p=npc->mapBone(node.name); add(p-Vec3(50,50,50)); add(p+Vec3(50,50,50));
      }
    }
  const float halfY=camera->fovY()*float(M_PI)/360.f;
  const auto projection=camera->projective();
  const float aspect=std::abs(projection.at(1,1)/projection.at(0,0));
  const float halfFov=std::min(halfY,std::atan(std::tan(halfY)*aspect));
  const float distance=((high-low)*.5f).length()*1.25f/std::sin(halfFov);
  if(!std::isfinite(distance) || distance<=0) return false;
  ctx.ragdoll.ragdollArenaTarget=(high+low)*.5f;
  ctx.camera.camPos=ctx.ragdoll.ragdollArenaTarget+Vec3(0,.9238795f*distance,.3826834f*distance);
  ctx.camera.cam=C_RagdollArena; ctx.ragdoll.ragdollArenaCamera=true; ctx.camera.camAuto=false; ctx.camera.smValid=false;
  lookFrom(ctx,ctx.camera.camPos,ctx.ragdoll.ragdollArenaTarget);
  event("ragdoll_arena_camera","\"origin\":"+ragdollVector(ctx.camera.camPos)+",\"target\":"+
        ragdollVector(ctx.ragdoll.ragdollArenaTarget)+",\"bounds_min\":"+ragdollVector(low)+",\"bounds_max\":"+
        ragdollVector(high)+",\"fov_y\":"+std::to_string(camera->fovY())+",\"aspect\":"+std::to_string(aspect));
  return true;
  }

bool monsterCharacterRenderProof(const CameraContext& ctx,World& world,Npc& npc,std::string_view role) {
  npc.updateTransform();
  const auto* camera=Gothic::inst().camera();const auto* skeleton=npc.visualSkeleton();
  const auto bounds=npc.bounds();const auto object=npc.transform();
  const float infinity=std::numeric_limits<float>::infinity();
  Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);
  Vec3 ndcLow(infinity,infinity,infinity),ndcHigh(-infinity,-infinity,-infinity);
  bool projected=camera && skeleton;unsigned behind=0;std::string bones="[";
  auto add=[&](const Vec3& point) {
    low.x=std::min(low.x,point.x);low.y=std::min(low.y,point.y);low.z=std::min(low.z,point.z);
    high.x=std::max(high.x,point.x);high.y=std::max(high.y,point.y);high.z=std::max(high.z,point.z);
    if(!camera)return;
    float x=point.x,y=point.y,z=point.z,w=1;camera->viewProj().project(x,y,z,w);
    if(w<=0 || !std::isfinite(w)) {++behind;projected=false;return;}
    const Vec3 ndc(x/w,y/w,z/w);
    ndcLow.x=std::min(ndcLow.x,ndc.x);ndcLow.y=std::min(ndcLow.y,ndc.y);ndcLow.z=std::min(ndcLow.z,ndc.z);
    ndcHigh.x=std::max(ndcHigh.x,ndc.x);ndcHigh.y=std::max(ndcHigh.y,ndc.y);ndcHigh.z=std::max(ndcHigh.z,ndc.z);
    projected&=std::isfinite(ndc.x) && std::isfinite(ndc.y) && std::isfinite(ndc.z) &&
      std::abs(ndc.x)<=.98f && std::abs(ndc.y)<=.98f && ndc.z>=0 && ndc.z<=1;
    };
  for(unsigned i=0;i<8;++i) {
    Vec3 point(bounds.bbox[i&1].x,bounds.bbox[(i>>1)&1].y,bounds.bbox[(i>>2)&1].z);object.project(point);add(point);
    }
  if(skeleton)for(const auto& bone:skeleton->nodes) {
    const auto point=npc.mapBone(bone.name);add(point);
    if(bone.name.find("FOOT")==std::string::npos && bone.name.find("HEAD")==std::string::npos && bone.name.find("PELVIS")==std::string::npos)continue;
    if(bones.size()>1)bones+=',';
    bones+="{\"name\":"+jsonStr(bone.name)+",\"position_cm\":"+ragdollVector(point)+
      ",\"occluded\":"+(hidden(world,ctx.camera.camPos,point+Vec3(0,5,0))?"true":"false")+"}";
    }
  ctx.monster.monsterCharacterRenderRequest=&npc;const bool drawable=TestBench::ragdollBodyDrawable(npc);ctx.monster.monsterCharacterRenderRequest=nullptr;
  event("monster_character_render","\"route\":"+jsonStr(ctx.monster.monsterCharacterRoute.name)+",\"role\":"+jsonStr(role)+
    ",\"npc_id\":"+std::to_string(world.npcId(&npc))+",\"instance\":"+jsonStr(symName(world,&npc))+
    ",\"root_cm\":"+ragdollVector(npc.position())+",\"visual_root_cm\":"+ragdollVector(npc.characterVisualPosition())+
    ",\"world_min_cm\":"+ragdollVector(low)+",\"world_max_cm\":"+ragdollVector(high)+
    ",\"ndc_min\":"+ragdollVector(ndcLow)+",\"ndc_max\":"+ragdollVector(ndcHigh)+
    ",\"behind_camera_points\":"+std::to_string(behind)+",\"full_pose_projected\":"+(projected?"true":"false")+
    ",\"body_drawable\":"+(drawable?"true":"false")+",\"hidden_draw_types\":"+std::to_string(DrawCommands::hiddenTypes)+
    ",\"near_clip_cm\":"+ragdollNumber(camera?camera->zNear():0)+",\"bones\":"+bones+"]");
  return drawable && projected;
  }

void monsterCharacterCamera(const CameraContext& ctx,World& world,Npc& player,bool requested,float preferredYaw,float preferredElevation) {
  if(requested && (!std::isfinite(preferredYaw) || !std::isfinite(preferredElevation) || preferredElevation<=0 || preferredElevation>=1)) {
    monsterCharacterError("monster camera expects a finite yaw and elevation fraction strictly between 0 and 1");return;
    }
  const auto& route=ctx.monster.monsterCharacterRoute;
  if(ctx.monster.monsterCharacterCrowd) {
    if(requested) {monsterCharacterError("monster camera angle selection requires a prepared video fixture");return;}
    event("monster_character_camera","\"route\":\"performance\",\"eye_cm\":"+ragdollVector(ctx.camera.camPos)+",\"fixed\":true");return;
    }
  if(!route.npc) {monsterCharacterError("monster camera requires a prepared fixture");return;}
  std::vector<Vec3> points{route.start,route.goal,ctx.monster.monsterCharacterHeroStart};
  for(const auto& actor:ctx.monster.monsterCharacterExtras) {
    if(actor.npc)points.push_back(actor.start);
    for(const auto& point:actor.points)points.push_back(point.pos);
    }
  // Include the ground along the entire lane, not only the endpoints. The
  // resulting held camera is identical in OFF and ON and never tracks roots.
  const auto actorPoints=points.size();
  for(unsigned i=1;i<10;++i) {
    Vec3 floor;const auto at=route.start+(route.goal-route.start)*(float(i)/10.f);
    if(characterFloor(world,at,floor))points.push_back(floor);
    }
  const float infinity=std::numeric_limits<float>::infinity();
  Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);
  float height=180,horizontalPadding=75;
  auto envelope=[&](const Npc& npc) {
    const auto bounds=npc.bounds();height=std::max(height,bounds.bbox[1].y-bounds.bbox[0].y);
    const float x=std::max(std::abs(bounds.bbox[0].x),std::abs(bounds.bbox[1].x));
    const float z=std::max(std::abs(bounds.bbox[0].z),std::abs(bounds.bbox[1].z));
    // A static local XZ circle covers any later yaw. Extra space preserves
    // flapping wings beyond the rest pose without following live animation.
    horizontalPadding=std::max(horizontalPadding,std::hypot(x,z)+50.f);
    };
  envelope(player);envelope(*route.npc);
  for(const auto& actor:ctx.monster.monsterCharacterExtras)if(actor.npc)envelope(*actor.npc);
  for(const auto& point:points) {
    low.x=std::min(low.x,point.x-horizontalPadding);low.y=std::min(low.y,point.y-35);low.z=std::min(low.z,point.z-horizontalPadding);
    high.x=std::max(high.x,point.x+horizontalPadding);high.y=std::max(high.y,point.y+height+50);high.z=std::max(high.z,point.z+horizontalPadding);
    }
  const auto target=(low+high)*.5f;auto along=route.goal-route.start;along.y=0;
  along=along.length()>1?Vec3::normalize(along):Vec3(1,0,0);const Vec3 side(-along.z,0,along.x);
  auto* camera=Gothic::inst().camera();if(!camera) {monsterCharacterError("monster video camera unavailable");return;}
  const float halfY=camera->fovY()*float(M_PI)/360.f;const auto projection=camera->projective();
  const float aspect=std::abs(projection.at(1,1)/projection.at(0,0));
  const float halfFov=std::min(halfY,std::atan(std::tan(halfY)*aspect));
  const float distance=std::max(600.f,((high-low)*.5f).length()*1.1f/std::sin(halfFov));
  Vec3 eye;bool found=false;float selectedYaw=0,selectedElevation=0;bool fallback=false;
  std::vector<std::pair<float,float>> shots;
  if(requested)shots.emplace_back(preferredYaw,preferredElevation);
  for(float elevation:{.42f,.62f,.85f})for(float yaw:{0.f,180.f,30.f,-30.f,150.f,210.f,60.f,-60.f})
    if(!requested || std::abs(yaw-preferredYaw)>.001f || std::abs(elevation-preferredElevation)>.001f)shots.emplace_back(yaw,elevation);
  for(size_t shot=0;shot<shots.size();++shot) {
    const auto [yaw,elevation]=shots[shot];
    const auto direction=rotY(side,yaw);const auto candidate=target+direction*(distance*std::sqrt(1.f-elevation*elevation))+Vec3(0,distance*elevation,0);
    bool visible=true;
    for(size_t pointId=0;pointId<points.size();++pointId)for(float y:{15.f,height*.5f,height}) {
      const auto& point=points[pointId];
      // Theoretical lane samples inside a prop describe its occupied volume.
      // Keep that volume in the frame without demanding a view through it;
      // actual actors and authored endpoints retain every sight-line check.
      bool propFootprint=false;
      if(pointId>=actorPoints && ctx.character.characterProp) {
        const auto delta=point-ctx.character.characterPropInitial;propFootprint=std::hypot(delta.x,delta.z)<120;
        }
      if(pointId>=actorPoints)for(const auto& prop:ctx.monster.monsterCharacterProps) {
        const auto delta=point-prop.start;propFootprint|=std::hypot(delta.x,delta.z)<120;
        }
      if(!propFootprint && hidden(world,candidate,point+Vec3(0,y,0))) {visible=false;break;}
      }
    if(!visible)continue;
    eye=candidate;found=true;selectedYaw=yaw;selectedElevation=elevation;fallback=requested && shot!=0;break;
    }
  if(!found) {monsterCharacterError("monster fixed camera cannot see the complete route and feet");return;}
  ctx.camera.cam=C_Water;ctx.water.cameraTarget=target;ctx.camera.camAuto=false;ctx.camera.smValid=false;lookFrom(ctx,eye,target);
  std::string actors="[{\"npc_id\":"+std::to_string(world.npcId(route.npc))+",\"role\":\"primary\",\"start_cm\":"+ragdollVector(route.start)+"}";
  for(const auto& actor:ctx.monster.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
    actors+=",{\"npc_id\":"+std::to_string(world.npcId(actor.npc))+",\"role\":"+jsonStr(actor.role)+",\"start_cm\":"+ragdollVector(actor.start)+"}";
  std::string ready="[{\"npc_id\":"+std::to_string(world.npcId(route.npc))+",\"root_cm\":"+ragdollVector(route.npc->position())+"}";
  for(const auto& actor:ctx.monster.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
    ready+=",{\"npc_id\":"+std::to_string(world.npcId(actor.npc))+",\"root_cm\":"+ragdollVector(actor.npc->position())+"}";
  event("monster_character_camera","\"route\":"+jsonStr(route.name)+",\"camera_cm\":"+ragdollVector(eye)+
    ",\"requested_shot\":"+(requested?"true":"false")+",\"requested_yaw_deg\":"+ragdollNumber(preferredYaw)+
    ",\"requested_elevation_fraction\":"+ragdollNumber(preferredElevation)+",\"selected_yaw_deg\":"+ragdollNumber(selectedYaw)+
    ",\"selected_elevation_fraction\":"+ragdollNumber(selectedElevation)+",\"used_fallback\":"+(fallback?"true":"false")+
    ",\"seed\":"+std::to_string(ctx.monster.monsterCharacterSeed)+",\"hour\":"+std::to_string(world.time().hour())+
    ",\"minute\":"+std::to_string(world.time().minute())+",\"hero_cm\":"+ragdollVector(ctx.monster.monsterCharacterHeroStart)+",\"actors\":"+actors+"]"+
    ",\"ready_actors\":"+ready+"],\"hero_ready_cm\":"+ragdollVector(player.position())+
    ",\"target_cm\":"+ragdollVector(target)+",\"bounds_min_cm\":"+ragdollVector(low)+",\"bounds_max_cm\":"+ragdollVector(high)+
    ",\"horizontal_actor_padding_cm\":"+ragdollNumber(horizontalPadding)+",\"fov_y\":"+ragdollNumber(camera->fovY())+",\"aspect\":"+ragdollNumber(aspect)+",\"fixed\":true");
  bool rendered=monsterCharacterRenderProof(ctx,world,player,"hero");
  rendered=monsterCharacterRenderProof(ctx,world,*route.npc,"primary") && rendered;
  for(auto& actor:ctx.monster.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
    rendered=monsterCharacterRenderProof(ctx,world,*actor.npc,actor.role) && rendered;
  if(!rendered)monsterCharacterError("monster fixed camera requires drawable actor meshes and their complete projected poses");
  }

// track: where a tracked npc is on this frame, into events.jsonl: x, y the head (or the trunk: track ... body),
// and every zone: hx hy (head), bx by (trunk), lx ly (the legs, below the knees); pixels of the frame
void trackActorFrame(const CameraContext& ctx,World& world, const std::string& symbol, int w, int h) {
  auto   cam  = Gothic::inst().camera();
  Npc*   n    = npcBySymbol(world,symbol);
  if(n==nullptr)
    return;
  // HP/identity do not depend on visibility. Exporters still get one sample if a head is offscreen.
  if(ctx.bodyLive) {
    const Npc* target=n->target()!=nullptr ? n->target() : world.player();
    event("body_live_sample","\"npc\":"+jsonStr(symbol)+",\"entity_id\":"+
          std::to_string(BodyObservations::entityId(*n))+",\"t_game_ms\":"+std::to_string(world.tickCount())+
          ",\"self_hp\":"+std::to_string(n->attribute(ATR_HITPOINTS))+",\"target_hp\":"+
          (target!=nullptr ? std::to_string(target->attribute(ATR_HITPOINTS)) : "null"));
    }
  if(cam==nullptr)
    return;
  std::vector<HitZones::Posed> caps;
  n->hitZonesPosed(caps);
  const float knees = n->position().y + 80.f;
  Vec3  sum[3] = {};
  float cnt[3] = {};
  for(auto& c:caps) {
    const Vec3 m = (c.a+c.b)*0.5f;
    const int  k = c.zone==HitZones::Head ? 0 : (c.zone==HitZones::Body ? 1 : (m.y<knees ? 2 : -1));
    if(k>=0) {
      sum[k] += m;
      cnt[k] += 1;
      }
    }
  const Matrix4x4 vp = cam->viewProj();
  float sx[3] = {-1,-1,-1}, sy[3] = {-1,-1,-1};
  for(int k=0; k<3; ++k) {
    if(cnt[k]==0)
      continue;
    Vec3  p  = sum[k]*(1.f/cnt[k]);
    float pw = 1;
    vp.project(p.x,p.y,p.z,pw);
    if(pw<=1e-3f)
      continue;
    sx[k] = (0.5f*p.x/pw+0.5f)*float(w);
    sy[k] = (0.5f*p.y/pw+0.5f)*float(h);
    }
  const int main = ctx.camera.trackHead ? 0 : 1;
  if(sx[main]<0)
    return;
  char buf[320];
  std::snprintf(buf,sizeof(buf),"\"npc\":%s,\"x\":%.1f,\"y\":%.1f,\"hx\":%.1f,\"hy\":%.1f,\"bx\":%.1f,\"by\":%.1f,"
                "\"lx\":%.1f,\"ly\":%.1f,\"w\":%d,\"h\":%d",
                jsonStr(symbol).c_str(),double(sx[main]),double(sy[main]),double(sx[0]),double(sy[0]),
                double(sx[1]),double(sy[1]),double(sx[2]),double(sy[2]),w,h);
  event("track",buf);
  }

void trackFrame(const CameraContext& ctx,int w, int h) {
  if(auto* world=Gothic::inst().world())
    for(const auto& symbol:ctx.camera.trackNpcs)
      trackActorFrame(ctx,*world,symbol,w,h);
  }

// sight: the hero's line of sight to the npc on this frame, into events.jsonl; points are [x, y] in pixels of the
// frame (null behind the camera): h0 the shared standing head, c0 where its ray to the npc's
// centre hits (null: nothing; c0d cm of d0), e the minimum lean as Npc::canSeeNpcOverEdge puts it (room
// for it), ec where its ray hits; h the head now, c where its ray hits (cd of d, down degrees); t the npc's
// centre; the bow's focus fan from h0: f level, g at FOCUS_RANGED's elevdo down (elev), fan cm long
void sightFrame(const CameraContext& ctx,int w, int h, const std::string& shot) {
  World* wrld = Gothic::inst().world();
  auto   cam  = Gothic::inst().camera();
  Npc*   pl   = Gothic::inst().player();
  Npc*   n    = wrld!=nullptr && pl!=nullptr ? findNpc(*wrld,*pl,ctx.camera.sightNpc) : nullptr;
  if(n==nullptr || n==pl || cam==nullptr)
    return;
  const Matrix4x4 vp = cam->viewProj();
  auto pt = [&](Vec3 p, bool has = true) -> std::string {
    float pw = 1;
    vp.project(p.x,p.y,p.z,pw);
    if(!has || pw<=1e-3f)
      return "null";
    char b[64];
    std::snprintf(b,sizeof(b),"[%.1f,%.1f]",double((0.5f*p.x/pw+0.5f)*float(w)),double((0.5f*p.y/pw+0.5f)*float(h)));
    return b;
    };
  auto*       phys = wrld->physic();
  const Vec3  tg   = n->collosionCenter();
  const bool  manual = pl->hasManualAim();
  const Vec3  aim  = manual ? pl->manualAimPoint() : tg;
  const Vec3  h0   = pl->aimHead(aim,0.f);
  const Vec3  hd   = pl->mapHeadBone();
  const Vec3  predicted = pl->aimCurrentHead();
  const float predictionError = (predicted-hd).length();
  const auto  lean = pl->aimLean(aim,manual);
  auto        r0   = phys->ray(h0,tg);
  auto        r1   = phys->ray(hd,tg);
  // The manual pose's directional solver is independent of this observed NPC.
  const Vec3  d    = tg-h0;
  const Vec3  eye  = pl->aimHead(aim,lean.angle);
  const Vec3  full = pl->aimHead(aim,Aim::LeanMax*float(M_PI)/180.f);
  auto        rt   = phys->ray(eye,tg);
  // the focus fan in the vertical plane toward the npc
  const float elev = -float(wrld->script().focusRange().npc_elevdo)*float(M_PI)/180.f;
  const Vec3  lvl  = Vec3::normalize(Vec3(d.x,0,d.z));
  const Vec3  fl   = h0 + lvl*ctx.camera.sightFan;
  const Vec3  fg   = h0 + (lvl*std::cos(elev) - Vec3(0,std::sin(elev),0))*ctx.camera.sightFan;
  const float down = -std::atan2(tg.y-hd.y,std::sqrt((tg.x-hd.x)*(tg.x-hd.x)+(tg.z-hd.z)*(tg.z-hd.z)))*180.f/float(M_PI);

  char buf[2300];
  std::snprintf(buf,sizeof(buf),"\"npc\":%s,\"shot\":%s,\"w\":%d,\"h\":%d,"
                "\"h0\":%s,\"c0\":%s,\"c0d\":%.0f,\"d0\":%.0f,\"e\":%s,\"room\":%d,\"ec\":%s,"
                "\"h\":%s,\"c\":%s,\"cd\":%.0f,\"d\":%.0f,\"down\":%.1f,\"t\":%s,\"f\":%s,\"g\":%s,\"elev\":%.0f,"
                "\"seen\":%d,\"over\":%d,\"required_deg\":%.2f,\"current_deg\":%.2f,\"max_forward_cm\":%.1f,"
                "\"error_cm\":%.2f,\"actual_seen\":%d,\"edge\":%d,\"hit_cm\":%.2f,\"visible\":%d,\"line\":%s,\"shot_clear\":%d,\"manual\":%d",
                jsonStr(ctx.camera.sightNpc).c_str(),jsonStr(shot).c_str(),w,h,
                pt(h0).c_str(),pt(r0.v,r0.hasCol).c_str(),r0.hasCol ? double((r0.v-h0).length()) : -1.0,double((tg-h0).length()),
                pt(eye).c_str(),int(lean.room),pt(rt.v,rt.hasCol).c_str(),
                pt(hd).c_str(),pt(r1.v,r1.hasCol).c_str(),r1.hasCol ? double((r1.v-hd).length()) : -1.0,double((tg-hd).length()),
                double(down),pt(tg).c_str(),pt(fl).c_str(),pt(fg).c_str(),double(elev*180.f/float(M_PI)),
                int(!r0.hasCol),int(pl->canSeeNpcOverEdge(*n)),double(lean.angle*180.f/float(M_PI)),
                double(Aim::playerProbe.lean),double(Vec3::dotProduct(full-pl->position(),lvl)),
                double(predictionError),int(!r1.hasCol),int(lean.edge),double(lean.hitDistance),int(lean.visible),
                jsonStr(leanLineName(lean.line)).c_str(),int(lean.shotClear),int(manual));
  event("sight",buf);
  // Captured after all pose passes. A shot captured during a recording also calls sightFrame;
  // only its ordinary frame writes the diagnostic, keeping exactly one sample per recorded frame.
  if(ctx.camera.rec && shot.empty()) {
    const std::string focus = symName(*wrld,pl->target());
    Vec3 arrow, arrowDir;
    const bool hasArrow = pl->arrowRay(arrow,arrowDir);
    int actualShotClear = -1;
    if(hasArrow) {
      const auto shotRay = phys->ray(arrow,arrow+Vec3::normalize(arrowDir)*200.f);
      actualShotClear = int(!shotRay.hasCol || (lean.edge && shotRay.v.y<lean.edgeHeight-100.f));
      }
    std::snprintf(buf,sizeof(buf),"\"npc\":%s,\"error_cm\":%.2f,\"current_deg\":%.2f,\"required_deg\":%.2f,"
                  "\"edge\":%d,\"hit_cm\":%.2f,\"room\":%d,\"visible\":%d,\"line\":%s,\"shot_clear\":%d,\"actual_seen\":%d,"
                  "\"focus\":%s,\"manual\":%d,\"actual_shot_clear\":%d,\"actual\":[%.2f,%.2f,%.2f],\"predicted\":[%.2f,%.2f,%.2f],"
                  "\"aim\":[%.2f,%.2f,%.2f],\"head_clear\":%d,\"edge_cm\":%.2f,\"drop_cm\":%.2f,\"support_y\":%.2f,"
                  "\"head_hit_cm\":%.2f,\"shot_hit_cm\":%.2f,\"limited\":%d,\"room_limited\":%d",
                  jsonStr(ctx.camera.sightNpc).c_str(),double(predictionError),double(Aim::playerProbe.lean),
                  double(lean.angle*180.f/float(M_PI)),
                  int(lean.edge),double(lean.hitDistance),int(lean.room),int(lean.visible),jsonStr(leanLineName(lean.line)).c_str(),int(lean.shotClear),
                  int(!r1.hasCol),jsonStr(focus).c_str(),int(manual),actualShotClear,
                  double(hd.x),double(hd.y),double(hd.z),double(predicted.x),double(predicted.y),double(predicted.z),
                  double(aim.x),double(aim.y),double(aim.z),int(lean.headClear),double(lean.edgeDistance),
                  double(std::isfinite(lean.edgeDrop) ? lean.edgeDrop : -1.f),double(lean.edgeHeight),double(lean.headHitDistance),double(lean.shotHitDistance),
                  int(lean.limited),int(lean.roomLimited));
    event("lean-frame",buf);
    std::snprintf(buf,sizeof(buf),"lean-frame %s frame %lld: error_cm %.2f current_deg %.2f required_deg %.2f "
                  "edge %d hit_cm %.2f room %d visible %d line %s shot_clear %d actual_shot_clear %d actual_seen %d focus %s manual %d "
                  "actual (%.2f %.2f %.2f) predicted (%.2f %.2f %.2f)",
                  ctx.camera.recName.c_str(),(long long)ctx.camera.recFrame,double(predictionError),double(Aim::playerProbe.lean),
                  double(lean.angle*180.f/float(M_PI)),
                  int(lean.edge),double(lean.hitDistance),int(lean.room),int(lean.visible),leanLineName(lean.line),int(lean.shotClear),actualShotClear,
                  int(!r1.hasCol),focus.c_str(),int(manual),
                  double(hd.x),double(hd.y),double(hd.z),double(predicted.x),double(predicted.y),double(predicted.z));
    say(buf);
    }
  }

void aimCameraFrame(const CameraContext& ctx,const std::string& shot) {
  auto* pl=Gothic::inst().player();
  auto* camera=Gothic::inst().camera();
  if(pl==nullptr || camera==nullptr || !pl->hasManualAim())
    return;
  const Vec3 origin=camera->originLwc();
  const Vec3 front=camera->listenerPosition().front;
  Vec3 crosshair=pl->manualAimPoint();
  float spread=0;
  if(ctx.ctrl!=nullptr)
    ctx.ctrl->crosshair(crosshair,spread);
  const bool centered=ctx.ctrl!=nullptr && ctx.ctrl->isCrosshairCentered();
  // Input/pose precedes the camera tick. Use the rendered center ray at the target's depth,
  // retaining both input points so a one-frame tracking error cannot look like clear sight.
  const Vec3 aim=centered ? origin+front*std::max(0.f,Vec3::dotProduct(crosshair-origin,front)) : crosshair;
  const float distance=(aim-origin).length();
  const float pitch=-std::atan2(front.y,std::hypot(front.x,front.z))*180.f/float(M_PI);
  std::vector<HitZones::Posed> capsules;
  pl->hitZonesPosed(capsules);
  float clearance=std::numeric_limits<float>::infinity(), closestAlong=0;
  size_t closest=0, hits=0;
  std::string intersections="[";
  for(size_t i=0;i<capsules.size();++i) {
    const auto& capsule=capsules[i];
    float along=0;
    const float gap=aimSegmentDistance(origin,aim,capsule.a,capsule.b,along)-capsule.r;
    if(gap<clearance) { clearance=gap; closest=i; closestAlong=along; }
    if(gap<=0) {
      if(hits++>0) intersections+=',';
      intersections+="{\"index\":"+std::to_string(i)+",\"zone\":"+jsonStr(HitZones::name(capsule.zone))+
                     ",\"clearance_cm\":"+ragdollNumber(gap)+"}";
      }
    }
  intersections+=']';
  std::string equipment;
  for(bool ammo:{false,true}) {
    Matrix4x4 matrix;
    Vec3 box[2];
    const bool available=pl->aimAttachmentBounds(ammo,matrix,box);
    equipment+=std::string(",\"")+(ammo ? "arrow" : "bow")+"\":{\"available\":"+(available ? "true" : "false");
    if(available) {
      auto inverse=matrix;
      inverse.inverse();
      Vec3 a=origin, b=aim;
      inverse.project(a); inverse.project(b);
      // NPC attachment scales are uniform. The smallest basis length is conservative if
      // a model supplies a nonuniform scale (rotation and scale have no shear here).
      float scale=std::numeric_limits<float>::infinity();
      for(int i=0;i<3;++i)
        scale=std::min(scale,Vec3(matrix.at(i,0),matrix.at(i,1),matrix.at(i,2)).length());
      const float gap=aimBoxDistance(a,b,box)*scale;
      equipment+=",\"model\":\"morph-expanded-attachment-obb\",\"conservative\":true,\"clearance_cm\":"+ragdollNumber(gap)+
                 ",\"intersects\":"+(gap<=1e-4f ? "true" : "false");
      }
    equipment+='}';
    }
  // Check the rendered near plane and a 10 cm shell around the optical origin.
  // Cast both ways so a wall's triangle winding cannot hide an embedded camera.
  int wallHits = 0;
  auto* world = Gothic::inst().world();
  auto nearView = camera->viewProj();
  nearView.inverse();
  if(world!=nullptr) {
    auto probe = [&](const Vec3& point) {
      if(world->physic()->ray(origin,point).hasCol || world->physic()->ray(point,origin).hasCol)
        ++wallHits;
      };
    for(float x:{-1.f,1.f})
      for(float y:{-1.f,1.f}) {
        Vec3 corner(x,y,0);
        nearView.project(corner);
        probe(corner);
        }
    for(const Vec3 axis:{Vec3(1,0,0),Vec3(0,1,0),Vec3(0,0,1)}) {
      probe(origin+axis*10.f);
      probe(origin-axis*10.f);
      }
    }
  const Vec3 forward = facing(*pl);
  const Vec3 shoulder = Camera::aimShoulder(pl->position(),forward);
  Vec3 pivot, direction;
  camera->aimRay(pivot,direction);
  const bool wallAhead = world!=nullptr && world->physic()->ray(shoulder,shoulder+forward*85.f).hasCol;
  const Vec3 look = pl->aimLook();
  const Vec3 head = pl->mapHeadBone();
  const auto edge = pl->aimEdge(look);
  const auto headRay = pl->world().physic()->ray(head,head+look*200.f);
  const bool headClear = !headRay.hasCol || (edge.found && headRay.v.y<edge.supportY-100.f);
  event("aim-camera","\"shot\":"+jsonStr(shot)+",\"origin\":"+ragdollVector(origin)+",\"front\":"+ragdollVector(front)+
        ",\"camera_wall_hits\":"+std::to_string(wallHits)+",\"wall_ahead\":"+(wallAhead ? "true" : "false")+
        ",\"pivot_reach_cm\":"+ragdollNumber(Vec3::dotProduct(pivot-shoulder,forward))+
        ",\"feet\":"+ragdollVector(pl->position())+",\"head\":"+ragdollVector(head)+",\"stance_look\":"+ragdollVector(look)+
        ",\"lean_deg\":"+ragdollNumber(Aim::playerProbe.lean)+",\"stance_head_clear\":"+(headClear ? "true" : "false")+
        ",\"edge_distance_cm\":"+ragdollNumber(edge.found ? edge.distance : -1.f)+
        ",\"aim_point\":"+ragdollVector(aim)+",\"manual_aim_point\":"+ragdollVector(pl->manualAimPoint())+
        ",\"crosshair_point\":"+ragdollVector(crosshair)+",\"centered\":"+(centered ? "true" : "false")+
        ",\"pitch_deg\":"+ragdollNumber(pitch)+",\"draw\":"+ragdollNumber(Archer::playerProbe.draw)+",\"distance_cm\":"+ragdollNumber(distance)+
        ",\"body_clearance_cm\":"+ragdollNumber(clearance)+",\"body_closest_zone\":"+
        (capsules.empty() ? "null" : jsonStr(HitZones::name(capsules[closest].zone)))+
        ",\"body_closest_index\":"+(capsules.empty() ? "null" : std::to_string(closest))+
        ",\"body_closest_along_cm\":"+ragdollNumber(closestAlong*distance)+",\"body_hit_count\":"+std::to_string(hits)+
        ",\"body_hits\":"+intersections+equipment);
  }

bool cameraCommand(const CameraContext& ctx,const std::string& line,const std::vector<std::string>& a,
                   World& w,Npc& pl,PlayerControl& ctrl) {
  const auto& c=a[0];
  if(c=="monstercharacter" && a.size()>=2 && a[1]=="camera") {
    if((a.size()!=2 && a.size()!=4) || (a.size()==4 && (!isNumber(a[2]) || !isNumber(a[3])))) {
      monsterCharacterError("monster camera expects no arguments or yaw_degrees elevation_fraction");return true;
      }
    monsterCharacterCamera(ctx,w,pl,a.size()==4,a.size()==4?num(a,2,NAN):0,a.size()==4?num(a,3,NAN):.42f);
    }
  else if(c=="monstercharacter" && a.size()==5 && a[1]=="follow") {
    auto& route=ctx.monster.monsterCharacterRoute;
    auto* watched=route.npc;
    if(ctx.monster.monsterCharacterPack && ctx.monster.monsterCharacterExtras.size()>=2)watched=ctx.monster.monsterCharacterExtras[1].npc;
    if(!watched) {monsterCharacterError("follow requires prepared monster");return true;}
    exec("watch id:"+std::to_string(w.npcId(watched)),w,pl,ctrl);
    exec("cam follow "+a[2]+" "+a[3]+" "+a[4],w,pl,ctrl);
    exec("cam look "+ragdollNumber(std::min(130.f,std::max(45.f,(watched->bounds().bbox[1].y-watched->bounds().bbox[0].y)*.45f))),w,pl,ctrl);
    exec(route.name=="troll_barrel"?"view clean 500":"view clean 300",w,pl,ctrl);
    updateCamera(ctx,w,*watched);
    std::string actors="[{\"npc_id\":"+std::to_string(w.npcId(route.npc))+",\"role\":\"primary\",\"start_cm\":"+ragdollVector(route.start)+"}";
    for(const auto& actor:ctx.monster.monsterCharacterExtras)if(actor.npc && actor.npc!=route.npc)
      actors+=",{\"npc_id\":"+std::to_string(w.npcId(actor.npc))+",\"role\":"+jsonStr(actor.role)+",\"start_cm\":"+ragdollVector(actor.start)+"}";
    event("monster_character_camera","\"route\":"+jsonStr(route.name)+",\"mode\":\"follow\",\"fixed\":false,\"watch_npc_id\":"+std::to_string(w.npcId(watched))+
      ",\"distance_cm\":"+ragdollNumber(ctx.camera.camDist)+",\"height_cm\":"+ragdollNumber(ctx.camera.camHeight)+",\"angle_deg\":"+ragdollNumber(ctx.camera.camAngle)+
      ",\"look_height_cm\":"+ragdollNumber(ctx.camera.camLook)+",\"seed\":"+std::to_string(ctx.monster.monsterCharacterSeed)+",\"hour\":"+std::to_string(w.time().hour())+
      ",\"minute\":"+std::to_string(w.time().minute())+",\"hero_cm\":"+ragdollVector(ctx.monster.monsterCharacterHeroStart)+",\"actors\":"+actors+"]");
    monsterCharacterRenderProof(ctx,w,*watched,"followed");
    }
  else if(c=="cam") {
    const std::string m = a.size()>1 ? a[1] : "";
    ctx.camera.camAuto = false;
    if(m!="shoulder" && m!="face" && m!="two" && m!="smooth" && m!="arrow")
      ctx.camera.smValid = false;
    if(m=="game") {
      ctx.camera.cam = C_Game;
      // the game's camera as the player has it: without the near clip `view clean` gives the bench's shots
      // (130 cm after `cam follow 230`: the hero was clipped away, the aim camera's collision rays hit
      // the ground under him)
      if(auto cam = Gothic::inst().camera()) {
        cam->setMarvinMode(Camera::M_Normal);
        cam->setNearClip(10);
        }
      }
    else if(m=="follow" || m=="front" || m=="auto") {
      ctx.camera.cam       = C_Follow;
      ctx.camera.camDist   = num(a,2,220);
      ctx.camera.camHeight = num(a,3,40);
      ctx.camera.camAngle  = m=="front" ? 0 : num(a,4,90);
      if(m=="auto") {
        ctx.camera.camAuto = true;
        pickCamAngle(ctx,w,ctx.subjectOf(pl));
        }
      }
    else if(m=="world") {
      ctx.camera.cam       = C_World;
      ctx.camera.camDist   = num(a,2,220);
      ctx.camera.camHeight = num(a,3,40);
      ctx.camera.camAngle  = num(a,4,0);
      }
    else if(m=="fixed") {
      ctx.camera.cam = C_Fixed;
      }
    else if(m=="hold") {
      ctx.camera.cam = C_Hold;  // a shot of a scene: the subject may fall out of it, the frame stays
      }
    else if(m=="shoulder" || m=="two" || (m=="face" && a.size()>2 && !isNumber(a[2]))) {
      // cam shoulder <npc> [dist 120] [height 15] [shoulder 1|-1]
      // cam face <npc> [dist 140] [height 0] [degrees from the hero 30]
      // cam two <npc> [dist 250] [height 10] [side 1|-1]
      if(a.size()<3) {
        say("cam " + m + ": no npc");
        return true;
        }
      ctx.camera.cam       = m=="shoulder" ? C_Shoulder : (m=="face" ? C_NpcFace : C_Two);
      ctx.camera.camNpc    = a[2];
      ctx.camera.camDist   = num(a,3,m=="shoulder" ? 120 : (m=="face" ? 140 : 250));
      ctx.camera.camHeight = num(a,4,m=="shoulder" ? 15 : (m=="face" ? 0 : 10));
      ctx.camera.camAngle  = num(a,5,m=="face" ? 30 : 1);
      if(ctx.camera.camNpc!="arena" && npcBySymbol(w,ctx.camera.camNpc)==nullptr)
        say("cam " + m + ": no npc " + ctx.camera.camNpc);
      }
    else if(m=="arrow") {
      // cam arrow [dist 600] [height 40] [side 1|-1] [ahead 80]: beside the newest arrow's way (overlay trail
      // records it), from the hero until one flies; glides by `cam smooth`
      ctx.camera.cam       = C_Arrow;
      ctx.camera.camDist   = num(a,2,600);
      ctx.camera.camHeight = num(a,3,40);
      ctx.camera.camAngle  = num(a,4,1);
      ctx.camera.camLook   = num(a,5,80);
      }
    else if(m=="cut") {
      ctx.camera.smValid  = false;  // the next shot starts where it is, without moving there
      ctx.camera.cutPending = true; // and TAA starts its history over there (lookFrom)
      }
    else if(m=="smooth") {
      ctx.camera.camSmooth = std::clamp(num(a,2,0.12f),0.01f,1.f);
      }
    else if(m=="face") {
      // cam face <dist> [angle] [height]: close to the face of the watched one, looking at the mouth
      ctx.camera.cam       = C_Face;
      ctx.camera.camDist   = num(a,2,60);
      ctx.camera.camAngle  = num(a,3,0);
      ctx.camera.camHeight = num(a,4,0);
      ctx.camera.camLook   = 5;
      }
    else if(m=="look") {
      ctx.camera.camLook = num(a,2,45);
      }
    else if(m=="lift") {
      ctx.camera.camLift = !(a.size()>2 && a[2]=="off");
      }
    else {
      say("unknown cam mode: " + line);
      }
    }
  else if(c=="view") {
    const std::string m = a.size()>1 ? a[1] : "full";
    // Static and Movable objects share draw commands (same shaders), hiding them by type hides
    // parts of creatures too: what stands between the camera and the subject is clipped instead
    ctx.camera.clip = 0;
    DrawCommands::hiddenTypes    = 0;
    DrawCommands::hidePfxObjects = false;
    if(m=="clean") {
      ctx.camera.clip = num(a,2,100);
      DrawCommands::hiddenTypes = (1u<<DrawCommands::Pfx);
      }
    else if(m=="nopfx")
      DrawCommands::hiddenTypes = (1u<<DrawCommands::Pfx);
    else if(m=="hide") {
      // view hide static movable morph pfx ...
      DrawCommands::hiddenTypes = 0;
      for(size_t i=2; i<a.size(); ++i) {
        const std::string& t = a[i];
        if(t=="static")   DrawCommands::hiddenTypes |= (1u<<DrawCommands::Static);
        if(t=="movable")  DrawCommands::hiddenTypes |= (1u<<DrawCommands::Movable);
        if(t=="morph")    DrawCommands::hiddenTypes |= (1u<<DrawCommands::Morph);
        if(t=="pfx")      DrawCommands::hiddenTypes |= (1u<<DrawCommands::Pfx);
        if(t=="pfx")      DrawCommands::hidePfxObjects = true; // the particles of PfxObjects too
        if(t=="animated") DrawCommands::hiddenTypes |= (1u<<DrawCommands::Animated);
        }
      }
    if(ctx.camera.clip<=0)
      if(auto cam = Gothic::inst().camera())
        cam->setNearClip(10);
    }
  else if(c=="slowmo") {
    // slowmo <k>: the game runs k times slower than the video (1: normal); the game's sound is not recorded
    // while slowed (silence keeps the sound in step with the picture)
    ctx.camera.slow    = std::clamp(num(a,1,1),1.f,20.f);
    ctx.camera.slowAcc = 0;
    say("slowmo " + std::to_string(ctx.camera.slow));
    }
  else if(c=="track") {
    // track <npc[,npc...]|off> [head|body]: one ordinary track row per actor and recorded frame.
    ctx.camera.trackNpcs.clear();
    if(a.size()>1 && a[1]!="off") {
      size_t begin=0;
      while(begin<a[1].size()) {
        const size_t end=a[1].find(',',begin);
        const auto symbol=a[1].substr(begin,end==std::string::npos ? end : end-begin);
        if(!symbol.empty() && std::find(ctx.camera.trackNpcs.begin(),ctx.camera.trackNpcs.end(),symbol)==ctx.camera.trackNpcs.end())
          ctx.camera.trackNpcs.push_back(symbol);
        if(end==std::string::npos) break;
        begin=end+1;
        }
      }
    ctx.camera.trackHead = !(a.size()>2 && a[2]=="body");
    say("track " + (ctx.camera.trackNpcs.empty() ? std::string("off") : a[1]));
    }
  else if(c=="sight") {
    // sight <npc|off> [fan cm]: each recorded frame and shot, the hero's line of sight to it on the picture
    // (events.jsonl "sight"): the shared standing head, the current head, and the minimum lean that reveals
    // the target; the fan of the bow's focus down (FOCUS_RANGED elevdo)
    ctx.camera.sightNpc = a.size()>1 && a[1]!="off" ? a[1] : "";
    ctx.camera.sightFan = num(a,2,700);
    say("sight " + (ctx.camera.sightNpc.empty() ? std::string("off") : ctx.camera.sightNpc));
    }
  else if(c=="rec") {
    if(ctx.ragdollPerf && a.size()>1 && a[1].starts_with("twenty_") &&
       !arenaFullSchema(w,std::string_view(a[1]).substr(7))) {
      arenaAbort("refusing timing without twenty complete visible bodies"); return true;
      }
    stopRecording(ctx);
    ctx.camera.rec      = true;
    ctx.camera.recVideo = !(a.size()>2 && a[2]=="novideo");
    ctx.camera.recName  = a.size()>1 ? a[1] : "rec";
    ctx.camera.recFrame = 0;
    ctx.camera.recStart = ctx.time;
    ctx.anim1.clear();
    ctx.anim2.clear();
    ctx.camera.recWall  = std::chrono::steady_clock::now();
    if(ctx.camera.recVideo && SoundDevice::loopbackRate()>0) {
      ctx.camera.audio       = std::fopen((outDir+"/"+ctx.camera.recName+".wav").c_str(),"wb");
      ctx.camera.audioFrames = 0;
      if(ctx.camera.audio!=nullptr)
        writeWavHeader(ctx.camera.audio,SoundDevice::loopbackRate(),0);
      }
    event("rec","\"fps\":" + std::to_string(Fps) + ",\"audio\":" + (ctx.camera.audio!=nullptr ? "true" : "false"));
    say("rec " + ctx.camera.recName + (ctx.camera.recVideo ? "" : " (metrics only)") + (ctx.camera.audio!=nullptr ? " with sound" : ""));
    }
  else if(c=="stop") {
    if(a.size()==2 && a[1]=="shadow") {
      // Preserve each duel before the ordinary 8 MiB shadow journal rotates again.
      for(const auto* suffix:{".jsonl",".previous.jsonl"}) {
        const std::string source="melee-blade-shadow"+std::string(suffix);
        if(std::filesystem::exists(source))
          std::filesystem::copy_file(source,outDir+"/"+ctx.camera.recName+"-"+source,
                                     std::filesystem::copy_options::overwrite_existing);
        }
      }
    stopRecording(ctx);
    }
  else if(c=="shot") {
    ctx.camera.shot = a.size()>1 ? a[1] : "shot";
    }
  else if(c=="views") {
    // views <npc> [count] [max dist] [from <min dist>] [shoot <prefix>]: where a camera sees the hero (the top of
    // the head, the chest: a wall's top may hide the legs) and the npc in one frame, from open air (nothing within
    // 40 cm, a clear line to the hero's head: the same room), as `cam lift off`, `cam follow <dist> <height>
    // <angle>`, `cam look <look>` (halfway between them) put it; the nearest of each side first, no nearer than
    // `from`. For a scene on a roof, in a hall or under a gallery, where a camera put by hand lands in a wall
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("views: no npc " + line);
      return true;
      }
    const size_t count = size_t(num(a,2,10));
    const float  maxD  = num(a,3,900);
    float        minD  = 150;
    std::string  prefix;
    for(size_t i=4; i+1<a.size(); i+=2) {
      if(a[i]=="from")
        minD = num(a,i+1,150);
      else if(a[i]=="shoot")
        prefix = a[i+1];
      }
    const Vec3   p     = pl.position();
    const Vec3   top   = pl.mapHeadBone() + Vec3(0,20,0);
    const Vec3   chest = pl.mapHeadBone() - Vec3(0,50,0);
    const Vec3   tg    = n->centerPosition();
    const float  look  = std::round(((top.y+tg.y)*0.5f - p.y)/10.f)*10.f;
    const Vec3   at    = Vec3(p.x,p.y+look,p.z);
    auto         cam   = Gothic::inst().camera();
    const float  half  = (cam!=nullptr ? cam->fovY() : 67.5f)*0.5f*0.9f; // a margin at the frame's edge
    const Vec3   f     = facing(pl);
    const Vec3   sd    = {-f.z,0,f.x};
    auto         clear = [&](const Vec3& x, const Vec3& y) { return !w.physic()->ray(x,y).hasCol; };
    struct View { float dist, height, angle, spread; };
    std::vector<View> found;
    for(float dist=minD; dist<=maxD; dist+=50)
      for(float ang=-165; ang<=180; ang+=15)
        for(float hgt=-700; hgt<=700; hgt+=50) {
          const float b   = ang*float(M_PI)/180.f;
          const Vec3  org = p + (f*std::cos(b) + sd*std::sin(b))*dist + Vec3(0,hgt,0);
          if(!clear(top,org) || !clear(org,chest) || !clear(org,tg))
            continue;
          bool open = true;
          for(const Vec3 d:{Vec3(40,0,0),Vec3(-40,0,0),Vec3(0,40,0),Vec3(0,-40,0),Vec3(0,0,40),Vec3(0,0,-40)})
            open = open && clear(org,org+d);
          if(!open)
            continue;
          // up and down from where it looks: both within the field of view
          const Vec3 v = Vec3::normalize(at-org);
          auto angTo = [&](const Vec3& q) {
            return std::acos(std::clamp(Vec3::dotProduct(v,Vec3::normalize(q-org)),-1.f,1.f))*180.f/float(M_PI);
            };
          const float s = std::max({angTo(top),angTo(chest),angTo(tg)});
          if(s>half)
            continue;
          found.push_back({dist,hgt,ang,s});
          }
    std::sort(found.begin(),found.end(),[](const View& x, const View& y) {
      return x.dist!=y.dist ? x.dist<y.dist : std::abs(x.height)<std::abs(y.height);
      });
    char buf[200];
    std::snprintf(buf,sizeof(buf),"views %s: %zu spots, cam look %.0f; field of view %.0f deg",symName(w,n).c_str(),
                  found.size(),double(look),double(half*2.f/0.9f));
    say(buf);
    // the nearest of each side (angle): different shots, not ten of the same; `shoot <prefix>`: a shot from each
    // goes into the scenario right after this line
    const bool         shoot  = !prefix.empty();
    std::vector<float> sides;
    std::vector<std::string> shots;
    for(auto& v:found) {
      if(sides.size()>=count || std::find(sides.begin(),sides.end(),v.angle)!=sides.end())
        continue;
      sides.push_back(v.angle);
      std::snprintf(buf,sizeof(buf),"  cam follow %.0f %.0f %.0f  (both within %.0f deg of the middle)",
                    double(v.dist),double(v.height),double(v.angle),double(v.spread));
      say(buf);
      if(!shoot)
        continue;
      char cmd[120];
      std::snprintf(cmd,sizeof(cmd),"cam follow %.0f %.0f %.0f",double(v.dist),double(v.height),double(v.angle));
      shots.push_back(cmd);
      shots.push_back("cam look " + std::to_string(int(look)));
      shots.push_back("wait 300");
      std::snprintf(cmd,sizeof(cmd),"shot %s%.0f_%.0f_%.0f",prefix.c_str(),double(v.dist),double(v.height),double(v.angle));
      shots.push_back(cmd);
      shots.push_back("wait 100");
      }
    if(!shots.empty()) {
      shots.insert(shots.begin(),"cam lift off");
      ctx.prog.insert(ctx.prog.begin()+std::ptrdiff_t(ctx.pc),shots.begin(),shots.end());
      }
    }
  else if(c=="place") {
    // place <dist> <angle>: the hero stands that far from the watched npc, at that angle from its facing;
    // place <dist> <angle> <npc|nearest>: that npc stands so around the hero, facing him, the hero stays
    if(a.size()>3) {
      Npc* n = findNpc(w,pl,a[3]);
      if(n==nullptr || n==&pl) {
        say("place: no npc " + line);
        return true;
        }
      standAround(w,pl,*n,num(a,1,150),num(a,2,0));
      say("place " + symName(w,n) + " " + a[1] + " " + a[2]);
      return true;
      }
    ctx.camera.placeDist  = num(a,1,150);
    ctx.camera.placeAngle = num(a,2,0);
    placeHero(ctx,w,pl);
    ctx.hasPrev = false;
    }
  else if(c=="orbit") {
    // orbit <deg> <ms>: the hero goes around the watched npc
    const float ms = std::max(1.f,num(a,2,3000));
    ctx.camera.orbitSpeed = num(a,1,90)*1000.f/ms;
    ctx.camera.orbitUntil = ctx.time + uint64_t(ms);
    }
  else {
    return false;
    }
  return true;
  }

void capture(const CameraContext& ctx,Pixmap& frame, float uiScale) {
  const auto waterFilmFraming=ctx.waterFilmFraming;
  ctx.overlay.uiScale = uiScale;
  Pixmap pm;
  if(frame.w()>0 && frame.format()!=TextureFormat::RGBA8)
    pm = Pixmap(frame,TextureFormat::RGBA8); else
    pm = std::move(frame);

  if(pm.w()>0)
    if(auto* world=Gothic::inst().world())waterFilmFraming(*world,pm.w(),pm.h());

  if(pm.w()>0 && ctx.overlay.overlay)
    drawOverlay(ctx.overlayContext,pm);
  if(pm.w()>0 && ctx.camera.cam==C_Two && ctx.camera.camNpc=="arena")
    drawArenaMark(pm);
  if(pm.w()>0 && ctx.living.livingDebug) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    if(!ctx.overlay.overlay) drawZones(ctx.overlayContext,cv,Gothic::inst().camera());
    const auto counts=livingCounts();char label[200];
    cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});
    cv.text(6,6,"LIVING BODIES: "+ctx.living.livingPhase,2,Text);
    std::snprintf(label,sizeof(label),"LIVE %zu  NATIVE KIN %zu / %zu PARTS  DYNAMIC %zu",counts.alive,counts.kinematic,counts.parts,counts.dynamic);
    cv.text(6,26,label,2,Text);cv.text(6,46,"OVERLAY = GAME HIT ZONES (NOT NATIVE SHAPES)",2,Text);
    }
  if(pm.w()>0 && ctx.archery.arrowTrace && ctx.archery.arrowNpc!=nullptr) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    const auto& npc=*ctx.archery.arrowNpc;
    const auto* body=npc.livingBody()!=nullptr ? npc.livingBody() : npc.ragdollBody();
    const auto& hit=npc.arrowPhysicsProbe();
    const auto* skeleton=npc.visualSkeleton();
    const auto bone=skeleton!=nullptr && hit.bone<skeleton->nodes.size() ?
                    std::string_view(skeleton->nodes[hit.bone].name) : std::string_view("-");
    const char* mode=body==nullptr ? "NONE" : (body->isKinematic() ? "KINEMATIC" :
                     (body->reactionWeight()>0.f ? "ACTIVE MOTORS" : "LIMP"));
    char label[200];
    cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});
    cv.text(6,6,"ARROW PHYSICS: "+ctx.archery.arrowCase,2,Text);
    std::snprintf(label,sizeof(label),"NATIVE %s  WEIGHT %.3f  PARTS %zu",mode,
                  double(body!=nullptr ? body->reactionWeight() : 0.f),body!=nullptr ? body->partCount() : 0);
    cv.text(6,26,label,2,Text);
    std::snprintf(label,sizeof(label),"HIT %.*s  IMPULSE %.2f NS",int(std::min<size_t>(bone.size(),38)),bone.data(),double(hit.impulseNs.length()));
    cv.text(6,46,label,2,Text);
    }
  if(pm.w()>0 && ctx.archery.stickingTrace) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    const auto* npc=ctx.archery.arrowNpc;auto* world=Gothic::inst().world();
    float error=0;
    if(npc)
      for(size_t i=0;i<npc->stuckArrowCount();++i) {
        Npc::ArrowPinProbe pin;
        if(!npc->stuckArrowProbe(i,pin)) continue;
        const auto expected=npc->renderedBone(pin.bone)*pin.local;
        error=std::max(error,Vec3(pin.world.at(3,0)-expected.at(3,0),pin.world.at(3,1)-expected.at(3,1),pin.world.at(3,2)-expected.at(3,2)).length());
        }
    char label[200];cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});
    cv.text(6,6,"ARROW STICKING: "+ctx.archery.stickingCase,2,Text);
    std::snprintf(label,sizeof(label),"ACTUAL DRAW PINS  NPC %zu  WORLD %zu  ERROR %.4f CM",npc?npc->stuckArrowCount():0,
      world?world->stuckArrowCount():0,double(error));
    cv.text(6,26,label,2,Text);
    std::snprintf(label,sizeof(label),"BONE LOCAL -> RENDER POSE  %s  NATIVE BODIES %zu",npc && npc->isDown()?"LIMP":"LIVING",
      world?world->physic()->physicsStats().bodies:0);
    cv.text(6,46,label,2,Text);
    }
  if(pm.w()>0 && ctx.movables.spellTrace) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    auto* world=Gothic::inst().world();char label[200];
    cv.rect(0,0,std::min(cv.w,790),66,{0,0,0,190});cv.text(6,6,"SPELL PHYSICS: "+ctx.movables.spellCase,2,Text);
    std::snprintf(label,sizeof(label),"REAL CAST %d  FIELDS %zu  NATIVE BODIES %zu",ctx.movables.spellCastingId,
      world?world->spellFieldCount():0,world?world->physic()->physicsStats().bodies:0);cv.text(6,26,label,2,Text);
    if(!ctx.movables.spellActors.empty()) {
      const auto& npc=*ctx.movables.spellActors[0].npc;
      std::snprintf(label,sizeof(label),"HUMAN %.0f KG  SPEED %.0f CM/S  FROZEN %d  PROPS %zu",double(npc.physicsMass()),
        double(npc.spellVelocity().length()),int(npc.spellFrozen()),ctx.movables.spellItems.size());cv.text(6,46,label,2,Text);
      }
    }
  if(pm.w()>0 && ctx.routine.questTrace && ctx.routine.questNpc!=nullptr) {
    Canvas cv;cv.px=reinterpret_cast<uint8_t*>(pm.data());cv.w=int(pm.w());cv.h=int(pm.h());
    cv.rect(0,0,std::min(cv.w,640),66,{0,0,0,190});
    cv.text(6,6,ctx.routine.questCase,2,Text);
    const auto& npc=*ctx.routine.questNpc;const auto& q=npc.ragdollQuestReturnState();char label[160];
    std::snprintf(label,sizeof(label),"IMMORTAL %d GUARD %d KO %d RETURN %u",int(npc.isImmortal()),
      Gothic::settingsGetI("GAME","ragdoll_quest_guard"),int(npc.isUnconscious()),npc.ragdollQuestReturnCount());
    cv.text(6,26,label,2,Text);
    std::snprintf(label,sizeof(label),"REASON %s DROP %.0f CM",questReason(q.hazards),double(ctx.routine.questStable.y-npc.position().y));
    cv.text(6,46,label,2,Text);
    }
  // This runs after the final pose and camera, independently of sight/track overlays.
  // A simultaneous shot and video frame share one sample with the current recording index.
  if(pm.w()>0 && (ctx.camera.rec || !ctx.camera.shot.empty()))
    aimCameraFrame(ctx,ctx.camera.shot);
  if(!ctx.camera.shot.empty()) {
    if(pm.w()>0 && !ctx.camera.sightNpc.empty())
      sightFrame(ctx,int(pm.w()),int(pm.h()),ctx.camera.shot);
    if(pm.w()>0)
      pm.save((outDir+"/"+ctx.camera.shot+".png").c_str());
    say("shot " + ctx.camera.shot);
    ctx.camera.shot.clear();
    ctx.ordered=false;
    benchTexturesFrozen=false;
    }
  if(ctx.camera.rec && ctx.camera.recVideo) {
    if(!ctx.camera.trackNpcs.empty() && pm.w()>0)
      trackFrame(ctx,int(pm.w()),int(pm.h()));
    if(!ctx.camera.sightNpc.empty() && pm.w()>0)
      sightFrame(ctx,int(pm.w()),int(pm.h()),"");
    writeVideo(ctx.overlayContext,pm);
    ++ctx.camera.recFrame;
    }
  }

}
