#pragma once

#include <cstdint>

namespace RendererInternal {

uint32_t nextPot(uint32_t x);

// probe_stats: live, traced, slots, free slots, allocated at hits, freed (probe_aux.comp, Stats)
inline constexpr uint32_t giStatsCount = 6;

}
