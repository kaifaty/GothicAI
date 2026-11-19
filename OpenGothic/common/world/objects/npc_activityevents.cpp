#include "npc.h"

#include "asset/activitydefinitions.h"
#include "world/mobreservations.h"
#include "world/objects/interactive.h"
#include "world/world.h"

// The automaton of a routine activity at an object (world/activityevents.h). Its code runs only
// in stepActivityEvents(), for a wake; tickActivityEvents() between wakes compares two times.

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

bool Npc::tickActivityEvents() {
  auto& s = activityEvents;
  if(s.init!=aiState.funcIni.ptr) {
    // the state runs without its Init here: a loaded game
    stopActivityEvents(0);
    s.init     = aiState.funcIni.ptr;
    s.activity = owner.rules().eventActivity(aiState.funcIni);
    s.pending  = ActivityEvents::Loaded;
    }
  if(s.activity==nullptr)
    return false;

  const auto now = owner.time();
  // The end of the routine row, on the same condition as the polled Loop: ZS_TALK and ZS_ATTACK are not cut.
  if(aiState.eTime<=now && currentTarget==nullptr && outputPipe->isFinished()) {
    stopActivityEvents(ActivityEvents::RowEnd);
    clearState(false);
    currentOther  = nullptr;
    currentVictim = nullptr;
    return true;
    }
  if(s.awaitQueue && aiQueue.size()==0) {
    s.awaitQueue = false;
    s.pending   |= ActivityEvents::QueueDone;
    }
  if(s.pending==0) {
    if(now<s.deadline)
      return true;
    s.pending = ActivityEvents::Watchdog;
    }
  const uint16_t wakes = s.pending;
  s.pending = 0;
  ActivityEvents::countWakes(aiState.hint,wakes,true);
  stepActivityEvents(wakes);
  return true;
  }

void Npc::stepActivityEvents(uint16_t wakes) {
  using namespace ActivityEvents;
  auto& s = activityEvents;
  if(wakes & Watchdog) {
    seekActivityPlace();
    return;
    }
  if(wakes & Loaded) {
    // after a load: an NPC at an object of its scheme keeps it, any other starts the activity over
    if(currentInteract!=nullptr && currentInteract->checkMobName(s.activity->scheme)) {
      seekActivityPlace();
      return;
      }
    stopActivityEvents(0);
    aiState.started = false;
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
      if(wakes & Freed)
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
  // uses one. A new use reserves its object first; a taken scheme waits for a freed place.
  using namespace ActivityEvents;
  auto&       s      = activityEvents;
  auto&       places = owner.mobReservations();
  const auto& a      = *s.activity;
  const auto  now    = owner.time();
  places.unwait(*this);
  auto*      mob  = owner.availableMob(*this,a.scheme);
  const bool busy = ActivityDefinitions::bodyStateContains(int32_t(bodyState()),a.busyBodyState);
  if(mob!=nullptr && !busy && (mob==currentInteract || places.reserve(*this,*mob))) {
    aiPush(AiQueue::aiUseMob(a.scheme,a.useState));
    s.stage      = Stage::Attach;
    s.awaitQueue = (mob==currentInteract); // no attach event comes for an object already in use
    s.deadline   = after(now,WatchdogMs);
    return;
    }
  places.release(*this);
  if(mob!=nullptr && mob==currentInteract) {
    s.stage    = Stage::Hold;
    s.deadline = gtime::endOfTime();
    return;
    }
  places.wait(*this,a.scheme);
  s.stage    = Stage::Wait;
  s.deadline = after(now,WatchdogMs);
  }
