#include "npc.h"

#include "asset/activitydefinitions.h"
#include "game/npcactivityhost.h"
#include "world/mobreservations.h"
#include "world/objects/interactive.h"
#include "world/world.h"

// Npc::tickRoutine owns the end of the row. Between wakes an active routine only checks its clocks.

namespace {
gtime after(gtime now, uint64_t ms) {
  now.addMilis(ms);
  return now;
  }
}

void Npc::wakeActivity(uint16_t wakes) {
  if(activityEvents.activity!=nullptr)
    activityEvents.pending |= wakes;
  }

void Npc::stopActivityEvents(uint16_t why) {
  if(activityEvents.init==size_t(-1))
    return; // nothing set up: no reservation, no wait
  if(activityEvents.activity!=nullptr && why!=0)
    ActivityEvents::countWakes(aiState.hint,why,false);
  if(activityEvents.activity && activityEvents.activity->point) {
    stopWaitingForPoints();
    dropActivityCommand();
    if(currentFp && currentFp->isFreePoint() && currentFp->checkName(activityEvents.activity->point->point))
      attachToPoint(nullptr);
    } else {
    auto& places = owner.mobReservations();
    places.release(*this);
    places.unwait(*this);
    }
  activityEvents = ActivityEvents::State();
  }

void Npc::startActivityEvents(const ActivityEvents::Record* activity) {
  // Init ran: a state with its record and events on starts at the approach, a return starts over
  activityEvents.init     = aiState.funcIni.ptr;
  activityEvents.activity = activity;
  if(activityEvents.activity==nullptr)
    return;
  activityEvents.stage      = ActivityEvents::Stage::Approach;
  activityEvents.awaitQueue = true;
  activityEvents.deadline   = after(owner.time(),ActivityEvents::WatchdogMs);
  ActivityEvents::countWakes(aiState.hint,ActivityEvents::Entered,false);
  }

void Npc::restoreActivityEvents(const ActivityEvents::Record* activity) {
  stopActivityEvents(0);
  activityEvents.init     = aiState.funcIni.ptr;
  activityEvents.activity = activity;
  activityEvents.pending  = ActivityEvents::Loaded;
  }

void Npc::tickActivityEvents() {
  auto& s = activityEvents;
  if(s.activity->point && (aiQueue.size()!=0 || !go2.empty()))
    return;
  if(s.restart) {
    runStateInit(); // Init again on this tick: the state stays started, an interruption still runs End
    return;
    }
  if(s.awaitQueue && aiQueue.size()==0) {
    s.awaitQueue = false;
    s.pending   |= ActivityEvents::QueueDone;
    }
  if(!s.retry && s.pending==0 && s.deadline<=owner.time())
    fireActivityWatchdog();
  if(!s.retry && s.pending==0) {
    if(s.activity->point)
      tickPointGesture();
    return;
    }
  const uint16_t wakes = s.pending;
  const bool     retry = s.retry;
  s.pending = 0;
  s.retry   = false;
  if(wakes!=0)
    ActivityEvents::countWakes(aiState.hint,wakes,false);
  stepActivityEvents(retry ? uint16_t(wakes|ActivityEvents::Watchdog) : wakes);
  }

void Npc::tickActivityDeadline() {
  // The routine tick runs only with an empty queue and no walk. A wait that a queued use or a walk holds is
  // checked here: its watchdog, and for a queued use also the end of the row on the perception clock.
  using ActivityEvents::Stage;
  auto& s = activityEvents;
  const bool busy = aiQueue.size()>0 || !go2.empty();
  if(s.activity==nullptr || s.retry || (s.stage!=Stage::Approach && s.stage!=Stage::Attach && s.stage!=Stage::WalkToPoint) || !busy)
    return;
  const auto now = owner.time();
  if(s.deadline<=now) {
    fireActivityWatchdog();
    return;
    }
  if(s.stage==Stage::Attach && aiState.loopNextTime<=owner.tickCount() && aiState.eTime<=now &&
     currentTarget==nullptr && outputPipe->isFinished())
    dropActivityCommand(); // the routine tick ends the row once the queue is empty
  }

