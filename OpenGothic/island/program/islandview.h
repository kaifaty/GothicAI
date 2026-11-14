#pragma once

// What the island draws (engine layers, step 8.6): a WorldView of the island's bounds and sky, the ground streamed
// by chunks through a ChunkResidency of its own (the seven calls of World::updateLandscape), the props and two lamps
// at the shed, and the hero's capsule. Needs Resources with a device.

#include "graphics/worldview.h"
#include "world/chunkresidency.h"

#include <memory>
#include <vector>

class Camera;
class ProtoMesh;

namespace island {

class World;

class View final {
  public:
    explicit View(World& world);
    ~View();

    WorldView&            view() { return *wview; }
    const ChunkResidency& residency() const { return *chunks; }

    void setHero(const Tempest::Matrix4x4& body);
    // Before a frame is drawn: which chunks are near the hero and the camera, and the textures they ask for.
    void updateLandscape(const Camera& camera, const Tempest::Vec3& hero);

  private:
    std::unique_ptr<ChunkResidency>    chunks;
    std::unique_ptr<WorldView>         wview;
    std::vector<MeshObjects::Mesh>     props;
    std::vector<LightGroup::Light>     lamps;
    std::unique_ptr<ProtoMesh>         capsule;
    MeshObjects::Mesh                  hero;
    Resources::TextureStartup          startup;
    bool                               loadingPending = true;
  };

// The hero's capsule as a mesh: BodyRadius and BodyHeight of islandbody.h, its feet at the origin, one colour.
std::unique_ptr<ProtoMesh> capsuleMesh();

}
