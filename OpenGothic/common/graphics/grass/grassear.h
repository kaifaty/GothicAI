#pragma once

#include <cstdint>
#include <string_view>

#include "graphics/grass/grasspatch.h"

// A stalk with an ear: the rye of a field (reports/content/2026-11-19-crop-fields/stalk.md). The record's
// "kind": "ear" makes every blade of its ground one: as tall as a man's chest, of an even stand without the
// meadow's tufts and bare spots, nearly upright. The stalk and the drooping ear are shaped by
// shader/grass/grass_ear.glsl; the rank and the share of the ring are a blade's.
namespace Grass {

constexpr uint8_t KindEar       = 2;                  // Params::kind and Batch::kind
constexpr float   EarMaxHeight  = 160.f;              // cm, the record's
constexpr float   EarHeightStep = 0.75f;              // cm of the blade's height byte (GrassEarHeightStep)
constexpr float   EarTop        = 255.f*EarHeightStep; // cm: the tallest the byte holds
constexpr int32_t EarLodgeStep  = 700;                // cm of the noise of the lodged spots
constexpr float   EarLodgeFrom  = 0.66f;              // of the noise: the stand starts to lie over
constexpr float   EarLodgeFull  = 0.82f;              // and lies
constexpr float   EarLodgeLean  = 46.f;               // degrees from upright in a lodged spot's middle
constexpr float   EarEdgeLower  = 0.15f;              // of the height is gone at an open edge of the stand
constexpr float   EarEdgeLean   = 26.f;               // degrees: the stalks of the edge lean out towards it

// cm of a step of the height byte of a blade of the kind
constexpr float heightStep(uint8_t kind) { return kind==KindEar ? EarHeightStep : 0.25f; }

// "kind" of a record's `grass`. Without the field the kind stays the grass's own; false on anything but "ear".
// Value: rapidjson's
template<class Value>
bool parseKind(const Value& v, uint8_t& kind) {
  const auto it = v.FindMember("kind");
  if(it==v.MemberEnd())
    return true;
  if(!it->value.IsString() || std::string_view(it->value.GetString(),it->value.GetStringLength())!="ear")
    return false;
  kind = KindEar;
  return true;
  }

// The node's tuft for an even stand: its phase and its tint are kept, so the wind and the dryness still go by
// places; its count, its gathering to the centre and its kind of patch are not
Tuft  evenTuft(const Tuft& t);
// Against the record's height: 0.9..1.1 by the node's hash `h`, and up to a fifth lower by spots of 8 m (the root
// in units)
float earHeight(uint64_t h, int32_t x, int32_t z);
// At rest: the side it leans to, radians, and by how much, 8..18 degrees. The side is the place's (the root in
// units), within 0.45 of a radian of it either way. In a lodged spot the stand lies over, up to EarLodgeLean
void  earLean(uint64_t h, int32_t x, int32_t z, float& angle, float& lean);

}
