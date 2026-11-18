#include "movealgo.h"

#include "game/gamerules.h"
#include "settings.h"
#include "world/objects/npc.h"
#include "world/objects/npc_melee.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace Tempest;

namespace {

std::string bumpWho(const Npc* npc) {
  if(npc==nullptr)
    return "null";
  if(npc->isPlayer())
    return "\"player\"";
  const auto name = npc->world().rules().symbolName(size_t(npc->instanceSymbol()));
  // Daedalus instance names are identifiers, so they need no JSON escaping.
  return "\""+std::string(name.empty() ? "npc" : name)+"\"";
  }

const char* bumpKind(BumpHit::Kind kind) {
  switch(kind) {
    case BumpHit::Kind::Wall:   return "wall";
    case BumpHit::Kind::Waist:  return "waist";
    case BumpHit::Kind::Person: return "person";
    case BumpHit::Kind::Pushed: return "pushed";
    }
  std::abort();
  }

void logBump(const Npc& who, const Npc* other, const BumpHit& hit, bool dynamic,
             const BumpSignal::Result& signal) {
  writeMeleePosture("{\"event\":\"bump\",\"tick_ms\":"+std::to_string(who.world().tickCount())+
                    ",\"who\":"+bumpWho(&who)+",\"other\":"+bumpWho(other)+
                    ",\"kind\":\""+bumpKind(hit.kind)+"\",\"dynamic\":"+(dynamic ? "true" : "false")+
                    ",\"speed_cm_s\":"+std::to_string(hit.speed)+
                    ",\"avg_speed_cm_s\":"+std::to_string(signal.avgSpeed)+
                    ",\"angle_deg\":"+std::to_string(signal.angleDeg)+
                    ",\"lost\":"+std::to_string(signal.lost)+
                    ",\"strength\":"+std::to_string(hit.strength)+"}");
  }

}

void MoveAlgo::tickBumpSignal(uint64_t dt, const Vec3& origin, const Vec3& displacement,
                             const DynamicWorld::NpcItem::VirtualMoveResult& moved, bool meleeStep) {
  const auto bs = npc.bodyStateMasked();
  BumpSignal::Input input;
  input.nowMs = npc.world().tickCount();
  input.dtMs = dt;
  input.requested = displacement;
  input.accepted = moved.position-origin;
  input.normal = moved.obstacleNormal;
  input.obstacle = moved.obstacleNormal!=Vec3();
  // the hands, the stand and the wall stop read the same observations: the answer below is the bump's own
  const auto& flag = Settings::physicsFlags();
  input.enabled = flag.bumpReactions || flag.wallReach || flag.wallStop;
  input.human = npc.isHuman();
  input.grounded = moved.grounded && !moved.collision.preFall;
  input.airborne = flags==InAir;
  input.falling = flags==Falling || bs==BS_FALL;
  input.sliding = flags==Slide || moved.steep;
  input.swimming = flags==Swim || bs==BS_SWIM;
  input.diving = flags==Dive || bs==BS_DIVE;
  input.jumping = flags==Jump || flags==JumpUp || flags==ClimbUp || bs==BS_JUMP;
  input.attacking = npc.isAttackAnim() || npc.hasState(BS_HIT);
  input.blocking = npc.hasState(BS_PARADE);
  input.dodging = npc.isMeleeDodging();
  input.rolling = npc.isMeleeDodgeRoll();
  input.meleeStep = meleeStep;
  input.down = npc.isLie() || npc.isDown() || npc.isMeleeKnockedDown();
  input.ragdoll = npc.hasRagdoll();
  input.interactive = npc.interactive()!=nullptr;
  const auto signal = BumpSignal::update(bumpSignal,input);
  tickReachSignal(dt,origin,displacement,moved,signal);
  if(!flag.bumpReactions || !signal.contactStarted)
    return;

  Npc* other = moved.obstacleNpc;
  BumpHit hit;
  hit.kind = other!=nullptr ? BumpHit::Kind::Person : BumpHit::Kind::Wall;
  hit.normal = moved.obstacleNormal;
  hit.strength = signal.strength;
  hit.speed = signal.speed;
  if(other==nullptr) {
    // Query physical hulls, including moving furniture, without the actor's own living rig.
    const auto reach = hit.normal*-(npc.physic.capsuleRadius()+40.f);
    std::array<bool,3> blocked;
    constexpr std::array<float,3> heights = {90.f,130.f,165.f};
    for(size_t i=0; i<heights.size(); ++i) {
      const auto from = moved.position+Vec3(0,heights[i],0);
      blocked[i] = npc.world().physic()->navigationRay(from,from+reach,npc.livingBody()).hasCol;
      }
    hit.kind = blocked[1] ? BumpHit::Kind::Wall : BumpHit::Kind::Waist;
    }
  logBump(npc,other!=nullptr && other->isHuman() ? other : nullptr,hit,moved.obstacleDynamic,signal);
  if(!signal.react)
    return;
  npc.bumpReaction(hit);
  if(other!=nullptr && other->isHuman()) {
    hit.kind = BumpHit::Kind::Pushed;
    hit.normal = Vec3::normalize(Vec3(displacement.x,0,displacement.z));
    hit.strength *= 0.8f;
    logBump(*other,&npc,hit,false,signal);
    other->bumpReaction(hit);
    }
  }

