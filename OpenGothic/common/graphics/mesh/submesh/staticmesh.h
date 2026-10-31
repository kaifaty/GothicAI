#pragma once

#include <Tempest/VertexBuffer>
#include <Tempest/IndexBuffer>
#include <Tempest/Device>

#include <vector>
#include <memory>

#include "graphics/material.h"
#include "graphics/bounds.h"

#include "resources.h"

class PackedMesh;
class LandscapeRange;

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

    const Tempest::StorageBuffer& vboBuffer() const;
    const Tempest::StorageBuffer& ibo8Buffer() const;
    size_t firstMeshlet() const;

    Tempest::VertexBuffer<Vertex>   vbo;
    Tempest::IndexBuffer<uint32_t>  ibo;
    Tempest::StorageBuffer          ibo8;
    MorphAnim                       morph;

    std::vector<SubMesh>            sub;
    Bounds                          bbox;

  private:
    void initSubMeshes(const PackedMesh& mesh);
    void retireRange();
    std::shared_ptr<LandscapeRange> pageRange;
  };
