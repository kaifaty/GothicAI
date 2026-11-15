#include "npckeys.h"

#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace {

struct HeldKeys final {
  NpcKeys::KeyState keys;
  WalkBit originalWalk = WalkBit::WM_Run;
  Tempest::Vec2 originalHead;
  uint64_t until = 0;
  uint64_t lastTick = std::numeric_limits<uint64_t>::max();
  bool hasCommand = false;
  };

std::unordered_map<const Npc*,HeldKeys> held;
NpcKeys::Stats counters;
bool profiling = false;

float finite(float value) {
  return std::isfinite(value) ? value : 0.f;
  }

NpcKeys::KeyState sanitized(NpcKeys::KeyState keys) {
  keys.forward = std::clamp(finite(keys.forward),-1.f,1.f);
  keys.strafe  = std::clamp(finite(keys.strafe), -1.f,1.f);
  keys.turn    = std::clamp(finite(keys.turn),   -1.f,1.f);
  keys.headYaw   = std::clamp(finite(keys.headYaw),  -65.f,65.f);
  keys.headPitch = std::clamp(finite(keys.headPitch),-35.f,30.f);
  return keys;
  }

}

void NpcKeys::applyKeys(Npc& npc, const KeyState& input, uint64_t dt) {
  const auto keys = sanitized(input);
  if(npc.spellFrozen() || npc.spellWhirling() || npc.hasRagdoll() || npc.isDown())
    return;

  // Suspend destination clipping and native route collision decisions without discarding the
  // queued routine. The hero has no go-to destination while PlayerControl drives it.
  const auto destination = npc.go2;
  npc.go2.clear();
  npc.mvAlgo.tick(dt,MoveAlgo::WaitMove);
  npc.go2 = destination;

  npc.setWalkMode(keys.walk ? WalkBit::WM_Walk : WalkBit::WM_Run);
  npc.headInput = Tempest::Vec2(keys.headYaw,keys.headPitch);
  npc.visual.setHeadRotation(0,0);

  const auto bs = npc.bodyStateMasked();
  if(bs==BS_DEAD || bs==BS_UNCONSCIOUS || npc.interactive()!=nullptr)
    return;

  const float dtF = float(dt)/1000.f;
  float rot  = npc.rotation();
  float rotY = npc.rotationY();
  const float speed = (npc.weaponState()==WeaponState::NoWeapon ? 90.f : 180.f)*dtF;
  int rotation = 0;
  // Npc::isRotationAllowed also forbids ordinary airborne turns for native AI. Keyboard
  // control uses the hero's physical gates regardless of the actor's process policy.
  const bool allowRotation = npc.interactive()==nullptr && !npc.isFinishingMove() &&
                             bs!=BS_CLIMB && bs!=BS_LIE && !npc.isFallingDeep();
  if(allowRotation) {
    if(keys.turn<0) {
      rot += speed;
      rotation = -1;
      }
    else if(keys.turn>0) {
      rot -= speed;
      rotation = 1;
      }
    }

  // Input turns a jumping actor too, but only MoveAlgo may advance its airborne animation.
  if(npc.isFalling() || npc.isSlide() || npc.isInAir() || npc.isJump() || npc.isJumpUp()) {
    npc.setDirection(rot);
    npc.setRunAngle(0);
    return;
    }

  Npc::Anim ani = Npc::Anim::Idle;
  if(keys.strafe<0)
    ani = Npc::Anim::MoveL;
  else if(keys.strafe>0)
    ani = Npc::Anim::MoveR;
  else if(keys.forward>0) {
    if((npc.walkMode()&WalkBit::WM_Dive)!=WalkBit::WM_Dive)
      ani = Npc::Anim::Move;
    else if(npc.isDive()) {
      npc.setDirectionY(rotY-speed);
      return;
      }
    }
  else if(keys.forward<0) {
    if((npc.walkMode()&WalkBit::WM_Dive)!=WalkBit::WM_Dive)
      ani = Npc::Anim::MoveBack;
    else if(npc.isDive()) {
      npc.setDirectionY(rotY+speed);
      return;
      }
    }

  if(keys.jump || (keys.climb && keys.forward>0)) {
    if(npc.bodyStateMasked()==BS_JUMP)
      ani = Npc::Anim::Idle;
    else if(npc.isDive())
      ani = Npc::Anim::Move;
    else if(npc.isSwim())
      npc.startDive();
    else if(npc.isInWater()) {
      auto& g = npc.world().rules().guildVal();
      const auto gl = npc.guild();
      if(0<=gl && gl<GIL_MAX && npc.isStanding()) {
        Npc::JumpStatus jump;
        jump.anim = Npc::Anim::JumpUp;
        jump.height = float(g.jumpup_height[gl])+npc.position().y;
        npc.startClimb(jump);
        }
      }
    else if(npc.isStanding()) {
      auto jump = npc.tryJump();
      if(!npc.isFalling() && !npc.isSlide() && jump.anim!=Npc::Anim::Jump) {
        npc.startClimb(jump);
        return;
        }
      ani = Npc::Anim::Jump;
      }
    else if(!npc.isAttackAnim() && !npc.isCasting())
      ani = Npc::Anim::Jump;
    }

  if(!npc.isCasting()) {
    if(ani==Npc::Anim::Jump) {
      npc.setAnimRotate(0);
      rotation = 0;
      }
    if(npc.isAttackAnim()) {
      if((ani==Npc::Anim::MoveL || ani==Npc::Anim::MoveR) && npc.hasState(BS_RUN))
        ani = Npc::Anim::Idle;
      if(!npc.hasState(BS_RUN) && ani==Npc::Anim::Idle)
        ani = Npc::Anim::NoAnim;
      if((ani==Npc::Anim::MoveL || ani==Npc::Anim::MoveR) && npc.hasState(BS_STAND) && npc.hasState(BS_HIT))
        ani = Npc::Anim::NoAnim;
      }
    if(bs==BS_LIE) {
      ani = ani==Npc::Anim::Move ? Npc::Anim::Idle : Npc::Anim::NoAnim;
      rot = npc.rotation();
      }
    if(ani!=Npc::Anim::NoAnim)
      npc.setAnim(ani);
    }

  npc.setAnimRotate(ani==Npc::Anim::Idle && !npc.isAttackAnim() ? rotation : 0);
  npc.setDirection(rot);
  // The renderer supplies turn lean for every actor. PlayerControl's camera-only legacy
  // run-angle target is zero on the CPU stand and is eased back at 35 degrees/s.
  const float runAngle = npc.runAngle();
  npc.setRunAngle(std::abs(runAngle)<=35.f*dtF ? 0.f : runAngle-std::copysign(35.f*dtF,runAngle));
  }

