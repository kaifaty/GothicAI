#pragma once

#include <vector>
#include <cstdint>
#include <unordered_map>
#include <Tempest/Matrix4x4>

#include "asset/material.h"

#include "physics/physics.h"
#include "resources.h"

class PackedMesh;

class PhysicVbo final {
  public:
    using Vertex = Resources::Vertex;

    PhysicVbo(PackedMesh&& packed);
    PhysicVbo(const std::vector<JPH::Vec3>* v);

    PhysicVbo(const PhysicVbo&)=delete;
    PhysicVbo(PhysicVbo&&)=delete;

    void                    addIndex(const std::vector<uint32_t>& index, size_t iboOff, size_t iboLen, asset::MaterialGroup material);
    void                    addIndex(const std::vector<uint32_t>& index, size_t iboOff, size_t iboLen, asset::MaterialGroup material, const char* sector);
    asset::MaterialGroup   materialId(size_t triangle) const;
    auto                    sectorName(size_t triangle) const -> const char*;
    bool                    triangleVertices(uint32_t triangle, JPH::Vec3 (&vertices)[3]) const;
    float                   rayTriangleFraction(uint32_t triangle, JPH::Vec3Arg from, JPH::Vec3Arg to,
                                               bool backfaces=false) const;
    bool                    rayTriangleNormal(uint32_t triangle, JPH::Vec3& normal) const;
    bool                    isRayEdge(uint32_t triangle, JPH::Vec3Arg point, float tolerance) const;
    bool                    hasRayDuplicates(uint32_t triangle) const;
    uint32_t                resolveRayTriangle(uint32_t triangle, JPH::Vec3Arg from, JPH::Vec3Arg to,
                                              bool backfaces=false) const;
    bool                    isEmpty() const;
    JPH::ShapeRefC          shape(bool splitLargeTriangles = false) const;
    // Exact source geometry, before Jolt's triangle BVH quantization.
    void                    appendVertices(std::vector<JPH::Vec3>& points, const Tempest::Matrix4x4& transform) const;
    size_t                  vertexCount() const { return vert.size(); }
    size_t                  triangleCount() const { return id.size()/3; }

    std::string_view        validateSectorName(std::string_view name) const;

  private:
    friend class DynamicWorld;
    friend struct BaseContentContract;
    friend struct AssetDigest;
    PhysicVbo(const std::vector<Vertex>& v);

    void addSegment(size_t indexSize,size_t offset, asset::MaterialGroup material, const char* sector);
    void prepareTriangleData() const;

    struct Segment {
      size_t                off;
      size_t                size;
      asset::MaterialGroup mat;
      const char*           sector=nullptr;
      };

    std::vector<JPH::Vec3>        vStorage;
    const std::vector<JPH::Vec3>&  vert;
    std::vector<uint32_t>         id;
    std::vector<Segment>          segments;
    mutable bool                 triangleDataReady=false;
    mutable std::vector<uint32_t> triangleSegments;
    mutable std::unordered_map<uint32_t,uint32_t> duplicateGroup;
    mutable std::vector<std::vector<uint32_t>> rayDuplicates;
    mutable JPH::ShapeRefC        meshShape;
    mutable JPH::ShapeRefC        splitMeshShape;
  };
