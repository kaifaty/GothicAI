#include "npc.h"
#include "npc_transformback.h"

#include <Tempest/Log>
#include <climits>
#include <exception>
#include <memory>

#include "game/serialize.h"
#include "gothic.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"

using namespace Tempest;

namespace {

constexpr uint32_t BodyTraceMagic = 0x31544442; // "BDT1": /body-trace of a dead NPC

}

void Npc::save(Serialize &fout, size_t id, std::string_view directory) {
  tickFoodPool();
  fout.setEntry("worlds/",fout.worldName(),directory,id,"/data");
  fout.write(*hnpc);
  fout.write(body,head,vHead,vTeeth,bdColor,vColor,bdFatness);
  if(meleeKnockdown)
    fout.write(meleeSavePosition.x,meleeSavePosition.y,meleeSavePosition.z,angle,sz);
  else
    fout.write(x,y,z,angle,sz);
  fout.write(wlkMode,trGuild,talentsSk,talentsVl,refuseTalkMilis);
  fout.write(permAttitude,tmpAttitude);
  fout.write(perceptionTime,perceptionNextTime);
  for(auto& i:perception)
    fout.write(i.func);

  // extra state
  fout.write(lastHitType,lastHitSpell);
  if(currentSpellCast<uint32_t(-1))
    fout.write(uint32_t(currentSpellCast)); else
    fout.write(uint32_t(-1));
  fout.write(uint8_t(castLevel),castNextTime,manaInvested,aiExpectedInvest);
  fout.write(spellInfo);

  saveTrState(fout);
  saveAiState(fout);

  fout.write(currentInteract,currentOther,currentVictim);
  fout.write(currentLookAt,currentLookAtNpc,currentTarget,nearestEnemy);

  go2.save(fout);
  fout.write(currentFp,currentFpLock);
  wayPath.save(fout);

  mvAlgo.save(fout);
  fghAlgo.save(fout);
  fout.write(lastEventTime,angleY,runAng);
  fout.write(invTorch);
  fout.write(isUsingTorch());

  Vec3 phyPos = meleeKnockdown ? meleeSavePosition : physic.position();
  fout.write(phyPos);

  fout.setEntry("worlds/",fout.worldName(),directory,id,"/visual");
  visual.save(fout,*this);

  fout.setEntry("worlds/",fout.worldName(),directory,id,"/inventory");
  if(!invent.isEmpty() || id==size_t(-1))
    invent.save(fout);

  // Optional independent entry: the old NPC/visual byte layouts remain readable.
  fout.setEntry("worlds/",fout.worldName(),directory,id,"/ragdoll");
  RagdollSave::Snapshot state;
  state.legacyDown = !meleeKnockdown && (ragdollLegacyDown || ragdollRecovering);
  if(!meleeKnockdown)
    state.body = ragdoll ? std::optional<Ragdoll::State>(ragdoll->state()) : ragdollDormant;
  RagdollSave::save(fout,state);
  if(!meleeKnockdown) {
    fout.setEntry("worlds/",fout.worldName(),directory,id,"/ragdoll-recovery");
    visual.pose().saveRagdollRecovery(fout);
    }
  fout.setEntry("worlds/",fout.worldName(),directory,id,"/ragdoll-quest-return");
  auto savedReturn = ragdollQuestReturn;
  if(meleeKnockdown) {
    savedReturn.armed = false;
    savedReturn.hazards = 0;
    }
  RagdollSave::saveQuestReturn(fout,savedReturn);
  if(visual.stuckArrowCount()>0) {
    fout.setEntry("worlds/",fout.worldName(),directory,id,"/stuck-arrows");
    visual.saveStuckArrows(fout);
    }
  // Optional independent entry: older builds skip it, and a body without it is a legacy one.
  if(isDead() && bodyTraceRecord.id!=0) {
    fout.setEntry("worlds/",fout.worldName(),directory,id,"/body-trace");
    fout.write(BodyTraceMagic,uint16_t(1),bodyTraceRecord.id,uint64_t(bodyTraceRecord.diedAt+1),
               uint64_t(bodyTraceRecord.clockFrom+1),bodyTraceRecord.culprit);
    }
  if(foodPool.remaining>0) {
    fout.setEntry("worlds/",fout.worldName(),directory,id,"/food-pool");
    Healing::save(fout,foodPool);
    }
  if(itemUse.item!=0) {
    fout.setEntry("worlds/",fout.worldName(),directory,id,"/item-use");
    fout.write(uint32_t(itemUse.item),itemUse.started,itemUse.food);
    }
  }

