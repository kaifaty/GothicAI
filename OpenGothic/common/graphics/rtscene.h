#pragma once

#include <Tempest/AccelerationStructure>
#include <Tempest/CommandBuffer>
#include <Tempest/DescriptorArray>
#include <Tempest/Encoder>
#include <Tempest/StorageBuffer>
#include <Tempest/Texture2d>

#include <chrono>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <vector>

#include "graphics/material.h"
#include "resources.h"

class StaticMesh;

// The scene of the ray queries, kept between frames. Every object in it holds a slot: an instance of the TLAS and a
// range of rtDesc, an entry per geometry of its BLAS (rt_common.glsl reads entry instanceCustomIndex+geometryIndex).
// A change of an object rewrites its slot, and the frame uploads the changed slots and rebuilds the TLAS (pass
// RT-TLAS); a frame without changes records nothing. The solid and the alpha-tested landscape of a Landscape (the
// whole world, or a chunk of it while it is published) are a slot each, of one BLAS with a geometry per submesh.
class RtScene {
  public:
    RtScene();
    ~RtScene();

    enum Category : uint8_t {
      None,
      Landscape,
      Static,
      Movable,
      };

    enum CullMask : uint32_t {
      CM_Opaque      = 0x1,
      CM_Transparent = 0x2,
      CM_Water       = 0x4,
      CM_Unknown     = 0x80,
      };

    struct RtObjectDesc {
      uint32_t instanceId;
      uint32_t firstPrimitive : 24;
      uint32_t bits: 8;
      };

    // A geometry of a BLAS as a hit on it reads it: the texture, the 9-float vertices and the 32-bit indices of them
    // (StaticMesh::vboBuffer, rtIbo), and where its indices start in that buffer
    struct Geometry {
      const Tempest::Texture2d*     tex        = nullptr;
      const Tempest::StorageBuffer* vbo        = nullptr;
      const Tempest::StorageBuffer* ibo        = nullptr;
      size_t                        firstIndex = 0;
      size_t                        indices    = 0;
      };

    struct Stats {
      size_t   slots          = 0;  // slots given out
      size_t   landscapeSlots = 0;  // ...of them a landscape BLAS (setLandscapeSlot)
      size_t   landscapeShown = 0;  // ...of those with a BLAS and visible
      uint32_t entries        = 0;  // the end of the ranges of rtDesc given out
      };

    static constexpr uint32_t NoSlot = uint32_t(-1);

    // The objects the scene holds: these categories and materials
    static bool isSupported(const Material& mat, Category cat);
    // A supported landscape object that is a geometry of its Landscape's BLAS rather than a slot (water keeps a slot)
    static bool isMergedLandscape(const Material& mat, Category cat);

    // A slot of an object: compact, a freed one is given again. A slot is inactive (rays never meet it) until set.
    uint32_t allocSlot();
    void     freeSlot(uint32_t slot);
    // The state of the slot's object; a hidden object keeps its slot with mask 0
    void     setSlot(uint32_t slot, const Tempest::Matrix4x4& pos, const Tempest::AccelerationStructure& blas,
                     const Material& mat, const StaticMesh& mesh, size_t firstIndex, size_t iboLength, bool visible);
    // A landscape BLAS of `count` geometries, all of `alpha` (Solid or AlphaTest), in place: an entry of rtDesc per
    // geometry in their order. Every ray mask meets it, as it met the merged landscape of the whole world.
    void     setLandscapeSlot(uint32_t slot, const Tempest::AccelerationStructure& blas, Material::AlphaFunc alpha,
                              const Geometry* geom, size_t count, bool visible);

    // Uploads the changed slots and records the TLAS build into the frame (debug marker "RT-TLAS"), if anything changed
    void commit(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);
    bool refreshTextures();
    Stats stats() const;

    Tempest::AccelerationStructure             tlas;

    Tempest::DescriptorArray                   tex;
    Tempest::DescriptorArray                   vbo;
    Tempest::DescriptorArray                   ibo;
    Tempest::StorageBuffer                     rtDesc;