void NpcKeys::enable(Npc& npc, bool enabled) {
  if(enabled) {
    auto [it,inserted] = held.try_emplace(&npc);
    if(inserted) {
      it->second.originalWalk = npc.wlkMode;
      it->second.originalHead = npc.visual.headRotation();
      }
    return;
    }
  auto it = held.find(&npc);
  if(it==held.end())
    return;
  npc.setWalkMode(it->second.originalWalk);
  npc.headInput.reset();
  npc.visual.setHeadRotation(it->second.originalHead.x,it->second.originalHead.y);
  held.erase(it);
  }

bool NpcKeys::set(Npc& npc, const KeyState& keys, uint64_t ttlMs) {
  auto it = held.find(&npc);
  if(it==held.end())
    return false;
  auto& slot = it->second;
  slot.keys = sanitized(keys);
  const auto now = npc.world().tickCount();
  slot.until = now + std::min(ttlMs,std::numeric_limits<uint64_t>::max()-now);
  slot.hasCommand = true;
  ++counters.commands;
  return true;
  }

bool NpcKeys::enabled(const Npc& npc) {
  return held.find(&npc)!=held.end();
  }

Tempest::Vec3 NpcKeys::physicalPosition(const Npc& npc) {
  return npc.physic.position();
  }

bool NpcKeys::tick(Npc& npc, uint64_t dt) {
  if(held.empty())
    return false;
  auto it = held.find(&npc);
  if(it==held.end())
    return false;
  auto& slot = it->second;
  const auto now = npc.world().tickCount();
  if(slot.hasCommand && now>=slot.until) {
    // TTL is the stand's dead-man release: walking transitions can otherwise keep their
    // root motion for hundreds of milliseconds despite a neutral Idle request. Remove only
    // walking layers before movement; airborne/climbing layers and the native queue survive.
    npc.stopWalkAnimation();
    npc.setRunAngle(0);
    slot.keys = {};
    slot.hasCommand = false;
    ++counters.expired;
    }

  if(profiling) {
    bool partial = false;
    for(const auto& peer:held) {
      if(peer.first==&npc || &peer.first->world()!=&npc.world())
        continue;
      ++counters.peerChecks;
      partial |= peer.second.lastTick==now;
      }
    counters.partialPeerTicks += partial ? 1 : 0;
    const auto start = std::chrono::steady_clock::now();
    applyKeys(npc,slot.keys,dt);
    const auto cost = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    counters.totalNanoseconds += uint64_t(cost);
    counters.maxNanoseconds = std::max(counters.maxNanoseconds,uint64_t(cost));
    } else {
    applyKeys(npc,slot.keys,dt);
    }
  slot.lastTick = now;
  ++counters.ticks;
  return true;
  }

void NpcKeys::forget(const Npc* npc) {
  held.erase(npc);
  }

void NpcKeys::reset() {
  while(!held.empty())
    enable(*const_cast<Npc*>(held.begin()->first),false);
  profiling = false;
  counters = {};
  }

NpcKeys::Status NpcKeys::status(const Npc& npc) {
  Status result;
  auto it = held.find(&npc);
  if(it==held.end())
    return result;
  result.enabled = true;
  const auto now = npc.world().tickCount();
  result.hasCommand = it->second.hasCommand && now<it->second.until;
  if(result.hasCommand) {
    result.keys = it->second.keys;
    result.remainingMs = it->second.until-now;
    }
  return result;
  }

void NpcKeys::setProfiling(bool enabled) {
  profiling = enabled;
  }

void NpcKeys::resetStats() {
  counters = {};
  }

NpcKeys::Stats NpcKeys::stats() {
  return counters;
  }
