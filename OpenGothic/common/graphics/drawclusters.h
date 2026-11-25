#pragma once

#include <Tempest/StorageBuffer>
#include <Tempest/Vec>
#include <cstdint>

#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/drawbuckets.h"

class DrawClusters {
  public:
    DrawClusters();
    ~DrawClusters();

    using Bucket = DrawBuckets::Bucket;
    static constexpr uint32_t HistoryStatic = 0x80000000u;
    // The lower half of shadowCommandId keeps the shadow command. Bits 16-18 are diagnostic.
    static constexpr uint32_t ShadowVisual = 1u << 16;
    static constexpr uint32_t ShadowCoarse = 2u << 16;
    static constexpr uint32_t ShadowProbeChanged = 1u << 18;
    static constexpr uint32_t ShadowSunLod = 1u << 19; // instance has an active solar caster
    static constexpr uint32_t ShadowMidSunShift = 20; // two bits of TreeLevelBand::MidSun

    struct alignas(16) Cluster final {
      Tempest::Vec3 pos;
      float         r            = 0;
      uint16_t      commandId    = 0;
      uint16_t      bucketId     = 0;
      uint32_t      firstMeshlet = 0;
      uint32_t      meshletCount = 0;
      uint32_t      instanceId   = 0;
      uint32_t      shadowFirstMeshlet = 0;
      uint32_t      shadowMeshletCount = 0; // zero keeps the visual range and command
      uint32_t      shadowCommandId    = 0;
      uint32_t      historyGeneration  = 0;
      };
    static_assert(sizeof(Cluster)==48);

    Cluster& operator[](size_t i) { return clusters[i]; }
    size_t   size() const { return clusters.size(); }
    void     markClusters(size_t id, size_t count = 1);

    uint32_t alloc(const PackedMesh::Cluster* cluster, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId,
                   uint16_t commandId, std::span<const PackedMesh::ClusterLevel> levels, uint32_t instanceId);
    uint32_t alloc(const Bucket&  bucket,  size_t firstMeshlet, size_t meshletCount, uint16_t bucketId, uint16_t commandId);
    void     free(uint32_t id, uint32_t numCluster);

    bool     commit(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);

    auto     ssbo() -> Tempest::StorageBuffer& { return clustersGpu; }
    auto     ssbo() const -> const Tempest::StorageBuffer& { return clustersGpu; }
    bool     hasLevels() const { return !levels.empty(); }
    const Tempest::StorageBuffer& levelIndicesSsbo() const { return hasLevels() ? levelIndicesGpu : noLevelsGpu; }
    const Tempest::StorageBuffer& levelsSsbo() const { return hasLevels() ? levelsGpu : noLevelsGpu; }
    size_t   levelBufferBytes() const { return levelIndicesGpu.byteSize() + levelsGpu.byteSize(); }

  private:
    struct Range {
      size_t begin = 0;
      size_t end   = 0;
      };

    struct ScratchPatch {
      std::vector<uint32_t> header;
      std::vector<Cluster>  patch;
      };

    struct Patch {
      Tempest::StorageBuffer indices;
      Tempest::StorageBuffer data;
      };

    void                           patchClusters(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);
    void                           commitLevels(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);
    void                           allocLevels(size_t first, std::span<const PackedMesh::ClusterLevel> source);
    void                           freeLevels(uint32_t first, uint32_t count);
    size_t                         implAlloc(size_t count);

    std::vector<Cluster>           clusters;
    std::vector<Range>             freeList;
    Tempest::StorageBuffer         clustersGpu;
    std::vector<uint32_t>          clustersDurty;
    std::atomic_bool               clustersDurtyBit {false};

    // The zero index leaves the cluster on its original draw path. Record slots
    // can be recycled independently of the cluster allocator's contiguous ranges.
    std::vector<uint32_t>          levelIndices;
    std::vector<PackedMesh::ClusterLevel> levels;
    std::vector<uint32_t>          freeLevelsIds;
    Range                          dirtyLevelIndices {size_t(-1), 0};
    Range                          dirtyLevels {size_t(-1), 0};
    Tempest::StorageBuffer         levelIndicesGpu, levelsGpu, noLevelsGpu;
    std::vector<uint32_t>          levelPatchScratch;
    Tempest::StorageBuffer         levelPatch[Resources::MaxFramesInFlight][2];

    ScratchPatch                   scratch;

    Patch                          patch[Resources::MaxFramesInFlight];
  };
