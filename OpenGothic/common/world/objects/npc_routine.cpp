#include "npc.h"

#include <algorithm>
#include <limits>

#include "asset/activitydefinitions.h"
#include "game/bodycontroller.h"
#include "game/npckeys.h"
#include "world/objects/interactive.h"
#include "world/waymatrix.h"
#include "world/world.h"

bool Npc::startQueuedState(const AiQueue::AiAction& act) {
  // Preserve the routine end time for transitions such as ZS_GotoBed -> ZS_Sleep.
  if(!startState(act.func,act.s0,aiState.eTime,act.i0==0))
    return false;
  setOther(act.target);
  setVictim(act.victim);
  return true;
  }

bool Npc::startState(ScriptFn id, std::string_view wp) {
  return startState(id,wp,gtime::endOfTime(),false);
  }

bool Npc::startState(ScriptFn id, std::string_view wp, gtime endTime, bool noFinalize) {
  if(!id.isValid())
    return false;
  if(BodyController::owns(*this)) {
    const std::string bodyDeferredWp(wp);
    BodyController::deferNative(*this,[this,id,bodyDeferredWp,endTime,noFinalize] {
      startState(id,bodyDeferredWp,endTime,noFinalize);
      });
    return true;
    }

  wakeSociety();
  if(aiState.funcIni==id) {
    if(!noFinalize) {
      // NOTE: B_AssessQuietSound can cause soft-lock on npc without this
      aiState.started = false;
      }
    if(!wp.empty())
      hnpc->wp = wp;
    return true;
    }

  clearAiQueue();
  // The last point commands have drained. A new row at the same post inherits its lock.
  if(aiState.funcIni.isValid() || !activityEvents.activity || aiPrevState!=id || wp!=hnpc->wp)
    clearState(noFinalize);
  if(!wp.empty())
    hnpc->wp = wp;

  {
    // ZS_GotoBed -> ZS_Sleep relie on clean state
    for(size_t i=0; i<PERC_Count; ++i)
      setPerceptionDisable(PercType(i));
  }

  if(wp=="TOT" && aiPolicy!=NpcProcessPolicy::Player && aiPolicy!=NpcProcessPolicy::AiNormal) {
    // workaround for Pedro removal script
    auto& point = owner.deadPoint();
    attachToPoint(nullptr);
    setPosition(point.position());
    }

  auto& st = owner.rules().aiState(id);
  if(isPlayer() && !isPlayerEnabledState(st)) {
    // disable for now, as it causes infinite lock in freeze state
    // https://github.com/Try/OpenGothic/issues/906
    // extra 'aiStandup' to avoid issue with B_StopMagicFreeze
    aiPush(AiQueue::aiStandup());
    return false;
    }

  aiState.started      = false;
  aiState.funcIni      = st.funcIni;
  aiState.funcLoop     = st.funcLoop;
  aiState.funcEnd      = st.funcEnd;
  aiState.sTime        = owner.tickCount();
  aiState.eTime        = endTime;
  aiState.loopNextTime = owner.tickCount();
  aiState.hint         = st.name();
  if(hasRestingBodyActivity())
    owner.noteNpcCellChange(*this);
  if(bodyTraceRecord.id!=0 && !bodyTraceEntered && isDead())
    bodyTraceEntered = true; // a deferred ZS_Dead has started: a later death is a new one
  return true;
  }

void Npc::setBodyTrace(BodyTrace trace) {
  bodyTraceRecord  = std::move(trace);
  bodyTraceEntered = bodyTraceRecord.id!=0 && isDead();
  }

void Npc::clearState(bool noFinalize) {
  finishState(noFinalize,ActivityEvents::Interrupted);
  }

void Npc::finishState(bool noFinalize, uint16_t reason) {
  if(BodyController::deferNative(*this,[this,noFinalize,reason] { finishState(noFinalize,reason); })) return;
  monsterRestSleeping = false;
  resumeBody();
  stopActivityEvents(reason);
  if(aiState.funcIni.isValid() && aiState.started) {
    if(!noFinalize)
      owner.rules().invokeState(this,currentOther,currentVictim,aiState.funcEnd);  // cleanup
    aiPrevState = aiState.funcIni;
    invent.putState(*this,0,0);
    visual.stopItemStateAnim(*this);
    }
  const bool mightStopResting = hasRestingBodyActivity();
  aiState = AiState();
  if(mightStopResting)
    owner.noteNpcCellChange(*this);
  }

