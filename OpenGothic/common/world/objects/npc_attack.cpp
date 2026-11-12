#include "npc.h"
#include "npc_melee.h"
#include "npc_melee_shadow.h"

#include "game/bodyobservations.h"
#include "game/bodycontroller.h"
#include "game/bodytrajectory.h"
#include "game/meleeparry.h"
#include "game/gamescript.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "world/benchhooks.h"
#include "gothic.h"

#include <cmath>
#include <cstdlib>
#include <string>

using namespace Tempest;

// Attack choice and launch of Npc: the native attack tick, melee strikes, blocks and finishing moves.
// Moved from npc.cpp unchanged.

namespace {

float meleeParryLeadMs() {
  static const float lead = [] {
    const char* text = std::getenv("OPENGOTHIC_MELEE_PARRY_LEAD_MS");
    const float value = text==nullptr ? 0.f : std::strtof(text,nullptr);
    return std::isfinite(value) ? value : 0.f;
    }();
  return lead;
  }

}

bool Npc::implAttack(uint64_t dt, bool bodyOverride, FightAlgo::Action bodyAction) {
  if(currentTarget==nullptr || isPlayer() || isTalk()) {
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    return false;
    }

  if(currentTarget->isDown()){
    traceCombatGate("target_down");
    // NOTE: don't clear internal target, to make scripts happy
    // currentTarget=nullptr;
    fghAlgo.onClearTarget();
    return false;
    }

  if(aiQueue.size()>0 && !(aiQueue.size()==1 && pendingSimpleGoTo()!=nullptr)) {
    traceCombatGate("script_queue");
    // do not messup weapon change animations by MOVE intruction
    return false;
    }

  // A paired victim belongs to its riposter until handover (or the victim clip ends).
  if(currentTarget->meleeRiposteAiHeld && currentTarget->isMeleeRiposteHeld()) {
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    visual.stopWalkAnim(*this);
    if(!visual.pose().isStanding())
      setAnim(Anim::Idle);
    implTurnToFai(*currentTarget,dt);
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return true;
    }

  const auto ws = weaponState();
  const auto bs = bodyStateMasked();
  const auto& flags = Gothic::physicsFlags();
  const bool reactions = isHuman() && ws==WeaponState::W1H;
  const bool dodgeAi = reactions && flags.meleeAiDodge && flags.meleeDodge;
  if(bodyOverride || !dodgeAi || (meleeAiDodgeTiming && meleeAiDodgeTiming->target!=currentTarget))
    meleeAiDodgeTiming.reset();
  const bool parryAi = !bodyOverride && flags.meleeParryAi && flags.meleeParry && isHuman() &&
                       (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H);
  if(!parryAi || (meleeParadeTiming && meleeParadeTiming->target!=currentTarget))
    meleeParadeTiming.reset();
  const bool downTactics = !bodyOverride && flags.meleeDownTactics && flags.meleePosture && flags.meleePhysicsHits &&
                          (ws==WeaponState::W1H || ws==WeaponState::W2H);
  const bool targetKnockdown = downTactics && currentTarget->meleeKnockdown;
  const bool targetStunned = parryAi && currentTarget->isMeleeStunned();
  // Discard the remaining native combo, but let the attack already in flight finish below.
  if(targetKnockdown || targetStunned) {
    fghAlgo.onTargetKnockdown();
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    }
  const bool magnetHeading = visual.pose().isAttackAnim() && meleeShadow && meleeShadow->window && meleeShadow->physicsHits &&
                             meleeShadow->magnet.eligible;

  if((bs==BS_HIT || ((targetKnockdown || targetStunned) && visual.pose().isAttackAnim())) &&
     (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H)) {
    traceCombatGate("melee_hit_lock");
    //NOTE: 'storm' attack has BS_RUN state and not meant to be auto-rotated
    if(!magnetHeading)
      implTurnToFai(*currentTarget,dt);
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }

  if(!bodyOverride && !targetKnockdown && !targetStunned && !fghAlgo.hasInstructions()) {
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    traceCombatGate("no_instructions");
    return false;
    }

  if(bs==BS_LIE) {
    traceCombatGate("lie");
    setAnim(Npc::Anim::Idle);
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }
  if(bs==BS_STUMBLE || bs==BS_FALL || isInAir()) {
    traceCombatGate("stumble_fall_air");
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }

  if(faiWaitTime>=owner.tickCount() || waitTime>=owner.tickCount()) {
    traceCombatGate("fai_or_script_wait");
    if(!magnetHeading)
      implTurnToFai(*currentTarget,dt);
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }

  if(!bodyOverride && tryMeleeAiBarge()) {
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return true;
    }

  if(targetKnockdown || targetStunned) {
    const bool rising = currentTarget->meleeRiseTick!=0;
    // Use the same root-transformed distance as isInFinishRange, bounded by its script limit.
    const float finishRange = std::min(fghAlgo.attackFinishDistance(owner.script()),ws==WeaponState::W1H ? 165.f : 170.f);
    const bool inRange = targetStunned || rising ? fghAlgo.isInWRange(*this,*currentTarget,owner.script()) :
                                 fghAlgo.qDistTo(*this,*currentTarget)<=finishRange*finishRange;
    const bool focus = fghAlgo.isInFocusAngle(*this,*currentTarget,5.f);
    auto action = FightAlgo::resolveDownTactics(downTactics,currentTarget->meleeKnockdown,rising,inRange,focus);
    if(targetStunned) {
      switch(MeleeParry::resolveStunnedTactics(parryAi,true,inRange,focus)) {
        case MeleeParry::StunnedAction::Move:   action = FightAlgo::MV_MOVE; break;
        case MeleeParry::StunnedAction::Turn:   action = FightAlgo::MV_TURN2HIT; break;
        case MeleeParry::StunnedAction::Attack: action = FightAlgo::MV_ATTACK; break;
        case MeleeParry::StunnedAction::None:   break;
        }
      if(Bench::bodyTracing() && action!=FightAlgo::MV_ATTACK)
        Bench::bodyEvent(*this,currentTarget,"stunned_target",owner.tickCount(),
                            "\"action\":"+Bench::bodyJson(action==FightAlgo::MV_MOVE ? "move" : "turn"));
      }
    if(action==FightAlgo::MV_MOVE) {
      implTurnToFai(*currentTarget,dt);
      setAnim(AnimationSolver::Move);
      go2.set(currentTarget,GT_Enemy);
      go2.nativeCombatTarget = currentTarget;
      mvAlgo.tick(dt,MoveAlgo::FaiMove);
      go2.clear();
      wayPath.clear();
      }
    else {
      visual.stopWalkAnim(*this);
      if(!visual.pose().isStanding())
        setAnim(Anim::Idle);
      implTurnToFai(*currentTarget,dt);
      if(action==FightAlgo::MV_ATTACK && visual.pose().isStanding()) {
        if(targetStunned && tryMeleeAiRiposte())
          return true;
        if(!allowStaminaAction("attack",meleeAttackCost(),true)) {
          waitForMeleeStamina(dt);
          return true;
          }
        const auto hit = owner.physic()->ray(collosionCenter(),currentTarget->collosionCenter());
        if(!hit.hasCol && doAttack(Anim::Attack,BS_HIT)) {
          if(targetStunned && Bench::bodyTracing())
            Bench::bodyEvent(*this,currentTarget,"stunned_target",owner.tickCount(),"\"action\":\"attack\"");
          implFaiWait(visual.pose().atkTotalTime()+1);
          }
        }
      mvAlgo.tick(dt,MoveAlgo::FaiMove);
      }
    return true;
    }

  const auto act = bodyOverride ? bodyAction : fghAlgo.nextFromQueue(*this,*currentTarget,owner.script());
  if(act!=FightAlgo::MV_BLOCK)
    meleeParadeTiming.reset();
  const auto consume = [&] {
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    if(bodyOverride) { BodyController::consumed(*this); return; }
    const auto before = fghAlgo.queuedAction();
    BodyTrajectory::nativeConsumed(*this);
    fghAlgo.consumeAction();
    if(Bench::bodyTracing())
      Bench::bodyEvent(*this,currentTarget,"native_consume",owner.tickCount(),
                          "\"choice_id\":" + std::to_string(fghAlgo.traceChoiceId()) +
                          ",\"primitive\":" + std::to_string(int(before)) +
                          ",\"next_primitive\":" + std::to_string(int(fghAlgo.queuedAction())));
    };

  // NOTE: in original-game, this behaviour seem to be hardcoded
  // test case: wolf jump-back quite often when close, but programmed to jump only if attacked
  // so far promoting wait to jump seem to work best
  FightAlgo::ResolverState nativeResolution;
  nativeResolution.inCloseup = fghAlgo.isInCloseupRange(*this,*currentTarget,owner.script());
  nativeResolution.focus30 = nativeResolution.inCloseup && fghAlgo.isInFocusAngle(*this,*currentTarget);
  const bool jmp = FightAlgo::resolveNative(FightAlgo::MV_WAIT,nativeResolution)==FightAlgo::MV_JUMPBACK;
  const bool jumpBack = act==FightAlgo::MV_JUMPBACK || ((act==FightAlgo::MV_WAIT || act==FightAlgo::MV_TURN) && jmp);
  if(!jumpBack)
    meleeAiDodgeTiming.reset();

  // vanilla behavior, required for orcs in G1 orcgraveyard
  if(ws==WeaponState::NoWeapon && isAiQueueEmpty() && canSwitchWeapon()) {
    drawWeaponMelee();
    return true;
    }

  // Recorder labels describe the physical opportunity after all native gates. The pure
  // normalization mirrors the executor below without changing its queue, RNG or decisions.
  auto trajectoryEffective=act;
  if(BodyTrajectory::enabled() && !bodyOverride) {
    // Diagnostic effective primitives follow the native executor even when LOS makes the
    // separate shared policy mask invalid. Hidden geometry never enters capture features.
    auto resolver=nativeResolution;
    resolver.focus5=fghAlgo.isInFocusAngle(*this,*currentTarget,5.f);
    resolver.inWRange=fghAlgo.isInWRange(*this,*currentTarget,owner.script());
    trajectoryEffective=FightAlgo::resolveNative(act,resolver);
    // Native TURN uses the same approach executor as MOVE outside weapon range. Inside
    // range that branch only rotates. Policy TurnToTarget uses TURN2HIT, so labels must
    // describe those physical semantics rather than the native instruction's name.
    if(trajectoryEffective==FightAlgo::MV_MOVE || trajectoryEffective==FightAlgo::MV_TURN) {
      if(!resolver.inWRange) trajectoryEffective=FightAlgo::MV_MOVE;
      else if(!resolver.focus5) trajectoryEffective=FightAlgo::MV_TURN2HIT;
      }
    }
  BodyTrajectory::Attempt trajectory(*this,!bodyOverride,int(act),int(trajectoryEffective),
                                    fghAlgo.traceChoiceId(),fghAlgo.traceParentInstruction(),fghAlgo.traceEntrySerial());

  if(act==FightAlgo::MV_BLOCK) {
    if(parryAi) {
      const auto now = owner.tickCount();
      const auto skip = [&](std::string_view reason) {
        if(Bench::bodyTracing())
          Bench::bodyEvent(*this,currentTarget,"parade_skipped",now,"\"reason\":"+Bench::bodyJson(reason));
        consume();
        };
      const auto hold = [&] {
        visual.stopWalkAnim(*this);
        if(!visual.pose().isStanding())
          setAnim(Anim::Idle);
        implTurnToFai(*currentTarget,dt);
        mvAlgo.tick(dt,MoveAlgo::FaiMove);
        };
      if(!meleeParadeTiming)
        meleeParadeTiming = MeleeParadeTiming{currentTarget,nullptr,0,now};
      const auto held = now-meleeParadeTiming->began;
      if(held>=1000) {
        skip("timeout");
        return true;
        }
      const auto swing = currentTarget->visual.pose().nextMeleeContact(now);
      if(meleeParadeTiming->sequence==nullptr) {
        switch(MeleeParry::awaitSwing(held,bool(swing))) {
          case MeleeParry::SwingAction::Wait:
            hold();
            return true;
          case MeleeParry::SwingAction::GiveUp:
            skip("no_swing");
            return true;
          case MeleeParry::SwingAction::TimeParade:
            break;
          }
        }
      if(!swing) {
        // A late timing error may put the start after the optimal frame. Keep that
        // deadline while the same swing still exists; an interrupted swing is gone.
        bool same = false;
        for(const auto& layer:currentTarget->visual.pose().combatLayers(now))
          same |= layer.sequence==meleeParadeTiming->sequence && layer.origin==meleeParadeTiming->origin;
        if(!same) {
          skip("swing_gone");
          return true;
          }
        }
      const int talent = std::clamp(hitChance(ws==WeaponState::W2H ? TALENT_2H : TALENT_1H),0,100);
      if(swing && (meleeParadeTiming->sequence!=swing->sequence || meleeParadeTiming->origin!=swing->origin)) {
        const auto began = meleeParadeTiming->began;
        const double u = (double(owner.script().rand(1u<<24,this))+.5)/double(1u<<24);
        const double v = (double(owner.script().rand(1u<<24,this))+.5)/double(1u<<24);
        const float error = MeleeParry::timingSigmaMs(talent)*float(std::sqrt(-2.*std::log(u))*std::cos(2.*M_PI*v));
        meleeParadeTiming = MeleeParadeTiming{currentTarget,swing->sequence,swing->origin,began,swing->contact,error};
        if(Bench::bodyTracing()) {
          const auto start = MeleeParry::paradeStartMs(now,swing->contact,talent,error,meleeParryLeadMs());
          Bench::bodyEvent(*this,currentTarget,"parade_timed",now,
                              "\"predicted_contact_ms\":"+std::to_string(swing->contact-now)+
                              ",\"held_ms\":"+std::to_string(held)+
                              ",\"wait_ms\":"+std::to_string(start-now)+",\"error_ms\":"+std::to_string(error)+
                              ",\"aim_ms\":"+std::to_string(MeleeParry::windowMs(talent)*.5f+meleeParryLeadMs())+
                              ",\"talent\":"+std::to_string(talent));
          }
        }
      if(swing)
        meleeParadeTiming->contact = swing->contact;
      const auto start = MeleeParry::paradeStartMs(now,meleeParadeTiming->contact,talent,meleeParadeTiming->errorMs,meleeParryLeadMs());
      if(now<start) {
        hold();
        return true;
        }
      }
    if(!fghAlgo.isInFocusAngle(*this, *currentTarget)) {
      consume();
      return true;
      }

    switch(ws) {
      case WeaponState::Fist: {
        if(blockFist())
          consume();
        break;
        }
      case WeaponState::W1H:
      case WeaponState::W2H: {
        if(blockSword())
          consume();
        break;
        }
      default:
        consume();
        break;
      }
    return true;
    }

  if(act==FightAlgo::MV_ATTACK || act==FightAlgo::MV_ATTACKL || act==FightAlgo::MV_ATTACKR) {
    if(!bodyOverride && (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) &&
       !allowStaminaAction("attack",meleeAttackCost(),true)) {
      waitForMeleeStamina(dt);
      return true;
      }
    //NOTE: FIGHT_DIST_CANCEL in scipts is often longer, than senses_range of npc
    const auto sense = fghAlgo.isInFocusAngle(*this,*currentTarget,5.f);
    nativeResolution.focus5=sense;
    if(FightAlgo::resolveNative(act,nativeResolution)==FightAlgo::MV_TURN2HIT) {
      traceCombatGate("attack_turn_to_focus");
      implTurnToFai(*currentTarget,dt);
      mvAlgo.tick(dt,MoveAlgo::FaiMove);
      return true;
      }
#if 0
    consume(); //debug
    return true;
#endif

    static const Anim ani[4] = {Anim::Attack, Anim::AttackL, Anim::AttackR};
    if((act!=FightAlgo::MV_ATTACK && bodyState()!=BS_RUN) &&
       !fghAlgo.isInWRange(*this,*currentTarget,owner.script())) {
      consume();
      return true;
      }

    if(ws==WeaponState::Bow || ws==WeaponState::CBow || ws==WeaponState::Mage) {
      bool obsticle = false;
      if(currentTarget!=nullptr) {
        auto hit = owner.physic()->rayNpc(this->mapWeaponBone(),currentTarget->centerPosition(),this);
        if(hit.hasCol && hit.npcHit!=currentTarget) {
          obsticle = true;
          // if(hit.npcHit!=nullptr && owner.script().personAttitude(*this,*hit.npcHit)==ATT_HOSTILE)
          //   obsticle = false;
          if(hit.npcHit!=nullptr && hit.npcHit!=currentTarget && owner.script().isFriendlyFire(*this,*hit.npcHit))
            obsticle = false;
          }
        }
      if(auto spl = activeWeapon()) {
        if(spl->isSpell() && !spl->isSpellShoot())
          obsticle = false;
        }
      if(obsticle) {
        auto anim = (owner.script().rand(2,this)==0 ? Npc::Anim::MoveL : Npc::Anim::MoveR);
        if(setAnim(anim)){
          visual.setAnimRotate(*this,0);
          implFaiWait(visual.pose().animationTotalTime());
          consume();
          return true;
          }
        }
      }

    if(ws==WeaponState::Mage) {
      const auto cast = beginCastSpell();
      if(cast==BeginCastResult::BC_No)
        return false;
      consume();
      }
    else if(ws==WeaponState::Bow || ws==WeaponState::CBow) {
      if(archAi.isDriving(*this)) {
        // aimed by hand (game/archerai): drawn and let go frame by frame, the attack is over with the arrow;
        // a run (a walk) stops first, as shootBow does: the bow is raised standing
        if(bs==BS_RUN || bs==BS_WALK || bs==BS_SNEAK)
          setAnim(Anim::Idle);
        if(archAi.attack(*this))
          consume();
        }
      else if(shootBow()) {
        consume();
        }
      else if(!implTurnToFai(*currentTarget,dt)) {
        aimBow();
        }
      }
    else if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) {
      const auto hit = owner.physic()->ray(this->collosionCenter(),currentTarget->collosionCenter());
      if(hit.hasCol) {
        // blocked by wall
        consume();
        return true;
        }
      if(bodyOverride && act==FightAlgo::MV_ATTACK && tryMeleeAiRiposte()) {
        consume();
        return true;
        }
      if(!bodyOverride && act==FightAlgo::MV_ATTACK && tryMeleeAiCharge()) {
        consume();
        return true;
        }
      const auto atkType = (ws==WeaponState::Fist) ? Anim::Attack : ani[act-FightAlgo::MV_ATTACK];
      const bool atk     = doAttack(atkType, BS_HIT);

      if(atk || mvAlgo.isSwim() || mvAlgo.isDive()) {
        uint64_t aniTime = visual.pose().atkTotalTime()+1;
        implFaiWait(aniTime);
        if(bs==BS_RUN)
          implAniWait(aniTime);
        consume();
        } else {
        implTurnToFai(*currentTarget,dt);
        }
      }
    else {
      // Attack action without any weapon. Can happend at weapon transition(orc shaman) - skip it.
      consume();
      }
    return true;
    }

  if(act==FightAlgo::MV_TURN2HIT) {
    if(!implTurnTo(*currentTarget,dt))
      consume();
    return true;
    }

  if(act==FightAlgo::MV_STRAFEL) {
    if(setAnim(Npc::Anim::MoveL)) {
      visual.setAnimRotate(*this,0);
      implFaiWait(visual.pose().animationTotalTime());
      consume();
      }
    else if(!hasAnim(Npc::Anim::MoveL)) {
      // avoid soft-locks
      visual.setAnimRotate(*this,0);
      consume();
      }
    return true;
    }

  if(act==FightAlgo::MV_STRAFER) {
    if(setAnim(Npc::Anim::MoveR)) {
      visual.setAnimRotate(*this,0);
      implFaiWait(visual.pose().animationTotalTime());
      consume();
      }
    else if(!hasAnim(Npc::Anim::MoveR)) {
      // avoid soft-locks
      visual.setAnimRotate(*this,0);
      consume();
      }
    return true;
    }

  if(act==FightAlgo::MV_STRAFE_E) {
    // finalize strafe
    if(!setAnim(Npc::Anim::Idle))
      return false;
    consume();
    return true;
    }

  if(jumpBack) {
    if(isSwim()) {
      consume();
      return true;
      }
    if(bodyStateMasked()==BS_PARADE) {
      consume();
      return true;
      }
    if(!fghAlgo.isInFocusAngle(*this, *currentTarget) && !jmp) {
      //NOTE: jump-back is ultimate defence, so better to use it only if npc face player directly
      consume();
      aiState.loopNextTime = owner.tickCount(); // force ZS_MM_Attack_Loop call
      return true;
      }
    if(dodgeAi && (!bodyOverride || act==FightAlgo::MV_JUMPBACK)) {
      const auto now = owner.tickCount();
      const auto skip = [&](std::string_view reason) {
        if(Bench::isActive())
          Bench::note(("melee ai dodge "+std::string(Bench::who(this))+": skipped "+std::string(reason)).c_str());
        if(Bench::bodyTracing())
          Bench::bodyEvent(*this,currentTarget,"ai_dodge_skipped",now,"\"reason\":"+Bench::bodyJson(reason));
        consume();
        };
      const auto hold = [&] {
        visual.stopWalkAnim(*this);
        if(!visual.pose().isStanding())
          setAnim(Anim::Idle);
        implTurnToFai(*currentTarget,dt);
        mvAlgo.tick(dt,MoveAlgo::WaitMove);
        };
      if(!meleeAiDodgeTiming) {
        const auto* armor = invent.currentArmor();
        const auto kind = MeleeDodge::kind(armor ? std::optional<MeleeImpact::ArmorClass>(MeleeImpact::armorClass(armor->handle().protection[PROT_EDGE])) :
                                                 std::optional<MeleeImpact::ArmorClass>());
        const auto choose = [&](MeleeDodge::Direction dir) -> std::optional<MeleeDodge::Move> {
          for(const auto& candidate:MeleeDodge::plan(kind,MeleeDodge::Stance::OneHand,dir).candidates)
            if(visual.hasAnim(candidate.name))
              return MeleeDodge::Move{candidate.kind,MeleeDodge::Stance::OneHand,dir};
          return {};
          };
        const auto back = choose(MeleeDodge::Direction::Back);
        if(back || (!bodyOverride && (choose(MeleeDodge::Direction::Left) || choose(MeleeDodge::Direction::Right)))) {
          const auto direction = bodyOverride ? MeleeDodge::Direction::Back :
                                 MeleeAi::dodgeDirection(float(owner.script().rand(1u<<24,this))/float(1u<<24));
          auto move = choose(direction);
          if(!move)
            move = back;
          if(move)
            meleeAiDodgeTiming = MeleeAiDodgeTiming{currentTarget,nullptr,0,now,0,0,*move};
          }
        }
      // With no usable clip execute the original jump back below, including its price.
      if(meleeAiDodgeTiming) {
        if(!bodyOverride && !meleeAiDodgeStreak.allows(now)) {
          skip("streak");
          return true;
          }
        auto& timing = *meleeAiDodgeTiming;
        if(!allowStaminaAction("dodge",MeleeDodge::cost(timing.move.kind),true)) {
          if(bodyOverride) {
            // P30 falls back immediately; only native AI waits for dodge stamina.
            if(visual.startAnimAndGet(*this,Anim::MoveBack,calcAniComb(),ws,walkMode())) {
              spendStamina("jumpback",MeleeStamina::JumpBackCost);
              implFaiWait(visual.pose().animationTotalTime());
              consume();
              }
            return true;
            }
          timing.began = 0; // Stamina recovery is not time spent waiting for a swing.
          waitForMeleeStamina(dt);
          return true;
          }
        if(timing.began==0)
          timing.began = now;
        if(!bodyOverride) {
          if(timing.sequence!=nullptr && now-timing.began>=1000) {
            skip("timeout");
            return true;
            }
          const auto swing = currentTarget->visual.pose().nextMeleeContact(now);
          if(!swing && timing.sequence!=nullptr) {
            bool same = false;
            for(const auto& layer:currentTarget->visual.pose().combatLayers(now))
              same |= layer.sequence==timing.sequence && layer.origin==timing.origin;
            if(!same) {
              skip("swing_gone");
              return true;
              }
            }
          if((swing || timing.sequence!=nullptr) && !fghAlgo.isInWRange(*currentTarget,*this,owner.script())) {
            skip("out_of_reach");
            return true;
            }
          const int talent = hitChance(TALENT_1H);
          if(swing && (timing.sequence!=swing->sequence || timing.origin!=swing->origin)) {
            const double u = (double(owner.script().rand(1u<<24,this))+.5)/double(1u<<24);
            const double v = (double(owner.script().rand(1u<<24,this))+.5)/double(1u<<24);
            timing.sequence = swing->sequence;
            timing.origin = swing->origin;
            timing.errorMs = MeleeParry::timingSigmaMs(talent)*float(std::sqrt(-2.*std::log(u))*std::cos(2.*M_PI*v));
            }
          if(swing)
            timing.contact = swing->contact;
          const auto window = MeleeDodge::invulnerability(timing.move.kind);
          const auto start = MeleeAi::dodgeStartMs(timing.origin,timing.contact,float(window.beginMs+window.endMs)*.5f,talent,timing.errorMs);
          if(timing.sequence!=nullptr && now<start) {
            hold();
            return true;
            }
          }
        if(meleeDodge(timing.move.direction)) {
          const auto kind = timing.move.kind==MeleeDodge::Kind::HeavyRoll ? "heavy roll" : MeleeDodge::name(timing.move.kind);
          const std::string description = std::string(MeleeDodge::name(timing.move.direction))+" "+std::string(kind);
          const auto swingMs = now-timing.origin;
          const auto contactMs = timing.contact-timing.origin;
          if(Bench::isActive())
            Bench::note(("melee ai dodge "+std::string(Bench::who(this))+": "+description+
                            (timing.sequence ? " at "+std::to_string(swingMs)+" ms of "+std::string(Bench::who(currentTarget))+"'s swing, contact "+std::to_string(contactMs)+" ms" :
                                               " now, no swing")).c_str());
          if(Bench::bodyTracing())
            Bench::bodyEvent(*this,currentTarget,"ai_dodge",now,
                                "\"direction\":"+Bench::bodyJson(MeleeDodge::name(timing.move.direction))+
                                ",\"dodge_kind\":"+Bench::bodyJson(MeleeDodge::name(timing.move.kind))+
                                ",\"swing\":"+(timing.sequence ? "true" : "false")+
                                ",\"swing_ms\":"+std::to_string(timing.sequence ? swingMs : 0)+
                                ",\"contact_ms\":"+std::to_string(timing.sequence ? contactMs : 0));
          if(!bodyOverride)
            meleeAiDodgeStreak.dodged(now);
          implFaiWait(visual.pose().animationTotalTime());
          consume();
          }
        return true;
        }
      }
    if(!bodyOverride && (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) &&
       !isDive() && !allowStaminaAction("jumpback",MeleeStamina::JumpBackCost,true)) {
      waitForMeleeStamina(dt);
      return true;
      }
    if(setAnim(Npc::Anim::MoveBack)) {
      implFaiWait(visual.pose().animationTotalTime());
      consume();
      }
    return true;
    }

  if(act==FightAlgo::MV_MOVE || act==FightAlgo::MV_TURN) {
    if(currentTarget->isDown()) {
      if(setAnim(Anim::Idle))
        consume();
      return true;
      }

    const bool prGRange = fghAlgo.isInGRange(*this, *currentTarget, owner.script());
    const bool prWRange = fghAlgo.isInWRange(*this, *currentTarget, owner.script());
    const auto prBs     = bs;

    const float distance = qDistTo(*currentTarget);
    const float range    = float(handle().senses_range);

    if(!prGRange && distance<range*range) {
      // if npc is reasonably far, we can switch to propper pathfinding
      const auto hit = owner.physic()->ray(this->collosionCenter(), currentTarget->collosionCenter());
      if(hit.hasCol) {
        auto near = owner.findWayPoint(currentTarget->position(), [this](const WayPoint &wp) {
          if(!currentTarget->canRayHitPoint(wp.pos))
            return false;
          return true;
          });
        if(near!=nullptr) {
          if(near!=wayPath.last()) {
            wayPath = owner.wayTo(*this,*near);
            go2.set(wayPath.first(), GT_Way);
            go2.nativeCombatTarget = currentTarget;
            }
          return false;
          }
        }
      }

    if(prWRange) {
      //NOTE: bloodfly and other monsters may run to close to player otherwise
      //NOTE2: also for bloodfly we have to use 'hard-stop', to avoid trailing flight
      visual.stopWalkAnim(*this);
      //setAnim(Anim::Idle);
      implTurnToFai(*currentTarget,dt);
      } else {
      if(mvAlgo.checkLastBounce()) {
        if(implTurnToFai(*currentTarget,dt))
          return true;
        }
      setAnim(AnimationSolver::Move);
      go2.set(currentTarget, GT_Enemy);
      go2.nativeCombatTarget = currentTarget;
      mvAlgo.tick(dt, MoveAlgo::FaiMove);
      go2.clear();
      wayPath.clear();
      }

    const bool isGRange = fghAlgo.isInGRange(*this, *currentTarget, owner.script());
    const bool isWRange = fghAlgo.isInWRange(*this, *currentTarget, owner.script());
    const bool isFocus  = fghAlgo.isInFocusAngle(*this, *currentTarget, 5.f);

    if((isWRange || (isGRange!=prGRange) || prBs!=bodyStateMasked()) && isFocus) {
      visual.setAnimRotate(*this, 0);
      consume();
      aiState.loopNextTime = owner.tickCount(); // force ZS_MM_Attack_Loop call
      implAiTick(dt);
      return true;
      }

    implAiTick(dt);
    return true;
    }

  if(act==FightAlgo::MV_WAIT) {
    implFaiWait(200);
    consume();
    stopWalkAnimation();
    return true;
    }

  if(act==FightAlgo::MV_WAITLONG) {
    implFaiWait(300);
    consume();
    stopWalkAnimation();
    return true;
    }

  if(act==FightAlgo::MV_NULL) {
    consume();
    stopWalkAnimation();
    return true;
    }

  return true;
  }

