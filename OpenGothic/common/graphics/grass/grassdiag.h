#pragma once

#include <cstdint>
#include <cstdlib>

// What the look of the grass is measured with (reports/engine/2026-11-14-grass-system, "Круг правки вида"). Nothing
// here changes a frame until a bench asks: `grass parts <mask>`, OPENGOTHIC_GRASS_PATCH and OPENGOTHIC_GRASS_FINE.
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
  PartCover       = 256, // the meadow's own ground painted by its cover: sward green, earth red (grass_layers.glsl)
  // a ground with its own layers taken back, a part at a time, to what a painted ground has
  // (reports/engine/2026-11-14-meadow-ground, "Что отняло жизнь")
  PartOwnTip      = 512,  // the blades' tips and root shade
  PartOwnBase     = 1024, // the colour a blade starts from: the painted texture's, not the sward's
  PartOwnShade    = 2048, // the ground's shade between the blades and its far tint
  PartLayers      = 4096, // the painted ground in place of the two layers
  PartCells       = 8192, // with the mask's paint (1+64+128): squares of 0.5 m in red, of 5 m in green, by the world's x and z
  PartAll         = 16383,
  };

// SceneDesc::grassRing.w carries it: 1 + 2*mask
inline uint32_t& partsOff() {
  static uint32_t mask = 0;
  return mask;
  }

// OPENGOTHIC_GRASS_FINE=0: the short sward keeps its broad blades where the record asks for fine ones
inline bool fineOff() {
  static const bool value = [] {
    const char* e = std::getenv("OPENGOTHIC_GRASS_FINE");
    return e!=nullptr && e[0]=='0' && e[1]=='\0';
    }();
  return value;
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