bool Npc::isPlayerEnabledState(const ::AiState& st) const {
  // allowed player states are hard-coded
  // https://forum.worldofplayers.de/forum/threads/1533803-G1-AI_StartState-hardcoded-ZS-states-for-Player?p=26034737&viewfull=1#post26034737

  static const std::array playerEnabledStatesG2 = {
    "ZS_DEAD",   "ZS_UNCONSCIOUS", "ZS_MAGICFREEZE",
    "ZS_PYRO",   "ZS_ASSESSMAGIC", "ZS_ASSESSSTOPMAGIC",
    "ZS_ZAPPED", "ZS_SHORTZAPPED", "ZS_MAGICSLEEP",
    "ZS_WHIRLWIND"
    };

  const auto sym = owner.rules().symbolName(st.funcIni);
  if(sym.empty())
    return false;

  for(auto* pState:playerEnabledStatesG2)
    if(sym==pState)
      return true;
  return false;
  }

int Npc::invokeRoutine(ScriptFn function) {
  // A state alone is not provenance: a perception or a bridge command can append
  // story actions while ZS_ATTACK is active. Mark only this state's own callbacks.
  struct Restore final {
    Npc*& slot;
    Npc* previous;
    ~Restore() { slot=previous; }
    } restore{nativeCombatQueueTarget,nativeCombatQueueTarget};
  nativeCombatQueueTarget=std::string_view(aiState.hint)=="ZS_ATTACK" &&
                          (function==aiState.funcIni || function==aiState.funcLoop) ? currentTarget : nullptr;
  return owner.rules().invokeState(this,currentOther,currentVictim,function);
  }

void Npc::runStateInit() {
  aiState.started      = true;
  aiState.loopNextTime = owner.tickCount();
  // WA: for gothic1 dialogs
  perceptionNextTime   = owner.tickCount();
  // The commands of an activity by events' Init are its own: it can take them back (world/activityevents.h).
  const auto* activity = owner.rules().eventActivity(aiState.funcIni);
  stopActivityEvents(0);
  activityCommands = activity!=nullptr;
  invokeRoutine(aiState.funcIni);
  activityCommands = false;
  startActivityEvents(activity);
  }

bool Npc::supportsSocietyActivity(const ActivityEvents::Record& activity) {
  using Form = ActivityDefinitions::PointActivity::Form;
  if(const auto* point = activity.point)
    return point->timed() || point->form==Form::Campfire;
  return activity.mob!=nullptr && (!activity.mob->timer || activity.mob->skipApproachWhenBusy);
  }

bool Npc::societyFurnitureHold() const {
  const auto* record = activityEvents.activity;
  const auto* mob = record!=nullptr ? record->mob : nullptr;
  return mob!=nullptr && activityEvents.stage==ActivityEvents::Stage::Hold &&
         ActivityDefinitions::mobInitChecksBody(aiState.hint) && mob->loopResult==LOOP_CONTINUE &&
         currentInteract!=nullptr && currentInteract->checkMobName(mob->scheme) &&
         currentInteract->stateId()==mob->useState && settledFurniturePose();
  }

const WayPoint* Npc::societyWaitingPoint() const {
  return currentRoutine().point;
  }

