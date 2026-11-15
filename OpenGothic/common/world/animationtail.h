#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

// The main-thread loops after the parallel animation pass (WorldObjects::updateAnimation): faces, melee
// shadow bodies, melee contacts and the logical bodies, each in the order of the NPC array. The pass
// decides for each NPC, while it has it in cache, which of these steps can change anything (its plan,
// Npc::animationTailPlan); the loops pass the others over without reading them.
//
// The plan holds while nothing has changed an NPC since the pass. Two things can: a melee window (its
// contacts run damage scripts, which may change, add or remove any NPC) and an animation update of an NPC on
// the main thread (a sync on read, a forced update of a teleport). Either marks the plan stale; the loops
// then go on as the full loops: every NPC, followed by its pointer through insertions and removals.
namespace AnimationTail {

enum Step : uint8_t {
  Face     = 1, // MdlVisual::updateFace: an own head's face, or the eye probe's subject
  Shadow   = 2, // Npc::updateMeleeShadow
  Contacts = 4, // Npc::resolveMeleeContacts: an open melee window
  Logic    = 8, // Npc::syncLogicalBodyState
  All      = Face|Shadow|Contacts|Logic,
  };

namespace Detail {
extern std::atomic<bool> watching, touched;
}

// Main thread: begin() right after the pass, end() after the last loop.
void begin();
void end();
// Something changed an NPC after the pass: the rest of the loops run in full.
inline void touch() { Detail::touched.store(true,std::memory_order_relaxed); }
inline bool stale() { return Detail::touched.load(std::memory_order_relaxed); }
// Npc::updateAnimation: between begin() and end() any animation update makes the plan stale.
inline void noteAnimationUpdate() {
  if(Detail::watching.load(std::memory_order_relaxed))
    touch();
  }

// OPENGOTHIC_ANIM_TAIL_CHECK=1: every step the plan skips runs the old way instead and is compared;
// at exit stderr gets "[anim-tail-check] checked=<N> mismatched=<M>" and the first NPC that differed.
bool checking();
void checked(const char* step, std::string_view npc, bool same);
uint64_t checkedCount();
uint64_t mismatchedCount();

// Runs `run(item,planned)` over the items in their order. A step may add, remove or move items: the loop
// follows the current item by its pointer, as the full loops always did. While the plan holds (not stale,
// the array as the pass saw it), an item whose plan lacks `step` is passed over without reading it, or,
// when checking, given to `skipped(item)`. `planned` tells `run` that the plan chose it; otherwise `run`
// applies the full loop's own rules.
template<class T, class Run, class Skipped>
void loop(std::vector<std::unique_ptr<T>>& items, const std::vector<uint8_t>& plan, uint8_t step,
          Run&& run, Skipped&& skipped) {
  const bool check = checking();
  for(size_t i=0; i<items.size();) {
    const bool planned = !stale() && items.size()==plan.size();
    const bool skip    = planned && (plan[i]&step)==0;
    if(skip && !check) {
      ++i;
      continue;
      }
    T* item = items[i].get();
    const size_t count = items.size();
    if(skip)
      skipped(*item);
    else
      run(*item,planned);
    if(items.size()!=count)
      touch();
    if(i<items.size() && items[i].get()==item) {
      ++i;
      continue;
      }
    touch();
    for(size_t at=0; at<items.size(); ++at)
      if(items[at].get()==item) {
        i = at+1;
        break;
        }
    }
  }

}
