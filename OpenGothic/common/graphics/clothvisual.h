#pragma once

#include <Tempest/Matrix4x4>
#include "asset/worldtypes.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zenkit { class Mesh; }

class World;
class Cloth;
class MeshObjects;

#include "graphics/meshobjects.h"

// Owns a cloth mesh-vob or a surface extracted from the level mesh. Nearby
// cloth uses one subdivided surface for simulation and rendering; the native
// mesh and its uninterrupted morph/wind animation remain the distant fallback.
class ClothVisual final {
  public:
    static bool supports(std::string_view visual);
    static std::unique_ptr<ClothVisual> create(MeshObjects::Mesh& view, World& world,
                                               const asset::VirtualObject& vob, bool enableCollision);
    static std::unique_ptr<ClothVisual> createSurface(World& world, const asset::WorldMesh& source,
                                                     const std::vector<uint32_t>& pinnedSourcePositionIds,
                                                     std::string visual, uint32_t id, float stiffness, float linearDamping);
    ~ClothVisual();
    void setView(MeshObjects::Mesh& view);
    void setMatrix(const Tempest::Matrix4x4& matrix);

  private:
    struct State;
    ClothVisual();
    std::shared_ptr<State> state;
  };
