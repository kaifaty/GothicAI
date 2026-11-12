#include "npc.h"

#include <Tempest/Log>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "game/bodycontroller.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "gothic.h"
#include "npcdayjournal.h"
#include "world/benchhooks.h"

using namespace Tempest;

bool Npc::simplifiedMovementEnabled() {
  static const bool enabled = [] {
    const auto* value = std::getenv("OPENGOTHIC_NPC_SIMPLE_MOVE");
    return value==nullptr || value[0]!='0';
    }();
  return enabled;
  }

bool Npc::usesSimplifiedMovement() const {
  return simplifiedMovementEnabled() && !simpleMovement.nativeFallback &&
         simTier()==NpcSimTier::Simplified && !isPlayer() &&
         !nativeCombatMovement() && !nativeMobsiApproach() && !nativeMobsiTransition();
  }

bool Npc::nativeMobsiApproach() const {
  // AI_UseMob's direct final approach needs native body support and root
  // movement. A preceding waypoint route remains graph locomotion.
  if(go2.flag!=GT_Point || go2.wp!=nullptr || go2.npc!=nullptr)
    return false;
  const auto* action=aiQueue.front();
  return action!=nullptr && action->act==AI_UseMob;
  }

bool Npc::nativeMobsiTransition() const {
  const auto& pose=visual.pose();
  // Match MoveAlgo's native ToStand ownership, including a loaded pose whose
  // transient virtual-mob flag was reset. Walking transitions stay graph-owned.
  return interactive()==nullptr && !pose.isStanding() &&
         (pose.hasState(BS_MOBINTERACT) || pose.hasState(BS_CLIMB));
  }

bool Npc::nativeCombatMovement() const {
  // Routine locomotion is 6c; combat, pursuit and escape keep their native
  // body and physics executor until 6e. Keep every admission rule here.
  if(weaponState()!=WeaponState::NoWeapon || currentTarget!=nullptr || go2.nativeCombatTarget!=nullptr ||
     fghAlgo.hasInstructions() || go2.flag==GT_Flee || go2.flag==GT_Enemy || go2.flag==GT_EnemyG ||
     meleeKnockdown || isMeleeStunned() || visual.pose().isAttackAnim())
    return true;
  const std::string_view state=aiState.hint;
  return state=="ZS_ATTACK" || state=="ZS_MM_ATTACK" || state=="ZS_FLEE" ||
         state=="ZS_FLEETOWP" || state=="ZS_MAGICFLEE" || state=="ZS_MM_FLEE" ||
         state=="ZS_MM_THREATENENEMY" || state=="ZS_MM_EATBODY" || state=="ZS_WATCHFIGHT";
  }

auto Npc::simplifiedBodyState() const -> std::optional<BodyState> {
  if(!usesSimplifiedMovement() || !simpleMovement.body || nativeBodyAction())
    return std::nullopt;
  const auto& pose=visual.pose();
  if(pose.logicalLayerCount()==0)
    return std::nullopt;
  // Start/stop transitions retain their native layer state and logical clock.
  // A requested graph role does not admit or finish the underlying animation.
  for(size_t i=0;i<pose.logicalLayerCount();++i)
    if(pose.logicalLayer(i).sequence->animCls==Animation::Transition)
      return std::nullopt;
  return simpleMovement.body;
  }

bool Npc::nativeBodyAction() const {
  const auto now=owner.tickCount();
  if(interactive()!=nullptr || aniWaitTime>=now || outWaitTime>now || Gothic::inst().isNpcInDialog(*this))
    return true;
  const auto& pose=visual.pose();
  for(size_t i=0;i<pose.logicalLayerCount();++i) {
    const auto layer=pose.logicalLayer(i);
    // Native rotation ownership is Pose bookkeeping; authored turn clips
    // such as T_WALKTURNL need not carry the MDS ROTATE flag.
    if(pose.isRotationLayer(layer.sequence))
      continue;
    const auto state=BodyState(layer.state&(BS_MAX|BS_FLAG_MASK));
    if(state==BS_STAND && layer.sequence->animCls==Animation::Loop &&
       (pose.isStanding() || layer.sequence==visual.idleSequence(walkMode(),weaponState(),*this) ||
        layer.sequence==visual.idleSequence(WalkBit::WM_Run,weaponState(),*this) ||
        layer.sequence==visual.idleSequence(WalkBit::WM_Walk,weaponState(),*this) ||
        layer.sequence==visual.idleSequence(WalkBit::WM_Sneak,weaponState(),*this)))
      continue;
    if(state==BS_WALK || state==BS_RUN || state==BS_SNEAK || state==BS_SPRINT)
      continue;
    return true;
    }
  return false;
  }