  private:
    struct Slot {
      Tempest::RtInstance inst;            // id: the first entry of its range
      uint32_t            descFirst = 0;
      uint32_t            descCount = 0;   // 0: no range
      uint64_t            triangles = 0;
      int64_t             blasBytes = 0;   // a landscape slot's, with the frame profile (-1: the backend cannot tell)
      bool                used      = false;
      bool                dirty     = false;
      bool                landscape = false;
      };

    // Ranges of entries of rtDesc: the smallest free range that fits, freed ones merge with their neighbours
    struct Ranges {
      std::map<uint32_t,uint32_t>             byFirst;  // first -> count
      std::set<std::pair<uint32_t,uint32_t>>  bySize;   // (count, first)
      uint32_t                                end = 0;

      uint32_t alloc(uint32_t count);
      void     free(uint32_t first, uint32_t count);
      };

    struct BucketKey {
      const Tempest::Texture2d*     tex;
      const Tempest::StorageBuffer* vbo;
      const Tempest::StorageBuffer* ibo;
      bool operator==(const BucketKey& o) const { return tex==o.tex && vbo==o.vbo && ibo==o.ibo; }
      };
    struct BucketHash {
      size_t operator()(const BucketKey& k) const;
      };

    // The CPU copy of a buffer of the GPU: written in place, its changed blocks reach the GPU inside the frame by the
    // patch shader (as InstanceStorage::commit), so the frames in flight read it in the order of the queue
    struct Mirror {
      std::vector<uint8_t>   cpu;
      std::vector<uint32_t>  dirty;      // bit per block
      bool                   anyDirty = false;
      Tempest::StorageBuffer patch[Resources::MaxFramesInFlight];
      std::vector<uint8_t>   patchCpu;

      void resize(size_t bytes);
      void mark(size_t offset, size_t size);
      // Records the patch of `gpu`; a `gpu` smaller than the copy is replaced by a new one with a margin, written whole
      void commit(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, Tempest::StorageBuffer& gpu);
      };

    uint32_t     bucketId(const Tempest::Texture2d* tex, const Tempest::StorageBuffer* vbo, const Tempest::StorageBuffer* ibo);
    RtObjectDesc objectDesc(const Geometry& geom, Material::AlphaFunc alpha);
    // The instance and the entries of a slot: a new range when the number of geometries changes
    void         writeSlot(uint32_t slot, Tempest::RtInstance ix, Material::AlphaFunc alpha,
                           const Geometry* geom, size_t count);
    void         releaseRange(Slot& s);
    void         markSlot(uint32_t slot);
    void         packInstance(size_t index, const Tempest::RtInstance& inst);
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
    void         logBuild(const char* reason, double buildMs) const;
    void         logLandscape();
#endif

    std::vector<Slot>                                         slots;
    std::priority_queue<uint32_t,std::vector<uint32_t>,std::greater<uint32_t>> freeSlots;
    std::vector<uint32_t>                                     dirtySlots;
    Ranges                                                    entries;

    // Meshes, landscape pages and textures live for the session (the mesh cache, the pages of the world's landscape,
    // the texture cache): buckets are never freed. A chunk of the landscape is a bucket per texture and page.
    std::unordered_map<BucketKey,uint32_t,BucketHash>         buckets;
    std::vector<const Tempest::Texture2d*>                    bucketTex;
    std::vector<const Tempest::StorageBuffer*>                bucketVbo;
    std::vector<const Tempest::StorageBuffer*>                bucketIbo;
    size_t                                                    bucketsUploaded = size_t(-1);
    uint64_t                                                  textureGeneration = 0;

    // the landscape slots: given out and, with the frame profile, their triangles and BLAS bytes (-1: unknown)
    size_t                                                    landscapeSlots = 0;
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
    uint64_t                                                  landscapeTriangles = 0;
    int64_t                                                   landscapeBytes = 0;
    bool                                                      landscapeChanged = false;
    std::chrono::steady_clock::time_point                     landscapeLogged = {};
#endif

    Mirror                                                    instanceCpu;  // the TLAS instances, the backend's layout
    Mirror                                                    descCpu;      // rtDesc
    Tempest::StorageBuffer                                    instanceGpu;
  };
