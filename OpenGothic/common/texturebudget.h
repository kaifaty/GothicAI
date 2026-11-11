#pragma once

#include <Tempest/AbstractGraphicsApi>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

// Step 3 of the texture levels (reports/engine/2026-11-10-texture-mips/README.md, docs/world-streaming-textures.md):
// the memory target of the textures from the device's budget, and the stage that makes the wanted levels coarser when
// everything standing at its wanted level would still be above the target. Pure: no device, no Resources.
namespace TextureBudget {
  using Device = Tempest::AbstractGraphicsApi::MemoryBudget;
  using Clock  = std::chrono::steady_clock;
  constexpr uint64_t MiB = 1048576;

  // The target: never below today's fixed target, which is also the target when the device budget is not readable.
  constexpr uint64_t Floor           = 192*MiB;
  // Left free on the device for the rest of the game, the driver and other processes: at least this...
  constexpr uint64_t MinHeadroom     = 512*MiB;
  // ...or budget/HeadroomDivisor, whichever is larger.
  constexpr uint64_t HeadroomDivisor = 8;
  // The textures' share of the budget: budget/ShareDiscrete on a device with its own memory...
  constexpr uint64_t ShareDiscrete   = 4;
  // ...budget/ShareShared on memory shared with the CPU, where the budget is a part of the system memory.
  constexpr uint64_t ShareShared     = 8;
  // The device query, the target and the stage are evaluated this often, on the main thread.
  constexpr auto     Period          = std::chrono::seconds(1);
  // The working target follows the rule only when the new value differs from it by more than this fraction.
  constexpr double   Step            = 0.05;
  // The log line is written again when the working target moved by more than this fraction since the last line...
  constexpr double   LogStep         = 0.25;
  // ...at most once in this time.
  constexpr auto     LogInterval     = std::chrono::seconds(10);
  // Going back to a shallower stage s-1 needs D(s-1) at most this part of the target...
  constexpr double   BackFraction    = 0.9;
  // ...for this long in a row.
  constexpr auto     BackDelay       = std::chrono::seconds(5);

  // other    = max(0, usage - liveTextures)        everything else the game holds on the device
  // headroom = max(MinHeadroom, budget/HeadroomDivisor)
  // target   = max(Floor, min(budget/share, budget - other - headroom))
  // Floor when the device is not known.
  uint64_t target(const Device& device, uint64_t liveTextures);
  // The working target after the rule gave `next`: `next` only when it differs from `working` by more than Step.
  uint64_t follow(uint64_t working, uint64_t next);
  // Whether a working target moved from `logged` to `now` by more than LogStep.
  bool     logWorthy(uint64_t logged, uint64_t now);

  enum class Source : uint8_t {
    Default,      // the rule, but the device budget is not readable: Floor
    Rule,         // the rule from the device budget
    Setting,      // [GAME] texture_target_mb
    Environment,  // OPENGOTHIC_TEXTURE_TARGET_MB
    };
  const char* name(Source source);

  struct Choice {
    bool     automatic = true;   // the rule (a budget: follows the target)
    uint64_t bytes     = 0;      // the fixed value otherwise
    Source   source    = Source::Rule;
    };
  // A number of MiB in `environment` (nullptr: absent) or else in `setting` fixes the value; absent, empty or `auto`
  // means the rule. Anything else throws, naming `field`.
  Choice   choose(const char* environment, std::string_view setting, const char* field);

  // An owner that needs the whole chain (TextureScreenDemand::WholeChain): never made coarser.
  constexpr int32_t WholeChain = std::numeric_limits<int32_t>::max();

  // The reserve of whole levels the levels use at `stage`: `reserve` at stage 0, none deeper.
  uint32_t reserve(uint32_t stage, uint32_t reserve);
  // How many levels coarser `stage` makes a texture whose nearest owner is in chunk ring `ring`. Stage 1 only drops
  // the reserve; each further stage adds one level, in turn for ring 2 and farther, for ring 1 and farther, for all:
  //   stage 2: ring >= 2 +1; 3: ring >= 1 +1; 4: all +1; 5: ring >= 2 +2; 6: ring >= 1 +2; 7: all +2; ...
  uint32_t coarser(uint32_t stage, uint32_t ring);
  // A level at the stage's reserve (texturelevels.h: level, clamped to [0, lowest]) made coarser by the stage for the
  // nearest ring `ring`, up to `lowest`; `whole` (an owner needs the whole chain) keeps it.
  uint8_t  coarsen(uint8_t level, uint8_t lowest, uint32_t stage, uint32_t ring, bool whole);

  // D(s) for every stage at once: the bytes the demanded textures would take at stage s, from a difference array over
  // the stages. A texture adds its bytes at each level through `bytes(level)`.
  class Sums {
    public:
      void clear();
      // bytes that do not depend on the stage
      void fixed(uint64_t bytes);
      // A texture measured on screen: `level0` and `level1` its levels at the reserve of stage 0 and at reserve 0
      // (texturelevels.h: level, within [0, lowest]), `cap` the coarsest it can take (its lowest allowed level, or
      // sharper when an owner judged by the ring rule holds it), `ring` its nearest owner's chunk ring.
      template<class Bytes>
      void measured(uint8_t level0, uint8_t level1, uint8_t cap, uint8_t lowest, uint32_t ring, bool whole, Bytes bytes);
      // D(0) ... D(deepest): `deepest` is the last stage that changes anything, where every texture measured on
      // screen stands at its coarsest allowed level.
      std::vector<uint64_t> finish() const;

    private:
      void add(uint32_t stage, int64_t delta);
      uint64_t d0 = 0, d1 = 0;
      std::vector<int64_t> deltas;  // from stage 2 on: the change of D at that stage
    };

  struct Ladder {
    uint32_t          stage   = 0;
    bool              backing = false;  // D(stage-1) has been within BackFraction of the target since `backSince`
    Clock::time_point backSince{};
    };
  // The stage for the sums `d` (D(0)..D(deepest), not increasing): the shallowest that fits the target, deeper at
  // once; back to a shallower one only after D(stage-1) <= BackFraction*target for BackDelay in a row, then to the
  // shallowest within BackFraction. Nothing fits: the deepest. `force` (a world load) takes the shallowest that fits
  // at once.
  uint32_t step(Ladder& ladder, const std::vector<uint64_t>& d, uint64_t target, Clock::time_point now, bool force = false);
  }

template<class Bytes>
void TextureBudget::Sums::measured(uint8_t level0, uint8_t level1, uint8_t cap, uint8_t lowest, uint32_t ring, bool whole,
                                   Bytes bytes) {
  cap = std::min(cap,lowest);
  d0 += bytes(std::min(level0,cap));
  uint8_t level = std::min(level1,cap);
  d1 += bytes(level);
  if(whole)
    return;
  // Each stage adds at most one level: the loop ends within three stages per level up to the cap.
  for(uint32_t stage=2; level<cap; ++stage) {
    const uint8_t next = std::min(coarsen(level1,lowest,stage,ring,whole),cap);
    if(next==level)
      continue;
    add(stage,int64_t(bytes(next))-int64_t(bytes(level)));
    level = next;
    }
  }
