#pragma once

#include <string_view>

class World;

// Diagnostic of the texture levels (docs/world-streaming-textures.md): with OPENGOTHIC_TEXTURE_DUMP=<directory> the
// bench `mark` command and the end of a `perf sample` segment write <stage>-textures.csv (every tracked texture: size,
// levels, demand) and <stage>-owners.csv (every VisualObjects texture owner: kind, measurement, bounds, need).
namespace TextureDump {
  void write(std::string_view stage, World& world);
  }