bool Npc::canFinish(Npc& oth) {
  if(isNonlethalFight(oth))
    return false;
  auto ws = weaponState();
  if(ws!=WeaponState::W1H && ws!=WeaponState::W2H)
    return false;

  if(!oth.isUnconscious())
    return false;

  if(!fghAlgo.isInFinishRange(*this,oth,owner.script()))
    return false;
  return true;
  }

bool Npc::doAttack(Anim anim, BodyState bs) {
  if(isMeleeCharging())
    return false;
  if(meleeKnockdown)
    return false;
  if(!allowMeleeStunAction(bs==BS_PARADE ? "parade" : "attack"))
    return false;
  if(bs==BS_PARADE && Gothic::physicsFlags().meleeParry && !allowStaminaAction("parade",0.f))
    return false;
  const auto weaponSt = weaponState();
  const bool costsStamina = hasStamina() && bs==BS_HIT &&
                           (weaponSt==WeaponState::Fist || weaponSt==WeaponState::W1H || weaponSt==WeaponState::W2H) &&
                           !(anim==Anim::AttackFinish && currentTarget!=nullptr && canFinish(*currentTarget));
  if(costsStamina && !allowStaminaAction("attack",meleeAttackCost()))
    return false;
  // Pose accepts a repeated request for an already playing layer too. Only a new
  // layer or combo segment pays, never a repeated input within the same swing.
  const auto staminaBefore = costsStamina ? meleeStaminaIdentity(visual.pose(),owner.tickCount()) : std::string();
  const bool trace = Bench::bodyTracing() && currentTarget!=nullptr;
  const auto before = trace ? combatPoseIdentity() : std::string();
  const auto result = [&](bool accepted) {
    if(accepted && isUsingItemInHand())
      interruptItemUse(bs==BS_PARADE ? "parade" : "attack");
    if(accepted && (bs==BS_HIT || bs==BS_PARADE))
      meleeAiDodgeStreak.reset();
    if(accepted && costsStamina && staminaBefore!=meleeStaminaIdentity(visual.pose(),owner.tickCount()))
      spendStamina("attack",meleeAttackCost());
    if(accepted && bs==BS_PARADE)
      tickMeleeParry();
    BodyObservations::noteEvent(*this,currentTarget,accepted ? "animation_accepted" : "animation_rejected",owner.tickCount());
    if(trace)
      traceCombatAttempt(anim,accepted,before,"do_attack");
    return accepted;
    };
  if(weaponSt==WeaponState::NoWeapon || weaponSt==WeaponState::Mage)
    return result(false);

  if(mvAlgo.isSwim())
    return result(false);

  if(bs==BS_PARADE && hasState(BS_PARADE))
    return result(false);

  const auto wlk = walkMode();
  visual.setAnimRotate(*this,0);
  // A blow at one lying in a knockdown is the original's chop from above (T_1HSFINISH,
  // T_2HSFINISH): the swings at standing height pass over a body on the ground. The blade
  // decides the hit and the damage as for any blow; only the unconscious are finished off.
  if(isMeleeDownStrike(anim,weaponSt))
    if(visual.continueCombo(*this,Anim::AttackFinish,bs,weaponSt,wlk)!=nullptr) {
      meleeDownStrike = true;
      return result(true);
      }
  if(auto sq = visual.continueCombo(*this,anim,bs,weaponSt,wlk)) {
    (void)sq;
    // implAniWait(uint64_t(sq->atkTotalTime(visual.comboLength())+1));
    return result(true);
    }
  return result(false);
  }

