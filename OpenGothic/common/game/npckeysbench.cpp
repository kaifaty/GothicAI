#include "npckeysbench.h"
#include "npckeys.h"
#include "navigationeye.h"
#include "playercontrol.h"

#include "game/gamescript.h"
#include "physics/dynamicworld.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "gothic.h"
#include "settings.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
using Tempest::Vec3;
constexpr float Pi=3.14159265358979323846f;
constexpr std::array<KeyCodec::Action,7> HeroKeys={KeyCodec::Forward,KeyCodec::Back,
    KeyCodec::Left,KeyCodec::Right,KeyCodec::RotateL,KeyCodec::RotateR,KeyCodec::Jump};

struct State {
  struct NativeTarget {
    Npc* npc=nullptr;
    int64_t handleId=0;
    std::string label;
  };
  std::string directory,scene,anchor,trace;
  Vec3 root,direction,goal;
  Vec3 voidApproach;
  float voidStartBack=0,voidEdgeBack=0;
  float flatMinimumHeight=0,flatMaximumHeight=0;
  size_t flatProbeCount=0;
  Npc::JumpStatus climbProbe;
  Vec3 climbFace;
  WayPoint destination;
  std::vector<Npc*> actors;
  std::vector<NativeTarget> native;
  bool prepared=false,stress=false,removedRelaxedOverlay=false;
  uint64_t heroUntil=0,stressBegan=0,stressNext=0,stressCommands=0,random=1;
  uint64_t sampleDt=0;
  uint64_t frames=0,invalidRoots=0;
  std::vector<Vec3> previous;
  std::vector<double> travelled;
  uint64_t requestedMotionFrames=0,stillMotionFrames=0,nearPeerFrames=0;
  float maximumFrameTravel=0;
  float minimumPeerDistance=1000000;
} state;

std::string quote(const std::string& value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.String(value.data(),rapidjson::SizeType(value.size()));
  return {buffer.GetString(),buffer.GetSize()};
}

std::string number(double value) {
  if(!std::isfinite(value)) return "null";
  std::ostringstream out;
  out << std::setprecision(10) << value;
  return out.str();
}

bool finiteVector(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::string vector(const Vec3& value) {
  return '['+number(value.x)+','+number(value.y)+','+number(value.z)+']';
}

void append(const char* file,const std::string& json) {
  std::ofstream out(state.directory+"/"+file,std::ios::app);
  if(!out) throw std::runtime_error(std::string("cannot open keys journal ")+file);
  out << json << '\n';
  if(!out) throw std::runtime_error(std::string("cannot write keys journal ")+file);
}

bool floorAt(World& world,const Vec3& at,Vec3& floor,float depth=600.f) {
  const auto hit=world.physic()->landRay(at+Vec3(0,180,0),depth);
  if(!hit.hasCol || (hit.n.quadLength()>.01f && hit.n.y<.65f)) return false;
  floor=hit.v;
  return !world.physic()->ray(floor+Vec3(0,25,0),floor+Vec3(0,180,0)).hasCol;
}

bool lane(World& world,const Vec3& from,const Vec3& to,float tolerance=35.f) {
  Vec3 previous=from;
  for(unsigned i=0;i<=20;++i) {
    Vec3 floor;
    if(!floorAt(world,from+(to-from)*(float(i)/20.f),floor) || std::abs(floor.y-previous.y)>tolerance) return false;
    if(i && world.physic()->ray(previous+Vec3(0,110,0),floor+Vec3(0,110,0)).hasCol) return false;
    previous=floor;
  }
  return true;
}

bool parkingFloor(World& world,const Vec3& preferred,const Vec3& root,const Vec3& goal,Vec3& floor,
                  const std::vector<Vec3>& occupied={}) {
  const auto separate=[&](const Vec3& candidate) {
    for(const auto& other:occupied) if((candidate-other).length()<120.f) return false;
    return true;
  };
  // Keep successful existing fixtures on precisely their original positions.
  if(floorAt(world,preferred,floor) && separate(floor)) return true;
  const auto delta=goal-root;
  const float lengthSquared=delta.x*delta.x+delta.z*delta.z;
  const auto awayFromRoute=[&](const Vec3& candidate) {
    const float fraction=lengthSquared>.01f ? std::clamp(((candidate.x-root.x)*delta.x+
        (candidate.z-root.z)*delta.z)/lengthSquared,0.f,1.f) : 0.f;
    const auto closest=root+delta*fraction;
    const float dx=candidate.x-closest.x,dz=candidate.z-closest.z;
    return dx*dx+dz*dz>150.f*150.f;
  };
  for(float radius:{250.f,325.f,400.f,475.f,550.f,625.f,700.f,775.f,800.f})
    for(unsigned angle=0;angle<24;++angle) {
      const float radians=float(angle)*Pi/12;
      const auto candidate=root+Vec3(std::cos(radians)*radius,0,std::sin(radians)*radius);
      if(!floorAt(world,candidate,floor) || std::abs(floor.y-root.y)>150.f ||
         !separate(floor) || !awayFromRoute(floor)) continue;
      bool clear=true;
      for(const auto offset:{Vec3(40,0,0),Vec3(-40,0,0),Vec3(0,0,40),Vec3(0,0,-40)})
        if(world.physic()->ray(floor+Vec3(0,100,0),floor+Vec3(0,100,0)+offset).hasCol) {clear=false;break;}
      if(clear) return true;
    }
  return false;
}

void freeze(Npc& npc) {
  npc.setInteraction(nullptr,true);npc.clearAiQueue();npc.clearState(false);
  npc.clearGoTo();npc.stopWalking();npc.setExternalControl(true);
}

Npc* liveNative(World& world,const State::NativeTarget& target) {
  // A stock NPC may be removed by its script between commands. Compare addresses
  // before dereferencing, then verify its script handle if an address was reused.
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc==target.npc && npc->handle().id==target.handleId) return npc;
  return nullptr;
}

