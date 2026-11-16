#include "landscapestart.h"

#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/worldview.h"
#include "resources.h"
#include "worldchunkcatalogue.h"

LandscapeStart landscapeStart(const asset::WorldMesh& landscape, const ChunkTable& chunks) {
  const auto map=PackedMesh::landscapeMaterialMap(landscape);
  const auto* catalogue=chunks.source ? chunks.source->catalogue() : nullptr;
  LandscapeStart out;
  std::vector<bool> listed(landscape.materials.size());
  const auto add=[&](uint32_t id) {
    id=map[id];
    if(!listed[id]) {
      out.materials.push_back(id);
      listed[id]=true;
      }
    };
  if(catalogue!=nullptr) {
    for(const auto id:catalogue->landscapeMaterials())
      add(id);
    const auto [min,max]=catalogue->landscapeBounds();
    out.bounds={Tempest::Vec3(min.x,min.y,min.z),Tempest::Vec3(max.x,max.y,max.z)};
    }
  else {
    for(const auto id:landscape.polygons.material_indices)
      add(id);
    out.bounds=PackedMesh::landscapeBbox(landscape);
    }
  return out;
  }

std::unique_ptr<WorldView> streamingWorldView(const asset::WorldMesh& landscape, const ChunkTable& chunks,
                                              std::string_view skyPreset) {
  const auto start=landscapeStart(landscape,chunks);
  {
  Resources::TextureLoadScope textureLoadScope(true);
  for(const auto id:start.materials)
    Resources::loadMaterial(landscape.materials[id],true);
  }
  return std::make_unique<WorldView>(start.bounds,skyPreset);
  }
