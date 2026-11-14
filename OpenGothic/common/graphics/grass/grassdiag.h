#pragma once

#include <cstdint>
#include <cstdlib>

// What the look of the grass is measured with (reports/engine/2026-11-14-grass-system, "Круг правки вида"). Nothing
// here changes a frame until a bench asks: `grass parts <mask>` and OPENGOTHIC_GRASS_PATCH.
namespace Grass {

// Parts of the look a bench switches off one at a time, to weigh what each does to the tone; the last two measure
// the share of the ground the blades cover. GrassPart* of shader/grass/grass_common.glsl
enum Part : uint32_t {
  PartGroundShade = 1,   // the ground between the blades in their shade
  PartFar         = 2,   // the ground's look of the grass from afar
  PartTip         = 4,   // the tips' tint
  PartRoot        = 8,   // the blade's darker root
  PartRootAo      = 16,  // the ambient shade at the root
  PartNormal      = 32,  // the blade's own share of its normal: the ground's alone
  PartPaint       = 64,  // blades red, the ground in bands of the distance from the camera, the rest black
  PartHide        = 128, // no blades: the bands alone
  PartAll         = 255,
  };

// SceneDesc::grassRing.w carries it: 1 + 2*mask
inline uint32_t& partsOff() {
  static uint32_t mask = 0;
  return mask;
  }

// OPENGOTHIC_GRASS_PATCH=<0..65535>: every tuft takes this number for the patches' noise, so the whole meadow is
// short grass or tall. The blades only: the ground's shader keeps the real patches. -1 without
inline int32_t forcedPatch() {
  static const int32_t value = [] {
    const char* e = std::getenv("OPENGOTHIC_GRASS_PATCH");
    if(e==nullptr || *e=='\0')
      return int32_t(-1);
    const long v = std::strtol(e,nullptr,10);
    return int32_t(v<0 ? 0 : (v>65535 ? 65535 : v));
    }();
  return value;
  }

}