Npc& actor(World& world,const std::string& selector) {
  const auto digits=[](const std::string& value) {
    return !value.empty() && std::all_of(value.begin(),value.end(),[](char c){return c>='0' && c<='9';});
  };
  if(digits(selector)) {
    const auto id=std::stoull(selector);
    if(id>=state.actors.size()) throw std::runtime_error("keys fixture actor index out of range");
    return *state.actors[size_t(id)];
  }
  Npc* npc=nullptr;
  if(selector.starts_with("id:")) {
    const auto value=selector.substr(3);
    if(!digits(value)) throw std::runtime_error("keys id selector requires an unsigned world NPC index");
    const auto id=std::stoull(value);
    if(id<=std::numeric_limits<uint32_t>::max()) npc=world.npcById(uint32_t(id));
  } else {
    const auto symbol=world.script().findSymbolIndex(selector);
    if(symbol!=size_t(-1)) npc=world.findNpcByInstance(symbol);
  }
  if(!npc) throw std::runtime_error("keys NPC unavailable: "+selector);
  if(npc==world.player()) throw std::runtime_error("keys hero inputs use the hero selector and PlayerControl");
  npc->setCharacterVirtualDiagnosticsEnabled(state.scene!="market");
  if(std::find(state.actors.begin(),state.actors.end(),npc)==state.actors.end()) {
    bool tracked=false;
    for(const auto& target:state.native)
      tracked|=target.npc==npc && target.handleId==npc->handle().id;
    if(!tracked) state.native.push_back({npc,npc->handle().id,selector});
  }
  return *npc;
}

void releaseHero(PlayerControl& control) {
  for(const auto key:HeroKeys) control.onKeyReleased(key,KeyCodec::Mapping::Primary);
  state.heroUntil=0;
}

void heroKeys(Npc& player,PlayerControl& control,const NpcKeys::KeyState& keys,uint64_t ttl) {
  releaseHero(control);
  player.setCharacterVirtualDiagnosticsEnabled(state.scene!="market");
  const auto press=[&](KeyCodec::Action action) {
    control.onKeyPressed(action,Tempest::KeyEvent::K_NoKey,KeyCodec::Mapping::Primary);
  };
  player.setWalkMode(keys.walk ? WalkBit::WM_Walk : WalkBit::WM_Run);
  if(keys.forward>0) press(KeyCodec::Forward);
  if(keys.forward<0) press(KeyCodec::Back);
  if(keys.strafe>0) press(KeyCodec::Right);
  if(keys.strafe<0) press(KeyCodec::Left);
  if(keys.turn>0) press(KeyCodec::RotateR);
  if(keys.turn<0) press(KeyCodec::RotateL);
  if(keys.jump || keys.climb) press(KeyCodec::Jump);
  // The hero path deliberately uses real PlayerControl inputs. The executor's
  // independent head channel is checked on NPCs with NavigationEye below.
  state.heroUntil=player.world().tickCount()+ttl;
}

std::string supportRay(World& world,const Vec3& root,float above=50.f,float below=500.f) {
  if(!finiteVector(root)) return "null";
  // The production land query excludes NPC ellipsoids and tests terrain plus
  // walking-collision objects. A miss has no invented position or height.
  const auto hit=world.physic()->landRay(root+Vec3(0,above,0),above+below);
  if(!hit.hasCol || !finiteVector(hit.v)) return "null";
  return "{\"point_cm\":"+vector(hit.v)+",\"normal\":"+vector(hit.n)+
      ",\"drop_cm\":"+number(root.y-hit.v.y)+"}";
}

std::string diagnosticBody(const CharacterVirtualDiagnostics::Body& body) {
  if(!body.present) return "null";
  return "{\"id\":"+std::to_string(body.id)+",\"layer\":"+std::to_string(body.layer)+
      ",\"pointer_type\":"+std::to_string(body.pointerType)+",\"motion_type\":"+std::to_string(body.motionType)+
      ",\"label\":"+quote(body.label)+",\"mass_kg\":"+number(body.mass)+
      ",\"position_cm\":"+vector(body.position)+",\"point_velocity_cm_s\":"+vector(body.pointVelocity)+"}";
}

std::string diagnosticContacts(const CharacterVirtualDiagnostics::Pose& pose) {
  std::string json="[";
  const auto flag=[](bool value){return value?"true":"false";};
  for(const auto& contact:pose.contacts) {
    if(json.size()>1) json+=',';
    json+="{\"body\":"+diagnosticBody(contact.body)+",\"subshape\":"+std::to_string(contact.subShape)+
        ",\"point_cm\":"+vector(contact.position)+",\"normal\":"+vector(contact.contactNormal)+
        ",\"surface_normal\":"+vector(contact.surfaceNormal)+",\"point_velocity_cm_s\":"+vector(contact.pointVelocity)+
        ",\"distance_cm\":"+number(contact.distance)+",\"supporting_volume_distance_cm\":"+number(contact.supportingVolumeDistance)+
        ",\"had_collision\":"+flag(contact.hadCollision)+",\"discarded\":"+flag(contact.discarded)+
        ",\"sensor\":"+flag(contact.sensor)+",\"can_push_character\":"+flag(contact.canPushCharacter)+
        ",\"within_supporting_volume\":"+flag(contact.withinSupportingVolume)+
        ",\"walkable_slope\":"+flag(contact.walkableSlope)+",\"support_allowed_by_policy\":"+flag(contact.supportAllowedByPolicy)+
        ",\"support_eligible\":"+flag(contact.supportEligible)+"}";
  }
  return json+']';
}