void Npc::fistShoot() {
  doAttack(Anim::Attack,BS_HIT);
  }

bool Npc::blockFist() {
  if(meleeKnockdown)
    return false;
  auto weaponSt=weaponState();
  if(weaponSt!=WeaponState::Fist)
    return false;
  visual.setAnimRotate(*this,0);
  return setAnim(Anim::AttackBlock);
  }

bool Npc::isFinishSequence(const Animation::Sequence& seq) {
  return seq.name=="T_1HSFINISH" || seq.name=="T_2HSFINISH";
  }

bool Npc::isMeleeDownStrike(Anim anim, WeaponState ws) const {
  // lying, not yet rising: one who gets up is met by the ordinary blows at his height
  if(!Gothic::physicsFlags().meleePhysicsHits || currentTarget==nullptr || !currentTarget->meleeKnockdown ||
     currentTarget->meleeRiseTick!=0)
    return false;
  if(ws!=WeaponState::W1H && ws!=WeaponState::W2H)
    return false;
  if(anim!=Anim::Attack && anim!=Anim::AttackL && anim!=Anim::AttackR)
    return false;
  return fghAlgo.isInFinishRange(*this,*currentTarget,owner.script());
  }

bool Npc::finishingMove() {
  if(currentTarget==nullptr || !canFinish(*currentTarget))
    return false;

  if(doAttack(Anim::AttackFinish,BS_HIT)) {
    currentTarget->hnpc->attribute[ATR_HITPOINTS] = 0;
    currentTarget->checkHealth(true,false);
    owner.sendPassivePerc(*this,*this,*currentTarget,PERC_ASSESSMURDER);
    return true;
    }
  return false;
  }

void Npc::swingSword() {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return;
  doAttack(Anim::Attack,BS_HIT);
  }

bool Npc::swingSwordL() {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return false;
  return doAttack(Anim::AttackL,BS_HIT);
  }

bool Npc::swingSwordR() {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return false;
  return doAttack(Anim::AttackR,BS_HIT);
  }

bool Npc::blockSword() {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return false;
  return doAttack(Anim::AttackBlock,BS_PARADE);
  // return setAnimAngGet(Anim::AttackBlock,calcAniComb())!=nullptr;
  }
