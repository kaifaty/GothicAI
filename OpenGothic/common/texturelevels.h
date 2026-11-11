#pragma once

#include <Tempest/AbstractGraphicsApi>

#include <algorithm>
#include <cstdint>
#include <limits>

// The resident mip level of a texture chosen by its size on screen
// (reports/engine/2026-11-10-texture-mips/README.md, docs/world-streaming-textures.md).
//
// An owner's demand is the number of screen pixels one UV unit covers, R = K/(d*u):
// K = H/(2*tan(fovY/2)) is the view scale (pixels per metre at one metre), d the distance in metres from the camera or
// the hero to the owner's bounds, u the owner's lowest UV density (UV units per metre). The owner reports the part of
// R that does not depend on the view, quantised to a power of two towards the sharper side:
//   exponent = ceil(log2(1/(d*u)))
// so it re-registers only when its distance changes about twofold, and a change of the view (aim, window) changes the
// levels without touching the owners. A texture whose top level is W x H then needs
//   level = floor(log2(sqrt(W*H)) - log2(K) - exponent) - reserve, clamped to [0, lowest].
namespace TextureLevels {
  // an owner with no resolution demand (no UV density, or demand by rings only)
  constexpr int32_t  NoExponent  = std::numeric_limits<int32_t>::min();
  // closer than this counts as this, metres
  constexpr float    MinDistance = 0.25f;
  // both sides of a resident top level stay at least this
  constexpr uint32_t MinSide     = 64;
  // as the first level to read (Pixmap::loadDdsMipTail): the coarsest allowed one
  constexpr uint32_t CoarsestRead = 255;
  // the smallest distance change, metres, an owner waits for before it is evaluated again
  constexpr double   MinMargin   = 0.05;
  // An owner moving away keeps its sharper exponent until its distance passes the band by this fraction: it errs only
  // sharper, and after any switch the next one is at least a quarter of a band away.
  constexpr double   Hysteresis  = 0.25;

  // The coarsest level that may be the top of a resident chain, by the rule of Pixmap::loadDdsMipTail: both sides
  // stay at least MinSide and at least one level remains. 0 without a readable tail (single level, a side under 128).
  uint8_t  lowest(uint32_t width, uint32_t height, uint32_t mipCount);

  // The level a texture with top level width x height needs for the demand `exponent` at the view scale log2K,
  // `reserve` whole levels sharper, clamped to [0, lowest]. NoExponent needs the whole chain.
  uint8_t  level(uint32_t width, uint32_t height, float log2K, int32_t exponent, uint32_t reserve, uint8_t lowest);

  // log2 of the view scale K = height/(2*tan(fovY/2)); fovY in degrees
  float    viewScale(float height, float fovYDegrees);

  struct Demand {
    int32_t exponent = NoExponent;
    // how much the distance may change, metres, before the exponent does
    double  margin   = 0;
    };
  // The coarsest exponent that still matters at the view scale log2K: at it every texture, whatever its size, is at
  // its lowest allowed level (floor(log2(side) - log2 K - e) - reserve >= log2(side/MinSide) when
  // e <= 6 - reserve - log2 K). An owner held at it registers an exponent at least as sharp as its own, so a later
  // change of the view or the reserve can leave it sharper than needed, never coarser.
  int32_t  coarsest(uint32_t reserve, float log2K);

  // The demand of an owner `distance` metres away with `uvPerMetre`, `previous` its exponent so far (or NoExponent);
  // without a density (<=0) NoExponent and no margin. An exponent at `coarsest` holds until the owner comes closer.
  Demand   demand(double distance, float uvPerMetre, int32_t previous = NoExponent,
                  int32_t coarsest = std::numeric_limits<int32_t>::min());

  // Which of two textures short of their wanted level loads first: the larger shortfall in levels, then the nearer ring.
  inline bool loadsBefore(int64_t shortfall, uint32_t ring, int64_t otherShortfall, uint32_t otherRing) {
    return shortfall!=otherShortfall ? shortfall>otherShortfall : ring<otherRing;
    }

  // Bytes of the chain of a width x height texture with mipCount levels, from level `first` down.
  uint64_t chainBytes(Tempest::TextureFormat format, uint32_t width, uint32_t height, uint32_t mipCount, uint32_t first);
  }
