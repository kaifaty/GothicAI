#pragma once

#include <Tempest/DescriptorArray>
#include <Tempest/StorageBuffer>
#include <array>
#include <unordered_map>

#include "material.h"

class StaticMesh;
class AnimMesh;

class DrawBuckets {
  public:
    DrawBuckets();

    struct Bucket {
      const StaticMesh* staticMesh = nullptr;
      const AnimMesh*   animMesh   = nullptr;
      Material          mat;
      };

    class Id {
      public:
        Id() = default;
        Id(DrawBuckets& owner):owner(&owner){}
        Id(Id&& other) noexcept;
        Id& operator = (Id&& other) noexcept;
        ~Id();

        uint16_t toInt() const { return id; }

        Bucket& operator *  () { return  owner->bucketsCpu[id]; }
        Bucket* operator -> () { return &owner->bucketsCpu[id]; }

      private:
        Id(DrawBuckets* owner, size_t id);

        DrawBuckets* owner = nullptr;
        uint16_t     id    = 0;
      friend class DrawBuckets;
      };

    Id   alloc(const Material& mat, const StaticMesh& mesh);
    Id   alloc(const Material& mat, const AnimMesh&   mesh);
    void invalidate(const StaticMesh& mesh);

    const Bucket& operator[](size_t i) { return bucketsCpu[i]; }

    auto ssbo() const -> const Tempest::StorageBuffer& { return bucketsGpu; }
    auto buckets() const -> const std::vector<Bucket>&;

    auto& textures() const { return textureDesc[currentFrame].arrays[0]; }
    auto& normals()  const { return textureDesc[currentFrame].arrays[1]; }
    auto& orm()      const { return textureDesc[currentFrame].arrays[2]; }
    auto& vbo()      const { return desc.vbo;     }
    auto& ibo()      const { return desc.ibo;     }
    auto& morphId()  const { return desc.morphId; }
    auto& morph()    const { return desc.morph;   }

    bool commit(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);

  private:
    void updateBindlessArrays(bool updateBuffers, uint8_t fId);
    void updateTextureArrays(uint8_t fId);

    enum BucketFlg : uint32_t {
      BK_SOLID = 0x1,
      BK_SKIN  = 0x2,
      BK_MORPH = 0x4,
      BK_WATER = 0x8,
      BK_NORMAL = 0x10,
      BK_ORM    = 0x20,
      BK_MATERIAL_MAPS = 0x40,
      BK_ANTITILE = 0x80,
      BK_ANTITILE_ROTATE = 0x100,
      BK_HASHED_ALPHA = 0x200,
      BK_HAIR = 0x400,
      BK_STRAND = 0x800,
      BK_EYE = 0x1000,
      BK_PROJECTION = 0x2000,
      BK_FRINGE = 0x4000,
      };

    struct BucketGpu final {
      Tempest::Vec4  bbox[2];
      Tempest::Point texAniMapDirPeriod;
      float          bboxRadius       = 0;
      float          waveMaxAmplitude = 0;
      float          alphaWeight      = 1;
      float          envMapping       = 0;
      uint32_t       flags            = 0;
      float          uvScale          = 1.f;
      float          antitileCell     = 1.f;
      uint32_t       diffuseFrames   = 0;
      uint32_t       frameCount      = 0;
      uint32_t       frameDuration   = 0;
      float          turbidity        = 0.f;
      float          turbidityDepth   = 0.f;
      float          emissive         = 0.f;
      float          reactive         = 0.f; // Material::taaReactive (graphics plan 1v); scalars: an array member here
                                             // faulted the GPU on Metal
      // the layer over a projection (BK_FRINGE, material_projection.glsl): where its three maps sit in the bindless
      // arrays, its repeats per repeat of the projection, its bands as shares of v (Material::Fringe)
      uint32_t       fringeTex        = 0;
      float          fringeScale      = 0.f;
      float          fringeTop0       = 0.f;
      float          fringeTop1       = 0.f;
      float          fringeBottom0    = 0.f;
      float          fringeBottom1    = 0.f;
      float          padding0         = 0.f;
      float          padding1         = 0.f;
      };
    static_assert(sizeof(BucketGpu)==128); // GPU array stride of Bucket in scene.glsl

    struct {
      Tempest::DescriptorArray vbo;
      Tempest::DescriptorArray ibo;
      Tempest::DescriptorArray morphId;
      Tempest::DescriptorArray morph;
      } desc;

    using TextureSlot = std::pair<uint8_t,uint32_t>;
    struct TextureArrays {
      std::array<Tempest::DescriptorArray,3> arrays;
      std::vector<TextureSlot> pending;
      bool rebuild = true;
      };
    std::vector<TextureArrays> textureDesc;
    std::array<std::vector<const Tempest::Texture2d*>,3> textureSlots;
    std::unordered_map<const Tempest::Texture2d*,std::vector<TextureSlot>> textureUsers;
    uint8_t currentFrame = 0;

    // the fringes' maps (albedo, normal, ORM) in the bindless arrays after the buckets and their frames, one entry
    // per distinct set: BucketGpu::fringeTex is the first's index plus its place here
    std::vector<std::array<const Tempest::Texture2d*,3>> fringeMaps;

    std::vector<Bucket>      bucketsCpu;
    Tempest::StorageBuffer   bucketsGpu;
    bool                     bucketsDurtyBit = false;
    uint64_t                 textureGeneration = 0;
  };
