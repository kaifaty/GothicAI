#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>
#include "asset/staticmesh.h"

// Loads the supported static, embedded-buffer glTF 2.0 subset into Gothic
// centimetres. No resources, graphics device or archive access is involved.
// Throws std::runtime_error with the rejection reason on invalid input.
// Optionally returns the exact GLB bytes used by the successful conversion.
asset::StaticMesh loadGltfMesh(const std::filesystem::path& path, std::vector<uint8_t>* source = nullptr);

struct GltfMeshNode {
  std::string name;
  asset::StaticMesh mesh;
  };

// One mesh per mesh-bearing scene node, still in model space after its world
// transform. Empty nodes are ignored; names are the decoded glTF node names.
std::vector<GltfMeshNode> loadGltfMeshNodes(const std::filesystem::path& path, std::vector<uint8_t>* source = nullptr);
