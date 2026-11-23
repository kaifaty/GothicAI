#pragma once

#include "physics/physics.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

// Offline convex pieces in visual coordinates. The cache contains centimetres;
// the resulting native shape uses metres, like every CollisionWorld shape.
struct ConvexParts {
  JPH::ShapeRefC shape;
  size_t partCount = 0;

  static std::shared_ptr<const ConvexParts> load(std::string_view visual);
  // Direct checked reader for generated GLB sidecars and native contracts.
  static std::shared_ptr<const ConvexParts> read(const std::filesystem::path& path,
                                                size_t sourceSize, uint32_t sourceCrc32);
  };