float Npc::simplifiedMoveSpeed() const {
  return simplifiedMoveSpeed(walkMode());
  }

float Npc::simplifiedMoveSpeed(WalkBit mode) const {
  const auto* sequence = visual.movementSequence(mode,weaponState(),*this);
  if(sequence==nullptr || sequence->totalTime()<=0.f)
    return 0.f;
  const auto duration = uint64_t(sequence->totalTime());
  const auto displacement = sequence->speed(0,duration);
  float playback = 1.f;
  const auto& pose = visual.pose();
  for(const auto& [name,speed]:pose.logicalDefaultSpeeds())
    if(name==sequence->name)
      playback = speed;
  for(size_t i=0;i<pose.logicalLayerCount();++i) {
    const auto layer = pose.logicalLayer(i);
    if(layer.sequence==sequence)
      playback = layer.speed;
    }
  return std::sqrt(displacement.x*displacement.x+displacement.z*displacement.z)*playback/float(duration);
  }

void Npc::updateSimpleMovement() {
  const bool simple = usesSimplifiedMovement();
  simpleMovement.body.reset();
  if(simple)
    mvAlgo.takeRagdollControl();
  if(simple==simpleMovement.active)
    return;
  simpleMovement.active = simple;
  simpleMovement.edgeTo = nullptr;
  simpleMovement.ground = nullptr;
  if(simple) {
    const bool sensorEnabled = physic.isEnabled();
    suspendRagdoll();
    // The ghost remains a combat/perception sensor. Pair collision filtering
    // excludes graph actors; it must not become invisible to targeting rays.
    physic.setEnable(sensorEnabled);
    if(!nativeBodyAction())
      setAnimRotate(0);
    resetCharacterVisualHeight();
    } else {
    // Only the returning Full body needs a ground ray. The path and queue
    // survive the handoff; do not call resetPositionToTA/fixNpcPosition here.
    const auto ground = owner.physic()->landRay(position()+Vec3(0,30,0));
    if(ground.hasCol)
      setPosition(Vec3(x,ground.v.y,z));
    mvAlgo.takeRagdollControl();
    }
  }

auto Npc::pendingSimpleGoTo() const -> const AiQueue::AiAction* {
  const auto* action=aiQueue.front();
  if(action==nullptr || action->point==nullptr)
    return nullptr;
  switch(action->act) {
    case AI_GoToNpc: case AI_GotoItem: case AI_GoToNextFp:
      return action;
    case AI_GoToPoint:
      return action->i0==1 ? action : nullptr;
    default:
      return nullptr;
    }
  }

void Npc::fallbackSimpleMovement(std::string_view reason, bool ground) {
  if(!simpleMovement.nativeFallback) {
    const auto* symbol=owner.script().findSymbol(instanceSymbol());
    const auto from=position();
    const auto to=go2.empty() ? from : go2.target();
    Log::e("npc_graph_fallback_",ground ? "ground" : "segment",": npc=",
           symbol!=nullptr ? symbol->name() : std::to_string(instanceSymbol()),
           " world_id=",owner.npcId(this)," id=",hnpc->id,
           " tick=",owner.tickCount()," game_ms=",owner.time().toInt(),
           " segment=",from.x,",",from.y,",",from.z," -> ",to.x,",",to.y,",",to.z,
           " reason=",reason);
    owner.noteNpcGraphFallback(ground);
    }
  simpleMovement.nativeFallback=true;
  if(Bench::strictGraphMovement(*this))
    throw std::runtime_error(std::string(reason));
  // Preserve the action, its remaining path and the transform. The native
  // body owns this action until arrival/cancellation, including tier changes.
  updateSimpleMovement();
  }

