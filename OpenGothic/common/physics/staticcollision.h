#pragma once

// The static landscape and water of a DynamicWorld before their bodies exist (docs/landscape-plan.md, Д3, task 6):
// the sector names, the vertices in metres, the land and water meshes (PhysicVbo: index lists, segments with
// material group and sector, prepared triangle data) and their Jolt shapes. It asks the collision cache
// (collisioncache.h) first; on a hit everything comes from the file and the input is never called: no collision
// input, no PackedMesh. On a miss, or with the cache off, it calls the input once, packs it with PackedMesh
// PK_Physic, builds the shapes and writes the cache. Both ways give the same bodies, triangle order and answers.

#include "physics/collisioncache.h"

#include "asset/worldmesh.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class PhysicVbo;
class PhysicsHost;

class StaticCollision final {
  public:
    // The collision input: called at most once, only when the cache misses or is off.
    using Input = std::function<const asset::WorldMesh&()>;
    struct Cache {
      std::filesystem::path root; // empty: no cache
      CollisionCache::Key   key;
      };
    struct Stats {
      bool   cacheHit = false, inputCalled = false;
      double keyMs = 0, readMs = 0, buildMs = 0, writeMs = 0; // build: PackedMesh and the shapes, not the input
      };

    // The game's cache of host's world: BaseStore::cacheRoot() and the key of Resources::wayHeightInputs, the
    // physics flags, split and the world cloth rules; no root when the cache is off or unavailable.
    static Cache gameCache(const PhysicsHost& host, bool split);
    // WorldCloth::rulesSha256 in hex and WorldCloth::Algorithm, for Key::clothRules.
    static std::string clothRules();

    StaticCollision(const Input& input, const Cache& cache, bool split);
    StaticCollision(const StaticCollision&) = delete;
    StaticCollision& operator=(const StaticCollision&) = delete;
    ~StaticCollision();

    std::vector<std::string>   sectors;  // one per PK_Physic sub mesh: its material's name
    std::vector<JPH::Vec3>     vertices; // metres; land and water index them
    std::unique_ptr<PhysicVbo> land, water;
    JPH::ShapeRefC             landShape, waterShape;
    Stats                      stats;

  private:
    void pack(const asset::WorldMesh& mesh);
  };
