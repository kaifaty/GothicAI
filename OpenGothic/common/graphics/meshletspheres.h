#pragma once

#include <Tempest/CommandBuffer>
#include <Tempest/Encoder>
#include <Tempest/StorageBuffer>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "resources.h"

class StaticMesh;

// The spheres of the meshlets of the static meshes the buckets draw, in one array on the GPU: meshlet i of a mesh is at
// the mesh's offset plus i (Bucket::meshletSpheres of scene.glsl). The shadow pages take an object's meshlets by them
// instead of its whole mesh (vsm_cull_meshlets.comp, reports/engine/2026-11-16-modern-trees/shadow-flicker.md).
// DrawBuckets places the mesh of every bucket as it writes the buckets for the GPU; a mesh whose buckets are dropped
// (DrawBuckets::invalidate) frees its range. A mesh without spheres (StaticMesh::meshletSpheres) has none.
class MeshletSpheres final {
  public:
    static constexpr uint32_t None = 0xFFFFFFFF; // VSM_NO_SPHERES of vsm_common.glsl

    MeshletSpheres();
    ~MeshletSpheres();

    // the offset of the mesh's first sphere, placed at the first call; None without spheres
    uint32_t place(const StaticMesh& mesh);
    // the mesh goes away: its range is free for another
    void     forget(const StaticMesh& mesh);
    // the spheres placed since the last commit into the GPU's array, in the commands of frame fId
    void     commit(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);

    const Tempest::StorageBuffer& ssbo() const { return gpu; }
    uint32_t                      size() const { return end; } // spheres from the first to the last range

  private:
    struct Range final {
      uint32_t first = 0;
      uint32_t count = 0;
      };

    uint32_t alloc(uint32_t count);
    void     release(Range r);
    void     upload(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);

    std::unordered_map<const StaticMesh*,Range> ranges;
    std::vector<Range>                          freeList; // by first, merged
    std::vector<const StaticMesh*>              pending;  // placed, not in the GPU's array yet
    uint32_t                                    end      = 0;
    uint32_t                                    capacity = 0;

    Tempest::StorageBuffer                      gpu;
    Tempest::StorageBuffer                      stage[Resources::MaxFramesInFlight];
    std::vector<uint8_t>                        patchCpu;
  };
