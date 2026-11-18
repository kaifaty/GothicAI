#pragma once

#include <cstdint>
#include <cstdlib>

// What the look of the grass is measured with (reports/engine/2026-11-14-grass-system, "Круг правки вида"). Nothing
// here changes a frame until a bench asks: `grass parts <mask>`, OPENGOTHIC_GRASS_PATCH, OPENGOTHIC_GRASS_FINE and
// OPENGOTHIC_GRASS_BROAD.
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
  // 512, 1024 and 2048 took back parts of a palette the blades of a ground with its own layers had for one round
  // (reports/engine/2026-11-14-meadow-ground, "Что отняло жизнь": the table was made with them); unused since
  PartLayers      = 4096, // a ground with its own layers as a painted ground: no layers, its blades as there
  PartCells       = 8192, // with the mask's paint (1+64+128): squares of 0.5 m in red, of 5 m in green, by the world's x and z
  PartTrack       = 16384, // a road with `track` as with its `verge` alone: the painted road and the strip (grass_track.glsl)
  PartOffscreen   = 32768, // the blades go through the whole vertex stage and then leave the screen: nothing is rasterised,
                           // so a frame less this one is the blades' rasterising and pixels, this one less PartHide their vertex stage
  PartTrackWander = 1u << 16,
  PartTrackFade   = 1u << 17,
  PartTrackRagged = 1u << 18,
  PartTrackOrm    = 1u << 19,
  PartTrackColor  = 1u << 20,
  PartTrackSpots  = 1u << 21,
  PartTrackTone   = 1u << 22,
  PartTrackForm   = 1u << 23,
  PartTrackGrain  = 1u << 24,
  PartAll         = (1u << 25) - 1u,
  };

inline uint32_t& benchPartsOff() {
  static uint32_t mask = 0;
  return mask;
  }

// SceneDesc::grassRing.w carries the low bits as 1 + 2*mask. The track bits have their own integer.
inline uint32_t partsOff() {
  return benchPartsOff() & 0xFFFFu;
  }

// OPENGOTHIC_GRASS_SQUARES=0: a cell is drawn and thinned whole, as before the squares (reports/engine/2026-11-17-
// grass-cost): the pair of their measure
inline bool squaresOff() {
  static const bool value = [] {
    const char* e = std::getenv("OPENGOTHIC_GRASS_SQUARES");
    return e!=nullptr && e[0]=='0' && e[1]=='\0';
    }();
  return value;
  }

// OPENGOTHIC_GRASS_FINE=0: the short sward keeps its broad blades where the record asks for fine ones
inline bool fineOff() {
  static const bool value = [] {
    const char* e = std::getenv("OPENGOTHIC_GRASS_FINE");
    return e!=nullptr && e[0]=='0' && e[1]=='\0';
    }();
  return value;
  }

// OPENGOTHIC_GRASS_BROAD=<0..100>: this many of a hundred of the short sward's broad blades stand among the fine
// ones, whatever the record's `broad` says. -1 without
inline int32_t forcedBroad() {
  static const int32_t value = [] {
    const char* e = std::getenv("OPENGOTHIC_GRASS_BROAD");
    if(e==nullptr || *e=='\0')
      return int32_t(-1);
    const long v = std::strtol(e,nullptr,10);
    return int32_t(v<0 ? 0 : (v>100 ? 100 : v));
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
