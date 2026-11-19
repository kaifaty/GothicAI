#pragma once

#include <Tempest/VertexBuffer>
#include <Tempest/IndexBuffer>
#include <Tempest/Device>
#include <Tempest/AccelerationStructure>

#include <array>
#include <vector>
#include <memory>

#include "graphics/material.h"
#include "graphics/bounds.h"
#include "graphics/mesh/texeldensity.h"
#include "graphics/mesh/submesh/packedmesh.h"

#include "resources.h"

class LandscapeRange;
namespace MeshLevels { struct Selection; }

// Pages keep their buffers and addresses while individual chunk ranges recycle.
class LandscapePages final {
  public:
    struct Stats {
      size_t occupiedBytes = 0;
      size_t freeBytes     = 0;
      size_t retiredBytes  = 0;
      size_t pageCount     = 0;
      };

    LandscapePages();
    ~LandscapePages();
    LandscapePages(const LandscapePages&) = delete;
    LandscapePages& operator=(const LandscapePages&) = delete;

    // Occupied excludes retired ranges; all three byte counts sum to capacity.
    Stats stats() const;

  private:
    struct State;
    std::shared_ptr<State> state;
    std::shared_ptr<LandscapeRange> allocate(const PackedMesh& mesh);

    friend class StaticMesh;
    friend class LandscapeRange;
  };

class StaticMesh {
  public:
    using Vertex=Resources::Vertex;

    StaticMesh(const PackedMesh& data);
    StaticMesh(const PackedMesh& data, LandscapePages& pages);
    StaticMesh(const Material& mat, std::vector<Resources::Vertex> vbo, std::vector<uint32_t> ibo);
    StaticMesh(StaticMesh&&)=default;
    StaticMesh& operator=(StaticMesh&&);
    ~StaticMesh();

    struct SubMesh {
      Material                       material;
      size_t                         iboOffset = 0;
      size_t                         iboLength = 0;
      std::string                    texName;
      Tempest::AccelerationStructure blas;
      TexelDensity                   texel;  // lowest UV density and bounds in mesh space (texeldensity.h)
      // the triangles blas is built from: the submesh's own, or the coarsest level's (meshlevels.h)
      size_t                         rtOffset = 0;
      size_t                         rtLength = 0;
      };

    // A level of detail (graphics/meshlevels.h, PackedMesh::Level): a slice of indices of each of sub
    struct Level {
      float                                 px = 0;
      std::vector<std::pair<size_t,size_t>> sub;
      };

    struct Impostor {
      float                              px = 0;
      std::array<Tempest::Texture2d,3>     textures; // albedo/coverage, normal/depth, ORM
      };

    struct Morph {
      std::string            name;
      size_t                 numFrames       = 0;
      size_t                 samplesPerFrame = 0;
      int32_t                layer           = 0;
      uint64_t               tickPerFrame    = 50;
      uint64_t               duration        = 0;
      float                  blendIn         = 0;  // weight per ms as it comes and goes (faces: a second), 0: at once
      float                  blendOut        = 0;

      size_t                 index           = 0;
      std::vector<float>     strength;  // per frame: how far it moves the vertices, the strongest frame is 1
      float                  peak            = 0;  // the farthest any vertex goes, in the mesh's units
      Tempest::Vec3          peakAt          = {}; // that vertex at rest (mesh space)...
      Tempest::Vec3          peakMove        = {}; // ...and how it moves at the strongest frame
      Tempest::Vec3          moveMin         = {}; // component bounds of every sample, including rest;
      Tempest::Vec3          moveMax         = {}; // conservative bounds for posed attachment diagnostics
      };

    struct MorphAnim {
      const std::vector<Morph>*     anim    = nullptr;
      const Tempest::StorageBuffer* index   = nullptr;
      const Tempest::StorageBuffer* samples = nullptr;
      };

    const Tempest::AccelerationStructure* blas(size_t iboOffset, size_t iboLen) const;
    // The slice of the triangles blas() of the submesh at this slice is built from (SubMesh::rtOffset, rtLength)
    std::pair<size_t,size_t>              rtSlice(size_t iboOffset, size_t iboLen) const;
    // The meshlets (first, count) the submesh at this slice draws at a level of detail: 0 or one the mesh has not, its own
    std::pair<uint32_t,uint32_t>          levelMeshlets(size_t iboOffset, size_t iboLen, size_t level) const;
    MeshLevels::Selection                pickLevel(uint8_t previous, float height, bool allowImpostor) const;

    const Tempest::StorageBuffer& vboBuffer() const;
    const Tempest::StorageBuffer& ibo8Buffer() const;
    size_t firstMeshlet() const;

    // What rays read the mesh from (RtScene, rt_common.glsl): the vertices of vboBuffer() and 32-bit indices of them,
    // its own or, in landscape pages with the ray scene, the page's, which count from the page's first vertex
    const Tempest::StorageBuffer& rtIbo() const;
    size_t                        rtFirstIndex(size_t iboOffset) const;
    // A submesh's triangles for a BLAS, from the same buffers
    Tempest::RtGeometry           rtGeometry(size_t iboOffset, size_t iboLength) const;

    Tempest::VertexBuffer<Vertex>   vbo;
    Tempest::IndexBuffer<uint32_t>  ibo;
    Tempest::StorageBuffer          ibo8;
    MorphAnim                       morph;

    std::vector<SubMesh>            sub;
    Bounds                          bbox;
    // bbox with every level of detail in it: what the culling keeps (the clusters' spheres, the buckets' boxes);
    // bbox itself is the mesh's, as a tree's height for the wind is. The same as bbox without levels
    Bounds                          drawn;
    std::vector<Level>              levels;
    Impostor                        impostor;
    float                           strandWidth = 0; // PackedMesh::strandWidth
    // the sphere of each meshlet in the mesh's space (centre, radius), levels included: the shadow pages take an
    // object's meshlets by them (graphics/meshletspheres.h); empty for the landscape and meshes without them
    std::vector<Tempest::Vec4>      meshletSpheres;
    std::vector<PackedMesh::ClusterLevel> clusterLevels;

  private:
    void initSubMeshes(const PackedMesh& mesh);
    void retireRange();
    std::shared_ptr<LandscapeRange> pageRange;
  };
