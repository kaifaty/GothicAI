#pragma once

#include <cstdint>
#include <atomic>
#include <mutex>

#include "asset/staticmesh.h"

#include <Tempest/Matrix4x4>
#include <Tempest/Point>

class PackedMesh;
class Pose;

class PfxEmitterMesh {
  public:
    PfxEmitterMesh(const PackedMesh& src);
    PfxEmitterMesh(const asset::ModelMesh& src);

    // a point on the mesh; a skinned one is posed by skin[skinCount] when given (the body's render rig palette,
    // graphics/mesh/renderrig: its bone indices are the rig's), else by the pose
    Tempest::Vec3 randCoord(float rnd, const Pose* pose, const Tempest::Matrix4x4* skin = nullptr, size_t skinCount = 0) const;

  private:
    struct Triangle {
      uint32_t id[3]  = {};
      float    sz     = 0;
      float    prefix = 0;
      };

    struct AnimData {
      Tempest::Vec3 v[4];
      uint8_t       id[4] = {};
      float         weights[4] = {};
      };

    Tempest::Vec3 randCoord(const Triangle& t, float rnd, const Pose* pose, const Tempest::Matrix4x4* skin, size_t skinCount) const;
    Tempest::Vec3 animCoord(const Pose& pose, uint32_t index) const;
    Tempest::Vec3 animCoord(const Tempest::Matrix4x4* skin, size_t skinCount, uint32_t index) const;
    void          mkIndex();
    void          mkPosedIndex(const Pose* pose, const Tempest::Matrix4x4* skin, size_t skinCount) const;

    static float area(float x1, float y1, float z1, float x2, float y2, float z2, float x3, float y3, float z3);

    // a skinned mesh's vertices are kept bone by bone, so the areas of its triangles are measured once in the
    // first pose the mesh is asked in (the areas hardly change with the pose)
    mutable std::vector<Triangle> triangle;
    mutable std::atomic_bool      posed{false};
    mutable std::mutex            posedSync;
    std::vector<Tempest::Vec3> vertices;
    std::vector<AnimData>      vertAnim;
  };

