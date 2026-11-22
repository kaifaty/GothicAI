#include "npc.h"

#include "asset/activitydefinitions.h"
#include "game/npcactivityhost.h"
#include "world/mobreservations.h"
#include "world/objects/interactive.h"
#include "world/waymatrix.h"
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
  const auto* record=activityEvents.activity;
  const bool atPoint=record && record->point;
  // Point activities take and count their final step before End.
  if(record && aiState.started && why!=0 && !(atPoint && why==ActivityEvents::RowEnd))
    ActivityEvents::countWakes(aiState.hint,why,false);
  if(atPoint) {
    const auto& point=*record->point;
    // A point Loop leaves its physical post through movement or interaction, even if End
    // was queued before the pose began. The guard has its own accepted row handover.
    const bool keepPoint=point.form!=ActivityDefinitions::PointActivity::Form::Guard &&
                         isOnFreePoint(point.point) && currentFpLock.point()==currentFp;
    stopWaitingForPoints();
    if(why==ActivityEvents::RowEnd) {
      const auto& next=currentRoutine();
      if(!keepPoint && isActivityPoint(currentFp) && (next.callback!=aiState.funcIni || next.wayPointName()!=hnpc->wp))
        attachToPoint(nullptr);
      // The last walk/align/gesture precedes End in the queue. Keep ownership
      // until the next state is known, including a dialogue queued during the handover.
      activityEvents=ActivityEvents::State();
      activityEvents.activity=record;
      activityEvents.init=aiState.funcIni.ptr;
      return;
      }
    dropActivityCommand();
    if(!keepPoint && isActivityPoint(currentFp) && !(why==0 && activityEvents.stage==ActivityEvents::Stage::Off))
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

void Npc::tickActivityEvents(bool runLoop) {
  auto& s = activityEvents;
  const auto* point = s.activity->point;
  if(point && (aiQueue.size()!=0 || !go2.empty()))
    return;
  if(s.restart && !(s.pending & ActivityEvents::RowEnd)) {
    runStateInit(); // Init again on this tick: the state stays started, an interruption still runs End
    return;
    }
  if(s.awaitQueue && aiQueue.size()==0) {
    s.awaitQueue = false;
    s.pending   |= ActivityEvents::QueueDone;
    }
  if(!s.retry && s.pending==0 && s.deadline<=owner.time()) {
    if(point && s.stage==ActivityEvents::Stage::Wait)
      s.pending |= ActivityEvents::Visibility;
    else
      fireActivityWatchdog();
    }
  const auto wakes = s.pending;
  if(wakes!=0 || s.retry) {
    const auto retry = s.retry;
    s.pending = 0;
    s.retry   = false;
    if(wakes!=0)
      ActivityEvents::countWakes(aiState.hint,wakes,false);
    stepActivityEvents(retry ? uint16_t(wakes|ActivityEvents::Watchdog) : wakes);
    }
  // Arrivals and releases are independent of the routine clock. The new timed poses
  // draw only on the perception beat that would have run their Loop.
  if(point && point->timed() &&
     (point->form==ActivityDefinitions::PointActivity::Form::Guard ? !(wakes & ActivityEvents::Loaded) : runLoop))
    tickPointGesture();
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
    if((wakes & Watchdog) && !(wakes & RowEnd) && s.stage==Stage::Approach &&
       s.activity->point->form==ActivityDefinitions::PointActivity::Form::Guard) {
      runStateInit();
      return;
      }
    seekActivityPoint(wakes);
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
  if(const auto* point=activityEvents.activity->point) {
    action.i1=AiQueue::AiAction::SavedGuardCommand;
    if(action.act==AI_GoToPoint && action.point->isFreePoint())
      action.s0=point->point;
    }
  aiPush(std::move(action));
  }

bool Npc::isActivityPoint(const WayPoint* point) const {
  const auto* record=activityEvents.activity;
  return record && record->point && point && point->isFreePoint() && point->checkName(record->point->point);
  }

void Npc::stopWaitingForPoints() {
  for(auto* p:activityEvents.waitPoints)
    p->stopWaiting(*this);
  activityEvents.waitPoints.clear();
  }

void Npc::seekActivityPoint(uint16_t wakes) {
  using namespace ActivityEvents;
  using Form = ActivityDefinitions::PointActivity::Form;
  auto& s = activityEvents;
  const auto& a = *s.activity->point;
  auto& stage = hnpc->aivar[19];
  countWakes(aiState.hint,0,true);
  stopWaitingForPoints();

  if(a.form==Form::Waypoint) {
    // Init already queued GotoWP and AlignToWP. Its Loop only enters the stance and gestures.
    if(stage==a.initialStage) {
      pushActivityCommand(AiQueue::aiPlayAnim(a.enterAnimation));
      stage=a.poseStage;
      }
    s.stage=Stage::Hold;
    s.deadline=gtime::endOfTime();
    return;
    }
  const bool campfire = a.form==Form::Campfire;
  const bool seated = campfire && ActivityDefinitions::bodyStateContains(int32_t(bodyState()),a.enterBodyState);
  // The guard returns to its authored row after a lost walk. The other FP Loops
  // choose from the NPC's actual position, including after a relocation or a load.
  const auto* wp = a.form==Form::Guard ? NpcEngineCalls::resolveWaypoint(owner,*this,hnpc->wp) : nullptr;
  // An old save has no pending failure. A post beyond both the approach and the FP search
  // could only have come from a different destination, even if its name matches.
  const float reach = float(a.approachDistance) + WayMatrix::distanceThreshold;
  const bool foreign = wp && isActivityPoint(currentFp) && (wakes & (Loaded|Detached)) &&
                       currentFp->qDistTo(wp->position())>reach*reach;
  if(isActivityPoint(currentFp) && (currentFpLock.point()!=currentFp || currentFp->useCounter()>1 ||
                                 foreign || !isOnFreePoint(a.point)))
    attachToPoint(nullptr);
  const bool on = isOnFreePoint(a.point);
  if(on && !currentFp->isGuarded())
    currentFpLock=FpLock(currentFp,true); // Init can start on a post held by the preceding script.
  if(seated) {
    s.stage=Stage::Hold;
    s.deadline=gtime::endOfTime();
    return;
    }
  bool occluded = false;
  if(!on || (wp && (wakes & AttachFailed))) {
    const bool returning = wp && !(wakes & RowEnd) &&
                           (NpcEngineCalls::distanceToWaypoint(*this,wp)>a.approachDistance ||
                            ((wakes & (Detached|AttachFailed)) && currentFp!=wp));
    const auto* post = returning ? nullptr : owner.findFreePoint(*this,a.point,&s.waitPoints);
    if(post!=nullptr) {
      s.waitPoints.clear();
      pushActivityCommand(AiQueue::aiGoToPoint(*post));
      pushActivityCommand(AiQueue::aiStandup());
      pushActivityCommand(AiQueue::aiAlignToFp());
      if(!campfire)
        stage = a.walkingStage;
      s.stage      = Stage::WalkToPoint;
      s.awaitQueue = true;
      s.deadline   = after(owner.time(),WatchdogMs);
      return;
      }
    // A previous free point (e.g. a campfire) must be vacated. An ordinary waypoint
    // inside the Init radius needs only the Loop's AlignToWP and guard pose.
    if(returning || (wp && currentFp && currentFp!=wp && currentFp->isFreePoint() && !(wakes & RowEnd))) {
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
  s.stage    = on ? Stage::Hold : Stage::Wait;
  s.deadline = occluded ? after(owner.time(),WatchdogMs) : gtime::endOfTime();
  if(campfire) {
    if(on) {
      pushActivityCommand(AiQueue::aiAlignToFp());
      pushActivityCommand(AiQueue::aiPlayAnimBs(a.enterAnimation,BodyState(a.enterBodyState)));
      s.awaitQueue=true; // a cleared sitting command leaves the body standing
      }
    return;
    }
  if(!(wakes & Loaded) || stage!=a.poseStage)
    pushActivityCommand(on ? AiQueue::aiAlignToFp() : AiQueue::aiAlignToWp());
  if(stage==a.walkingStage)
    stage = a.arrivedStage;
  if(stage==a.arrivedStage && (a.form!=Form::Pee || on)) {
    if(a.form==Form::Pee)
      pushActivityCommand(AiQueue::aiAlignToFp());
    if(a.form==Form::Pee || a.form==Form::ArmsCrossed)
      pushActivityCommand(AiQueue::aiStandup());
    pushActivityCommand(AiQueue::aiPlayAnim(a.enterAnimation));
    stage = a.poseStage;
    }
  }

void Npc::tickPointGesture() {
  using namespace ActivityEvents;
  const auto& s = activityEvents;
  if(s.stage!=Stage::Hold && s.stage!=Stage::Wait)
    return;
  const auto& a = *s.activity->point;
  if(int32_t(stateTime()/1000)<=a.timerSeconds || hnpc->aivar[19]!=a.poseStage)
    return;
  countWakes(aiState.hint,Gesture,false);
  owner.rules().runPointGesture(*this,a);
  }