WayPath Npc::simpleWayTo(const WayPoint& point) {
  // Native freepoint routes are terminal segments. Sending an unconnected
  // point through a nearby waynet can cross a different floor or an island.
  if(!point.isConnected())
    return owner.wayTo(*this,point);
  const auto connected=[](const WayPoint& p) { return p.isConnected(); };
  const auto* near=owner.findWayPoint(position(),connected);
  if(near==nullptr) {
    fallbackSimpleMovement("no connected waynet");
    return owner.wayTo(*this,point);
    }
  auto delta=near->groundPos-position();
  float distance2=delta.x*delta.x+delta.z*delta.z;
  for(const auto& connection:near->connections()) {
    const auto edge=connection.point->groundPos-near->groundPos;
    const float length2=edge.x*edge.x+edge.z*edge.z;
    if(length2==0.f)
      continue;
    const float progress=std::clamp(-(delta.x*edge.x+delta.z*edge.z)/length2,0.f,1.f);
    const auto offset=delta+edge*progress;
    distance2=std::min(distance2,offset.x*offset.x+offset.z*offset.z);
    }
  // Only graph actors may recover off-network by a straight segment to Near.
  auto path=distance2>4.f*MoveAlgo::closeToPointThreshold*MoveAlgo::closeToPointThreshold ?
            owner.wayTo(*near,point) : owner.wayTo(*this,point);
  if(path.first()==nullptr && !MoveAlgo::isClose(*this,point)) {
    fallbackSimpleMovement("no waynet path to target");
    return owner.wayTo(*this,point);
    }
  return path;
  }

void Npc::beginSimpleGoTo(AiQueue::AiAction&& action, const WayPoint& point) {
  GoTo terminal;
  if(action.act==AI_GoToNpc) {
    terminal.set(action.target,GT_Point);
    terminal.pointDistance=float(action.i0);
    terminal.nativeCombatTarget=action.nativeCombatTarget;
    } else if(action.act==AI_GotoItem) {
    if(action.item!=nullptr)
      terminal.set(action.item);
    else {
      terminal.flag=GT_Item;
      terminal.pos=go2.pos;
      }
    } else {
    terminal.set(action.point,action.act==AI_GoToNextFp ? GT_NextFp : GT_Way);
    attachToPoint(action.point);
    }
  float distance=MoveAlgo::closeToPointThreshold*(mvAlgo.checkLastBounce() ? .5f : 1.5f);
  if(terminal.npc!=nullptr)
    distance=fghAlgo.prefferedAttackDistance(*this,*terminal.npc,owner.script());
  else if(terminal.wp!=nullptr && terminal.wp->useCounter()>1)
    distance=float(MAX_AI_USE_DISTANCE);
  if(terminal.pointDistance>0.f)
    distance=terminal.pointDistance;
  if(terminal.isClose(*this,distance)) {
    wayPath.clear();
    go2=terminal;
    simpleMovement.edgeTo=nullptr;
    aiQueue.pushFront(std::move(action));
    return;
    }
  wayPath=simpleWayTo(point);
  const auto* first=wayPath.pop();
  if(first!=nullptr)
    go2.set(first);
  else
    go2=terminal; // Already at the target's connected anchor.
  simpleMovement.edgeTo=nullptr;
  aiQueue.pushFront(std::move(action));
  if(first!=nullptr)
    attachToPoint(first);
  }

