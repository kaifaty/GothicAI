#pragma once

#include "asset/worldmesh.h"
#include "asset/staticmesh.h"
#include "asset/material.h"

#include <Tempest/Vec>
#include <filesystem>
#include <span>
#include <utility>

#include "resources.h"
#include "graphics/mesh/texeldensity.h"

class Bounds;

class PackedMesh {
  public:
    using Vertex        = Resources::Vertex;
    using VertexA       = Resources::VertexA;

    enum {
      MaxVert     = 64,
      MaxPrim     = 64,
      MaxInd      = MaxPrim * 3,
      MaxMeshlets = 16,
      };

    enum PkgType {
      PK_Visual,
      PK_VisualLnd,
      PK_VisualMorph,
      PK_Physic,
      };

    struct SubMesh final {
      asset::Material material;
      size_t           iboOffset = 0;
      size_t           iboLength = 0;
      bool             alphaTest = true;
      bool             alphaTestRequired = false;
      TexelDensity     texel;  // its triangles' lowest UV density and bounds, for the texture's mip level
      };

    struct Cluster final {
      Tempest::Vec3 pos;
      float         r = 0;
      };

    std::vector<Vertex>   vertices;
    std::vector<VertexA>  verticesA;
    std::vector<uint32_t> indices;
    std::vector<uint8_t>  indices8;

    std::vector<SubMesh>  subMeshes;
    std::vector<Cluster>  meshletBounds;

    // Levels of detail of an own static mesh (graphics/meshlevels.h), coarser one after another: packed after the
    // mesh into the same buffers, so subMeshes, bbox() and everything before mainEnd are the mesh's own
    struct Level final {
      float                                px = 0;   // drawn while the object is under this many pixels high
      std::vector<std::pair<size_t,size_t>> sub;     // a slice of indices (offset, length) for each of subMeshes
      Tempest::Vec3                        bbox[2];  // of the level's own vertices
      };
    std::vector<Level>    levels;

    std::vector<uint32_t> verticesId; // only for morph meshes
    bool                  isUsingAlphaTest = true;
    float                 strandWidth      = 0;    // strands (groommesh.cpp): the mean width of a hair, cm; 0: not strands

    // colors: the vertex colour of each wedge of each sub-mesh (gltfmeshcolor.h); without them, and for a list that
    // is not of the sub-mesh's size, the vertices are drawn with ones
    PackedMesh(const asset::StaticMesh& mesh, PkgType type, bool explicitAlphaTest = false,
               const std::vector<std::vector<uint32_t>>* colors = nullptr);
    PackedMesh(const asset::WorldMesh& mesh, PkgType type);
    // Triangle IDs address the unchanged source polygon arrays. Materials and
    // vertex/feature IDs remain global to mesh, including unused materials.
    // Compact chunk sources may supply local-to-source vertex/feature IDs to
    // keep the heap's packing order identical to the unchanged source arrays.
    PackedMesh(const asset::WorldMesh& mesh, std::span<const uint32_t> triangles,
               std::span<const uint32_t> materialMap = {},
               std::span<const uint32_t> vertexIds = {}, std::span<const uint32_t> featureIds = {});
    PackedMesh(const asset::SoftSkinMesh& mesh);
    // Strands of a head as ribbons (graphics/mesh/groommesh.cpp): the file of groom_export.py; share<1 keeps that
    // share of the strands, wider. Throws std::runtime_error on a broken file.
    PackedMesh(const std::filesystem::path& groom, float share);

    static std::vector<uint32_t> landscapeMaterialMap(const asset::WorldMesh& mesh);
    // Bounds of the vertices the landscape polygons use, as bbox() of PK_VisualLnd
    // gives without packing; compiled world meshes store a zero bbox.
    static std::pair<Tempest::Vec3,Tempest::Vec3> landscapeBbox(const asset::WorldMesh& mesh);

    void debug(std::ostream &out) const;

    std::pair<Tempest::Vec3,Tempest::Vec3> bbox() const;

