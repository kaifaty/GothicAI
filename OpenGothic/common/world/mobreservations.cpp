#include "mobreservations.h"

#include "game/rpgvocabulary.h"
#include "world/activityevents.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <unordered_set>

MobReservations::MobReservations(World& world)
  :world(world) {
  }

bool MobReservations::isFree(const Interactive& mob, const Npc& npc) const {
  if(!mob.isAvailable())
    return false;
  for(auto& r:reservations)
    if(r.mob==&mob)
      return r.owner==&npc;
  return true;
  }

Interactive* MobReservations::reserved(const Npc& npc) const {
  for(auto& r:reservations)
    if(r.owner==&npc)
      return r.mob;
  return nullptr;
  }

bool MobReservations::reserve(Npc& npc, Interactive& mob) {
  if(!isFree(mob,npc))
    return false;
  auto until = world.time();
  until.addMilis(ActivityEvents::ReservationMs);
  for(auto& r:reservations)
    if(r.owner==&npc && r.mob==&mob) {
      r.until = until;
      return true;
      }
  release(npc);
  reservations.push_back({&npc,&mob,until});
  ++ActivityEvents::stats().reserved;
  return true;
  }

void MobReservations::release(const Npc& npc) {
  for(size_t i=0; i<reservations.size(); ++i)
    if(reservations[i].owner==&npc) {
      drop(i);
      return;
      }
  }

void MobReservations::drop(size_t i) {
  const auto* mob = reservations[i].mob;
  reservations.erase(reservations.begin()+int(i));
  freed(*mob);
  }

void MobReservations::attached(Npc& npc, Interactive& mob) {
  auto& st = ActivityEvents::stats();
  if(mob.userCount()>1)
    ++st.doubled;
  for(size_t i=0; i<reservations.size();) {
    auto& r = reservations[i];
    if(r.mob==&mob && r.owner==&npc) {
      // reserved -> occupied: the attachment is the place's state now
      reservations.erase(reservations.begin()+int(i));
      continue;
      }
    if(r.mob==&mob) {
      // taken past the reservation (a script's AI_UseMob, the hero): the owner looks again
      ++st.bypassed;
      auto* owner = r.owner;
      reservations.erase(reservations.begin()+int(i));
      owner->wakeActivity(ActivityEvents::AttachFailed);
      continue;
      }
    if(r.owner==&npc) {
      drop(i);
      continue;
      }
    ++i;
    }
  npc.wakeActivity(ActivityEvents::Attached);
  }

void MobReservations::detached(Npc& npc, Interactive& mob) {
  freed(mob);
  npc.wakeActivity(ActivityEvents::Detached);
  }

void MobReservations::wait(Npc& npc, std::string_view scheme) {
  unwait(npc);
  waiters.push_back({&npc,std::string(scheme)});
  }

void MobReservations::unwait(const Npc& npc) {
  waiters.erase(std::remove_if(waiters.begin(),waiters.end(),[&npc](const Waiter& w) { return w.npc==&npc; }),
                waiters.end());
  }

void MobReservations::freed(const Interactive& mob) {
  // A waiter checks the place itself when it wakes: an object that finishes its quit is free by then.
  const float dist = MOBSI_SEARCH_DISTANCE;
  for(auto& w:waiters)
    if(mob.checkMobName(w.scheme) && w.npc->qDistTo(mob)<dist*dist)
      w.npc->wakeActivity(ActivityEvents::Freed);
  }

void MobReservations::tick() {
  if(reservations.empty())
    return;
  audit(false);
  const auto now = world.time();
  for(size_t i=0; i<reservations.size();) {
    if(now<reservations[i].until) {
      ++i;
      continue;
      }
    ++ActivityEvents::stats().expired;
    auto* owner = reservations[i].owner;
    drop(i);
    owner->wakeActivity(ActivityEvents::AttachFailed);
    }
  }

void MobReservations::audit(bool final) {
  std::unordered_set<const Npc*> present;
  if(final)
    for(uint32_t i=0; i<world.npcCount(); ++i)
      present.insert(world.npcById(i));
  auto& st = ActivityEvents::stats();
  for(size_t i=0; i<reservations.size();) {
    const auto& r     = reservations[i];
    const auto& state = r.owner->activityEventState();
    // a reservation lives only while its owner walks to the object to use it
    const bool held = !r.owner->isDead() && state.stage==ActivityEvents::Stage::Attach && (!final || present.contains(r.owner));
    if(r.mob->userCount()>0 && !r.mob->isAttached(*r.owner))
      ++st.doubled;
    if(!held)
      ++st.ownerless;
    if(held || final) {
      ++i;
      continue;
      }
    drop(i);
    }
  }