std::string_view Npc::societyRoutineBlocker(std::string_view state, bool groupPilot, bool wide) const {
  using Stage = ActivityEvents::Stage;
  using Form = ActivityDefinitions::PointActivity::Form;
  const auto now = owner.time();
  if(state.empty() || std::string_view(aiState.hint)!=state)
    return "native_state_name";
  const auto* record = activityEvents.activity;
  const auto* point = record!=nullptr ? record->point : nullptr;
  const bool guardedPost = state=="ZS_STAND_GUARDING";
  const bool waiting = activityEvents.stage==Stage::Wait && (wide || (groupPilot && guardedPost));
  if(activityEvents.stage!=Stage::Hold && !waiting)
    return "native_activity_stage";
  const bool furniture = wide && owner.persistentSocietyEnabled() && societyFurnitureHold();
  if(wide && (record==nullptr || !supportsSocietyActivity(*record) || (bodySuspended() && !restingBodySuspended())))
    return "native_activity_type";
  if(wide && !furniture && (waiting ? record->mob!=nullptr && record->mob->timer : point==nullptr || !point->timed()))
    return "native_activity_type";
  if(!aiState.started || activityEvents.init!=aiState.funcIni.ptr ||
     activityEvents.pending!=0 || activityEvents.awaitQueue || activityEvents.retry || activityEvents.restart)
    return "native_activity_pending";
  if(!(now<activityEvents.deadline) || !(now<aiState.eTime))
    return "native_deadline";
  if((hnpc->type!=NpcType::AMBIENT && !((wide || (groupPilot && guardedPost)) && hnpc->type==NpcType::MAIN)) ||
     !isHuman() || isDead())
    return "native_actor_type";
  if(isExternalControl())
    return "native_external_control";
  const auto body = bodyStateMasked();
  // The stock standing poses can carry no body-state tag.
  const bool idle = body==BS_NONE && !isFalling() && visual.pose().isInAnim(guardedPost ? "S_HGUARD" : "S_LGUARD");
  if(isInAir() || (!furniture && body!=BS_STAND && !idle))
    return "native_body_state";
  if(weaponState()!=WeaponState::NoWeapon)
    return "native_weapon";
  if(animationLodPinned(furniture) || (furniture && !canPauseDistantPose(0)))
    return "native_pose_pinned";
  if(furniture) {
    if(!usesSimplifiedMovement() || societyWaitingPoint()==nullptr ||
       qDistTo(*currentInteract)>MAX_AI_USE_DISTANCE*MAX_AI_USE_DISTANCE)
      return "native_furniture_anchor";
    } else if(wide && waiting) {
    // Place-release events still arrive through the native wait registration.
    const auto* anchor = societyWaitingPoint();
    constexpr auto reach = WayMatrix::distanceThreshold;
    if(anchor==nullptr || qDistTo(anchor)>reach*reach)
      return "native_wait_anchor";
    } else {
    const bool needsFreePoint = !waiting && (guardedPost || (wide && point!=nullptr && point->form==Form::ArmsCrossed));
    if(currentFp==nullptr || currentFpLock.point()!=currentFp || currentFp->isFreePoint()!=needsFreePoint)
      return "native_waypoint_lock";
    if(needsFreePoint && (!currentFp->isGuarded() || currentFp->useCounter()!=1 ||
                         (wide && (point==nullptr || !isOnFreePoint(point->point)))))
      return "native_guard_lock";
    }
  if(currentTarget!=nullptr)
    return "native_target";
  if(!go2.empty())
    return "native_navigation";
  if(isAiBusy() || aiQueueOverlay.size()!=0)
    return "native_ai_busy";
  if(!outputPipe->isFinished())
    return "native_output";
  return {};
  }

bool Npc::trySocietyEarlyTick() {
  const auto* activity = activityEvents.activity;
  if(activity==nullptr)
    return false;
  const auto now = owner.tickCount();
  const bool loopDue = aiState.loopNextTime<=now;
  const auto* point = activity->point;
  if(point!=nullptr && point->timed() &&
     (loopDue || point->form==ActivityDefinitions::PointActivity::Form::Guard) && pointGestureDue())
    return false;

  // Let the native tick finish any work that precedes an otherwise idle routine.
  if(waitTime>=now || BodyController::owns(*this) || NpcKeys::enabled(*this) ||
     simpleMovement.active!=usesSimplifiedMovement() || mvAlgo.state()!=MoveAlgo::Run ||
     needsLayerPoseUpdate() || visual.pose().hasLayerTickWork() ||
     currentLookAt!=nullptr || currentLookAtNpc!=nullptr ||
     itemUse.item!=0 || foodPool.remaining!=0 || castLevel!=CS_NoCast || peeStream ||
     physicsNoiseUntil!=0 || physicsInvestigationUntil!=0 ||
     bodyImpactWindow.speed!=0.f || bodyImpactSpellFlight || meleeKnockdown ||
     meleePosture.value!=0.f || isMeleeStunned() || meleeChargePhase!=ChargePhase::None ||
     meleeAiSpecial!=MeleeAi::Special::None)
    return false;

  const auto& attributes = hnpc->attribute;
  if(attributes[ATR_REGENERATEHP]<0 || attributes[ATR_REGENERATEMANA]<0 ||
     (attributes[ATR_REGENERATEHP]>0 && attributes[ATR_HITPOINTS]<attributes[ATR_HITPOINTSMAX]) ||
     (attributes[ATR_REGENERATEMANA]>0 && attributes[ATR_MANA]<attributes[ATR_MANAMAX]) ||
     (hasStamina() && (meleeStamina.exhausted || stamina()<staminaMax())))
    return false;

  if(currentInteract!=nullptr) {
    const auto& pose = visual.pose();
    for(size_t i=0; i<pose.logicalLayerCount(); ++i)
      if(!pose.logicalLayer(i).sequence->data->sfx.empty())
        return false;
    }

  if(!owner.societyHoldsRoutine(*this,{}))
    return false;
  if(loopDue)
    aiState.loopNextTime = now + perceptionTimeClampt();
  // These cursors normally advance even when there are no events or healing.
  lastEventTime = now;
  foodPoolTick = now;
  headInput.reset();
  owner.recordSocietyEarlyTick(*this);
  return true;
  }

