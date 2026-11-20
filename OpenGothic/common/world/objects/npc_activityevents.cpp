#include "npc.h"

#include "asset/activitydefinitions.h"
#include "world/mobreservations.h"
#include "world/objects/interactive.h"
#include "world/world.h"

// The automaton of a routine activity at an object (world/activityevents.h). Its code runs only
// in stepActivityEvents(), for a wake; tickActivityEvents() between wakes compares a time. The end of
// the row is Npc::tickRoutine's, as for a polled Loop.

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
  auto& places = owner.mobReservations();
  places.release(*this);
  places.unwait(*this);
  activityEvents = ActivityEvents::State();
  }

void Npc::startActivityEvents() {
  // Init ran: a state with its record and events on starts at the approach, a return starts over
  stopActivityEvents(0);
  activityEvents.init     = aiState.funcIni.ptr;
  activityEvents.activity = owner.rules().eventActivity(aiState.funcIni);
  if(activityEvents.activity==nullptr)
    return;
  activityEvents.stage      = ActivityEvents::Stage::Approach;
  activityEvents.awaitQueue = true;
  activityEvents.deadline   = after(owner.time(),ActivityEvents::WatchdogMs);
  ActivityEvents::countWakes(aiState.hint,ActivityEvents::Entered,false);
  }

bool Npc::armActivityEvents() {
  auto& s = activityEvents;
  if(s.init!=aiState.funcIni.ptr) {
    // the state runs without its Init here: a loaded game
    stopActivityEvents(0);
    s.init     = aiState.funcIni.ptr;
    s.activity = owner.rules().eventActivity(aiState.funcIni);
    s.pending  = ActivityEvents::Loaded;
    }
  return s.activity!=nullptr;
  }

void Npc::tickActivityEvents() {
  auto& s = activityEvents;
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
  if(!s.retry && s.pending==0)
    return;
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
  if(aiState.started && aiState.funcIni.isValid() && s.init!=aiState.funcIni.ptr) {
    // A loaded game: the activity's saved use or walk waits for the check after the load.
    if(armActivityEvents() && busy)
      dropActivityCommand();
    return;
    }
  if(s.activity==nullptr || s.retry || (s.stage!=Stage::Approach && s.stage!=Stage::Attach) || !busy)
    return;
  const auto now = owner.time();
  if(s.deadline<=now) {
    fireActivityWatchdog();
    return;
    }
  if(s.stage!=Stage::Attach)
    return;
  const bool rowOver = aiState.loopNextTime<=owner.tickCount() && aiState.eTime<=now &&
                       currentTarget==nullptr && outputPipe->isFinished();
  // The reservation of a use not yet attached expired, or another NPC took the object: the use goes, the
  // activity looks again once the queue is empty.
  const bool lost    = currentInteract==nullptr && owner.mobReservations().reserved(*this)==nullptr;
  if(rowOver || lost)
    dropActivityCommand(); // the routine tick then ends the row or looks again, once the queue is empty
  }

void Npc::fireActivityWatchdog() {
  ActivityEvents::countWakes(aiState.hint,ActivityEvents::Watchdog,false);
  dropActivityCommand();
  activityEvents.retry = true;
  }

void Npc::dropActivityCommand() {
  // Only the activity's own use or its walk to the routine point: a command queued behind runs, the walk of
  // another command goes on, a leave (AI_UseMob with another state) stays.
  const auto& a     = *activityEvents.activity;
  const auto* front = aiQueue.front();
  const bool  use   = front!=nullptr && front->act==AI_UseMob && front->s0==a.scheme && front->i0==a.useState;
  const bool  step  = front!=nullptr && front->act==AI_GoToPoint && front->point!=nullptr && front->point->name==hnpc->wp;
  const bool  walk  = step || (wayPath.last()!=nullptr && wayPath.last()->name==hnpc->wp) ||
                      (go2.wp!=nullptr && go2.wp->name==hnpc->wp);
  if(use || step)
    aiQueue.pop();
  if(use || walk) {
    clearGoTo();
    wayPath.clear();
    }
  owner.mobReservations().release(*this);
  }

void Npc::stepActivityEvents(uint16_t wakes) {
  using namespace ActivityEvents;
  auto& s = activityEvents;
  if(wakes & Watchdog) {
    // a walk to the routine point starts over, any other wait looks for the place again
    if(s.stage==Stage::Approach)
      runStateInit();
    else
      seekActivityPlace();
    return;
    }
  if(wakes & Loaded) {
    // after a load: an NPC at an object of its scheme keeps it, any other starts the activity over
    if(currentInteract!=nullptr && currentInteract->checkMobName(s.activity->scheme)) {
      seekActivityPlace();
      return;
      }
    runStateInit();
    return;
    }
  switch(s.stage) {
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
  const auto& a      = *s.activity;
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
    aiPush(AiQueue::aiUseMob(a.scheme,a.useState));
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