std::string frame(World& world,Npc& npc,const std::string& label) {
  const auto goal=npc.navigationGoal();
  const auto status=NpcKeys::status(npc);
  const auto eye=npc.navigationEyePose();
  const auto physical=NpcKeys::physicalPosition(npc);
  CharacterVirtualDiagnostics diagnostics;
  const bool validDiagnostics=npc.characterVirtualActive() && npc.characterVirtualDiagnostics(diagnostics) && diagnostics.valid;
  const auto& pose=diagnostics.afterStairs;
  std::ostringstream out;
  out << std::setprecision(10) << "{\"actor\":" << quote(label) << ",\"npc_id\":" << world.npcId(&npc)
      << ",\"handle_id\":" << npc.handle().id
      << ",\"root_cm\":" << vector(npc.position()) << ",\"npc_root_cm\":" << vector(npc.position())
      << ",\"physical_root_cm\":" << vector(physical) << ",\"physics_root_cm\":" << vector(physical)
      << ",\"physical_root_source\":\"npc_physics\",\"physical_root_valid\":" << (finiteVector(physical)?"true":"false")
      << ",\"visual_root_cm\":" << vector(npc.characterVisualPosition()) << ",\"body_yaw_deg\":" << number(npc.rotation())
      << ",\"frame_dt_ms\":" << state.sampleDt
      << ",\"anim_root_local_cm\":" << (state.sampleDt?vector(npc.animMoveSpeed(state.sampleDt)):"null")
      << ",\"support_probe_above_cm\":50,\"support_probe_below_cm\":500"
      << ",\"support_physical\":" << supportRay(world,physical) << ",\"support_npc\":" << supportRay(world,npc.position())
      << ",\"cv_diagnostics_valid\":" << (validDiagnostics?"true":"false")
      << ",\"cv_root_cm\":" << (validDiagnostics?vector(pose.position):"null")
      << ",\"cv_position_cm\":" << (validDiagnostics?vector(pose.position):"null")
      << ",\"cv_native_position_cm\":" << (validDiagnostics?vector(pose.position):"null")
      << ",\"cv_accepted_cm\":" << (validDiagnostics?vector(diagnostics.npcMove.accepted):"null")
      << ",\"cv_velocity_cm_s\":" << (validDiagnostics?vector(pose.velocity):"null")
      << ",\"cv_ground_state\":" << (validDiagnostics?std::to_string(pose.groundState):"null")
      << ",\"cv_ground_position_cm\":" << (validDiagnostics?vector(pose.groundPosition):"null")
      << ",\"cv_ground_normal\":" << (validDiagnostics?vector(pose.groundNormal):"null")
      << ",\"cv_ground_velocity_cm_s\":" << (validDiagnostics?vector(pose.groundVelocity):"null")
      << ",\"cv_ground_body\":" << (validDiagnostics?diagnosticBody(pose.ground):"null")
      << ",\"cv_contacts\":" << (validDiagnostics?diagnosticContacts(pose):"null")
      << ",\"cv_before_cm\":" << (validDiagnostics?vector(diagnostics.before.position):"null")
      << ",\"cv_before_ground_state\":" << (validDiagnostics?std::to_string(diagnostics.before.groundState):"null")
      << ",\"cv_before_ground_normal\":" << (validDiagnostics?vector(diagnostics.before.groundNormal):"null")
      << ",\"cv_after_extended_cm\":" << (validDiagnostics?vector(diagnostics.afterExtendedUpdate.position):"null")
      << ",\"cv_dt_ms\":" << (validDiagnostics?std::to_string(diagnostics.dt):"null")
      << ",\"cv_initial_cm\":" << (validDiagnostics?vector(diagnostics.npcMove.initial):"null")
      << ",\"cv_requested_cm\":" << (validDiagnostics?vector(diagnostics.npcMove.requested):"null")
      << ",\"cv_native_requested_cm\":" << (validDiagnostics?vector(diagnostics.requested):"null")
      << ",\"cv_clipped_request_cm\":" << (validDiagnostics?vector(diagnostics.npcMove.clippedRequest):"null")
      << ",\"cv_request_clipped\":" << (validDiagnostics?(diagnostics.npcMove.requestClipped?"true":"false"):"null")
      << ",\"cv_resolved_clipped\":" << (validDiagnostics?(diagnostics.npcMove.resolvedClipped?"true":"false"):"null")
      << ",\"cv_blocked\":" << (validDiagnostics?(diagnostics.npcMove.blocked?"true":"false"):"null")
      << ",\"animations\":" << quote(npc.animNames()) << ",\"body_state\":" << uint32_t(npc.bodyState())
      << ",\"walk\":" << ((npc.walkMode()&WalkBit::WM_Walk)!=WalkBit::WM_Run?"true":"false")
      << ",\"air\":" << (npc.isInAir()?"true":"false") << ",\"falling\":" << (npc.isFalling()?"true":"false")
      << ",\"jumping\":" << ((npc.isJump() || npc.isJumpUp())?"true":"false")
      << ",\"npc_cv_flag\":" << (Settings::getI("GAME","npc_character_virtual")?"true":"false")
      << ",\"cv_active\":" << (npc.characterVirtualActive()?"true":"false")
      << ",\"keys_enabled\":" << (status.enabled?"true":"false") << ",\"has_command\":" << (status.hasCommand?"true":"false")
      << ",\"remaining_ms\":" << status.remainingMs << ",\"forward\":" << status.keys.forward
      << ",\"strafe\":" << status.keys.strafe << ",\"turn\":" << status.keys.turn
      << ",\"head_yaw_deg\":" << status.keys.headYaw << ",\"head_pitch_deg\":" << status.keys.headPitch
      << ",\"eye_forward\":" << vector(eye.forward) << ",\"goal_active\":" << (goal.active?"true":"false")
      << ",\"goal_cm\":" << vector(goal.position) << ",\"goal_kind\":" << quote(std::string(goal.kind)) << '}';
  return out.str();
}

std::string sample(World& world,Npc& player,const std::string& tag,const char* kind="keys_sample") {
  std::string json="{\"kind\":"+quote(kind)+",\"tag\":"+quote(tag)+",\"game_ms\":"+std::to_string(world.tickCount())+
      ",\"actors\":["+frame(world,player,"hero");
  for(size_t i=0;i<state.actors.size();++i) json+=","+frame(world,*state.actors[i],std::to_string(i));
  for(const auto& target:state.native)
    if(auto* npc=liveNative(world,target)) json+=","+frame(world,*npc,target.label);
  json+="]}";
  append("keys-frames.jsonl",json);
  return json;
}

std::string animationInfo(World& world,Npc& npc,const std::string& label) {
  std::ostringstream out;
  out << "{\"actor\":" << quote(label) << ",\"npc_id\":" << world.npcId(&npc)
      << ",\"handle_id\":" << npc.handle().id << ",\"guild\":" << npc.guild()
      << ",\"animations\":{ ";
  size_t index=0;
  for(const char* name:{"S_WALKL","S_RUNL","T_WALKWSTRAFEL","T_WALKWSTRAFER",
                       "T_RUNSTRAFEL","T_RUNSTRAFER","T_WALKB","T_RUNB","S_JUMP",
                       "T_JUMPUPLOW","T_JUMPUPMID","T_JUMPUP"}) {
    if(index++) out << ',';
    out << quote(name) << ':' << quote(npc.animInfo(name));
  }
  out << "},\"overlays\":{";
  index=0;
  for(const char* name:{"Humans_Relaxed.mds","HUMANS_MILITIA.MDS","HUMANS_MAGE.MDS",
                       "HUMANS_MERCENARY.MDS","HUMANS_TIRED.MDS","HUMANS_DRUNKEN.MDS",
                       "HUMANS_ARROGANCE.MDS","HUMANS_SPRINT.MDS","HUMANS_FLEE.MDS",
                       "HUMANS_FAT.MDS","HUMANS_BABE.MDS"}) {
    if(index++) out << ',';
    out << quote(name) << ':' << (npc.hasOverlay(name)?"true":"false");
  }
  out << "}}";
  return out.str();
}

std::string info(World& world,Npc& player) {
  std::string json="{\"kind\":\"keys_info\",\"game_ms\":"+std::to_string(world.tickCount())+
      ",\"actors\":["+animationInfo(world,player,"hero");
  for(size_t i=0;i<state.actors.size();++i) json+=","+animationInfo(world,*state.actors[i],std::to_string(i));
  for(const auto& target:state.native)
    if(auto* npc=liveNative(world,target)) json+=","+animationInfo(world,*npc,target.label);
  json+="]}";
  append("keys-events.jsonl",json);return json;
}