bool Npc::canSleepMonsterRest() const {
  // The stock Loop and row end share the perception deadline. Keep the
  // original callback, including its random draws, on that native beat.
  constexpr float maxPointDistance = 300.f;
  return aiState.started && std::string_view(aiState.hint)=="ZS_MM_RTN_REST" &&
         activityEvents.init==aiState.funcIni.ptr && activityEvents.activity==nullptr &&
         aiState.loopNextTime>owner.tickCount() && currentFp!=nullptr && currentFpLock.point()==currentFp &&
         qDistTo(currentFp)<maxPointDistance*maxPointDistance && outputPipe->isFinished() &&
         canDeferIdleMonsterBody() && !animationLodPinned();
  }

void Npc::tickRoutine() {
  if(!aiState.funcIni.isValid() && !isPlayer()) {
    auto r = currentRoutine();
    if(r.callback.isValid()) {
      auto t = endTime(r);
      startState(r.callback, r.wayPointName(), t, false);
      }
    else if(hnpc->start_aistate!=0) {
      auto endTime = owner.time();
      endTime.addMilis(uint64_t(gtime(4, 0).toInt()));
      startState(uint32_t(hnpc->start_aistate), "", endTime, false);
      }
    }

  if(!aiState.funcIni.isValid()) {
    owner.societyHoldsRoutine(*this,"no_state");
    return;
    }

  if(!aiState.started) {
    owner.societyHoldsRoutine(*this,"state_init");
    runStateInit();
    return;
    }

  // A routine activity run by events (world/activityevents.h) runs no Loop: its automaton wakes on events
  // below. The end of its row stays here, on the same perception clock as a polled Loop's.
  if(activityEvents.init!=aiState.funcIni.ptr)
    restoreActivityEvents(owner.rules().eventActivity(aiState.funcIni));
  const bool events = activityEvents.activity!=nullptr;
  const auto* pointActivity = events ? activityEvents.activity->point : nullptr;
  const bool atPoint = pointActivity!=nullptr;
  const bool loopDue = aiState.loopNextTime<=owner.tickCount();
  // Native perception, physics, pose and the waypoint lock stay live. A guard
  // can draw a gesture on a tick outside its perception cadence.
  if(owner.societyHoldsRoutine(*this,events ? std::string_view{} : "no_event_activity")) {
    const bool guard = pointActivity!=nullptr && pointActivity->form==ActivityDefinitions::PointActivity::Form::Guard;
    const bool skip = pointActivity==nullptr || !pointActivity->timed() ||
                      (!loopDue && !guard) || !pointGestureDue();
    owner.recordSocietyRoutineExecution(skip);
    if(skip) {
      if(loopDue)
        aiState.loopNextTime = owner.tickCount() + perceptionTimeClampt();
      return;
      }
    }
  bool runLoop = false;
  if(loopDue) {
    aiState.loopNextTime = owner.tickCount() + perceptionTimeClampt();
    int loop = LOOP_CONTINUE;
    if(aiState.funcLoop.isValid()) {
      static const float MAX_DIST = 300;
      const bool fastPath = aiPolicy==NpcProcessPolicy::AiFar2 && routines.empty();
      runLoop = !(fastPath && currentFp!=nullptr && qDistTo(currentFp)<MAX_DIST*MAX_DIST);
      if(runLoop) {
        if(!events && owner.rules().routineTimerDue(*this,aiState.funcLoop))
          loop = invokeRoutine(aiState.funcLoop);
        else if(events && !atPoint)
          tickMobTimer(); // The timer also fires on the final perception beat before End.
        }
      }
    else if(!events) {
      // ZS_DEATH   have no loop-function, in G1, G2-classic
      // ZS_GETMEAT have no loop-function, in G2-notr
      loop = 1;
      }

    if(aiState.eTime<=owner.time()) {
      // Avoid interruption of ZS_TALK/ZS_ATTACK
      if(currentTarget==nullptr && outputPipe->isFinished())
        loop = LOOP_END;
      }

    if(loop!=LOOP_CONTINUE) {
      if(atPoint) {
        wakeActivity(ActivityEvents::RowEnd);
        tickActivityEvents(runLoop);
        }
      finishState(false,ActivityEvents::RowEnd);
      currentOther  = nullptr;
      currentVictim = nullptr;
      return;
      }
    }
  if(events)
    tickActivityEvents(runLoop);
  }

