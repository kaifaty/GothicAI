#include "islandview.h"

#include "islandbody.h"
#include "islandcontent.h"
#include "islandworld.h"

#include "camera.h"
#include "resources.h"
#include "asset/staticmesh.h"
#include "asset/worldtypes.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/submesh/packedmesh.h"

#include <cmath>

using namespace Tempest;

namespace island {

namespace {
constexpr float Pi = 3.14159265358979f;
}

std::unique_ptr<ProtoMesh> capsuleMesh() {
  // rings of the lower half-sphere, the cylinder's two, the upper half-sphere's; a seam of doubled vertices at u=1
  constexpr int   Around = 32, Cap = 8;
  const float     r = BodyRadius*100.f, h = BodyHeight*100.f;
  asset::StaticMesh mesh;
  asset::SubMesh    sub;
  sub.mat.name  = "ISLAND_HERO";
  // no texture: the material is its colour, a red that stands out from the sand, the grass and the sea
  sub.mat.color = asset::Color(168, 52, 38, 255);
  sub.mat.group = asset::MaterialGroup::NONE;
  std::vector<std::pair<float,float>> rings; // (height of the ring, its radius)
  for(int i=0; i<=Cap; ++i) {
    const float a = -0.5f*Pi + 0.5f*Pi*float(i)/float(Cap);
    rings.emplace_back(r + r*std::sin(a), r*std::cos(a));
    }
  for(int i=0; i<=Cap; ++i) {
    const float a = 0.5f*Pi*float(i)/float(Cap);
    rings.emplace_back(h - r + r*std::sin(a), r*std::cos(a));
    }
  for(size_t ring=0; ring<rings.size(); ++ring) {
    const auto [y, radius] = rings[ring];
    // the normal of a capsule: from the axis' nearest point
    const float axisY = std::clamp(y, r, h-r);
    for(int k=0; k<=Around; ++k) {
      const float a = 2.f*Pi*float(k)/float(Around);
      const asset::Vec3 p = {radius*std::cos(a), y, radius*std::sin(a)};
      asset::Vec3 n = {p.x, p.y-axisY, p.z};
      const float l = std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
      n = l>0 ? asset::Vec3(n.x/l, n.y/l, n.z/l) : asset::Vec3(0, y<h*0.5f ? -1.f : 1.f, 0);
      asset::MeshWedge w;
      w.normal  = n;
      w.texture = asset::Vec2(float(k)/float(Around), 1.f-y/h);
      w.index   = uint16_t(mesh.positions.size());
      mesh.positions.push_back(p);
      mesh.normals.push_back(n);
      sub.wedges.push_back(w);
      }
    }
  const int row = Around+1;
  for(int ring=0; ring+1<int(rings.size()); ++ring)
    for(int k=0; k<Around; ++k) {
      const auto a = uint16_t(ring*row+k), b = uint16_t(ring*row+k+1);
      const auto c = uint16_t((ring+1)*row+k), d = uint16_t((ring+1)*row+k+1);
      sub.triangles.push_back({{a, c, b}});
      sub.triangles.push_back({{b, c, d}});
      }
  mesh.materials.push_back(sub.mat);
  mesh.sub_meshes.push_back(std::move(sub));
  mesh.bbox = {{-r, 0, -r}, {r, h, r}};
  PackedMesh packed(mesh, PackedMesh::PK_Visual);
  return std::make_unique<ProtoMesh>(std::move(packed), "ISLAND_HERO.3DS", false);
  }

View::View(World& world) {
  // Until the first frame: textures that can have a tail are read at their coarsest level, as World does.
  startup.begin();
  chunks = ChunkResidency::create(world.chunks());
  if(chunks==nullptr)
    throw std::runtime_error("the island's chunks give no residency");

  auto mesh = world.takeMesh();
  {
    // the ground's materials decoded before its chunks are published
    const auto map = PackedMesh::landscapeMaterialMap(mesh);
    Resources::TextureLoadScope scope(true);
    std::vector<bool> loaded(mesh.materials.size());
    for(auto id:mesh.polygons.material_indices) {
      id = map[id];
      if(!loaded[id]) {
        Resources::loadMaterial(mesh.materials[id], true);
        loaded[id] = true;
        }
      }
  }
  // WorldView of the bounds and the sky only: no World. The sky and the fog take the observer's height from the
  // lowest vertex of the ground.
  wview = std::make_unique<WorldView>(PackedMesh::landscapeBbox(mesh), WorldName);
  wview->setTextureResidency(chunks.get());
  wview->startLandscapeStreaming(std::move(mesh), *chunks, 0);

  for(const auto& p:world.props()) {
    auto m = wview->addStaticView(p.visual);
    if(m.nodesCount()==0)
      throw std::runtime_error("the island's prop is not drawn: "+p.visual);
    m.setObjMatrix(p.at);
    props.push_back(std::move(m));
    }

  // Two lanterns: a warm light of a flame (about 1900 K), 6 m of reach.
  for(const auto& at:world.lamps()) {
    asset::VLight light;
    light.preset     = "ISLAND_LAMP";
    light.light_type = asset::LightType::POINT;
    light.range      = 600;
    light.color      = asset::Color(255, 160, 90, 255);
    light.on         = true;
    light.position   = asset::Vec3(at.x, at.y, at.z);
    lamps.push_back(wview->addLight(light, 0));
    }

  capsule = capsuleMesh();
  hero    = wview->addStaticView(capsule.get(), false);
  }

View::~View() = default;

void View::setHero(const Matrix4x4& body) {
  hero.setObjMatrix(body);
  }

// World::updateLandscape, with the island's hero in place of the player.
void View::updateLandscape(const Camera& camera, const Vec3& at) {
  Resources::setTextureView(float(camera.viewportHeight()), camera.fovY());
  wview->prepareLandscapeWork();
  chunks->setLookAhead(Resources::textureLookAhead());
  chunks->update(at, camera.originLwc(), camera.zFar());
  wview->finishLandscapeUpdate();
  if(loadingPending) {
    wview->flushTextureDemand();
    startup.end();
    Resources::finishTextureLoading();
    loadingPending = false;
    }
  else if(chunks->viewJumped() && Resources::textureScreenMips()) {
    wview->flushTextureDemand();
    Resources::completeTextureJump();
    }
  Resources::updateTextures(chunks.get());
  }

}