void Npc::refreshSimpleGoTo() {
  const auto* pending=pendingSimpleGoTo();
  if(pending==nullptr || (pending->act!=AI_GoToNpc && pending->act!=AI_GotoItem))
    return;
  if((pending->act==AI_GoToNpc && pending->target==nullptr) ||
     (pending->act==AI_GotoItem && pending->item==nullptr && pending->i0!=1)) {
    aiQueue.pop();
    wayPath.clear();
    clearGoTo();
    return;
    }
  const auto target=pending->target!=nullptr ? pending->target->position() :
                    pending->item!=nullptr ? pending->item->position() : go2.pos;
  if(go2.flag==GT_Way && go2.wp!=nullptr) {
    GoTo terminal;
    float distance=MoveAlgo::closeToPointThreshold*(mvAlgo.checkLastBounce() ? .5f : 1.5f);
    if(pending->act==AI_GoToNpc) {
      terminal.set(pending->target,GT_Point);
      terminal.pointDistance=float(pending->i0);
      distance=terminal.pointDistance>0.f ? terminal.pointDistance :
               fghAlgo.prefferedAttackDistance(*this,*terminal.npc,owner.script());
      } else {
      terminal.flag=GT_Item;
      terminal.pos=target;
      }
    if(terminal.isClose(*this,distance)) {
      NpcDayJournal::arrived(*this);
      aiQueue.pop();
      wayPath.clear();
      clearGoTo();
      simpleMovement.body=BS_STAND;
      return;
      }
    }
  if(go2.flag==GT_Item)
    go2.pos=target;
  const auto* anchor=owner.findWayPoint(target,[](const WayPoint& p) { return p.isConnected(); });
  if(anchor==nullptr) {
    fallbackSimpleMovement("live target has no connected waypoint");
    return;
    }
  if(anchor==pending->point)
    return;
  if(go2.flag==GT_Way && go2.wp!=nullptr) {
    // Keep the current edge, replacing only its remaining suffix as a live
    // target moves. A Full body returning here follows the same native path.
    auto suffix=owner.wayTo(*go2.wp,*anchor);
    if(suffix.pop()!=go2.wp) {
      fallbackSimpleMovement("live target suffix lost current waynet edge");
      return;
      }
    auto action=aiQueue.pop();
    action.point=anchor;
    wayPath=std::move(suffix);
    aiQueue.pushFront(std::move(action));
    } else if(usesSimplifiedMovement() && !MoveAlgo::isClose(*this,*anchor)) {
    auto action=aiQueue.pop();
    action.point=anchor;
    beginSimpleGoTo(std::move(action),*anchor);
    } else {
    auto action=aiQueue.pop();
    action.point=anchor;
    aiQueue.pushFront(std::move(action));
    }
  }

bool Npc::finishSimpleGoTo(uint64_t dt) {
  const auto* pending=pendingSimpleGoTo();
  if(pending==nullptr)
    return false;
  switch(pending->act) {
    case AI_GoToNpc:
      go2.set(pending->target,GT_Point);
      go2.pointDistance=float(pending->i0);
      go2.nativeCombatTarget=pending->nativeCombatTarget;
      break;
    case AI_GotoItem:
      go2.clear();
      if(pending->item!=nullptr)
        go2.set(pending->item);
      else
        go2.flag=GT_Item; // A legacy Full save has only its native snapshot.
      break;
    case AI_GoToNextFp:
      go2.set(pending->point,GT_NextFp);
      break;
    case AI_GoToPoint:
      go2.set(pending->point);
      break;
    default:
      return false;
    }
  float distance=MoveAlgo::closeToPointThreshold*(mvAlgo.checkLastBounce() ? .5f : 1.5f);
  if(go2.npc!=nullptr)
    distance=fghAlgo.prefferedAttackDistance(*this,*go2.npc,owner.script());
  else if(go2.wp!=nullptr && go2.wp->useCounter()>1)
    distance=float(MAX_AI_USE_DISTANCE);
  if(go2.pointDistance>0.f)
    distance=go2.pointDistance;
  if(!go2.isClose(*this,distance))
    return implGoTo(dt,distance);
  if(go2.flag==GT_NextFp) {
    if(usesSimplifiedMovement())
      prepareTurn();
    if(implTurnTo(go2.wp,AnimationSolver::TurnType::Std,dt))
      return true;
    }
  NpcDayJournal::arrived(*this);
  aiQueue.pop();
  wayPath.clear();
  clearGoTo();
  simpleMovement.body=BS_STAND;
  return false;
  }

