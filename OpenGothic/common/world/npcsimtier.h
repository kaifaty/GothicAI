#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "game/constants.h"
#include "worldchunksruntime.h"

class ChunkResidency;

// NPC simulation tiers (world streaming, step 1, item 6). Every NPC is Full
// while its 64 m chunk is Near in ChunkResidency (the same ring and exit
// hysteresis as the other consumers; the hero is always Full), Simplified
// otherwise. The tier is separate from NpcProcessPolicy, whose 30/60 m rules
// remain unchanged. Both tiers currently run the same executor. The tier is
// runtime-only: it is not saved and is recomputed right after a load.
namespace NpcSimTiers {

// OPENGOTHIC_NPC_SIM_TIER: auto (default), full (every NPC Full) or
// simplified (every NPC but the hero Simplified). OPENGOTHIC_STREAMING=0
// forces full, as it keeps every other streaming consumer on the old path.
enum class Mode : uint8_t { Auto, Full, Simplified };

Mode        parseMode(const char* tier, const char* streaming, bool* valid = nullptr);
Mode        mode();  // read from the environment once per process
const char* name(Mode mode);
const char* name(NpcSimTier tier);

// The assignment rule without side effects. alwaysFull marks the hero and
// the actors that keep native simulation at every distance (training, arena).
// A null residency (streaming manager off) gives Full in auto mode.
NpcSimTier  desired(Mode mode, bool hero, bool alwaysFull, const ChunkResidency* residency,
                    ChunkId cell, NpcSimTier previous);

struct Stats {
  size_t   full = 0, simplified = 0;
  // Cumulative changes of an already assigned NPC; the first assignment after
  // insertion or a load is not a transition.
  uint64_t toFull = 0, toSimplified = 0;
  uint64_t transitionsLastTick = 0;
  // Wall time of Npc::tick on the main thread, split by tier: last world tick
  // and cumulative since the world was created.
  double   fullTickMs = 0, simplifiedTickMs = 0;
  double   fullTickMsTotal = 0, simplifiedTickMsTotal = 0;
  uint64_t ticks = 0;
  Mode     mode = Mode::Auto;

  std::string probeFields() const;
  };

}