void Npc::loseActivityReservation() {
  // The use of a lost reservation goes at once: the NPC sits down only with a new one.
  if(activityEvents.activity!=nullptr && activityEvents.stage==ActivityEvents::Stage::Attach && currentInteract==nullptr)
    dropActivityCommand();
  wakeActivity(ActivityEvents::AttachFailed);
  }

void Npc::fireActivityWatchdog() {
  ActivityEvents::countWakes(aiState.hint,ActivityEvents::Watchdog,false);
  dropActivityCommand();
  activityEvents.retry = true;
  }

void Npc::dropActivityCommand() {
  // Only what the activity queued itself (its Init's commands, its use; AiAction::activity) and the walk such a
  // command started (GoTo::activity): other commands and their walks stay.
  if(go2.activity) {
    clearGoTo();
    wayPath.clear();
    }
  aiQueue.removeActivity();
  if(activityEvents.activity->mob)
    owner.mobReservations().release(*this);
  }

void Npc::stepActivityEvents(uint16_t wakes) {
  using namespace ActivityEvents;
  auto& s = activityEvents;
  if(s.activity->point) {
    if((wakes & Watchdog) && s.stage==Stage::Approach) {
      runStateInit();
      return;
      }
    seekActivityPoint(wakes);
    if(!(wakes & Loaded))
      tickPointGesture();
    return;
    }
  if(wakes & Watchdog) {
    // a walk to the routine point starts over, any other wait looks for the place again
    if(s.stage==Stage::Approach)
      runStateInit();
    else
      seekActivityPlace();
    return;
    }
  if(wakes & Loaded) {
    // after a load the saved queue and walk ran as they would with the polled Loop; now one check of the place:
    // an NPC attached to an object of its scheme holds it, whatever state the object is in; any other looks for one
    if(currentInteract!=nullptr && currentInteract->checkMobName(s.activity->mob->scheme)) {
      countWakes(aiState.hint,0,true);
      s.stage    = Stage::Hold;
      s.deadline = gtime::endOfTime();
      return;
      }
    seekActivityPlace();
    return;
    }
  switch(s.stage) {
    case Stage::WalkToPoint:
    case Stage::Off:
      break;
    case Stage::Approach:
      if(wakes & QueueDone)
        seekActivityPlace();
      break;
    case Stage::Attach:
      if(wakes & (Attached|AttachFailed|QueueDone))
        seekActivityPlace();
      break;
    case Stage::Wait:
      // a freed place, or the body itself free of another object
      if(wakes & (Freed|Detached))
        seekActivityPlace();
      break;
    case Stage::Hold:
      if(wakes & Detached)
        seekActivityPlace();
      break;
    }
  }

void Npc::seekActivityPlace() {
  // The polled Loop's check, once per wake: use a free object of the scheme unless the body already
  // uses one. A new use reserves its object first; a taken scheme waits for a freed place. An object
  // in use at the use's state is held even when the body state is not the record's busy one (the
  // water pipe): the script would queue the same finished AI_UseMob again on every Loop.
  using namespace ActivityEvents;
  auto&       s      = activityEvents;
  auto&       places = owner.mobReservations();
  const auto& a      = *s.activity->mob;
  const auto  now    = owner.time();
  ActivityEvents::countWakes(aiState.hint,0,true);
  places.unwait(*this);
  auto*      mob  = owner.availableMob(*this,a.scheme);
  const bool busy = ActivityDefinitions::bodyStateContains(int32_t(bodyState()),a.busyBodyState);
  if(mob!=nullptr && mob==currentInteract && (busy || mob->stateId()==a.useState)) {
    places.release(*this);
    s.stage    = Stage::Hold;
    s.deadline = gtime::endOfTime();
    return;
    }
  if(mob!=nullptr && !busy && (mob==currentInteract || places.reserve(*this,*mob))) {
    pushActivityCommand(AiQueue::aiUseMob(a.scheme,a.useState));
    s.stage      = Stage::Attach;
    s.awaitQueue = (mob==currentInteract); // no attach event comes for an object already in use
    s.deadline   = after(now,WatchdogMs);
    return;
    }
  places.release(*this);
  places.wait(*this,a.scheme);
  s.stage    = Stage::Wait;
  s.deadline = after(now,WatchdogMs);
  }

