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
  auto&      device = Resources::device();
  const bool inRays = Shaders::options().doRtScene;

  if(wholeWorld) {
    meshletDesc = Resources::ssbo(packed.meshletBounds.data(), packed.meshletBounds.size()*sizeof(packed.meshletBounds[0]));
    }

  std::vector<RtGeometry>        rtBuild[2];
  std::vector<RtScene::Geometry> rtGeom[2];
  blocks.reserve(packed.subMeshes.size());
  for(size_t i=0; i<packed.subMeshes.size(); ++i) {
    auto& sub      = packed.subMeshes[i];
    auto  id       = uint32_t(sub.iboOffset/PackedMesh::MaxInd);
    auto  material = Resources::loadMaterial(sub.material,true);

    if(material.alpha==Material::AdditiveLight || sub.iboLength==0) {
      continue;
      }

    Block b;
    b.mesh = visual.get(mesh,material,sub.iboOffset,sub.iboLength,&packed.meshletBounds[id],DrawCommands::Landscape,texturePosition,
                        &sub.texel);
    b.mesh.setObjMatrix(Matrix4x4::mkIdentity());

    if(inRays && !b.mesh.isEmpty()) {
      if(RtScene::isMergedLandscape(material,RtScene::Landscape)) {
        const size_t k = material.alpha==Material::Solid ? 0 : 1;
        rtBuild[k].push_back(mesh.rtGeometry(sub.iboOffset,sub.iboLength));
        rtGeom [k].push_back({material.tex,&mesh.vboBuffer(),&mesh.rtIbo(),mesh.rtFirstIndex(sub.iboOffset),sub.iboLength});
        }
      else if(RtScene::isSupported(material,RtScene::Landscape)) {
        // water: a slot of its own, its object finds the BLAS by the submesh's slice
        mesh.sub[i].blas = device.blas({mesh.rtGeometry(sub.iboOffset,sub.iboLength)});
        }
      }
    blocks.emplace_back(std::move(b));
    }

  for(size_t k=0; k<2; ++k) {
    if(rtBuild[k].empty())
      continue;
    rt[k].blas = device.blas(rtBuild[k]);
    rt[k].id   = visual.addRtLandscape(rt[k].blas, k==0 ? Material::Solid : Material::AlphaTest, std::move(rtGeom[k]));
    }
  }

Landscape::~Landscape() {
  blocks.clear();
  for(auto& part:rt) {
    if(part.id!=size_t(-1))
      visual.freeRtLandscape(part.id);
    // the frames in flight keep it; the next one takes its slot out of the scene
    Resources::recycle(std::move(part.blas));
    }
  visual.invalidate(mesh);
  // The recycle slot is released only after the corresponding frame fence.
  Resources::recycle(std::move(mesh.vbo));
  Resources::recycle(std::move(mesh.ibo));
  Resources::recycle(std::move(mesh.ibo8));
  Resources::recycle(std::move(meshletDesc));
  for(auto& sub:mesh.sub)
    Resources::recycle(std::move(sub.blas));
  }

void Landscape::setVisible(bool visible) {
  for(auto& block:blocks)
    block.mesh.setVisible(visible);
  for(auto& part:rt)
    if(part.id!=size_t(-1))
      visual.setRtLandscapeVisible(part.id, visible);
  }