void Npc::load(Serialize &fin, size_t id, std::string_view directory) {
  simpleMovement = {};
  endMeleeAiSpecial();
  meleeAiSpecialSlot = {};
  meleeAiSpecialEnded.reset();
  bodyImpactWindow.clear();
  bodyImpactLaunchCulprit.reset();
  bodyImpactPendingCulprit.reset();
  bodyImpactSpellFlight=false;
  endMeleeStun("reset");
  meleeParadeOrigin.reset();
  meleeStamina = {};
  itemUse = {};
  Healing::clear(foodPool);
  foodPoolTick = owner.tickCount();
  collectingFood = false;
  meleeFighter = {};
  meleeChargePhase = ChargePhase::None;
  meleeChargeStart = 0;
  meleeChargeMult = 1.f;
  meleeChargeReleased = false;
  meleePosture.reset();
  meleeKnockdown = false;
  meleeGetupForced = false;
  meleeFallTick = meleeRiseTick = 0;
  meleeSlowSince = 0;
  releaseRagdoll(false);
  ragdollDormant.reset();
  ragdollRecovering = false;
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  ragdollQuestReturn = {};
  ragdollQuestSpellThrow = false;
  ragdollQuestSampleTick = 0;
  ragdollQuestReturns = 0;
  ragdollQuestFrom = {};
  fin.setEntry("worlds/",fin.worldName(),directory,id,"/data");

  hnpc = std::make_shared<daedalus::INpc>();
  hnpc->user_ptr        = this;
  fin.readNpc(owner.script().getVm(), hnpc);
  fin.read(body,head,vHead,vTeeth,bdColor,vColor,bdFatness);

  auto* sym = owner.script().findSymbol(hnpc->symbol_index());
  if (sym != nullptr)
    sym->set_instance(hnpc);

  fin.read(x,y,z,angle,sz);
  loadedFpPosition = position();
  loadedFpReferences = true;
  resetCharacterVisualHeight();
  lieVisualYOffset=0;
  fin.read(wlkMode,trGuild,talentsSk,talentsVl,refuseTalkMilis);
  durtyTranform = TR_Pos|TR_Rot|TR_Scale;
  if(fin.version()<55)
    angle -= 90;

  fin.read(permAttitude,tmpAttitude);
  fin.read(perceptionTime,perceptionNextTime);
  for(auto& i:perception)
    fin.read(i.func);

  // extra state
  fin.read(lastHitType,lastHitSpell);
  {
  uint32_t currentSpellCastU32 = uint32_t(-1);
  fin.read(currentSpellCastU32);
  currentSpellCast = (currentSpellCastU32==uint32_t(-1) ? size_t(-1) : currentSpellCastU32);
  }
  fin.read(reinterpret_cast<uint8_t&>(castLevel),castNextTime);
  if(fin.version()>44)
    fin.read(manaInvested,aiExpectedInvest);
  fin.read(spellInfo);
  loadTrState(fin);
  loadAiState(fin);

  fin.read(currentInteract,currentOther,currentVictim);
  if(fin.version()>=42)
    fin.read(currentLookAt);
  fin.read(currentLookAtNpc,currentTarget,nearestEnemy);

  go2.load(fin);
  fin.read(currentFp,currentFpLock);
  wayPath.load(fin);

  mvAlgo.load(fin);
  fghAlgo.load(fin);
  fin.read(lastEventTime,angleY,runAng);

  bool isUsingTorch = false;
  if(fin.version()>36) {
    fin.read(invTorch);
    fin.read(isUsingTorch);
    }

  Vec3 phyPos = {};
  fin.read(phyPos);

  fin.setEntry("worlds/",fin.worldName(),directory,id,"/visual");
  visual.load(fin,*this);
  physic.setPosition(phyPos);

  setVisualBody(vHead,vTeeth,vColor,bdColor,body,head);

  if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/inventory"))
    invent.load(fin,*this);
  if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/item-use")) {
    uint32_t item = 0;
    fin.read(item,itemUse.started,itemUse.food);
    itemUse.item = item;
    }
  if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/food-pool"))
    Healing::load(fin,foodPool);
  if(isDown() || !Gothic::physicsFlags().foodOverTime)
    Healing::clear(foodPool);

  // post-alignment
  updateTransform();
  if(isUsingTorch)
    visual.setTorch(true,owner);
  if(isDead())
    physic.setEnable(false);

  if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/ragdoll-quest-return") &&
     !RagdollSave::loadQuestReturn(fin,ragdollQuestReturn))
    Log::e("ragdoll: invalid quest NPC return point");

  // Absence denotes an old save: existing dead/KO poses stay animated until a new down event.
  ragdollLegacyDown = isDown();
  if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/ragdoll")) {
    RagdollSave::Snapshot state;
    if(RagdollSave::load(fin,state)) {
      ragdollLegacyDown = state.legacyDown;
      if(state.body && isDown())
        ragdollDormant = std::move(state.body);
      if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/ragdoll-recovery")) {
        try {
          visual.pose().loadRagdollRecovery(fin);
          ragdollRecovering = !ragdollDormant && visual.pose().isRagdollBlending();
          } catch(const std::exception& e) {
          visual.pose().clearRagdollPose();
          Log::e("ragdoll recovery load: ",e.what());
          }
        }
      if(ragdollDormant && Gothic::physicsFlags().ragdoll)
        prepareRagdoll();
      else if(visual.pose().isRagdollBlending())
        updateAnimation(0,true); // Optional recovery must be visible before the first loaded frame.
      } else {
      Log::e("ragdoll: invalid saved body, keeping the animated pose");
      }
    }
  // A live posture fall has no saved body. Wounded loops resume the ordinary
  // get-up transition without entering the script's unconscious state.
  if(Gothic::physicsFlags().meleePosture && !isDown() &&
     (visual.pose().isInAnim("S_WOUNDED") || visual.pose().isInAnim("S_WOUNDEDB"))) {
    setAnim(Anim::Idle);
    }
  // Existing save entries distinguish a live airborne throw from KO recovery.
  const auto movement = mvAlgo.state();
  ragdollQuestSpellThrow = ragdollQuestReturn.armed && !isDown() &&
      !ragdollLegacyDown && !ragdollRecovering &&
      (movement==MoveAlgo::InAir || movement==MoveAlgo::Falling ||
       movement==MoveAlgo::Swim || movement==MoveAlgo::Dive);
  if(Gothic::physicsFlags().arrowSticking &&
     fin.setEntry("worlds/",fin.worldName(),directory,id,"/stuck-arrows") &&
     !visual.loadStuckArrows(fin,owner))
    Log::e("arrows: invalid saved NPC attachments, keeping the body without arrows");
  bodyTraceRecord  = {};
  bodyTraceEntered = false;
  if(isDead() && fin.setEntry("worlds/",fin.worldName(),directory,id,"/body-trace")) {
    try {
      uint32_t magic=0, traceId=0;
      uint16_t schema=0;
      uint64_t died=0, clock=0;
      BodyTrace trace;
      fin.read(magic,schema,traceId,died,clock,trace.culprit);
      const auto minute = [](uint64_t v) { return v>0 && v<=uint64_t(INT64_MAX) ? int64_t(v-1) : int64_t(-1); };
      trace.id        = traceId;
      trace.diedAt    = minute(died);
      trace.clockFrom = minute(clock);
      if(magic==BodyTraceMagic && schema==1 && traceId!=0 && traceId!=uint32_t(-1) && trace.culprit.size()<=512)
        setBodyTrace(std::move(trace)); else
        Log::e("body trace: invalid saved record, the body counts as one from an older save");
      }
    catch(const std::exception& e) {
      bodyTraceRecord  = {};
      bodyTraceEntered = false;
      Log::e("body trace load: ",e.what());
      }
    }
  syncLogicalBodyState(true);
  }