    // Packs `level` (a mesh with the materials of this one, its submeshes in the order of subMeshes) after the mesh
    // and the levels before it; colors: its COLOR_0 as for the constructor
    void addLevel(const asset::StaticMesh& level, float px, const std::vector<std::vector<uint32_t>>* colors = nullptr);
    // The mesh alone again: what collision and the hit zones are built from
    void dropLevels();

  private:
    Tempest::Vec3 mBbox[2];
    // where the mesh's own vertices, indices and 8-bit indices end once levels follow them
    size_t        mainEnd[3] = {};

    struct Prim {
      uint32_t primId = 0;
      uint32_t mat    = 0;
      };

    struct SkeletalData {
      Tempest::Vec3 localPositions[4] = {};
      uint8_t       boneIndices[4]    = {};
      float         weights[4]        = {};
      };

    // The rest rotation of a node a soft skin is bound to (node frame -> model), found from the skin itself
    struct SkinRest {
      float rot[3][3] = {};
      bool  valid     = false;
      };
    static std::vector<SkinRest> fitSkinRest(const asset::SoftSkinMesh& skinned);

    using  Vert = std::pair<uint32_t,uint32_t>;
    struct PrimitiveHeap;
    struct Meshlet {
      Vert          vert   [MaxVert] = {};
      uint8_t       indexes[MaxInd ] = {};
      uint8_t       vertSz           = 0;
      uint8_t       indSz            = 0;
      Cluster       bounds;

      void    flush(std::vector<Vertex>& vertices,
                    std::vector<uint32_t>& indices, std::vector<uint8_t>& indices8,
                    std::vector<Cluster>& instances, const asset::WorldMesh& mesh);

      void    flush(std::vector<Vertex>& vertices, std::vector<VertexA>& verticesA,
                    std::vector<uint32_t>& indices, std::vector<uint8_t>& indices8,
                    std::vector<uint32_t>* verticesId, const std::vector<asset::Vec3>& vbo,
                    const std::vector<asset::MeshWedge>& wedgeList,
                    const std::vector<SkeletalData>* skeletal,
                    const std::vector<SkinRest>* rest,
                    const std::vector<uint32_t>* colors = nullptr);
      bool    validate() const;

      bool    insert(const Vert& a, const Vert& b, const Vert& c);
      void    clear();
      void    updateBounds(const asset::WorldMesh& mesh);
      void    updateBounds(const asset::StaticMesh& mesh);
      void    updateBounds(const std::vector<asset::Vec3>& vbo);
      bool    canMerge(const Meshlet& other) const;
      bool    hasIntersection(const Meshlet& other) const;
      float   qDistance(const Meshlet& other) const;
      void    merge(const Meshlet& other);
      };

    bool   addTriangle(Meshlet& dest, const asset::WorldMesh* mesh, const asset::SubMesh* proto_mesh, size_t id);

    void   packPhysics(const asset::WorldMesh& mesh, PkgType type);

    //
    void   packMeshletsLnd(const asset::WorldMesh& mesh);
    void   packMeshletsLnd(const asset::WorldMesh& mesh, std::span<const uint32_t> triangles,
                          std::span<const uint32_t> materialMap,
                          std::span<const uint32_t> vertexIds = {}, std::span<const uint32_t> featureIds = {});
    void   packMeshletsObj(const asset::StaticMesh& mesh, PkgType type,
                           const std::vector<SkeletalData>* skeletal,
                           const std::vector<SkinRest>* rest = nullptr,
                           const std::vector<std::vector<uint32_t>>* colors = nullptr);

    std::vector<Meshlet> buildMeshlets(const asset::WorldMesh* mesh, const asset::SubMesh* proto_mesh,
                                       PrimitiveHeap& heap, std::vector<bool>& used,
                                       std::span<const uint32_t> vertexIds = {}, std::span<const uint32_t> featureIds = {});

    void   computeBbox();
    void   measureTexels();
    void   measureBindTexels(const asset::StaticMesh& mesh);

    void   dbgUtilization(const std::vector<Meshlet>& meshlets);
    void   dbgMeshlets(const asset::WorldMesh& mesh, const std::vector<Meshlet*>& meshlets);
  };

