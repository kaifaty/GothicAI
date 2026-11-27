#include "npcsimtier.h"

#include <chrono>

#include "world/objects/npc.h"

// Diagnostic only (OPENGOTHIC_FAR_CLASS_PROFILE): times one Simplified Npc::tick and accumulates the
// wall time into a movement-class bucket, so the far-tick cost can be located by class. Kept out of
// the hot tick loop; the normal path never calls this.

namespace NpcSimTiers {

void profileFarTick(Npc& npc, uint64_t owed, Stats& s) {
  const auto t0 = std::chrono::steady_clock::now();
  npc.tick(owed);
  const double ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
  int cls;
  if(npc.nativeCombatMovement())       cls = FarCombat;
  else if(npc.nativeMobsiApproach())   cls = FarMobsi;
  else if(npc.graphMovementFallback()) cls = FarFallback;
  else if(npc.usesSimplifiedMovement())cls = FarGraph;
  else                                 cls = FarOther;
  s.classMsTotal[cls] += ms;
  s.classCount[cls]   += 1;
  }

}