void Npc::postValidate() {
  if(currentInteract!=nullptr && !currentInteract->isAttached(*this))
    currentInteract = nullptr;
  if(const auto* pending=pendingSimpleGoTo(); pending!=nullptr && pending->act==AI_GotoItem && pending->i1>0) {
    auto action=aiQueue.pop();
    action.item=owner.itmById(uint32_t(action.i1-1));
    action.i1=0;
    simpleMovement.nativeItem=action.item;
    aiQueue.pushFront(std::move(action));
    }
  syncLogicalBodyState(true);
  }

void Npc::saveAiState(Serialize& fout) const {
  fout.write(aniWaitTime,waitTime,faiWaitTime,outWaitTime);
  fout.write(uint8_t(aiPolicy));
  fout.write(aiState.funcIni,aiState.funcLoop,aiState.funcEnd,aiState.sTime,aiState.eTime,aiState.started,aiState.loopNextTime);
  fout.write(aiPrevState);

  const auto* pending=pendingSimpleGoTo();
  const uint32_t pendingItem = (pending!=nullptr && pending->act==AI_GotoItem && pending->item!=nullptr) ?
                               owner.itmId(&pending->item->handle()) : uint32_t(-1);
  if(pendingItem!=uint32_t(-1)) {
    // Item pointers were never part of AiQueue's layout. Its unused i1 carries
    // the world-item index for a retained graph terminal, without adding bytes.
    auto saved=aiQueue;
    auto action=saved.pop();
    action.i1=int(pendingItem)+1;
    saved.pushFront(std::move(action));
    saved.save(fout);
    } else {
    // A vanished item must not fail the save: the action loads as a plain GotoItem.
    if(pending!=nullptr && pending->act==AI_GotoItem && pending->item!=nullptr)
      Log::e("npc_graph: ",displayName()," GotoItem target is no longer in the world; saved without it");
    aiQueue.save(fout);
    }
  aiQueueOverlay.save(fout);

  fout.write(uint32_t(routines.size()));
  for(auto& i:routines) {
    fout.write(i.start,i.end,i.callback,i.point,i.fallbackName);
    }
  }

