#pragma once

#include "math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace asset {

// One FNT glyph, with the original FNT layout: advance width and the two atlas UV corners.
struct FontGlyph {
  uint8_t      width = 0;
  Vec2 uv[2] {};
  };

// An FNT font. Glyphs keep the original order: a text byte indexes them directly.
// The name is the exact FNT name line, metadata only; it never selects the atlas.
struct Font {
  std::string            name;
  uint32_t               height = 0;
  std::vector<FontGlyph> glyphs;
  };

}
