#include "npc.h"

#include <algorithm>
#include <limits>

#include "game/bodycontroller.h"
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

  auto& st = owner.script().aiState(id);
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
  if(bodyTraceRecord.id!=0 && !bodyTraceEntered && isDead())
    bodyTraceEntered = true; // a deferred ZS_Dead has started: a later death is a new one
  return true;
  }

void Npc::setBodyTrace(BodyTrace trace) {
  bodyTraceRecord  = std::move(trace);
  bodyTraceEntered = bodyTraceRecord.id!=0 && isDead();
  }

void Npc::clearState(bool noFinalize) {
  if(BodyController::deferNative(*this,[this,noFinalize] { clearState(noFinalize); })) return;
  if(aiState.funcIni.isValid() && aiState.started) {
    if(!noFinalize)
      owner.script().invokeState(this,currentOther,currentVictim,aiState.funcEnd);  // cleanup
    aiPrevState = aiState.funcIni;
    invent.putState(*this,0,0);
    visual.stopItemStateAnim(*this);
    }
  aiState = AiState();
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

  const auto* sym = owner.script().findSymbol(st.funcIni);
  if(sym==nullptr)
    return false;

  for(auto* pState:playerEnabledStatesG2)
    if(sym->name()==pState)
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
  return owner.script().invokeState(this,currentOther,currentVictim,function);
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

  if(!aiState.funcIni.isValid())
    return;

  if(!aiState.started) {
    aiState.started      = true;
    aiState.loopNextTime = owner.tickCount();
    // WA: for gothic1 dialogs
    perceptionNextTime   = owner.tickCount();
    invokeRoutine(aiState.funcIni);
    return;
    }

  const bool fastPath = (aiPolicy==NpcProcessPolicy::AiFar2 && routines.empty()); //HACK: don't process far away Npc
  if(aiState.loopNextTime<=owner.tickCount()) {
    aiState.loopNextTime = owner.tickCount() + perceptionTimeClampt();
    int loop = LOOP_CONTINUE;
    if(aiState.funcLoop.isValid()) {
      static const float MAX_DIST = 300;
      if(fastPath && currentFp!=nullptr && qDistTo(currentFp) < MAX_DIST*MAX_DIST) {
        loop = LOOP_CONTINUE;
        }
      else if(fastPath && currentFp!=nullptr) {
        // for debugging
        loop = invokeRoutine(aiState.funcLoop);
        }
      else {
        loop = invokeRoutine(aiState.funcLoop);
        }
      } else {
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
      clearState(false);
      currentOther  = nullptr;
      currentVictim = nullptr;
      }
    }
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
  aiState.sTime = owner.tickCount()-uint64_t(time);
  }

void Npc::addRoutine(gtime s, gtime e, uint32_t callback, std::string_view point) {
  auto wp = world().findPoint(point,false);

  Routine r;
  r.start    = s;
  r.end      = e;
  r.callback = callback;
  r.point    = wp;
  if(wp==nullptr)
    r.fallbackName = point;
  routines.push_back(r);

  std::stable_sort(routines.begin(), routines.end(), [](const Routine& l, const Npc::Routine& r) {
    return l.start < r.start;
    });
  }

void Npc::clearRoutine() {
  routines.clear();
  }

void Npc::excRoutine(size_t callback) {
  routines.clear();
  owner.script().invokeState(this,currentOther,currentVictim,callback);
  // aiState.eTime = gtime();
  }