const char* jumpName(Npc::Anim animation) {
  switch(animation) {
    case Npc::Anim::JumpUpLow: return "JumpUpLow";
    case Npc::Anim::JumpUpMid: return "JumpUpMid";
    case Npc::Anim::JumpUp: return "JumpUp";
    case Npc::Anim::Jump: return "Jump";
    default: return "other";
  }
}

bool climbable(const Npc::JumpStatus& probe) {
  return !probe.noClimb && (probe.anim==Npc::Anim::JumpUpLow ||
         probe.anim==Npc::Anim::JumpUpMid || probe.anim==Npc::Anim::JumpUp);
}

bool selectObstacle(World& world,Npc& player,const std::string& scene) {
  struct RestoreHero {
    Npc& player;
    Vec3 root;
    float rotation;
    bool moved=false;
    ~RestoreHero() {
      if(moved) {player.setPosition(root);player.setDirection(rotation);player.updateTransform();}
    }
  } restore{player,player.position(),player.rotation()};
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    if(!point.underWater && point.name.starts_with("NW_CITY_")) points.push_back(&point);
  });
  std::sort(points.begin(),points.end(),[](const auto* a,const auto* b){return a->name<b->name;});
  auto* physics=world.physic();
  for(const auto* point:points) {
    Vec3 center;
    if(!floorAt(world,point->position(),center)) continue;
    for(unsigned angle=0;angle<24;++angle) {
      const Vec3 direction(std::cos(float(angle)*Pi/12),0,std::sin(float(angle)*Pi/12));
      if(scene=="climb") {
        // Same real city ledge preconditions as character route jump_ledge.
        for(float distance:{150.f,200.f,250.f}) {
          const auto top=physics->landRay(center+direction*distance+Vec3(0,210,0),250);
          if(!top.hasCol || top.n.y<.8f || top.v.y-center.y<55 || top.v.y-center.y>110) continue;
          if(physics->ray(center+Vec3(0,180,0),top.v+Vec3(0,180,0)).hasCol) continue;
          Vec3 beyond;
          if(!floorAt(world,top.v+direction*80.f,beyond) || std::abs(beyond.y-top.v.y)>15) continue;
          const auto face=physics->ray(center+Vec3(0,35,0),Vec3(top.v.x,center.y+35.f,top.v.z));
          if(!face.hasCol) continue;
          // tryJump probes only climbMove (55 cm) ahead. Geometric ledge
          // proximity alone can otherwise choose an ordinary forward jump.
          for(float margin:{45.f,35.f,55.f,65.f}) {
            Vec3 root;
            if(!floorAt(world,face.v-direction*margin,root) || std::abs(root.y-center.y)>15.f) continue;
            player.setPosition(root);player.setDirection(direction);player.updateTransform();restore.moved=true;
            const auto probe=player.tryJump();
            if(!climbable(probe)) continue;
            state.root=root;state.goal=beyond;state.direction=direction;state.anchor=point->name;
            state.climbProbe=probe;state.climbFace=face.v;return true;
          }
        }
      } else {
        // A low stock obstruction has supported ground on both sides and a
        // clear running approach before the real forward-jump input.
        Vec3 end;
        if(!floorAt(world,center+direction*400.f,end) || std::abs(end.y-center.y)>15.f) continue;
        float peak=0;bool supported=true;
        for(unsigned i=0;i<=20;++i) {
          const auto at=center+direction*(float(i)*20.f);
          const auto hit=physics->landRay(at+Vec3(0,150,0),220);
          if(!hit.hasCol) {supported=false;break;}
          peak=std::max(peak,hit.v.y-center.y);
          if(i<=3 && std::abs(hit.v.y-center.y)>10.f) {supported=false;break;}
        }
        const auto blocked=physics->ray(center+Vec3(0,35,0),end+Vec3(0,35,0));
        if(!supported || peak<35 || peak>75 || !blocked.hasCol ||
           physics->ray(center+Vec3(0,160,0),end+Vec3(0,160,0)).hasCol) continue;
        // The native run-start transition takes roughly 500 ms and cannot be
        // interrupted by Jump. Leave a 200 cm flat runway before the earlier
        // 80 cm takeoff margin, so the script can finish that transition first.
        Vec3 root,takeoff;
        if(!floorAt(world,blocked.v-direction*280.f,root) || std::abs(root.y-center.y)>15.f ||
           !floorAt(world,blocked.v-direction*80.f,takeoff) || !lane(world,root,takeoff,15.f)) continue;
        bool runway=true;
        for(unsigned i=0;i<=20;++i) {
          Vec3 floor;
          if(!floorAt(world,root+(takeoff-root)*(float(i)/20.f),floor) || std::abs(floor.y-root.y)>15.f) {
            runway=false;break;
          }
        }
        if(!runway) continue;
        state.root=root;state.goal=end;state.direction=direction;state.anchor=point->name;return true;
      }
    }
  }
  return false;
}

bool selectFlat(World& world) {
  // MoveR's positive local X becomes world -side in MoveAlgo::applyRotation.
  // The parity sequence therefore follows an L: forward, then right; the
  // backwards leg after turning right retraces part of the perpendicular arm.
  // Check those supported strips with margin for transition root motion.
  for(const char* name:{"NW_TROLLAREA_PATH_23","NW_XARDAS_TOWER_VALLEY_091","NW_FARM1_PATH_01"}) {
    const auto* point=world.findWayPoint(name);
    if(!point) continue;
    for(float dx:{0.f,-500.f,500.f,-1000.f,1000.f})
      for(float dz:{0.f,-500.f,500.f,-1000.f,1000.f}) {
        Vec3 root;
        if(!floorAt(world,point->position()+Vec3(dx,0,dz),root)) continue;
        for(unsigned angle=0;angle<24;++angle) {
          const Vec3 direction(std::cos(float(angle)*Pi/12),0,std::sin(float(angle)*Pi/12));
          const Vec3 side(-direction.z,0,direction.x);
          bool valid=true;
          float minimumHeight=root.y,maximumHeight=root.y;
          size_t probes=0;
          const auto probe=[&](float forward,float sideways,Vec3& floor) {
            ++probes;
            if(!floorAt(world,root+direction*forward+side*sideways,floor) || std::abs(floor.y-root.y)>55.f) return false;
            minimumHeight=std::min(minimumHeight,floor.y);maximumHeight=std::max(maximumHeight,floor.y);
            return true;
          };
          for(float sideways:{-65.f,0.f,65.f}) {
            Vec3 previous;
            bool first=true;
            for(float forward:{-75.f,0.f,100.f,200.f,300.f,400.f,500.f,600.f,650.f}) {
              Vec3 floor;
              if(!probe(forward,sideways,floor) ||
                 (!first && !lane(world,previous,floor,12.f))) {valid=false;break;}
              previous=floor;first=false;
            }
            if(!valid) break;
          }
          if(!valid) continue;
          for(float forward:{250.f,350.f,450.f,550.f,650.f}) {
            Vec3 previous;
            bool first=true;
            for(float sideways:{-1400.f,-1275.f,-1150.f,-1025.f,-900.f,-775.f,-650.f,-525.f,-400.f,-275.f,-150.f,-25.f,100.f}) {
              Vec3 floor;
              if(!probe(forward,sideways,floor) ||
                 (!first && !lane(world,previous,floor,12.f))) {valid=false;break;}
              previous=floor;first=false;
            }
            if(!valid) break;
          }
          if(!valid) continue;
          state.root=root;state.direction=direction;state.goal=root+direction*500.f;state.anchor=name;
          state.flatMinimumHeight=minimumHeight;state.flatMaximumHeight=maximumHeight;state.flatProbeCount=probes;
          return true;
        }
      }
  }
  return false;
}