bool Npc::implGoTo(uint64_t dt) {
  refreshSimpleGoTo();
  float dist = 0;
  if(go2.npc) {
    dist = fghAlgo.prefferedAttackDistance(*this,*go2.npc,owner.script());
    } else {
    // use smaller threshold, to avoid edge-looping in script
    dist = MoveAlgo::closeToPointThreshold*0.5f;
    if(!mvAlgo.checkLastBounce())
      dist = MoveAlgo::closeToPointThreshold*1.5f;
    if(go2.wp!=nullptr && go2.wp->useCounter()>1)
      dist = float(MAX_AI_USE_DISTANCE);
    }
  if(go2.flag==GT_Point && go2.pointDistance>0.f)
    dist = go2.pointDistance;
  return implGoTo(dt,dist);
  }

bool Npc::implGoTo(uint64_t dt, float destDist) {
  // Native admission owns item/mobsi exits and other body transitions until
  // they release the body. The graph resumes the same path afterward.
  if(usesSimplifiedMovement() && !nativeBodyAction())
    return implSimpleGoTo(dt,destDist);
  if(go2.flag==GT_No)
    return false;

  if(isInAir() || interactive()!=nullptr) {
    mvAlgo.tick(dt);
    return true;
    }

  auto target = go2.target();
  auto dpos   = target - position();

  if(go2.flag==GT_Flee) {
    // nop
    }
  else if(go2.isClose(*this, destDist)) {
    bool finished = true;
    if(go2.flag==GT_Way) {
      go2.wp = go2.wp->hasLadderConn(wayPath.first()) ? wayPath.first() : wayPath.pop();
      if(go2.wp!=nullptr) {
        attachToPoint(go2.wp);
        if(setGoToLadder()) {
          mvAlgo.tick(dt);
          return true;
          }
        finished = false;
        }
      }
    if(finished) {
      if(pendingSimpleGoTo()!=nullptr)
        return finishSimpleGoTo(dt);
      if(go2.flag==Npc::GT_NextFp && implTurnTo(go2.wp,AnimationSolver::TurnType::Std,dt))
        return true;
      NpcDayJournal::arrived(*this);
      clearGoTo();
      }
    }
  else {
    if(setGoToLadder()) {
      mvAlgo.tick(dt);
      return true;
      }
    if(mvAlgo.checkLastBounce() && implTurnTo(dpos.x,dpos.z,AnimationSolver::TurnType::Std,dt)) {
      mvAlgo.tick(dt);
      return true;
      }
    }

  if(!go2.empty()) {
    setAnim(AnimationSolver::Move);
    mvAlgo.tick(dt);
    return true;
    }
  return false;
  }