void Npc::pushActivityCommand(AiQueue::AiAction&& action) {
  action.activity = true;
  aiPush(std::move(action));
  }

void Npc::stopWaitingForPoints() {
  for(auto* p:activityEvents.waitPoints)
    p->stopWaiting(*this);
  activityEvents.waitPoints.clear();
  }

void Npc::seekActivityPoint(uint16_t wakes) {
  using namespace ActivityEvents;
  auto& s = activityEvents;
  const auto& a = *s.activity->point;
  auto& stage = hnpc->aivar[19];
  countWakes(aiState.hint,0,true);
  stopWaitingForPoints();

  // Old saves can have several owners. Give up only this NPC's share.
  if(currentFp && currentFp->isFreePoint() && currentFp->checkName(a.point) &&
     (currentFpLock.point()!=currentFp || currentFp->useCounter()>1))
    attachToPoint(nullptr);
  const bool on = isOnFreePoint(a.point);
  bool occluded = false;
  if(!on || (wakes & AttachFailed)) {
    const auto* wp = NpcEngineCalls::resolveWaypoint(owner,*this,hnpc->wp);
    const bool away = wp && NpcEngineCalls::distanceToWaypoint(*this,wp)>a.approachDistance;
    const auto* post = away ? nullptr : owner.findFreePoint(*this,a.point,&s.waitPoints);
    if(post!=nullptr) {
      s.waitPoints.clear();
      auto walk = AiQueue::aiGoToPoint(*post);
      walk.s0 = a.point; // the saved GotoFP search name distinguishes a contested post from an ordinary GotoWP
      pushActivityCommand(std::move(walk));
      pushActivityCommand(AiQueue::aiStandup());
      pushActivityCommand(AiQueue::aiAlignToFp());
      stage        = a.walkingStage;
      s.stage      = Stage::WalkToPoint;
      s.awaitQueue = true;
      s.deadline   = after(owner.time(),WatchdogMs);
      return;
      }
    if(wp && (away || currentFp!=wp)) {
      s.waitPoints.clear();
      pushActivityCommand(AiQueue::aiGoToPoint(*wp));
      s.stage      = Stage::Approach;
      s.awaitQueue = true;
      s.deadline   = after(owner.time(),WatchdogMs);
      return;
      }
    for(auto* p:s.waitPoints)
      p->waitForFree(*this);
    occluded = owner.findFreePoint(centerPosition(),a.point)!=nullptr;
    }
  const auto next = on ? Stage::Hold : Stage::Wait;
  if(!(wakes & Loaded) || stage!=a.poseStage)
    pushActivityCommand(on ? AiQueue::aiAlignToFp() : AiQueue::aiAlignToWp());
  if(stage==a.walkingStage)
    stage = a.arrivedStage;
  if(stage==a.arrivedStage) {
    pushActivityCommand(AiQueue::aiPlayAnim(a.enterAnimation));
    stage = a.poseStage;
    }
  s.stage    = next;
  s.deadline = occluded ? after(owner.time(),WatchdogMs) : gtime::endOfTime();
  }

void Npc::tickPointGesture() {
  using namespace ActivityEvents;
  const auto& s = activityEvents;
  const auto& a = *s.activity->point;
  if((s.stage!=Stage::Hold && s.stage!=Stage::Wait) || int32_t(stateTime()/1000)<=a.timerSeconds ||
     hnpc->aivar[19]!=a.poseStage)
    return;
  countWakes(aiState.hint,Gesture,false);
  const auto& gesture = a.gestures[owner.rules().rollGuardGesture(*this,uint32_t(a.rollRange))];
  if(!gesture.animation.empty())
    pushActivityCommand(AiQueue::aiPlayAnim(gesture.animation));
  if(gesture.resetTimer)
    setStateTime(0);
  }
