#pragma once

#include <Tempest/Vec>

#include "graphics/grass/grassplace.h"

namespace Grass {

constexpr float FineFade = 1600.f;

bool gpuCullEnabled();

inline Tempest::Vec4 bladeOrigin(int32_t cx, int32_t cz, int32_t minY) {
  constexpr float cell = float(CellSize/Unit);
  constexpr float margin = float(CellMargin/Unit);
  return Tempest::Vec4(float(cx)*cell - margin, float(minY)/float(Unit), float(cz)*cell - margin, 0.f);
  }

inline Tempest::Vec4 bladeShape(bool fine, float fineFull) {
  return fine ? Tempest::Vec4(0.5f, 0.036f, fineFull, FineFade) : Tempest::Vec4();
  }

}
