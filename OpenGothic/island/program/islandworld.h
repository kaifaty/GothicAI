#pragma once

// The island as the program walks it (engine layers, step 8.6): the ground read back from <data>/world, its whole
// mesh as the static collision of one DynamicWorld without a host, and the props placed on it with collision of
// their own. Where the props, the lamps and the demo's start lie is decided here, from the generator's terrain
// (islandterrain.h: the shore and the stream) and the ground as it was read. Nothing here draws: the program's
// view (islandview.h) and the program's test take the same placements.
//
// Needs Resources (the props' meshes) and Settings (the physics' flags) alive.

#include "worldchunksruntime.h"
#include "physics/physicmesh.h"

#include <Tempest/Matrix4x4>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class DynamicWorld;

namespace island {

// The folders of <data>, the island-data target's output: world/, content/ and textures/ (islandcontent.h).
struct DataDir {
  std::filesystem::path root;
  std::filesystem::path world()    const { return root/"world"; }
  std::filesystem::path content()  const { return root/"content"; }
  std::filesystem::path textures() const { return root/"textures"; }
  };

// The content and the textures of the island in place of the game's (OPENGOTHIC_CONTENT, OPENGOTHIC_TEXTURES),
// set in this process before Resources is made: the environment it was started in is not asked.
void useIslandFolders(const DataDir& data);

struct Placed {
  std::string        visual;   // "<meshes.json key>.3DS"
  Tempest::Matrix4x4 at;       // centimetres
  Tempest::Vec3      position; // the origin, on the ground
  float              foot = 0; // the radius it stands on, metres
  };

struct Pose {
  Tempest::Vec3 feet; // centimetres
  float         yaw = 0;
  };

class World final {
  public:
    explicit World(const DataDir& data);
    ~World();

    DynamicWorld&            physics() { return *dynamic; }
    const ChunkTable&        chunks() const { return table; }
    // The ground and the water as read: once, for the view's streaming (the physics took its copy).
    asset::WorldMesh         takeMesh();

    const std::vector<Placed>& props() const { return placed; }
    // The two lamps under the front beam of the shed, centimetres.
    const std::vector<Tempest::Vec3>& lamps() const { return lampAt; }
    // The demo's start: on the dry beach, facing the summit, a chunk border 6-18 m ahead.
    const Pose&              start() const { return begin; }
    const Tempest::Vec3&     summit() const { return top; }

    // The ground's height at (x, z), centimetres, by a ray down onto the landscape and the props; NaN off the field.
    float                    ground(float x, float z) const;
    // The same on the landscape alone.
    float                    land(float x, float z) const;

  private:
    void place();
    void placeStart();
    void addProp(const std::string& name, float x, float z, float turn, float foot);

    ChunkTable                    table;
    asset::WorldMesh              mesh;
    std::unique_ptr<DynamicWorld> dynamic;
    std::vector<PhysicMesh>       bodies;
    std::vector<Placed>           placed;
    std::vector<Tempest::Vec3>    lampAt;
    Pose                          begin;
    Tempest::Vec3                 top;
  };

}