bool selectVoid(World& world) {
  const Vec3 approach(6738.694824f,270.1410828f,-6031.250977f);
  const Vec3 reference(6754.f,262.f,-6115.f);
  const auto direction=Vec3::normalize(Vec3(reference.x-approach.x,0,reference.z-approach.z));
  // The reproduced stress trace reaches this spot from the north. The southern
  // approach is behind another wall and cannot exercise the observed void.
  Vec3 edge;
  if(!floorAt(world,approach,edge)) return false;
  for(float back:{75.f,0.f}) {
    Vec3 root;
    if(!floorAt(world,edge-direction*back,root) || std::abs(root.y-edge.y)>100.f ||
       !lane(world,root,edge,35.f)) continue;
    state.root=root;state.direction=direction;state.goal=reference+direction*160.f;
    state.anchor="stock_void_north_6754_262_-6115";state.voidApproach=edge;
    state.voidStartBack=back;state.voidEdgeBack=0;return true;
  }
  return false;
}

std::string voidSupportGrid(World& world) {
  const Vec3 reference(6754.f,262.f,-6115.f);
  std::string json="{\"center_cm\":"+vector(reference)+
      ",\"step_cm\":50,\"radius_cm\":200,\"probe_above_cm\":180,\"probe_below_cm\":500,\"samples\":[";
  size_t count=0;
  for(int x=-200;x<=200;x+=50)
    for(int z=-200;z<=200;z+=50) {
      if(count++) json+=',';
      json+="{\"offset_x_cm\":"+std::to_string(x)+",\"offset_z_cm\":"+std::to_string(z)+
          ",\"support\":"+supportRay(world,reference+Vec3(float(x),0,float(z)),180.f,500.f)+"}";
    }
  return json+"]}";
}

