#include "npc.h"

#include <Tempest/Log>

#include "camera.h"
#include "commandline.h"
#include "game/bodycontroller.h"
#include "gothic.h"
#include "testbench.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"

using namespace Tempest;

void Npc::nextAiAction(AiQueue& queue, uint64_t dt) {
  if(isInAir() && !hasQueuedPhysicalSpellState(queue))
    return;
  if(queue.size()==0)
    return;
  auto act = queue.pop();
  NpcLogicalState::Admission logicalAdmission(*this,act.act);
  switch(act.act) {
    case AI_None: break;
    case AI_LookAtNpc:{
      currentLookAt=nullptr;
      currentLookAtNpc=act.target;
      break;
      }
    case AI_LookAt:{
      currentLookAtNpc=nullptr;
      currentLookAt=act.point;
      break;
      }
    case AI_TurnAway: {
      if(!prepareTurn()) {
        queue.pushFront(std::move(act));
        break;
        }
      if(act.target!=nullptr && implTurnAway(*act.target,dt)) {
        queue.pushFront(std::move(act));
        break;
        }
      break;
      }
    case AI_TurnToNpc: {
      if(!prepareTurn()) {
        queue.pushFront(std::move(act));
        break;
        }
      if(act.target!=nullptr && implTurnTo(*act.target,dt)) {
        queue.pushFront(std::move(act));
        break;
        }
      // Not looking quite correct in dialogs, when npc turns around
      // Example: Esteban dialog
      // currentLookAt    = nullptr;
      // currentLookAtNpc = nullptr;
      break;
      }
    case AI_WhirlToNpc: {
      if(!prepareTurn()) {
        queue.pushFront(std::move(act));
        break;
        }
      if(act.target!=nullptr && implWhirlTo(*act.target,dt)) {
        queue.pushFront(std::move(act));
        break;
        }
      break;
      }
    case AI_GoToNpc:
      if(!setInteraction(nullptr)) {
        queue.pushFront(std::move(act));
        break;
        }
      attachToPoint(nullptr);
      if(usesSimplifiedMovement() && act.target!=nullptr) {
        act.point=owner.findWayPoint(act.target->position(),[](const WayPoint& p) { return p.isConnected(); });
        if(act.point==nullptr) {
          fallbackSimpleMovement("NPC target has no connected waypoint");
          } else {
          const auto* anchor=act.point;
          beginSimpleGoTo(std::move(act),*anchor);
          break;
          }
        }
      go2.set(act.target,act.point!=nullptr || act.i0>0 ? GT_Point : GT_Way);
      go2.pointDistance = float(act.i0);
      go2.nativeCombatTarget = act.nativeCombatTarget==act.target ? act.nativeCombatTarget : nullptr;
      wayPath.clear();
      if(act.point!=nullptr)
        queue.pushFront(std::move(act));
      break;
    case AI_GoToNextFp: {
      if(!setInteraction(nullptr)) {
        queue.pushFront(std::move(act));
        break;
        }
      auto fp = act.point!=nullptr ? act.point : owner.findNextFreePoint(*this,act.s0);
      if(fp!=nullptr) {
        attachToPoint(fp);
        if(usesSimplifiedMovement()) {
          act.point=fp;
          beginSimpleGoTo(std::move(act),*fp);
          break;
          }
        go2.set(fp,GoToHint::GT_NextFp);
        wayPath.clear();
        if(act.point!=nullptr)
          queue.pushFront(std::move(act));
        }
      break;
      }
    case AI_GoToPoint: {
      if(isInAir() || !setInteraction(nullptr)) {
        queue.pushFront(std::move(act));
        break;
        }
      if(usesSimplifiedMovement() && act.point->isFreePoint()) {
        act.i0=1;
        const auto* target=act.point;
        beginSimpleGoTo(std::move(act),*target);
        break;
        }
      if(wayPath.last()!=act.point) {
        wayPath = usesSimplifiedMovement() ? simpleWayTo(*act.point) : owner.wayTo(*this,*act.point);
        simpleMovement.edgeTo = nullptr;
        auto wpoint = wayPath.pop();

        if(wpoint!=nullptr) {
          go2.set(wpoint);
          attachToPoint(wpoint);
          } else {
          attachToPoint(act.point);
          clearGoTo();
          }
        }
      break;
      }
    case AI_StopLookAt:
      currentLookAtNpc=nullptr;
      currentLookAt=nullptr;
      visual.setHeadRotation(0,0);
      break;
    case AI_RemoveWeapon:
      if(!isDead()) {
        if(closeWeapon(false)) {
          stopWalkAnimation();
          }
        auto ws = weaponState();
        if(ws!=WeaponState::NoWeapon){
          queue.pushFront(std::move(act));
          }
        }
      break;
    case AI_StartState:
      startQueuedState(act);
      break;
    case AI_PlayAnim:{
      if(auto sq = playAnimByName(act.s0,BS_NONE)) {
        logicalAdmission.sequence=sq;
        implAniWait(uint64_t(sq->totalTime()));
        implFaiWait(uint64_t(sq->totalTime()));
        } else {
        if(visual.hasAnim(act.s0))
          queue.pushFront(std::move(act));
        }
      break;
      }
    case AI_PlayAnimBs:{
      BodyState bs = BodyState(act.i0);
      if(auto sq = playAnimByName(act.s0,bs)) {
        logicalAdmission.sequence=sq;
        implAniWait(uint64_t(sq->totalTime()));
        implFaiWait(uint64_t(sq->totalTime()));
        } else {
        if(visual.hasAnim(act.s0)) {
          queue.pushFront(std::move(act));
          } else {
          /* ZS_MM_Rtn_Sleep will set NPC_WALK mode and run T_STAND_2_SLEEP animation.
           * The problem is: T_STAND_2_SLEEP may not exists, in that case only NPC_WALK should be applied,
           * we will do so by playing Idle anim.
           */
          setAnim(Anim::Idle);
          }
        }
      break;
      }
    case AI_Wait:
      if(isExternalControl() && CommandLine::inst().isDevMode()) {
        const auto* symbol = owner.script().findSymbol(instanceSymbol());
        Log::i("[ai] queue_wait ",symbol!=nullptr ? symbol->name() : ""," game_ms=",owner.tickCount(),
               " duration_ms=",act.i0);
        }
      implAiWait(uint64_t(act.i0));
      break;
    case AI_StandUp:
    case AI_StandUpQuick: {
      if(ragdoll) {
        if(isDead())
          break;
        if(!recoverRagdoll()) {
          queue.pushFront(std::move(act));
          break;
          }
        }
      const auto bs = bodyStateMasked();
      // NOTE: B_ASSESSTALK calls AI_StandUp, to make npc stand, if it's not on a chair or something
      if(interactive()!=nullptr) {
        if((interactive()->isLadder() && !isPlayer()) || !setInteraction(nullptr,false)) {
          queue.pushFront(std::move(act));
          }
        break;
        }
      else if(bs==BS_UNCONSCIOUS || bs==BS_LIE) {
        if(!setAnim(Anim::Idle))
          queue.pushFront(std::move(act)); else
          implAniWait(visual.pose().animationTotalTime());
        }
      else if(bs!=BS_DEAD) {
        visual.stopAnim(*this,"");
        setStateItem(MeshObjects::Mesh(),"");
        setAnim(Anim::Idle);
        }
      break;
      }
    case AI_EquipArmor:
      invent.equipArmor(act.i0,*this);
      break;
    case AI_EquipBestArmor:
      invent.equipBestArmor(*this);
      break;
    case AI_EquipMelee:
      invent.equipBestMeleeWeapon(*this);
      break;
    case AI_EquipRange:
      invent.equipBestRangedWeapon(*this);
      break;
    case AI_UseMob: {
      if(act.i0<0) {
        if(!setInteraction(nullptr))
          queue.pushFront(std::move(act));
        break;
        }
      /*
       * Rhademes doesn't quit talk properly
      if(owner.script().isTalk(*this)) {
        queue.pushFront(std::move(act));
        break;
        }*/

      auto inter = owner.availableMob(*this,act.s0);
      if(inter==nullptr) {
        /* in L`Hiver, version 1.3 there is a typo: "COOL" instead of "BSCOOL"
         * maybe 'scheme' need to be checked loosely, or maybe ignored.
         *
         * For now, if no mob found - discard command, to avoid npc soft-lock.
         */
        // queue.pushFront(std::move(act));
        break;
        }

      if(currentInteract!=nullptr && inter!=currentInteract) {
        setInteraction(nullptr);
        queue.pushFront(std::move(act));
        break;
        }

      if(inter!=nullptr) {
        auto pos = inter->nearestPoint(*this);
        if(currentInteract==nullptr && !MoveAlgo::isClose(*this, pos, MAX_AI_USE_DISTANCE)) { // too far
          go2.set(pos);
          // go to MOBSI and then complete AI_UseMob
          queue.pushFront(std::move(act));
          return;
          }
        if(!setInteraction(inter)) {
          // queue.pushFront(std::move(act));
          }
        }

      if(currentInteract==nullptr || currentInteract->stateId()!=act.i0) {
        queue.pushFront(std::move(act));
        return;
        }

      clearGoTo();
      break;
      }
    case AI_UseItem: {
      if(!isStanding()) {
        setAnim(Npc::Anim::Idle);
        queue.pushFront(std::move(act));
        break;
        }
      if(act.i0!=0)
        useItem(uint32_t(act.i0));
      break;
      }
    case AI_UseItemToState:
      if(act.i0!=0) {
        uint32_t itm   = uint32_t(act.i0);
        int      state = act.i1;
        if(state>0)
          visual.stopDlgAnim(*this);
        if(!invent.putState(*this,state>=0 ? itm : 0,state))
          queue.pushFront(std::move(act));
        }
      break;
    case AI_Teleport: {
      setPosition (act.point->position() );
      setDirection(act.point->direction());
      if(isPlayer()) {
        updateTransform();
        Gothic::inst().camera()->reset(this);
        }
      }
      break;
    case AI_DrawWeapon:
      if(canSwitchWeapon()) {
        if(!drawWeaponMelee() &&
           !drawWeaponBow())
          queue.pushFront(std::move(act));
        }
      break;
    case AI_DrawWeaponMelee:
      if(canSwitchWeapon()) {
        if(!drawWeaponMelee())
          queue.pushFront(std::move(act));
        }
      break;
    case AI_DrawWeaponRange:
      if(canSwitchWeapon()) {
        if(!drawWeaponBow())
          queue.pushFront(std::move(act));
        }
      break;
    case AI_DrawSpell: {
      if(canSwitchWeapon()) {
        const int32_t spell = act.i0;
        if(!invent.hasSpell(spell))
          break;
        if(drawSpell(spell))
          aiExpectedInvest = act.i1; else
          queue.pushFront(std::move(act));
        }
      break;
      }
    case AI_Attack:
      if(!BodyController::owns(*this) && currentTarget!=nullptr) {
        if(!fghAlgo.fetchInstructions(*this,*currentTarget,owner.script()))
          queue.pushFront(std::move(act));
        }
      break;
    case AI_Flee:
      if(!implAiFlee(dt))
        queue.pushFront(std::move(act));
      break;
    case AI_Dodge:
      if(auto sq = setAnimAngGet(Anim::MoveBack)) {
        visual.setAnimRotate(*this,0);
        implAniWait(uint64_t(sq->totalTime()));
        } else {
        queue.pushFront(std::move(act));
        }
      break;
    case AI_UnEquipWeapons:
      invent.unequipWeapons(owner.script(),*this);
      break;
    case AI_UnEquipArmor:
      invent.unequipArmor(owner.script(),*this);
      break;
    case AI_Output:
    case AI_OutputSvm:
    case AI_OutputSvmOverlay:{
      if(performOutput(act)) {
        uint64_t msgTime = 0;
        if(aiPolicy!=NpcProcessPolicy::AiFar2) {
          if(act.act==AI_Output) {
            msgTime = owner.script().messageTime(act.s0);
            } else {
            auto svm  = owner.script().messageFromSvm(act.s0,hnpc->voice);
            msgTime   = owner.script().messageTime(svm);
            }
          visual.startFaceAnim(*this,"VISEME",1,msgTime);
          // the speaker and the one it speaks to look at each other while the line lasts
          if(act.target!=nullptr && act.target!=this) {
            static const uint64_t tail = 1000;
            setTalkPartner(act.target,msgTime+tail);
            act.target->setTalkPartner(this,msgTime+tail);
            }
          }
        if(TestBench::isActive())
          TestBench::npcOutput(*this,act.s0,act.target,msgTime);
        if(act.act!=AI_OutputSvmOverlay) {
          visual.startAnimDialog(*this);
          visual.setAnimRotate(*this,0);
          }
        } else {
        queue.pushFront(std::move(act));
        }
      break;
      }
    case AI_ProcessInfo: {
      const int PERC_DIST_DIALOG = 2000;

      if(act.target==nullptr)
        break;

      if(owner.isInDialog()) {
        queue.pushFront(std::move(act));
        break;
        }

      if(this!=act.target && act.target->isPlayer() && act.target->currentInteract!=nullptr) {
        //queue.pushFront(std::move(act));
        break;
        }

      if(act.target->qDistTo(*this)>PERC_DIST_DIALOG*PERC_DIST_DIALOG) {
        break;
        }

      if(act.target->interactive()==nullptr && !act.target->isAiBusy())
        act.target->stopWalkAnimation();
      if(interactive()==nullptr && !isAiBusy())
        stopWalkAnimation();

      if(auto p = owner.script().openDlgOuput(*this,*act.target)) {
        outputPipe = p;
        setOther(act.target);
        act.target->setOther(this);
        act.target->outputPipe = p;
        } else {
        queue.pushFront(std::move(act));
        }
      }
      break;
    case AI_StopProcessInfo:
      if(outputPipe->close()) {
        outputPipe = owner.script().openAiOuput();
        if(currentOther!=nullptr)
          currentOther->outputPipe = owner.script().openAiOuput();
        } else {
        queue.pushFront(std::move(act));
        }
      break;
    case AI_ContinueRoutine:
      resumeAiRoutine();
      break;
    case AI_AlignToWp:
    case AI_AlignToFp:{
      if(auto fp = currentFp){
        if(fp->dir.x!=0.f || fp->dir.z!=0.f){
          if(usesSimplifiedMovement())
            prepareTurn();
          if(implTurnTo(fp->dir.x,fp->dir.z,AnimationSolver::TurnType::Std,dt))
            queue.pushFront(std::move(act));
          }
        }
      break;
      }
    case AI_SetNpcsToState:{
      const int32_t r = act.i0*act.i0;
      owner.detectNpc(position(),float(hnpc->senses_range),[&act,this,r](Npc& other) {
        if(&other==this)
          return;
        if(other.isDead())
          return;
        if(qDistTo(other)>float(r))
          return;
        other.aiPush(AiQueue::aiStartState(act.func,1,other.currentOther,other.currentVictim,other.hnpc->wp));
        });
      break;
      }
    case AI_SetWalkMode:{
      setWalkMode(WalkBit(act.i0));
      break;
      }
    case AI_FinishingMove:{
      if(act.target==nullptr || !act.target->isUnconscious())
        break;

      if(!fghAlgo.isInFinishRange(*this,*act.target,owner.script())){
        queue.pushFront(std::move(act));
        go2.set(act.target);
        setAnim(Npc::Anim::Move);
        implGoTo(dt,fghAlgo.attackFinishDistance(owner.script()));
        }
      else if(!isStanding()) {
        clearGoTo();
        queue.pushFront(std::move(act));
        }
      else if(!implTurnTo(*act.target,dt)) {
        queue.pushFront(std::move(act));
        }
      else if(canFinish(*act.target)){
        setTarget(act.target);
        if(!finishingMove())
          queue.pushFront(std::move(act));
        }
      break;
      }
    case AI_TakeItem:{
      if(act.item==nullptr)
        break;
      if(takeItem(*act.item)==nullptr)
        queue.pushFront(std::move(act));
      break;
      }
    case AI_GotoItem:{
      simpleMovement.nativeItem=act.item;
      if(usesSimplifiedMovement()) {
        if(act.item==nullptr && !(act.point!=nullptr && act.i0==1))
          break;
        const auto target=act.item!=nullptr ? act.item->position() : go2.pos;
        act.point=owner.findWayPoint(target,[](const WayPoint& p) { return p.isConnected(); });
        if(act.point==nullptr) {
          fallbackSimpleMovement("item target has no connected waypoint");
          } else {
          const auto* anchor=act.point;
          beginSimpleGoTo(std::move(act),*anchor);
          break;
          }
        }
      if(act.point!=nullptr) {
        go2.clear();
        if(act.item!=nullptr)
          go2.set(act.item);
        else if(act.i0==1)
          go2.flag=GT_Item;
        if(!go2.empty())
          queue.pushFront(std::move(act));
        break;
        }
      go2.set(act.item);
      break;
      }
    case AI_PointAt:{
      if(act.point==nullptr)
        break;
      if(!implPointAt(act.point->position()))
        queue.pushFront(std::move(act));
      break;
      }
    case AI_PointAtNpc:{
      if(act.target==nullptr)
        break;
      if(!implPointAt(act.target->position()))
        queue.pushFront(std::move(act));
      break;
      }
    case AI_StopPointAt:{
      visual.stopAnim(*this,"T_POINT");
      break;
      }
    case AI_PrintScreen:{
      auto  msg     = act.s0;
      auto  posx    = act.i0;
      auto  posy    = act.i1;
      int   timesec = act.i2;
      auto  font    = act.s1;

      bool complete = false;
      if(aiOutputBarrier<=owner.tickCount()) {
        if(outputPipe->printScr(*this,timesec,msg,posx,posy,font))
          complete = true;
        }

      if(!complete)
        queue.pushFront(std::move(act));
      break;
      }
    }
  }
