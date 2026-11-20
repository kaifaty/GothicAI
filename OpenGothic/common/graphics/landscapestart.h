#pragma once

#include <Tempest/Vec>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

class WorldView;
struct ChunkTable;
namespace asset { struct WorldMesh; }

// What the streaming landscape's view takes at world load (world/world.cpp), before its first chunk is published:
// the materials to decode, each as its packing representative (PackedMesh::landscapeMaterialMap), in first use over
// the landscape's triangles, and the landscape's bounds as PackedMesh::landscapeBbox takes them (landscape.bbox is
// zero for compiled worlds; the sky and fog take the observer's height from the lowest landscape vertex,
// SceneGlobals::setWorld). After a catalogue start (worldchunkcatalogue.h) both come from the world cloth bundle,
// which holds them as the stitched landscape gives them; otherwise from that landscape after WorldCloth::extract.
struct LandscapeStart {
  std::vector<uint32_t>                   materials;
  std::pair<Tempest::Vec3,Tempest::Vec3>  bounds;
  };
LandscapeStart landscapeStart(const asset::WorldMesh& landscape, const ChunkTable& chunks);

// Decodes landscapeStart's materials in their order and makes the view with its bounds.
std::unique_ptr<WorldView> streamingWorldView(const asset::WorldMesh& landscape, const ChunkTable& chunks,
                                              std::string_view skyPreset);