void Npc::forEachRoutine(const std::function<void(const RoutineInfo&)>& f) const {
  for(auto& r:routines)
    f(RoutineInfo{r.start,r.end,r.callback,r.wayPointName(),r.point});
  }

int Npc::currentRoutineIndex() const {
  if(routines.empty())
    return -1;
  const auto& r = currentRoutine();
  for(size_t i=0; i<routines.size(); ++i)
    if(&routines[i]==&r)
      return int(i);
  return -1;
  }

const Npc::Routine& Npc::currentRoutine(bool assertWp) const {
  // find routine for current time
  // if there is no such routine search counter clock-wise until one is found
  auto time = owner.time().timeInDay();
  for(auto& i:routines) {
    if(assertWp && i.point==nullptr)
      continue;
    if(i.end<i.start && (time<i.end || i.start<=time))
      return i;
    if(i.start<=time && time<i.end)
      return i;
    }

  const auto     day   = gtime(24,0).toInt();
  const Routine* rtn   = nullptr;
  int64_t        delta = std::numeric_limits<int64_t>::max();
  for(auto& i:routines) {
    if(assertWp && i.point==nullptr)
      continue;
    int64_t d = time.toInt() - i.end.toInt();
    if(d<0)
      d += day;
    // take the last one if multiple with same end time exist
    if(d<=delta) {
      rtn   = &i;
      delta = d;
      }
    }

  if(rtn!=nullptr)
    return *rtn;
  static Routine r;
  return r;
  }

const WayPoint* Npc::currentTaPoint() const {
  if(routines.empty())
    return owner.findPoint(hnpc->wp,false);
  return currentRoutine(true).point;
  }

gtime Npc::endTime(const Npc::Routine &r) const {
  auto wtime = owner.time();
  auto time  = wtime.timeInDay();

  //NOTE: should we consider time extension for invalid routine sequences?
  if(r.end<r.start) {
    if(time<r.end)
      return gtime(wtime.day(),r.end.hour(),r.end.minute());
    return gtime(wtime.day()+1,r.end.hour(),r.end.minute());
    }
  if(r.start<r.end) {
    if(r.end.hour()==0 || r.end<time)
      return gtime(wtime.day()+1,r.end.hour(),r.end.minute()); else
      return gtime(wtime.day(),r.end.hour(),r.end.minute());
    }
  if(r.start==r.end && r.end.toInt()==0) {
    // for example Rtn_Start_1081 in NTR is filled with zeros
    return gtime(wtime.day()+1,r.end.hour(),r.end.minute());
    }
  // error - routine is not active now
  return wtime;
  }

void Npc::resumeAiRoutine() {
  clearState(false);
  auto& r = currentRoutine();
  if(r.callback.isValid()) {
    auto t = endTime(r);
    startState(r.callback,r.wayPointName(),t,false);
    }
  }

bool Npc::isInState(ScriptFn stateFn) const {
  return aiState.funcIni==stateFn;
  }

bool Npc::isInRoutine(ScriptFn stateFn) const {
  auto& rout = currentRoutine();
  return rout.callback==stateFn && aiState.funcIni==stateFn;
  }

bool Npc::wasInState(ScriptFn stateFn) const {
  return aiPrevState==stateFn;
  }

uint64_t Npc::stateTime() const {
  return owner.tickCount()-aiState.sTime;
  }

void Npc::setStateTime(int64_t time) {
  wakeSociety();
  aiState.sTime = owner.tickCount()-uint64_t(time);
  }

void Npc::addRoutine(gtime s, gtime e, uint32_t callback, std::string_view point) {
  wakeSociety();
  auto wp = world().findPoint(point,false);
  NpcRoutineTable::add(routines,s,e,callback,wp,point);
  }

void Npc::clearRoutine() {
  wakeSociety();
  routines.clear();
  }