bool Npc::implSimpleGoTo(uint64_t dt, float destDist) {
  if(go2.empty() || interactive()!=nullptr)
    return !go2.empty();
  if(setGoToLadder()) {
    mvAlgo.tick(dt);
    return true;
    }
  if(pendingSimpleGoTo()==nullptr) {
    AiQueue::AiAction terminal;
    const WayPoint* destination=nullptr;
    if(go2.npc!=nullptr) {
      terminal=AiQueue::aiGoToNpc(go2.npc,int(go2.pointDistance));
      terminal.nativeCombatTarget=go2.nativeCombatTarget;
      destination=owner.findWayPoint(go2.npc->position(),[](const WayPoint& p) { return p.isConnected(); });
      terminal.point=destination;
      } else if(go2.flag==GT_Item) {
      terminal.act=AI_GotoItem;
      terminal.item=simpleMovement.nativeItem;
      terminal.i0=terminal.item==nullptr ? 1 : 0;
      destination=owner.findWayPoint(go2.pos,[](const WayPoint& p) { return p.isConnected(); });
      terminal.point=destination;
      } else if(go2.wp!=nullptr && go2.wp->isFreePoint()) {
      terminal=go2.flag==GT_NextFp ? AiQueue::aiGoToNextFp("") : AiQueue::aiGoToPoint(*go2.wp);
      terminal.point=go2.wp;
      terminal.i0=terminal.act==AI_GoToPoint ? 1 : 0;
      destination=go2.wp;
      }
    if(destination!=nullptr) {
      beginSimpleGoTo(std::move(terminal),*destination);
      return implGoTo(dt);
      }
    }
  // Keep the terminal command until arrival so moved items remain live targets.
  if(go2.flag==GT_Item) {
    const auto* action = aiQueue.front();
    if(action!=nullptr && action->act==AI_GotoItem && action->item!=nullptr)
      go2.pos = action->item->position();
    }

  const auto* pending=pendingSimpleGoTo();
  const bool internal=go2.flag==GT_Way && go2.wp!=nullptr &&
                      (wayPath.first()!=nullptr || (pending!=nullptr &&
                       (pending->act==AI_GoToNpc || pending->act==AI_GotoItem || pending->point!=go2.wp)));
  bool reached=false;
  if(internal) {
    // Occupancy enlarges terminal admission, not an internal graph vertex:
    // changing edges early would replace the authored corner with a chord.
    auto vertex=go2.wp->groundPos;
    if(simpleMovement.edgeTo==go2.wp && simpleMovement.ground!=nullptr)
      vertex.y=simpleMovement.ground->heightAt(vertex);
    reached=(vertex-position()).quadLength()<=.01f*.01f;
    } else {
    reached=go2.isClose(*this,destDist);
    }
  if(go2.flag!=GT_Flee && reached) {
    if(go2.flag==GT_Way && go2.wp!=nullptr && wayPath.first()!=nullptr) {
      go2.wp = go2.wp->hasLadderConn(wayPath.first()) ? wayPath.first() : wayPath.pop();
      attachToPoint(go2.wp);
      simpleMovement.edgeTo = nullptr;
      if(setGoToLadder()) {
        mvAlgo.tick(dt);
        return true;
        }
      } else {
      if(pendingSimpleGoTo()!=nullptr)
        return finishSimpleGoTo(dt);
      if(go2.flag==GT_NextFp && implTurnTo(go2.wp,AnimationSolver::TurnType::Std,dt))
        return true;
      NpcDayJournal::arrived(*this);
      clearGoTo();
      simpleMovement.body = BS_STAND;
      return false;
      }
    }

  auto target = go2.target();
  if(go2.wp!=nullptr)
    target = go2.wp->groundPos;
  else if(go2.npc!=nullptr)
    target = go2.npc->position();
  if(go2.flag==GT_Flee) {
    const float travel = simplifiedMoveSpeed()*float(dt);
    const auto direction = Vec3(std::cos(rotationRad()),0,std::sin(rotationRad()));
    target = position()+direction*travel;
    }

  if(simpleMovement.edgeTo!=go2.wp || go2.wp==nullptr) {
    simpleMovement.edgeTo = go2.wp;
    simpleMovement.edgeFrom = position();
    // A saved/full actor can be between waypoints. Recover the current edge
    // from its endpoint's connections and project progress, never restart it.
    float best = MoveAlgo::closeToPointThreshold*MoveAlgo::closeToPointThreshold*4.f;
    if(go2.wp!=nullptr) {
      for(const auto& connection:go2.wp->connections()) {
        const auto from = connection.point->groundPos;
        const auto edge = target-from;
        const float length2 = edge.x*edge.x+edge.z*edge.z;
        if(length2==0.f)
          continue;
        const auto offset = position()-from;
        const float progress = std::clamp((offset.x*edge.x+offset.z*edge.z)/length2,0.f,1.f);
        const auto projection = from+edge*progress;
        const float error = (projection.x-x)*(projection.x-x)+(projection.z-z)*(projection.z-z);
        if(error<best) {
          best = error;
          simpleMovement.edgeFrom = from;
          }
        }
      }
    else if(const auto* near = owner.findWayPoint(target)) {
      // Native go-to-NPC/item/mob approaches have a final direct segment.
      // Its ground endpoint also comes from the waynet, without a tick ray.
      target.y = near->groundPos.y;
      }
    const auto& guild=owner.script().guildVal();
    const float step=guild.step_height[this->guild()]>0 ? float(guild.step_height[this->guild()]) : 50.f;
    const float slope=std::clamp(std::sin((90.f-float(guild.slide_angle[this->guild()]))*(float(M_PI)/180.f)),0.f,1.f);
    try {
      if(go2.wp!=nullptr) {
        // Bake once on segment admission. The profile follows the real support
        // between these endpoints; its target-anchored grid survives save/load.
        const float distance=std::hypot(target.x-x,target.z-z);
        simpleMovement.ground=distance>0.f ? &owner.groundProfile(position(),target,step,slope) : nullptr;
        }
      else if(simpleMovement.ground==nullptr) {
        simpleMovement.ground=&owner.groundProfile(position(),position(),step,slope);
        }
      }
    catch(const std::runtime_error& error) {
      fallbackSimpleMovement(error.what(),true);
      return implGoTo(dt,destDist);
      }
    }

  const auto direction = target-position();
  if(go2.flag!=GT_Flee && implTurnTo(direction.x,direction.z,AnimationSolver::TurnType::Std,dt)) {
    simpleMovement.body = BS_STAND;
    visual.stopWalkAnim(*this);
    if(!visual.pose().hasAnim())
      setAnim(Anim::Idle);
    return true;
    }
  const float travel = simplifiedMoveSpeed()*float(dt);
  const auto edge = target-simpleMovement.edgeFrom;
  const float length2 = edge.x*edge.x+edge.z*edge.z;
  if(length2>0.f && travel>0.f) {
    const auto offset = position()-simpleMovement.edgeFrom;
    const float progress = std::clamp((offset.x*edge.x+offset.z*edge.z)/length2,0.f,1.f);
    const float nextProgress = std::min(1.f,progress+travel/std::sqrt(length2));
    auto next = simpleMovement.edgeFrom+edge*nextProgress;
    const auto correction = next-position();
    const float horizontal = std::sqrt(correction.x*correction.x+correction.z*correction.z);
    if(horizontal>travel) {
      next.x = x+correction.x*travel/horizontal;
      next.z = z+correction.z*travel/horizontal;
      const auto along = next-simpleMovement.edgeFrom;
      next.y = simpleMovement.edgeFrom.y+edge.y*std::clamp((along.x*edge.x+along.z*edge.z)/length2,0.f,1.f);
      }
    if(simpleMovement.ground!=nullptr) {
      // A live NPC/item tail uses the last anchor's cached support plane as a
      // local approximation. Its current X/Z still follows the moving target;
      // a changed waynet anchor admits a new profile, never a per-tick ray.
      next.y=go2.wp!=nullptr ? simpleMovement.ground->heightAt(next) : simpleMovement.ground->terminalHeightAt(next);
      }
    setPosition(next);
    }
  else if(length2==0.f && travel>0.f && target.y!=y) {
    setPosition(Vec3(x,y+std::copysign(std::min(std::abs(target.y-y),travel),target.y-y),z));
    }
  if(setAnim(AnimationSolver::Move))
    simpleMovement.body = bool(walkMode()&WalkBit::WM_Sneak) ? BS_SNEAK :
                          bool(walkMode()&WalkBit::WM_Walk) ? BS_WALK : BS_RUN;
  return true;
  }

