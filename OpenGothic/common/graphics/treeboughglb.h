#pragma once

#include "resources.h"

#include <filesystem>
#include <string>
#include <vector>

namespace TreeBough {

struct GlbPage {
  std::string                    texture;
  std::vector<Resources::Vertex> vertices;
  std::vector<uint32_t>          indices;
  std::vector<uint32_t>          cardIds;
  };

// Decode the generated hierarchy GLB in primitive order. The wood slot is empty.
std::vector<GlbPage> loadGlb(const std::filesystem::path& path);

}
