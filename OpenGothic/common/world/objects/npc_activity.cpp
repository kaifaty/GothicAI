#include "npc.h"

#include "game/bodycontroller.h"
#include "settings.h"
#include "world/mobreservations.h"
#include "world/npcactivityprobe.h"
#include "world/objects/interactive.h"
#include "world/world.h"

void Npc::aiPush(AiQueue::AiAction&& a) {
  // Vanilla quiet-sound scripts aim at OTHER. A physical sound is located at the
  // collision, and OTHER may be null or a distant culprit, so native turning owns it.
  if(physicsNoiseCallback && (a.act==AI_TurnToNpc || a.act==AI_LookAtNpc))
    return;
  a.physicsNoiseState = physicsNoiseCallback && a.act==AI_StartState;
  a.activity = a.activity || activityCommands;
  if(!a.activity && activityEvents.activity && activityEvents.activity->point) {
    activityEvents.awaitQueue = true; // recheck the place after external commands have finished
    if(a.act==AI_GoToPoint && isGuardPoint(a.point))
      a.i1=AiQueue::AiAction::ExternalPointWalk;
    }
  a.nativeCombatTarget=nativeCombatQueueTarget;
  if(BodyController::owns(*this) && a.act!=AI_OutputSvmOverlay)
    BodyController::takeover(*this,"native_script_queue");
  if(a.act==AI_OutputSvmOverlay)
    aiQueueOverlay.pushBack(std::move(a),this); else
    aiQueue.pushBack(std::move(a),this);
  }

bool Npc::setInteraction(Interactive *id, bool quick) {
  if(currentInteract==id)
    return true;

  if(currentInteract!=nullptr) {
    return currentInteract->detach(*this,quick);
    }

  if(id==nullptr)
    return (currentInteract==nullptr);

  if(id->attach(*this)) {
    currentInteract = id;
    owner.mobReservations().attached(*this,*id);
    if(auto* probe=NpcActivityProbe::instance) probe->mob(*this,*id,true);
    if(!isPlayer() && (isHuman() || Settings::physicsFlags().monsterCharacterVirtual)) {
      mvAlgo.resetCharacterVirtual(true);
      resetCharacterVisualHeight();
      }
    // Remove the contact rig in the sequential physics handoff, including
    // when an immediate detach leaves only its transition animation behind.
    livingWasInteracting = true;
    attachToPoint(nullptr); //NOTE: Fajeth campfire
    if(!quick) {
      visual.stopAnim(*this,"");
      setAnimRotate(0);
      }
    return true;
    }

  return false;
  }

void Npc::quitInteraction() {
  if(currentInteract==nullptr)
    return;
  if(!isPlayer() && (isHuman() || Settings::physicsFlags().monsterCharacterVirtual)) {
    mvAlgo.resetCharacterVirtual(true);
    resetCharacterVisualHeight();
    }
  if(invTorch)
    processDefInvTorch();
  setDirectionY(0);
  if(auto* probe=NpcActivityProbe::instance) probe->mob(*this,*currentInteract,false);
  auto* mob = currentInteract;
  currentInteract=nullptr;
  owner.mobReservations().detached(*this,*mob);
  }
