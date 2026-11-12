#pragma once

#include <string>
#include <string_view>

#include "resources.h"

class WorldView;

// Diagnostic of the texture levels (docs/world-streaming-textures.md): with OPENGOTHIC_TEXTURE_DUMP=<directory> the
// bench `mark` command and the end of a `perf sample` segment write <stage>-textures.csv (every tracked texture: size,
// levels, demand) and <stage>-owners.csv (every VisualObjects texture owner: kind, measurement, bounds, need).
namespace TextureDump {
  void write(std::string_view stage, WorldView* view);
  // Step 4 (textureflow.h): the JSON fields of a bench pass from `before` to `after`, each starting with a comma - the
  // shortfall's texture-seconds by state and the visible shortfall, loads started and textures that became short, the
  // latency demand -> publication and the wait for a load slot (p50, p90, max ms), bursts (p50, p90, max textures),
  // the publications' cost.
  std::string flowFields(const Resources::TextureStats& after, const Resources::TextureStats& before,
                         const TextureFlow::Distributions& now, const TextureFlow::Distributions& was);
  }
