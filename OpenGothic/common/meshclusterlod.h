#pragma once

#include <filesystem>
#include <optional>

#include "gltfmeshcolor.h"
#include "graphics/mesh/clusterlod.h"

namespace MeshClusterLod {

using Graph = ClusterLod::Graph;

bool alphaTest(const asset::Material& material);
bool locked(const asset::Material& material, bool twoSided);
Graph build(const asset::StaticMesh& mesh, const GltfMeshColors& colors, std::span<const uint8_t> twoSided);

// The file is next to the source GLB. Only the offline content converter writes it.
void write(const std::filesystem::path& glb, size_t glbSize, uint32_t glbCrc32, const Graph& graph);

// A missing, stale or invalid file leaves the mesh on the ordinary draw path.
std::optional<Graph> read(const std::filesystem::path& glb, size_t glbSize, uint32_t glbCrc32,
                          const asset::StaticMesh& mesh, std::span<const uint8_t> twoSided);

}
