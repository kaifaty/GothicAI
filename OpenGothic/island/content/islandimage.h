#pragma once

// Pictures of the island generator: four floats a texel, written as a DDS of DXT blocks with every mip level, the
// only kind the engine's material maps read (Resources::indexMaterialMaps, Tempest's pixmapcodecdds).

#include <cstddef>
#include <filesystem>
#include <functional>
#include <vector>

namespace island {

// Calls row(0..rows-1) on every core. Each row writes its own slots, so the result does not depend on the order.
void parallelRows(int rows, const std::function<void(int)>& row);

struct Image {
  int                w = 0, h = 0;
  std::vector<float> px; // 4 a texel

  Image() = default;
  Image(int w, int h) : w(w), h(h), px(size_t(w)*size_t(h)*4, 0.f) {}

  float*       at(int x, int y)       { return px.data() + (size_t(y)*size_t(w)+size_t(x))*4; }
  const float* at(int x, int y) const { return px.data() + (size_t(y)*size_t(w)+size_t(x))*4; }
  };

enum class Encoding {
  Colour,      // linear rgb, written by the exact sRGB curve; DXT1
  ColourAlpha, // the same with a linear alpha; DXT5
  Data,        // values as they are; DXT1
  DataAlpha,   // values as they are, with alpha; DXT5
  Normal,      // a unit vector: x to alpha, y to green (DXT5nm)
  };

// Mips are 2x2 averages of the floats (colour in linear light, normals renormalised) down to 1x1.
void writeDds(const std::filesystem::path& path, const Image& top, Encoding encoding);

// Mean of 0.2126 r + 0.7152 g + 0.0722 b.
float meanLuminance(const Image& image);

}
