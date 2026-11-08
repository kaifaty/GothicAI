#pragma once

#include <Tempest/AccelerationStructure>
#include <Tempest/DescriptorArray>
#include <Tempest/StorageBuffer>
#include <Tempest/Texture2d>

#include <vector>

class Material;
class StaticMesh;

class RtScene {
  public:
    RtScene();

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

    void notifyTlas(const Material& m, RtScene::Category cat) const;
    bool isUpdateRequired() const;

    void addInstance(const Tempest::Matrix4x4& pos, const Tempest::AccelerationStructure& blas,
                     const Material& mat, const StaticMesh& mesh, size_t firstIndex, size_t iboLength, Category cat);
    void buildTlas();
    bool refreshTextures();

    Tempest::AccelerationStructure             tlas;

    Tempest::DescriptorArray                   tex;
    Tempest::DescriptorArray                   vbo;
    Tempest::DescriptorArray                   ibo;
    Tempest::StorageBuffer                     rtDesc;

  private:
    struct BuildBlas {
      std::vector<Tempest::RtGeometry> geom;
      std::vector<RtObjectDesc>        rtDesc;
      };

    struct Build {
      std::vector<const Tempest::Texture2d*>     tex;
      std::vector<const Tempest::StorageBuffer*> vbo;
      std::vector<const Tempest::StorageBuffer*> ibo;

      std::vector<RtObjectDesc>                  rtDesc;
      std::vector<Tempest::RtInstance>           inst;

      BuildBlas                                  staticOpaque;
      BuildBlas                                  staticAt;
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
      uint64_t                                   triangles = 0;
#endif
      };

    uint32_t aquireBucketId(const Material& mat, const StaticMesh& mesh);
    void     addInstance(const BuildBlas& build, Tempest::AccelerationStructure& blas, Tempest::RtInstanceFlags flags);
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
    void     logBuild(double buildMs) const;
#endif

    Build                          build;
    std::vector<const Tempest::Texture2d*> textureIdentities;
    uint64_t                       textureGeneration = 0;
    Tempest::AccelerationStructure blasStaticOpaque;
    Tempest::AccelerationStructure blasStaticAt;

    mutable bool                   needToUpdate = true;
  };

