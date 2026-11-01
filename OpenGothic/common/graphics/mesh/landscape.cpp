#include "landscape.h"

#include <Tempest/Log>
#include <cstddef>

#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/shaders.h"

using namespace Tempest;

Landscape::Landscape(VisualObjects& visual, const PackedMesh &packed)
  :visual(visual),mesh(packed) {
  init(packed,true);
  }

Landscape::Landscape(VisualObjects& visual, const PackedMesh& packed, LandscapePages& pages, const Vec3& texturePosition)
  :visual(visual),mesh(packed,pages) {
  init(packed,false,&texturePosition);
  }

void Landscape::init(const PackedMesh& packed, bool wholeWorld, const Vec3* texturePosition) {
  if(packed.indices.empty())
    return;
  auto& device = Resources::device();

  if(wholeWorld) {
    meshletDesc = Resources::ssbo(packed.meshletBounds.data(), packed.meshletBounds.size()*sizeof(packed.meshletBounds[0]));
    bvhNodes    = Resources::ssbo(packed.bvhNodes.data(),  packed.bvhNodes.size()*sizeof(packed.bvhNodes[0]));
    }

  blocks.reserve(packed.subMeshes.size());
  for(size_t i=0; i<packed.subMeshes.size(); ++i) {
    auto& sub      = packed.subMeshes[i];
    auto  id       = uint32_t(sub.iboOffset/PackedMesh::MaxInd);
    auto  material = Resources::loadMaterial(sub.material,true);

    if(material.alpha==Material::AdditiveLight || sub.iboLength==0) {
      continue;
      }

    if(Shaders::options().doRtScene) {
      mesh.sub[i].blas = device.blas(mesh.vbo,mesh.ibo,sub.iboOffset,sub.iboLength);
      }

    Block b;
    b.mesh = visual.get(mesh,material,sub.iboOffset,sub.iboLength,&packed.meshletBounds[id],DrawCommands::Landscape,texturePosition);
    b.mesh.setObjMatrix(Matrix4x4::mkIdentity());
    blocks.emplace_back(std::move(b));
    }
  }

Landscape::~Landscape() {
  blocks.clear();
  visual.invalidate(mesh);
  // The recycle slot is released only after the corresponding frame fence.
  Resources::recycle(std::move(mesh.vbo));
  Resources::recycle(std::move(mesh.ibo));
  Resources::recycle(std::move(mesh.ibo8));
  Resources::recycle(std::move(meshletDesc));
  Resources::recycle(std::move(bvhNodes));
  for(auto& sub:mesh.sub)
    Resources::recycle(std::move(sub.blas));
  }

void Landscape::setVisible(bool visible) {
  for(auto& block:blocks)
    block.mesh.setVisible(visible);
  }
