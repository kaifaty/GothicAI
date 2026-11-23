#pragma once

#include <array>
#include <cstdint>

namespace TreeBough {

// Indirect vertex counts for compact foliage ranges in the classic path.
constexpr std::array<uint32_t, 6> indexClasses = {6, 12, 18, 27, 36, 48};

}
