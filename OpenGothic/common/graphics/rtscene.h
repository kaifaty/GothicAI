#pragma once

#include <Tempest/AccelerationStructure>
#include <Tempest/CommandBuffer>
#include <Tempest/DescriptorArray>
#include <Tempest/Encoder>
#include <Tempest/StorageBuffer>
#include <Tempest/Texture2d>

#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>

#include "resources.h"

class Material;
class StaticMesh;

// The scene of the ray queries, kept between frames. Every object in it holds a slot: an instance of the TLAS and
// its own entry of rtDesc. A change of an object rewrites its slot, and the frame uploads the changed slots and
// rebuilds the TLAS (pass RT-TLAS); a frame without changes records nothing. The solid and alpha-tested landscape
// is two merged BLAS, built once, whose entries are the prefix of rtDesc.
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

    static constexpr uint32_t NoSlot = uint32_t(-1);

    // The objects the scene holds: these categories and materials
    static bool isSupported(const Material& mat, Category cat);
    // A supported landscape object of the merged BLAS rather than of a slot (water keeps a slot)
    static bool isMergedLandscape(const Material& mat, Category cat);

    // A slot of an object: compact, a freed one is given again. A slot is inactive (rays never meet it) until set.
    uint32_t allocSlot();
    void     freeSlot(uint32_t slot);
    // The state of the slot's object; a hidden object keeps its slot with mask 0
    void     setSlot(uint32_t slot, const Tempest::Matrix4x4& pos, const Tempest::AccelerationStructure& blas,
                     const Material& mat, const StaticMesh& mesh, size_t firstIndex, size_t iboLength, bool visible);

    // The merged landscape anew from every visible object of it: begin, add each, and the next commit builds it
    void beginLandscape();
    void addLandscape(const Material& mat, const StaticMesh& mesh, size_t firstIndex, size_t iboLength);

    // Uploads the changed slots and records the TLAS build into the frame (debug marker "RT-TLAS"), if anything changed
    void commit(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);
    bool refreshTextures();

    Tempest::AccelerationStructure             tlas;

    Tempest::DescriptorArray                   tex;
    Tempest::DescriptorArray                   vbo;
    Tempest::DescriptorArray                   ibo;
    Tempest::StorageBuffer                     rtDesc;

  private:
    // Instances 0 and 1 of the TLAS are the merged landscape, slot s is instance LandscapeInstances+s
    static constexpr uint32_t LandscapeInstances = 2;

    struct Slot {
      Tempest::RtInstance inst;          // id is set when packed: the prefix of rtDesc goes first
      RtObjectDesc        desc      = {};
      uint32_t            triangles = 0;
      bool                used      = false;
      bool                dirty     = false;
      };

    struct Merged {
      std::vector<Tempest::RtGeometry> geom;
      std::vector<RtObjectDesc>        desc;
      Tempest::AccelerationStructure   blas;
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

    uint32_t     bucketId(const Material& mat, const StaticMesh& mesh);
    RtObjectDesc objectDesc(const Material& mat, const StaticMesh& mesh, size_t firstIndex);
    void         markSlot(uint32_t slot);
    void         packInstance(size_t index, const Tempest::RtInstance& inst);
    void         buildLandscape();
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
    void         logBuild(const char* reason, double buildMs) const;
#endif

    std::vector<Slot>                                         slots;
    std::priority_queue<uint32_t,std::vector<uint32_t>,std::greater<uint32_t>> freeSlots;
    std::vector<uint32_t>                                     dirtySlots;

    // Meshes and textures live for the session (the mesh cache, the landscape of the world): buckets are never freed
    std::unordered_map<BucketKey,uint32_t,BucketHash>         buckets;
    std::vector<const Tempest::Texture2d*>                    bucketTex;
    std::vector<const Tempest::StorageBuffer*>                bucketVbo;
    std::vector<const Tempest::StorageBuffer*>                bucketIbo;
    size_t                                                    bucketsUploaded = size_t(-1);
    uint64_t                                                  textureGeneration = 0;

    Merged                                                    landscape[LandscapeInstances];
    uint32_t                                                  prefix = 0;  // entries of the landscape in rtDesc
    bool                                                      landscapeDirty = false;
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
    uint64_t                                                  landscapeTriangles = 0;
#endif

    Mirror                                                    instanceCpu;  // the TLAS instances, the backend's layout
    Mirror                                                    descCpu;      // rtDesc
    Tempest::StorageBuffer                                    instanceGpu;
  };