auto Npc::navigationGoal() const -> NavigationGoal {
  NavigationGoal ret;
  if(!go2.empty()) {
    if(const auto* pending=pendingSimpleGoTo()) {
      ret.active=true;
      if(pending->act==AI_GoToNpc) {
        ret.npc=pending->target;
        ret.kind="npc";
        ret.position=pending->target!=nullptr ? pending->target->position() : go2.target();
        } else if(pending->act==AI_GotoItem) {
        ret.kind="item";
        ret.position=pending->item!=nullptr ? pending->item->position() : go2.pos;
        } else {
        ret.point=pending->point;
        ret.position=pending->point->position();
        ret.kind="freepoint";
        ret.name=pending->point->name;
        }
      return ret;
      }
    }
  if(!go2.empty()) {
    ret.active = true;
    ret.npc = go2.npc;
    ret.point = go2.wp;
    // Preserve the final goal as intermediate waypoints change along the same walk.
    if(go2.flag==GT_Way && go2.npc==nullptr && wayPath.last()!=nullptr)
      ret.point = wayPath.last();
    ret.position = ret.point!=nullptr ? ret.point->position() : go2.target();
    ret.kind = ret.npc!=nullptr ? "npc" : (ret.point!=nullptr ? (ret.point->isFreePoint() ? "freepoint" : "waypoint") : "point");
    if(ret.point!=nullptr)
      ret.name = ret.point->name;
    else if(go2.flag==GT_Item)
      ret.kind = "item";
    return ret;
    }

  // A future walk behind AI_Wait/occupation is not a moving NPC. Only the ready head counts.
  const auto now = owner.tickCount();
  if(waitTime>=now || aniWaitTime>=now || outWaitTime>now)
    return ret;
  const auto* action = aiQueue.front();
  if(action==nullptr)
    return ret;
  if(action->act==AI_GoToPoint && action->point!=nullptr) {
    ret.active = true;
    ret.point = action->point;
    ret.position = action->point->position();
    ret.kind = ret.point->isFreePoint() ? "freepoint" : "waypoint";
    ret.name = ret.point->name;
    }
  else if(action->act==AI_GoToNpc && action->target!=nullptr) {
    ret.active = true;
    ret.npc = action->target;
    ret.position = action->target->position();
    ret.kind = "npc";
    }
  else if(action->act==AI_GoToNextFp) {
    // Resolving the freepoint would acquire runtime state. Keep the queued name until scripts do it.
    ret.active = true;
    ret.kind = "freepoint_query";
    ret.name = action->s0;
    }
  else if(action->act==AI_GotoItem && action->item!=nullptr) {
    ret.active = true;
    ret.kind = "item";
    ret.position = action->item->position();
    }
  return ret;
  }

