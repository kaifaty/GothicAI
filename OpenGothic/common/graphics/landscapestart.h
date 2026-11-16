#pragma once

#include <memory>
#include <string_view>

class WorldView;
struct ChunkTable;
namespace asset { struct WorldMesh; }

// The streaming landscape's view at world load (world/world.cpp), before its first chunk is published: decodes the
// materials the landscape's triangles use, each through its packing representative (PackedMesh::landscapeMaterialMap),
// in first use, and gives the view the landscape's bounds as PackedMesh::landscapeBbox takes them (landscape.bbox is
// zero for compiled worlds; the sky and fog take the observer's height from the lowest landscape vertex,
// SceneGlobals::setWorld). After a catalogue start (worldchunkcatalogue.h) both come from the world cloth bundle,
// which holds them as the stitched landscape gives them; otherwise from that landscape after WorldCloth::extract.
std::unique_ptr<WorldView> streamingWorldView(const asset::WorldMesh& landscape, const ChunkTable& chunks,
                                              std::string_view skyPreset);