void Npc::loadAiState(Serialize& fin) {
  fin.read(aniWaitTime);
  fin.read(waitTime,faiWaitTime);
  fin.read(outWaitTime);
  fin.read(reinterpret_cast<uint8_t&>(aiPolicy));
  fin.read(aiState.funcIni,aiState.funcLoop,aiState.funcEnd,aiState.sTime,aiState.eTime,aiState.started,aiState.loopNextTime);
  fin.read(aiPrevState);

  aiState.hint = "";
  if(auto s = owner.script().findSymbol(aiState.funcIni.ptr)) {
    aiState.hint = s->name().c_str();
    }

  aiQueue.load(fin);
  aiQueueOverlay.load(fin);

  uint32_t size=0;
  fin.read(size);
  routines.resize(size);
  for(auto& i:routines) {
    fin.read(i.start,i.end,i.callback,i.point);
    if(fin.version()>51)
      fin.read(i.fallbackName);
    }
  }

void Npc::saveTrState(Serialize& fout) const {
  if(transformSpl!=nullptr) {
    fout.write(true);
    transformSpl->save(fout);
    } else {
    fout.write(false);
    }
  }

void Npc::loadTrState(Serialize& fin) {
  bool hasTr = false;
  fin.read(hasTr);
  if(hasTr)
    transformSpl.reset(new TransformBack(*this, owner.script().getVm(), fin));
  }
