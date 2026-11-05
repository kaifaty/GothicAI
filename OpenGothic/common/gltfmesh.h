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

struct GltfHeadMesh {
  struct Part {
    std::string name;
    size_t firstVertex = 0;
    size_t vertexCount = 0;
    };
  struct Delta {
    uint32_t vertex = 0;
    asset::Vec3 position;
    asset::Vec3 normal;
    };
  struct Target {
    std::string name;
    std::vector<Delta> vertices;
    };

  // Parts and targets address these logical vertices, before meshlet duplication.
  // normals is parallel to positions. Normalize normals after blending deltas.
  asset::StaticMesh mesh;
  std::vector<Part> parts;
  std::vector<Target> targets;
  // Filled when uploaded: render vertex -> logical vertex, UINT32_MAX for padding.
  // Expand blended logical positions/normals through this map for setDeformation.
  std::vector<uint32_t> renderVertexIds;
  };

GltfHeadMesh loadGltfHeadMesh(const std::filesystem::path& path);