std::string setup(World& world,Npc& player,const std::string& scene,size_t count) {
  const bool voidScene=scene=="void" || scene=="void-replay";
  if(count<1 || count>10) throw std::runtime_error("keys setup actor count must be 1..10");
  if(state.stress) throw std::runtime_error("keys stress end before setup");
  std::vector<Npc*> protectedNative;
  for(const auto& target:state.native)
    if(auto* npc=liveNative(world,target)) {
      NpcKeys::enable(*npc,false);npc->setCharacterVirtualDiagnosticsEnabled(false);protectedNative.push_back(npc);
    }
  state.native.clear();
  for(auto* npc:state.actors) { NpcKeys::enable(*npc,false);world.removeNpc(*npc); }
  state.actors.clear();state.prepared=false;state.scene=scene;state.trace.clear();state.removedRelaxedOverlay=false;
  if(scene=="climb" || scene=="jump") {
    if(!selectObstacle(world,player,scene)) throw std::runtime_error("keys stock "+scene+" fixture unavailable");
  } else if(scene=="void-replay") {
    const Vec3 observed(6718.370605f,270.1453552f,-6029.716797f);
    Vec3 floor;
    if(!floorAt(world,observed,floor)) throw std::runtime_error("keys recorded stress pre-jump root has no supported floor");
    const float yaw=-89.73f*Pi/180.f;
    state.root=Vec3(observed.x,floor.y,observed.z);state.direction=Vec3(std::cos(yaw),0,std::sin(yaw));
    state.goal=Vec3(6754.f,262.f,-6115.f);state.anchor="recorded_stress6_prejump";
    state.voidApproach=state.root;state.voidStartBack=state.voidEdgeBack=0;
  } else if(scene=="void") {
    if(!selectVoid(world)) throw std::runtime_error("keys void approach lacks a supported clear starting runway");
  } else if(scene=="here") {
    state.root=player.position();state.direction=Vec3(std::cos(player.rotationRad()),0,std::sin(player.rotationRad()));
    state.goal=state.root+state.direction*500.f;state.anchor="current_hero";
  } else {
    if(scene!="flat" && scene!="market") throw std::runtime_error("keys setup expects flat, jump, climb, market, void, void-replay or here");
    state.anchor=scene=="market"?"NW_CITY_MERCHANT_PATH_01":"NW_TROLLAREA_PATH_23";
    const auto* point=world.findWayPoint(state.anchor);
    if(!point || !floorAt(world,point->position(),state.root)) throw std::runtime_error("keys waypoint has no supported root: "+state.anchor);
    state.direction=point->direction();state.direction.y=0;
    if(state.direction.quadLength()<.01f) state.direction=Vec3(1,0,0);
    state.direction=Vec3::normalize(state.direction);
    state.goal=state.root+state.direction*500.f;
    if(scene=="flat") {
      if(!selectFlat(world)) throw std::runtime_error("keys flat sites lack supported clear parity-route strips");
    }
  }
  size_t frozen=0;
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player &&
       std::find(protectedNative.begin(),protectedNative.end(),npc)==protectedNative.end() &&
       (npc->position()-state.root).length()<2800.f) {freeze(*npc);++frozen;}
  freeze(player);player.closeWeapon(true);
  // Detailed CV contacts change the cost of moveVirtual. Keep the market's
  // executor timing clean; physical feet and post-tick support rays still work.
  player.setCharacterVirtualDiagnosticsEnabled(scene!="market");
  const Vec3 side(-state.direction.z,0,state.direction.x);
  Vec3 hero;
  if(!parkingFloor(world,state.root-state.direction*180.f+side*250.f,state.root,state.goal,hero))
    throw std::runtime_error("keys setup has no supported hero parking away from its route");
  player.setPosition(hero);player.setDirection(state.direction);player.updateTransform();
  const auto symbol=world.script().findSymbolIndex("BDT_1003_BANDIT_M");
  if(symbol==size_t(-1)) throw std::runtime_error("keys human fixture instance unavailable");
  std::vector<Vec3> places{state.root};
  for(float radius=180.f;places.size()<count && radius<=1300.f;radius+=160.f)
    for(unsigned angle=0;places.size()<count && angle<24;++angle) {
      const Vec3 direction(std::cos(float(angle)*Pi/12),0,std::sin(float(angle)*Pi/12));
      Vec3 root;
      if(!floorAt(world,state.root+direction*radius,root) || std::abs(root.y-state.root.y)>160.f || (root-hero).length()<120.f) continue;
      bool crowded=false;
      for(const auto& place:places) if((place-root).length()<150.f) crowded=true;
      if(!crowded) places.push_back(root);
    }
  if(places.size()!=count) throw std::runtime_error("keys fixture cannot place "+std::to_string(count)+" separate supported actors");
  for(size_t i=0;i<count;++i) {
    auto* npc=world.addNpc(symbol,places[i]);
    if(!npc) throw std::runtime_error("keys human fixture insertion failed");
    state.actors.push_back(npc);freeze(*npc);npc->setExternalControl(false);
    npc->setCharacterVirtualDiagnosticsEnabled(scene!="market");
    // This native bandit instance starts with the script's slower relaxed walk.
    // The comparison needs the loaded hero's clips. Use the authored spelling:
    // skeleton cache identity is case-sensitive even on case-insensitive assets.
    if(!player.hasOverlay("Humans_Relaxed.mds") && npc->hasOverlay("Humans_Relaxed.mds")) {
      npc->delOverlay("Humans_Relaxed.mds");state.removedRelaxedOverlay=true;
    }
    npc->setPosition(places[i]);npc->setDirection(state.direction);npc->updateTransform();
    npc->handle().attribute[ATR_HITPOINTS]=npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
    NpcKeys::enable(*npc,true);
  }
  if(scene=="flat" || scene=="jump" || scene=="climb" || voidScene)
    for(size_t i=0;i<state.actors.size();++i)
      for(const char* name:{"S_WALKL","S_RUNL","T_WALKWSTRAFER"}) {
        const auto hero=player.animInfo(name),npc=state.actors[i]->animInfo(name);
        if(hero!=npc) throw std::runtime_error("keys preparation: unequal "+std::string(name)+
            " clips for hero and NPC "+std::to_string(i)+"; hero="+hero+"; npc="+npc);
      }
  if(scene=="climb")
    for(size_t i=0;i<state.actors.size();++i) {
      const auto probe=state.actors[i]->tryJump();
      if(!climbable(probe)) throw std::runtime_error("keys preparation: NPC "+std::to_string(i)+
          " cannot select a native climb at the hero-verified ledge; anim="+jumpName(probe.anim)+
          "; noClimb="+std::to_string(probe.noClimb));
    }
  state.destination=WayPoint(state.goal,state.direction,"KEYS_NATIVE_DESTINATION",true);
  state.prepared=true;state.previous=places;state.travelled.assign(count,0);
  const std::string json="{\"kind\":\"keys_setup\",\"scene\":"+quote(scene)+",\"anchor\":"+quote(state.anchor)+
      ",\"root_cm\":"+vector(state.root)+",\"goal_cm\":"+vector(state.goal)+",\"forward\":"+vector(state.direction)+
      ",\"actors\":"+std::to_string(count)+",\"frozen_residents\":"+std::to_string(frozen)+
      ",\"cv_diagnostics_enabled\":"+(scene!="market"?"true":"false")+
      ",\"hero_parking_cm\":"+vector(hero)+
      ",\"void_reference_cm\":"+(voidScene?vector(Vec3(6754.f,262.f,-6115.f)):"null")+
      ",\"void_approach_side\":"+(voidScene?quote("north"):"null")+
      ",\"void_observed_start_cm\":"+(scene=="void-replay"?vector(Vec3(6718.370605f,270.1453552f,-6029.716797f)):
          scene=="void"?vector(Vec3(6738.694824f,270.1410828f,-6031.250977f)):"null")+
      ",\"void_observed_yaw_deg\":"+(scene=="void-replay"?"-89.73":"null")+
      ",\"void_approach_cm\":"+(voidScene?vector(state.voidApproach):"null")+
      ",\"void_start_back_cm\":"+(voidScene?number(state.voidStartBack):"null")+
      ",\"void_edge_back_cm\":"+(voidScene?number(state.voidEdgeBack):"null")+
      ",\"void_start_from_original_back_cm\":"+(voidScene?number(state.voidEdgeBack+state.voidStartBack):"null")+
      ",\"void_goal_beyond_cm\":"+(scene=="void-replay"?"0":scene=="void"?"160":"null")+
      ",\"void_support_grid\":"+(voidScene?voidSupportGrid(world):"null")+
      ",\"removed_relaxed_overlay\":"+(state.removedRelaxedOverlay?"true":"false")+
      ",\"flat_probe_count\":"+std::to_string(scene=="flat"?state.flatProbeCount:0)+
      ",\"flat_minimum_y_cm\":"+std::to_string(state.flatMinimumHeight)+
      ",\"flat_maximum_y_cm\":"+std::to_string(state.flatMaximumHeight)+
      ",\"climb_probe_animation\":"+quote(scene=="climb"?jumpName(state.climbProbe.anim):"")+
      ",\"climb_probe_height_cm\":"+std::to_string(scene=="climb"?state.climbProbe.height:0)+
      ",\"climb_probe_no_climb\":"+(scene=="climb" && state.climbProbe.noClimb?"true":"false")+
      ",\"climb_face_cm\":"+vector(state.climbFace)+
      ",\"hero_walk_animation\":"+quote(player.animInfo("S_WALKL"))+
      ",\"npc_walk_animation\":"+quote(state.actors.front()->animInfo("S_WALKL"))+
      ",\"hero_run_animation\":"+quote(player.animInfo("S_RUNL"))+
      ",\"npc_run_animation\":"+quote(state.actors.front()->animInfo("S_RUNL"))+
      ",\"hero_strafe_animation\":"+quote(player.animInfo("T_WALKWSTRAFER"))+
      ",\"npc_strafe_animation\":"+quote(state.actors.front()->animInfo("T_WALKWSTRAFER"))+"}";
  append("keys-events.jsonl",json);return json;
}

