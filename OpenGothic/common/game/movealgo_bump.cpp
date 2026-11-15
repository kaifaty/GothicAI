#include "movealgo.h"

#include "game/gamerules.h"
#include "settings.h"
#include "world/objects/npc.h"
#include "world/objects/npc_melee.h"
#include "world/world.h"

#include <array>
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
  input.enabled = Settings::physicsFlags().bumpReactions;
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
  if(!signal.contactStarted)
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
