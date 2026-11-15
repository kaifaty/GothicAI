#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

class Npc;

// Main-thread work between NPC ticks and physics. Decisions are made while the policy loop has the
// NPC in cache. A callback after that loop can change any NPC: invalidate before entering it. Physics
// can also create a ragdoll; its birth invalidates the sync plan before solving its first pose.
namespace WorldWalk {
enum Step : uint8_t { Prepare = 1, Sync = 2 };
uint64_t revision(Step step);
void invalidate();
// Only ragdoll birth can give sync work to an NPC recorded without a ragdoll. Other script changes
// invalidate preparation; slot identity already protects sync against additions and removals.
void ragdollCreated();

struct Plan final {
  struct Entry { const Npc* npc = nullptr; uint8_t steps = 0; };
  std::vector<Entry> entries;
  uint64_t stamp = uint64_t(-1);
  bool skips(size_t at, const Npc* npc, Step step) const {
    return stamp==revision(step) && at<entries.size() && entries[at].npc==npc && (entries[at].steps&step)==0;
    }
  };

bool checking();
void checked(const char* step, std::string_view npc, bool same);
uint64_t checkedCount();
uint64_t mismatchedCount();
std::string_view firstMismatchNpc();
std::string_view firstMismatchStep();
// Also called by the body runner before _exit, which does not run atexit handlers.
void reportCheck();
}