std::string eye(World& world,Npc& npc,const std::string& tag) {
  NavigationEye eye;
  eye.update(npc,world.tickCount());
  const auto pose=npc.navigationEyePose();
  std::ostringstream out;
  out << std::setprecision(10) << "{\"kind\":\"keys_eye_snapshot\",\"tag\":" << quote(tag)
      << ",\"game_ms\":" << world.tickCount() << ",\"npc_id\":" << world.npcId(&npc)
      << ",\"root_cm\":" << vector(npc.position()) << ",\"body_yaw_deg\":" << npc.rotation()
      << ",\"origin_cm\":" << vector(pose.origin) << ",\"forward\":" << vector(pose.forward)
      << ",\"right\":" << vector(pose.right) << ",\"up\":" << vector(pose.up)
      << ",\"animated_head\":" << (pose.animatedHead?"true":"false") << ",\"rays\":[";
  size_t index=0;
  for(const auto& ray:eye.samples()) {
    if(index) out << ',';
    out << "{\"index\":" << index++ << ",\"yaw_deg\":" << ray.yawDeg << ",\"pitch_deg\":" << ray.pitchDeg
        << ",\"distance_cm\":" << ray.distanceCm << ",\"origin_cm\":" << vector(ray.origin)
        << ",\"direction\":" << vector(ray.direction) << ",\"point_cm\":" << vector(ray.point)
        << ",\"hit\":" << (ray.hit?"true":"false") << ",\"hit_kind\":" << quote(NavigationEye::hitKindName(ray.kind)) << '}';
  }
  out << "]}";append("keys-eye-snapshots.jsonl",out.str());
  return "{\"kind\":\"keys_eye_written\",\"tag\":"+quote(tag)+",\"rays\":"+std::to_string(index)+"}";
}

uint32_t nextRandom() {
  state.random=state.random*6364136223846793005ULL+1442695040888963407ULL;
  return uint32_t(state.random>>32);
}

std::string stressEnd(World& world) {
  if(!state.stress) throw std::runtime_error("keys stress end needs a running span");
  state.stress=false;NpcKeys::setProfiling(false);
  const auto stats=NpcKeys::stats();
  for(auto* npc:state.actors) NpcKeys::set(*npc,{},0);
  std::ostringstream out;
  out << std::setprecision(10) << "{\"kind\":\"keys_stress_end\",\"elapsed_ms\":" << world.tickCount()-state.stressBegan
      << ",\"actors\":" << state.actors.size() << ",\"frames\":" << state.frames << ",\"commands\":" << state.stressCommands
      << ",\"invalid_roots\":" << state.invalidRoots << ",\"maximum_frame_travel_cm\":" << state.maximumFrameTravel
      << ",\"requested_motion_frames\":" << state.requestedMotionFrames << ",\"still_motion_frames\":" << state.stillMotionFrames
      << ",\"near_peer_frames\":" << state.nearPeerFrames << ",\"minimum_peer_distance_cm\":" << state.minimumPeerDistance
      << ",\"executor_ticks\":" << stats.ticks << ",\"executor_total_ns\":" << stats.totalNanoseconds
      << ",\"executor_mean_us\":" << (stats.ticks?double(stats.totalNanoseconds)/1000./double(stats.ticks):0)
      << ",\"executor_max_us\":" << double(stats.maxNanoseconds)/1000. << ",\"partial_peer_ticks\":" << stats.partialPeerTicks
      << ",\"peer_checks\":" << stats.peerChecks << ",\"travel_cm\":[";
  for(size_t i=0;i<state.travelled.size();++i) {if(i)out << ',';out << state.travelled[i];}
  out << "]}";append("keys-events.jsonl",out.str());return out.str();
}
}

void NpcKeysBench::reset() {
  if(auto* world=Gothic::inst().world()) {
    for(const auto& target:state.native)
      if(auto* npc=liveNative(*world,target)) {NpcKeys::enable(*npc,false);npc->setCharacterVirtualDiagnosticsEnabled(false);}
    for(auto* npc:state.actors)
      if(world->npcId(npc)!=uint32_t(-1)) npc->setCharacterVirtualDiagnosticsEnabled(false);
    if(auto* player=world->player()) player->setCharacterVirtualDiagnosticsEnabled(false);
  }
  NpcKeys::reset();state=State{};
}