bool Npc::isAiBusy() const {
  return !isAiQueueEmpty() ||
         aniWaitTime>=owner.tickCount() ||
         outWaitTime>=owner.tickCount();
  }

void Npc::clearAiQueue() {
  if(BodyController::deferNative(*this,[this] { clearAiQueue(); })) return;
  stopPhysicsInvestigation(false);
  currentLookAt    = nullptr;
  currentLookAtNpc = nullptr;
  visual.setHeadRotation(0,0);

  aiQueue.clear();
  aiQueueOverlay.clear();
  aniWaitTime = 0;
  waitTime    = 0;
  faiWaitTime = 0;
  fghAlgo.onClearTarget();
  wayPath.clear();
  clearGoTo();
  }

void Npc::attachToPoint(const WayPoint *p) {
  const auto* pending=pendingSimpleGoTo();
  if(pending!=nullptr && pending->point->isFreePoint() &&
     (pending->act==AI_GoToNextFp || pending->act==AI_GoToPoint) &&
     currentFp==pending->point && go2.flag==GT_Way && p==go2.wp && p!=pending->point)
    return;
  currentFp     = p;
  currentFpLock = FpLock(currentFp);
  }

bool Npc::goToPosition(const Vec3& position) {
  if(isInAir() || !setInteraction(nullptr))
    return false;
  attachToPoint(nullptr);
  wayPath.clear();
  go2.clear();
  // GoTo::isClose(Vec3) measures from the actor's center, whereas navmesh points
  // (like WayPoint::groundPos) lie on the walkable surface.
  go2.set(position+(centerPosition()-this->position()));
  go2.pointDistance = 20.f;
  return true;
  }

void Npc::clearGoTo() {
  simpleMovement.ground=nullptr;
  if(pendingSimpleGoTo()!=nullptr)
    aiQueue.pop();
  simpleMovement.nativeItem=nullptr;
  if(usesSimplifiedMovement())
    simpleMovement.edgeTo = nullptr;
  if(!go2.empty()) {
    stopWalking();
    go2.clear();
    }
  simpleMovement.nativeFallback=false;
  }

void Npc::stopWalking() {
  if(setAnim(Anim::Idle))
    return;
  // hard stop
  stopWalkAnimation();
  }