MoveAlgo::WallReach MoveAlgo::wallReach(uint64_t now) const {
  // a body the controller did not move this while shows nothing
  if(now<reach.tick || now-reach.tick>200)
    return WallReach();
  return reach;
  }

void MoveAlgo::tickReachSignal(uint64_t dt, const Vec3& origin, const Vec3& displacement,
                              const DynamicWorld::NpcItem::VirtualMoveResult& moved, const BumpSignal::Result& bump) {
  const uint64_t now = npc.world().tickCount();
  if(now<reach.tick || now-reach.tick>200) {
    reachSignal = {};
    reach = {};
    }
  const float length = std::hypot(displacement.x,displacement.z);
  ReachSignal::Input input;
  input.dtMs     = dt;
  input.enabled  = Settings::physicsFlags().wallReach;
  input.eligible = bump.eligible;
  input.moving   = length>0;
  input.obstacle = moved.obstacleNormal!=Vec3();
  input.contact  = bump.contact;
  input.moved    = length>0 ? std::hypot(moved.position.x-origin.x,moved.position.z-origin.z)/length : 1.f;
  input.avgSpeed = bump.avgSpeed;
  input.radius   = npc.physic.capsuleRadius();
  Vec3 ahead;
  if(ReachSignal::wantsProbe(input)) {
    // the look ahead along the move, at the chest and at the hip: what stands there, not a floor and not a person
    const Vec3  dir = Vec3(displacement.x,0,displacement.z)/length;
    const float far = ReachSignal::probeLength(input.avgSpeed,input.radius);
    for(const float height : {130.f,90.f}) {
      const Vec3 from = moved.position+Vec3(0,height,0);
      const auto ray  = npc.world().physic()->navigationRay(from,from+dir*far,npc.livingBody());
      if(!ray.hasCol || std::abs(ray.normal.y)>0.5f)
        continue;
      using Hit = DynamicWorld::NavigationHit;
      if(ray.kind!=Hit::World && ray.kind!=Hit::StaticVob && ray.kind!=Hit::MovableVob)
        continue;
      const Vec3  normal   = Vec3::normalize(Vec3(ray.normal.x,0,ray.normal.z));
      const float distance = (ray.point-from).length();
      if(input.ahead && distance>=input.distance)
        continue;
      input.ahead    = true;
      input.distance = distance;
      input.cosine   = -Vec3::dotProduct(dir,normal);
      ahead          = normal;
      }
    }
  const bool wasWarn  = reach.warn;
  const bool wasPress = reach.press>=0.5f;
  const auto signal = ReachSignal::update(reachSignal,input);
  reach.press     = signal.press;
  reach.warn      = signal.warn;
  reach.hands     = signal.hands;
  reach.plant     = signal.plant;
  reach.timeToHit = signal.timeToHit;
  reach.person    = moved.obstacleNpc!=nullptr;
  reach.tick      = now;
  if(signal.warn)
    reach.normal = ahead;
  else if(input.obstacle && input.eligible)
    reach.normal = Vec3::normalize(Vec3(moved.obstacleNormal.x,0,moved.obstacleNormal.z));
  // the wall stop: is what holds the hero a wall that stands flat to both sides of the body?
  reach.flat = false;
  if(Settings::physicsFlags().wallStop && npc.isPlayer() && input.eligible && input.obstacle &&
     !reach.person && !moved.obstacleDynamic && reach.normal!=Vec3()) {
    using Hit = DynamicWorld::NavigationHit;
    const Vec3 n     = reach.normal;
    const Vec3 along = Vec3(n.z,0,-n.x);
    const float beside = input.radius*1.2f;
    const std::array<float,4> side = {-ReachSignal::FlatReach,-beside,beside,ReachSignal::FlatReach};
    std::array<ReachSignal::WallLook,4> looks;
    for(size_t i=0; i<side.size(); ++i) {
      const Vec3 from = moved.position + Vec3(0,130,0) + along*side[i];
      const auto ray  = npc.world().physic()->navigationRay(from,from-n*(input.radius+40.f),npc.livingBody());
      looks[i].hit    = ray.hasCol && (ray.kind==Hit::World || ray.kind==Hit::StaticVob);
      looks[i].facing = Vec3::dotProduct(ray.normal,n);
      looks[i].gap    = (ray.point-from).length();
      }
    reach.flat = ReachSignal::flatWall(looks.data(),looks.size(),input.radius);
    if(bump.contactStarted) {
      std::string hit, gap, facing;
      for(size_t i=0; i<looks.size(); ++i) {
        const char* comma = i>0 ? "," : "";
        hit    += comma+std::to_string(int(looks[i].hit));
        gap    += comma+std::to_string(looks[i].gap);
        facing += comma+std::to_string(looks[i].facing);
        }
      writeMeleePosture("{\"event\":\"wall_flat\",\"tick_ms\":"+std::to_string(now)+",\"flat\":"+(reach.flat ? "true" : "false")+
                        ",\"hit\":["+hit+"],\"gap\":["+gap+"],\"facing\":["+facing+
                        "],\"radius\":"+std::to_string(input.radius)+"}");
      }
    }
  reach.stop = wallStopping;
  const bool press = reach.press>=0.5f;
  if((signal.warn && !wasWarn) || press!=wasPress)
    writeMeleePosture("{\"event\":\"reach\",\"tick_ms\":"+std::to_string(now)+",\"who\":"+bumpWho(&npc)+
                      ",\"phase\":\""+(signal.warn && !wasWarn ? "warn" : (press ? "press" : "free"))+
                      "\",\"time_to_hit_s\":"+std::to_string(signal.timeToHit)+
                      ",\"closing_cm_s\":"+std::to_string(signal.closing)+
                      ",\"moved\":"+std::to_string(input.moved)+
                      ",\"person\":"+(reach.person ? "true" : "false")+"}");
  }

Vec3 MoveAlgo::wallStop(const Vec3& displacement) {
  const uint64_t now    = npc.world().tickCount();
  const float    length = std::hypot(displacement.x,displacement.z);
  const bool     fresh  = Settings::physicsFlags().wallStop && npc.isPlayer() && now>=reach.tick && now-reach.tick<=200;
  const Vec3     n      = reach.normal;
  const float    into   = length>0 ? -(displacement.x*n.x+displacement.z*n.z)/length : 0.f;
  const bool     stop   = fresh && length>0 && ReachSignal::stops(wallStopping,reach.flat,into);
  if(stop!=wallStopping)
    writeMeleePosture("{\"event\":\"wall_stop\",\"tick_ms\":"+std::to_string(now)+",\"phase\":\""+(stop ? "begin" : "end")+
                      "\",\"angle_deg\":"+std::to_string(std::acos(std::clamp(into,-1.f,1.f))*(180.f/3.14159265f))+"}");
  wallStopping = stop;
  if(!stop)
    return displacement;
  return Vec3(-n.x*into*length,displacement.y,-n.z*into*length);
  }
