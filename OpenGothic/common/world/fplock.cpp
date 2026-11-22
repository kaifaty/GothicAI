#include "fplock.h"
#include "waypoint.h"
#include "world/objects/npc.h"

#include "game/serialize.h"

FpLock::FpLock() {
  }

FpLock::FpLock(const WayPoint &p)
  :pt(&p){
  pt->useCount++;
  }

FpLock::FpLock(const WayPoint *p)
  :pt(p){
  if(pt)
    pt->useCount++;
  }

FpLock::FpLock(FpLock &&other)
  :pt(other.pt){
  other.pt=nullptr;
  }

FpLock::~FpLock() {
  release();
  }

FpLock &FpLock::operator=(FpLock &&other) {
  release();
  pt = other.pt;
  other.pt=nullptr;
  return *this;
  }

void FpLock::release() {
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
