#pragma once

#include <Tempest/StorageBuffer>
#include <Tempest/Vec>

#include <array>
#include <cstdint>
#include <filesystem>

class StaticMesh;

namespace TreeNanite {

struct AssetFiles {
  std::filesystem::path dag, contours;
  std::array<std::filesystem::path, 4> levels;
  };

struct GpuAsset {
  Tempest::StorageBuffer vertices, meshlets, words, source, groups, skin, bones;
  Tempest::StorageBuffer contourFaces, contourShapes, shadow;
  Tempest::Vec4 boundsCenterHeight, boundsMin, boundsMax;
  std::array<float, 4> levelPx = {};
  std::array<uint32_t, 2> materialSub = {};
  std::array<uint32_t, 5> contourOffsets = {};
  uint32_t registrationSub = 0;
  uint32_t sourceCount = 0, meshletCount = 0, boneCount = 0, shadowCount = 0;
  float impostorPx = 0.f, windHeight = 0.f;
  };

bool enabled();
void attach(StaticMesh& mesh, const AssetFiles& files,
            const std::array<std::filesystem::path, 4>& authoredLevels);

}
