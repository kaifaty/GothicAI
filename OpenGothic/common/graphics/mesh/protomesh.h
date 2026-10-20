#pragma once

#include <Tempest/VertexBuffer>
#include <Tempest/IndexBuffer>
#include <Tempest/Device>
#include <Tempest/Matrix4x4>

#include <zenkit/Model.hh>
#include <zenkit/ModelMesh.hh>
#include <zenkit/ModelHierarchy.hh>
#include <zenkit/MorphMesh.hh>

#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/mesh/submesh/animmesh.h"

#include "resources.h"

class PackedMesh;
class PhysicMeshShape;

class ProtoMesh {
  public:
    using Vertex = Resources::VertexA;

    ProtoMesh(PackedMesh&&  pm, std::string_view fname);
    ProtoMesh(PackedMesh&&  pm, const std::vector<zenkit::MorphAnimation>& aniList, std::string_view fname);
    ProtoMesh(const zenkit::Model& lib, std::unique_ptr<Skeleton>&& sk, std::string_view fname, bool explicitAlphaTest = false);
    ProtoMesh(const zenkit::ModelHierarchy& lib, std::unique_ptr<Skeleton>&& sk, std::string_view fname);
    ProtoMesh(const zenkit::ModelMesh& lib, std::unique_ptr<Skeleton>&& sk, std::string_view fname, bool explicitAlphaTest = false);
    ProtoMesh(const Material& mat, std::vector<Resources::Vertex> vbo, std::vector<uint32_t> ibo); //decals
    ProtoMesh(ProtoMesh&&)=delete;
    ProtoMesh& operator=(ProtoMesh&&)=delete;
    ~ProtoMesh();

    struct SubMesh final {
      Material                         material;
      Tempest::IndexBuffer<uint32_t>   ibo;
      std::unique_ptr<PhysicMeshShape> shape;
      };

    struct SubMeshId final {
      size_t                id    = 0;
      size_t                subId = 0;
      };

    struct Node {
      size_t                attachId  = size_t(-1);
      size_t                parentId  = size_t(-1);
      bool                  hasChild  = false;
      Tempest::Matrix4x4    transform;

      size_t                submeshIdB = 0;
      size_t                submeshIdE = 0;
      };

    struct Attach : StaticMesh {
      using StaticMesh::StaticMesh;
      Attach(Attach&&)=default;
      Attach& operator=(Attach&&)=default;
      ~Attach();

      std::string                      name;
      bool                             hasNode=false;
      std::unique_ptr<PhysicMeshShape> shape;
      };

    struct Pos {
      std::string        name;
      Tempest::Matrix4x4 transform;
      size_t             node=0;
      };

    using Morph = StaticMesh::Morph;

    // animation
    std::unique_ptr<Skeleton>      skeleton;
    std::vector<AnimMesh>          skined;
    std::vector<Morph>             morph;
    Tempest::StorageBuffer         morphIndex;
    Tempest::StorageBuffer         morphSamples;

    std::vector<Attach>            attach;
    std::vector<Node>              nodes;
    std::vector<SubMeshId>         submeshId;

    std::vector<Pos>               pos;
    Tempest::Vec3                  bboxMesh[2];

    // the shape of the mesh, the hit zones are fitted to it (graphics/mesh/hitzones): its points, thinned; a
    // skinned mesh by the bone each follows the most, in the bone's frame (rigid parts at their bones too)
    std::vector<Tempest::Vec3>              points;
    std::vector<std::vector<Tempest::Vec3>> nodePoints;
    std::vector<std::vector<Tempest::Vec3>> attachPoints; // by attachment: those without a node go on the
                                                          // bone of their name (a golem's stones)

    std::string                    scheme, fname;

    size_t                         skinedNodesCount() const;
    size_t                         findNode(std::string_view name,size_t def=size_t(-1)) const;
    const Tempest::Vec3*           bbox() const;
    // Lowest point of the exact indexed body skin, using the renderer's four
    // bone weights and fatness displacement. No GPU access or pose changes.
    bool                           minSkinnedVertex(const Tempest::Matrix4x4* worldBones, size_t count,
                                                     const Tempest::Matrix4x4& object, float fatness,
                                                     Tempest::Vec3& point) const;

  private:
    std::vector<Vertex>            skinVertices;
    void                           setupScheme(std::string_view s);
    void                           shapeOfAttach(const std::vector<std::vector<Tempest::Vec3>>& attachPoints);
    void                           shapeOfSkin(const PackedMesh& pkg);
    void                           remap(const zenkit::MorphAnimation& a,
                                         const std::vector<uint32_t>& vertId,
                                         std::vector<int32_t>& remapId,
                                         std::vector<Tempest::Vec4>& samples,
                                         size_t idOffset);

    Morph                          mkAnimation(const zenkit::MorphAnimation& a);
  };