std::string NpcKeysBench::command(const std::vector<std::string>& args,World& world,Npc& player,
                                 PlayerControl& control,const std::string& outputDirectory) {
  state.directory=outputDirectory;
  player.setCharacterVirtualDiagnosticsEnabled(state.scene!="market");
  if(args.size()<2) throw std::runtime_error("keys command missing arguments");
  std::string result;
  if(args[1]=="setup" && (args.size()==3 || args.size()==4)) {
    releaseHero(control);return setup(world,player,args[2],args.size()==4?std::stoul(args[3]):1);
  }
  if(args[1]=="info" && (args.size()==2 || args.size()==3)) {
    if(args.size()==3) actor(world,args[2]);
    return info(world,player);
  }
  if(args[1]=="place" && (args.size()==3 || args.size()==5)) {
    if(!state.prepared) throw std::runtime_error("keys place needs setup");
    releaseHero(control);
    auto& npc=args[2]=="hero"?player:actor(world,args[2]);
    const Vec3 side(-state.direction.z,0,state.direction.x);
    const float forward=args.size()==5?std::stof(args[3]):0;
    const float sideways=args.size()==5?std::stof(args[4]):0;
    Vec3 root;
    if(!floorAt(world,state.root+state.direction*forward+side*sideways,root)) throw std::runtime_error("keys place has no supported floor");
    if(&npc==&player) {
      std::vector<Vec3> occupied;
      for(size_t i=0;i<state.actors.size();++i) {
        Vec3 park;
        if(!parkingFloor(world,state.root-state.direction*(300.f+float(i)*160.f)-side*300.f,
                         root,state.goal,park,occupied))
          throw std::runtime_error("keys hero placement lacks NPC parking floor");
        occupied.push_back(park);
        state.actors[i]->stopWalking();state.actors[i]->setPosition(park);state.actors[i]->updateTransform();
        NpcKeys::set(*state.actors[i],{},0);
      }
    } else {
      Vec3 park;
      if(!parkingFloor(world,state.root-state.direction*300.f+side*300.f,root,state.goal,park))
        throw std::runtime_error("keys NPC placement lacks hero parking floor");
      player.stopWalking();player.setPosition(park);player.updateTransform();
    }
    npc.stopWalking();npc.setPosition(root);npc.setDirection(state.direction);npc.setWalkMode(WalkBit::WM_Run);npc.updateTransform();
    result="{\"kind\":\"keys_place\",\"actor\":"+quote(args[2])+",\"root_cm\":"+vector(root)+"}";
  } else if(args[1]=="trace" && args.size()==4 && (args[3]=="on" || args[3]=="off")) {
    state.trace=args[3]=="on"?args[2]:"";
    result="{\"kind\":\"keys_trace\",\"tag\":"+quote(args[2])+",\"enabled\":"+(state.trace.empty()?"false":"true")+"}";
  } else if(args[1]=="sample" && (args.size()==3 || args.size()==4)) {
    if(args.size()==4) actor(world,args[3]);
    return sample(world,player,args[2]);
  } else if(args[1]=="eye" && args.size()==4) {
    return eye(world,actor(world,args[2]),args[3]);
  } else if(args[1]=="routine" && args.size()==3) {
    auto& npc=actor(world,args[2]);
    if(std::find(state.actors.begin(),state.actors.end(),&npc)==state.actors.end())
      throw std::runtime_error("keys routine prepares created fixture actors; native NPC routines are retained by on/off");
    npc.setExternalControl(false);npc.clearAiQueue();npc.clearState(false);npc.clearGoTo();
    npc.aiPush(AiQueue::aiGoToPoint(state.destination));npc.aiPush(AiQueue::aiWait(60000));
    result="{\"kind\":\"keys_native_goal\",\"actor\":"+quote(args[2])+",\"goal_cm\":"+vector(state.goal)+"}";
  } else if(args[1]=="stress" && args.size()==4 && args[2]=="begin") {
    if(state.actors.size()!=10 || state.stress) throw std::runtime_error("keys stress begin needs ten prepared actors and no running span");
    state.random=std::stoull(args[3]);state.stress=true;state.stressBegan=world.tickCount();state.stressNext=world.tickCount();
    state.stressCommands=state.frames=state.invalidRoots=0;state.maximumFrameTravel=0;state.travelled.assign(state.actors.size(),0);
    state.requestedMotionFrames=state.stillMotionFrames=state.nearPeerFrames=0;state.minimumPeerDistance=1000000;
    for(size_t i=0;i<state.actors.size();++i) {state.previous[i]=state.actors[i]->position();NpcKeys::enable(*state.actors[i],true);}
    NpcKeys::resetStats();NpcKeys::setProfiling(true);
    result="{\"kind\":\"keys_stress_begin\",\"seed\":"+args[3]+",\"decision_period_ms\":50,\"ttl_ms\":100}";
  } else if(args[1]=="stress" && args.size()==3 && args[2]=="end") {
    return stressEnd(world);
  } else if(args.size()==3 && (args[2]=="on" || args[2]=="off")) {
    NpcKeys::enable(actor(world,args[1]),args[2]=="on");
    result="{\"kind\":\"keys_enable\",\"actor\":"+quote(args[1])+",\"enabled\":"+(args[2]=="on"?"true":"false")+"}";
  } else if(args.size()==11) {
    NpcKeys::KeyState keys;
    keys.forward=std::stof(args[2]);keys.strafe=std::stof(args[3]);keys.turn=std::stof(args[4]);
    if(args[5]!="walk" && args[5]!="run") throw std::runtime_error("keys gait must be walk or run");
    keys.walk=args[5]=="walk";keys.jump=std::stoi(args[6])!=0;keys.climb=std::stoi(args[7])!=0;
    keys.headYaw=std::stof(args[8]);keys.headPitch=std::stof(args[9]);
    const auto ttl=std::stoull(args[10]);
    if(args[1]=="hero") heroKeys(player,control,keys,ttl);
    else if(!NpcKeys::set(actor(world,args[1]),keys,ttl)) throw std::runtime_error("keys actor is not enabled");
    result="{\"kind\":\"keys_command\",\"actor\":"+quote(args[1])+",\"game_ms\":"+std::to_string(world.tickCount())+
      ",\"forward\":"+std::to_string(keys.forward)+",\"strafe\":"+std::to_string(keys.strafe)+",\"turn\":"+std::to_string(keys.turn)+
      ",\"walk\":"+(keys.walk?"true":"false")+",\"jump\":"+(keys.jump?"true":"false")+",\"climb\":"+(keys.climb?"true":"false")+
      ",\"head_yaw_deg\":"+std::to_string(keys.headYaw)+",\"head_pitch_deg\":"+std::to_string(keys.headPitch)+",\"ttl_ms\":"+std::to_string(ttl)+"}";
  } else throw std::runtime_error("unknown keys fixture command");
  append("keys-events.jsonl",result);return result;
}

void NpcKeysBench::tick(World& world,Npc& player,PlayerControl& control,uint64_t dt) {
  if(!state.prepared && state.native.empty() && !state.heroUntil) return;
  state.sampleDt=dt;
  state.native.erase(std::remove_if(state.native.begin(),state.native.end(),[&](const auto& target) {
    return liveNative(world,target)==nullptr;
  }),state.native.end());
  if(state.heroUntil && world.tickCount()>=state.heroUntil) releaseHero(control);
  if(state.stress) {
    ++state.frames;
    for(size_t i=0;i<state.actors.size();++i) {
      const auto root=state.actors[i]->position();
      if(!std::isfinite(root.x) || !std::isfinite(root.y) || !std::isfinite(root.z)) ++state.invalidRoots;
      const auto travelled=(root-state.previous[i]).length();state.travelled[i]+=travelled;
      state.maximumFrameTravel=std::max(state.maximumFrameTravel,travelled);state.previous[i]=root;
      const auto status=NpcKeys::status(*state.actors[i]);
      if(status.hasCommand && (status.keys.forward!=0 || status.keys.strafe!=0)) {
        ++state.requestedMotionFrames;
        if(travelled<.1f) ++state.stillMotionFrames;
      }
      float nearest=1000000;
      for(size_t j=0;j<state.actors.size();++j)
        if(i!=j) nearest=std::min(nearest,(root-state.actors[j]->position()).length());
      state.minimumPeerDistance=std::min(state.minimumPeerDistance,nearest);
      if(nearest<100.f) ++state.nearPeerFrames;
    }
    if(world.tickCount()>=state.stressNext) {
      state.stressNext=world.tickCount()+50;
      for(auto* npc:state.actors) {
        NpcKeys::KeyState keys;
        keys.forward=float(int(nextRandom()%3)-1);keys.strafe=float(int(nextRandom()%3)-1);keys.turn=float(int(nextRandom()%3)-1);
        keys.walk=nextRandom()%2;keys.headYaw=float(int(nextRandom()%121)-60);keys.headPitch=float(int(nextRandom()%41)-20);
        keys.jump=nextRandom()%40==0;keys.climb=nextRandom()%50==0;
        NpcKeys::set(*npc,keys,100);++state.stressCommands;
      }
    }
  }
  if(!state.trace.empty()) sample(world,player,state.trace,"keys_frame");
  (void)dt;
}
