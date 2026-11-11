#include "inventoryrenderer.h"

#include "graphics/mesh/protomesh.h"

#include "shaders.h"

using namespace Tempest;

InventoryRenderer::InventoryRenderer()
  :visual(scene,std::pair<Vec3,Vec3>()),itmGroup(visual) {
  Tempest::Matrix4x4 p, mv, shMv[Resources::ShadowLayers];
  p.identity();
  mv.identity();
  mv.scale(0.8f,1.f,1.f);
  scene.setViewProject(mv,p,0,1,shMv);
  }

void InventoryRenderer::draw(Tempest::Encoder<CommandBuffer>& cmd) {
  Tempest::Matrix4x4 mv = Tempest::Matrix4x4::mkIdentity();
  mv.translate(0, 0, 0.5f);
  mv.scale(0.8f,1.f,1.f);

  auto& pso = Shaders::inst().inventory;
  for(auto& i:items) {
    cmd.setViewport(i.x,i.y,i.w,i.h);
    for(size_t r=0;r<i.mesh.nodesCount();++r) {
      auto  n = i.mesh.node(r);
      auto& m = n.material();

      if(auto s = n.mesh()) {
        auto sl = n.meshSlice();
        auto p  = mv;
        p.scale(1, 1, 0.25f);

        p.mul(n.position());
        p.mul(i.viewMat);

        cmd.setBinding(0, *m.tex);
        cmd.setPushData(p);
        cmd.setPipeline(pso);
        cmd.draw(s->vbo, s->ibo, sl.first, sl.second);
        }
      }
    }
  }

void InventoryRenderer::reset(bool full) {
  items.clear();
  }

void InventoryRenderer::drawItem(int x, int y, int w, int h, const ProtoMesh& mesh,
                                 int32_t material, const Matrix4x4& mat) {
  Itm itm;
  itm.mesh = MeshObjects::Mesh(itmGroup,mesh,material,0,material,false);
  itm.mesh.setObjMatrix(Matrix4x4::mkIdentity());
  itm.viewMat = mat;
  itm.x    = x;
  itm.y    = y;
  itm.w    = w;
  itm.h    = h;
  items.push_back(std::move(itm));
  }
