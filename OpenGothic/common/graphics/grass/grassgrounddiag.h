#pragma once

#include <cstdint>

namespace Grass {

// `grass ground <mask>`: the second word of the ground breakdown (SceneDesc::grassGroundParts).
constexpr uint32_t PartGroundTone = 1u;

inline uint32_t& benchGroundPartsOff() {
  static uint32_t mask = 0;
  return mask;
  }

}
