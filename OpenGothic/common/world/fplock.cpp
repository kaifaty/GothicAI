#include "fplock.h"
#include "waypoint.h"
#include "world/objects/npc.h"

#include "game/serialize.h"

FpLock::FpLock() {
  }

FpLock::FpLock(const WayPoint &p) : FpLock(&p) {
  }

FpLock::FpLock(const WayPoint *p, bool guard)
  :pt(p),guarding(guard){
  if(pt)
    pt->useCount++;
  if(pt && guarding)
    ++pt->guardUsers;
  }

FpLock::FpLock(FpLock &&other)
  :pt(other.pt),guarding(other.guarding){
  other.pt=nullptr;
  }

FpLock::~FpLock() {
  release();
  }

FpLock &FpLock::operator=(FpLock &&other) {
  release();
  pt = other.pt;
  guarding = other.guarding;
  other.pt=nullptr;
  return *this;
  }

void FpLock::release() {
  if(pt && guarding)
    --pt->guardUsers;
  if(pt && --pt->useCount==0)
    for(auto* npc:pt->waiters)
      npc->wakeActivity(ActivityEvents::Freed);
  pt = nullptr;
  }

void FpLock::load(Serialize &fin) {
  const WayPoint* saved = nullptr;
  fin.read(saved);
  *this = FpLock(saved);
  }

void FpLock::save(Serialize &fout) const {
  fout.write(pt);
  }
