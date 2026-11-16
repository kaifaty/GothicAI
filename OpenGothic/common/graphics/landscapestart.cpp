#include "landscapestart.h"

#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/worldview.h"
#include "resources.h"
#include "worldchunkcatalogue.h"

#include <vector>

std::unique_ptr<WorldView> streamingWorldView(const asset::WorldMesh& landscape, const ChunkTable& chunks,
                                              std::string_view skyPreset) {
  const auto map=PackedMesh::landscapeMaterialMap(landscape);
  const auto* catalogue=chunks.source ? chunks.source->catalogue() : nullptr;
  {
  Resources::TextureLoadScope textureLoadScope(true);
  std::vector<bool> loaded(landscape.materials.size());
  const auto load=[&](uint32_t id) {
    id=map[id];
    if(!loaded[id]) {
      Resources::loadMaterial(landscape.materials[id],true);
      loaded[id]=true;
      }
    };
  if(catalogue!=nullptr) {
    for(const auto id:catalogue->landscapeMaterials())
      load(id);
    }
  else {
    for(const auto id:landscape.polygons.material_indices)
      load(id);
    }
  }
  if(catalogue==nullptr)
    return std::make_unique<WorldView>(PackedMesh::landscapeBbox(landscape),skyPreset);
  const auto [min,max]=catalogue->landscapeBounds();
  return std::make_unique<WorldView>(std::pair{Tempest::Vec3(min.x,min.y,min.z),Tempest::Vec3(max.x,max.y,max.z)},skyPreset);
  }
