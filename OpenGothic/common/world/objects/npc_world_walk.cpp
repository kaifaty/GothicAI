#include "npc.h"

#include "world/worldwalk.h"

#include <array>
#include <cstring>
#include <string>

uint8_t Npc::worldWalkPlan() const {
  // prepareLivingBody still has to discard an existing living rig on entry to graph movement.
  return uint8_t((usesSimplifiedMovement() && !living ? 0 : WorldWalk::Prepare) |
                 (ragdoll ? WorldWalk::Sync : 0));
  }

void Npc::checkSkippedWorldWalk(uint8_t step) {
  const std::string name(displayName());
  const bool eligible = (worldWalkPlan()&step)==0;
  // Copy the representation, never the owning C++ object. A legitimate skipped step returns without
  // changing even a scalar or container header. Eligibility also protects indirect state: a native
  // prepare or a ragdoll sync may write through pointers without changing the owning field.
  std::array<unsigned char,sizeof(Npc)> before;
  std::memcpy(before.data(),static_cast<const void*>(this),before.size());
  if(step==WorldWalk::Prepare) {
    prepareRagdoll();
    prepareLivingBody();
    } else {
    syncRagdoll();
    }
  WorldWalk::checked(step==WorldWalk::Prepare ? "prepare" : "sync",name,
                     eligible && std::memcmp(before.data(),static_cast<const void*>(this),before.size())==0);
  }
