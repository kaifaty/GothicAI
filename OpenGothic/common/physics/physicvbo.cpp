#include "physicvbo.h"

#include "graphics/mesh/submesh/packedmesh.h"
#include "collisionworld.h"

#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace {
  // Jolt quantizes each mesh over its complete referenced vertex bounds. Keep
  // the diagonal below 256 m so the vertex rounding error is below 0.0611 mm.
  constexpr float maxMeshDiagonal = 256.f;

  struct MeshTriangle {
    JPH::IndexedTriangle triangle;
    size_t               order;
    };

  using MeshTriangles = std::vector<MeshTriangle>;

  constexpr float maxTriangleEdge = 4.f;

  float longestEdgeSquared(const std::vector<JPH::Vec3>& vertices, const JPH::IndexedTriangle& triangle) {
    const auto* index=triangle.mIdx;
    return std::max({(vertices[index[1]]-vertices[index[0]]).LengthSq(),
                     (vertices[index[2]]-vertices[index[1]]).LengthSq(),
                     (vertices[index[0]]-vertices[index[2]]).LengthSq()});
    }

  void splitTriangles(std::vector<JPH::Vec3>& vertices, MeshTriangles& triangles) {
    std::unordered_map<uint64_t,uint32_t> midpoints;
    for(size_t i=0;i<triangles.size();) {
      const auto source=triangles[i];
      const auto* index=source.triangle.mIdx;
      size_t edge=0;
      float lengthSquared=0;
      for(size_t e=0;e<3;++e) {
        const float length=(vertices[index[(e+1)%3]]-vertices[index[e]]).LengthSq();
        if(length>lengthSquared) {lengthSquared=length;edge=e;}
        }
      if(lengthSquared<=maxTriangleEdge*maxTriangleEdge) {
        ++i;
        continue;
        }
      const auto a=index[edge],b=index[(edge+1)%3],c=index[(edge+2)%3];
      const uint64_t key=(uint64_t(std::min(a,b))<<32)|std::max(a,b);
      auto [entry,inserted]=midpoints.try_emplace(key,uint32_t(vertices.size()));
      if(inserted)
        vertices.push_back((vertices[a]+vertices[b])*.5f);
      const auto midpoint=entry->second;
      const auto data=source.triangle.mUserData;
      triangles[i]={JPH::IndexedTriangle(a,midpoint,c,0,data),source.order};
      triangles.push_back({JPH::IndexedTriangle(midpoint,b,c,0,data),source.order});
      }
    }

  // Compare cyclic rotations only: opposite triangle winding remains distinct.
  bool cyclicDuplicate(const std::vector<JPH::Vec3>& vertices, const std::vector<uint32_t>& indices,
                       uint32_t a, uint32_t b) {
    for(size_t rotation=0;rotation<3;++rotation) {
      bool equal = true;
      for(size_t v=0;v<3;++v)
        equal &= vertices[indices[3*size_t(a)+v]]==vertices[indices[3*size_t(b)+(v+rotation)%3]];
      if(equal)
        return true;
      }
    return false;
    }

  uint64_t triangleHash(const std::vector<JPH::Vec3>& vertices, const std::vector<uint32_t>& indices, uint32_t triangle) {
    std::array<uint32_t,9> bits;
    for(size_t v=0;v<3;++v) {
      const auto& point = vertices[indices[3*size_t(triangle)+v]];
      for(JPH::uint axis=0;axis<3;++axis) {
        const float value = point[axis];
        bits[3*v+axis] = std::bit_cast<uint32_t>(value==0.f ? 0.f : value);
        }
      }
    auto canonical = bits;
    for(size_t rotation=1;rotation<3;++rotation) {
      std::array<uint32_t,9> rotated;
      for(size_t component=0;component<9;++component)
        rotated[component] = bits[(component+3*rotation)%9];
      if(rotated<canonical)
        canonical = rotated;
      }
    uint64_t hash = 14695981039346656037ULL;
    for(auto component:canonical) {
      hash ^= component;
      hash *= 1099511628211ULL;
      }
    return hash;
    }

  // Keep the scalar operation order used by the original triangle ray query.
  // Cyclic copies of a plane can otherwise disagree by a few float ULPs and
  // expose a different material, even though their geometry is identical.
  JPH::Vec3 scalarCross(JPH::Vec3Arg a, JPH::Vec3Arg b) {
    return JPH::Vec3(a.GetY()*b.GetZ()-a.GetZ()*b.GetY(),
                     a.GetZ()*b.GetX()-a.GetX()*b.GetZ(),
                     a.GetX()*b.GetY()-a.GetY()*b.GetX());
    }

  float scalarDot(JPH::Vec3Arg a, JPH::Vec3Arg b) {
    return (a.GetX()*b.GetX()+a.GetY()*b.GetY())+a.GetZ()*b.GetZ();
    }

  float rayPlaneFraction(const std::vector<JPH::Vec3>& vertices, const std::vector<uint32_t>& indices,
                         uint32_t triangle, JPH::Vec3Arg from, JPH::Vec3Arg to, bool backfaces) {
    const auto v0 = vertices[indices[3*size_t(triangle)]];
    const auto v1 = vertices[indices[3*size_t(triangle)+1]];
    const auto v2 = vertices[indices[3*size_t(triangle)+2]];
    const auto normal = scalarCross(v1-v0,v2-v0);
    const float plane = scalarDot(v0,normal);
    const float a = scalarDot(normal,from)-plane;
    const float b = scalarDot(normal,to)-plane;
    if(a*b>=0.f || (!backfaces && a<=0.f))
      return 1.f;
    const float fraction = a/(a-b);
    if(!std::isfinite(fraction) || fraction<=0.f || fraction>=1.f)
      return 1.f;
    const auto point = from*(1.f-fraction)+to*fraction;
    const float tolerance = scalarDot(normal,normal)*-0.0001f;
    if(scalarDot(scalarCross(v0-point,v1-point),normal)<tolerance ||
       scalarDot(scalarCross(v1-point,v2-point),normal)<tolerance ||
       scalarDot(scalarCross(v2-point,v0-point),normal)<tolerance)
      return 1.f;
    return fraction;
    }

  JPH::AABox meshBounds(const std::vector<JPH::Vec3>& vertices, const MeshTriangles& triangles,
                       size_t first, size_t last) {
    JPH::AABox bounds;
    for(size_t i=first;i<last;++i)
      for(auto index:triangles[i].triangle.mIdx)
        bounds.Encapsulate(vertices[index]);
    return bounds;
    }

  JPH::ShapeRefC createMesh(const std::vector<JPH::Vec3>& vertices, MeshTriangles& triangles,
                           size_t first, size_t last, JPH::Vec3Arg origin) {
    // Retain the original triangle order inside each child. The segment remains
    // independent of child indices and Jolt's internal triangle reordering.
    std::sort(triangles.begin()+MeshTriangles::difference_type(first),
              triangles.begin()+MeshTriangles::difference_type(last),[](const auto& a, const auto& b) {
      return a.order<b.order;
      });

    JPH::VertexList localVertices;
    JPH::IndexedTriangleList localTriangles;
    std::unordered_map<uint32_t,uint32_t> vertexMap;
    localTriangles.reserve(last-first);
    vertexMap.reserve(std::min(vertices.size(),3*(last-first)));
    for(size_t i=first;i<last;++i) {
      const auto& triangle = triangles[i].triangle;
      uint32_t indices[3];
      for(size_t v=0;v<3;++v) {
        const auto index = triangle.mIdx[v];
        auto [entry,inserted] = vertexMap.emplace(index,uint32_t(localVertices.size()));
        if(inserted) {
          const auto point = vertices[index]-origin;
          localVertices.emplace_back(point.GetX(),point.GetY(),point.GetZ());
          }
        indices[v] = entry->second;
        }
      localTriangles.emplace_back(indices[0],indices[1],indices[2],0,triangle.mUserData);
      }

    JPH::MeshShapeSettings settings(std::move(localVertices),std::move(localTriangles));
    settings.mPerTriangleUserData = true;
    if(settings.mIndexedTriangles.empty())
      return nullptr;
    const auto result = settings.Create();
    if(result.HasError())
      throw std::runtime_error("Unable to build physics mesh: " + std::string(result.GetError().c_str()));
    return result.Get();
    }

  void addMeshChunks(const std::vector<JPH::Vec3>& vertices, MeshTriangles& triangles,
                     size_t first, size_t last, JPH::StaticCompoundShapeSettings& compound) {
    const auto bounds = meshBounds(vertices,triangles,first,last);
    const auto extent = bounds.GetSize();
    if(extent.LengthSq()<=maxMeshDiagonal*maxMeshDiagonal || last-first==1) {
      // Integer translations do not lose mantissa bits when Jolt transforms a
      // query into this child's local space. Never split or duplicate a triangle,
      // including an individual triangle larger than the desired chunk bounds.
      const auto center = bounds.GetCenter();
      const auto origin = JPH::Vec3(std::floor(center.GetX()),std::floor(center.GetY()),std::floor(center.GetZ()));
      if(auto shape = createMesh(vertices,triangles,first,last,origin))
        compound.AddShape(origin,JPH::Quat::sIdentity(),shape.GetPtr());
      return;
      }

    const JPH::uint axis = extent.GetX()>=extent.GetY() && extent.GetX()>=extent.GetZ() ? 0u :
                           (extent.GetY()>=extent.GetZ() ? 1u : 2u);
    const size_t mid = first+(last-first)/2;
    auto centroid = [&](const MeshTriangle& triangle) {
      const auto* index = triangle.triangle.mIdx;
      return (vertices[index[0]][axis]+vertices[index[1]][axis]+vertices[index[2]][axis])/3.f;
      };
    std::nth_element(triangles.begin()+MeshTriangles::difference_type(first),
                     triangles.begin()+MeshTriangles::difference_type(mid),
                     triangles.begin()+MeshTriangles::difference_type(last),
                     [&](const MeshTriangle& a, const MeshTriangle& b) {
      const float ca = centroid(a);
      const float cb = centroid(b);
      return ca<cb || (ca==cb && a.order<b.order);
      });
    addMeshChunks(vertices,triangles,first,mid,compound);
    addMeshChunks(vertices,triangles,mid,last,compound);
    }
  }

PhysicVbo::PhysicVbo(PackedMesh&& packed)
  :PhysicVbo(packed.vertices) {
  id = std::move(packed.indices);
  for(auto& i:packed.subMeshes)
    if(!i.material.disable_collision && i.iboLength>0) {
      addSegment(i.iboLength,i.iboOffset,i.material.group,nullptr);
      }
  for(size_t i=0;i<id.size();i+=3){
    std::swap(id[i+1],id[i+2]);
    }
  }

void PhysicVbo::appendVertices(std::vector<JPH::Vec3>& points, const Tempest::Matrix4x4& transform) const {
  std::vector<bool> used(vert.size(),false);
  for(const auto& segment:segments)
    for(size_t i=segment.off;i<segment.off+3*segment.size;++i) {
      const auto index = id[i];
      if(index>=vert.size() || used[index])
        continue;
      used[index] = true;
      auto point = CollisionWorld::toCentimeters(vert[index]);
      transform.project(point);
      points.push_back(CollisionWorld::toMeters(point));
      }
  }

PhysicVbo::PhysicVbo(const std::vector<Vertex>& v)
  :vStorage(v.size()), vert(vStorage) {
  for(size_t i=0;i<v.size();++i){
    vStorage[i] = CollisionWorld::toMeters(Tempest::Vec3(v[i].pos[0],v[i].pos[1],v[i].pos[2]));
    }
  }

PhysicVbo::PhysicVbo(const std::vector<JPH::Vec3>* v)
  :vert(*v) {
  }

void PhysicVbo::addIndex(const std::vector<uint32_t>& index, size_t iboOff, size_t iboLen, asset::MaterialGroup material) {
  addIndex(index,iboOff,iboLen,material,nullptr);
  }

void PhysicVbo::addIndex(const std::vector<uint32_t>& index, size_t iboOff, size_t iboLen, asset::MaterialGroup material,
                         const char* sector) {
  if(iboLen==0)
    return;

  size_t off = id.size();

  id.resize(off+iboLen);
  for(size_t i=0; i<iboLen; i+=3){
    id[off+i+0] = index[iboOff+i+0];
    id[off+i+1] = index[iboOff+i+2];
    id[off+i+2] = index[iboOff+i+1];
    }

  addSegment(iboLen,off,material,sector);
  }

void PhysicVbo::addSegment(size_t indexSize, size_t offset, asset::MaterialGroup material, const char* sector) {
  Segment sgm;
  sgm.off    = offset;
  sgm.size   = indexSize/3;
  sgm.mat    = material;
  sgm.sector = sector;
  segments.push_back(sgm);
  triangleDataReady = false;
  triangleSegments.clear();
  duplicateGroup.clear();
  rayDuplicates.clear();
  meshShape = nullptr;
  splitMeshShape = nullptr;
  }

void PhysicVbo::prepareTriangleData() const {
  if(triangleDataReady)
    return;
  triangleSegments.assign(id.size()/3,std::numeric_limits<uint32_t>::max());
  duplicateGroup.clear();
  rayDuplicates.clear();

  size_t triangleCount = 0;
  for(auto& segment:segments)
    triangleCount += segment.size;
  // Retain only an original triangle ID in the temporary hash table. Hash
  // collisions are checked against the original vertices, never quantized data.
  std::unordered_multimap<uint64_t,uint32_t> firstTriangles;
  firstTriangles.reserve(triangleCount);
  for(size_t s=0;s<segments.size();++s) {
    const auto& segment = segments[s];
    for(size_t t=0;t<segment.size;++t) {
      const auto triangle = uint32_t(segment.off/3+t);
      triangleSegments[triangle] = uint32_t(s);
      const auto hash = triangleHash(vert,id,triangle);
      const auto [first,last] = firstTriangles.equal_range(hash);
      auto duplicate = last;
      for(auto i=first;i!=last;++i)
        if(cyclicDuplicate(vert,id,triangle,i->second)) {
          duplicate = i;
          break;
          }
      if(duplicate==last) {
        firstTriangles.emplace(hash,triangle);
        continue;
        }
      const auto original = duplicate->second;
      auto [group,inserted] = duplicateGroup.emplace(original,uint32_t(rayDuplicates.size()));
      if(inserted)
        rayDuplicates.push_back({original});
      rayDuplicates[group->second].push_back(triangle);
      duplicateGroup.emplace(triangle,group->second);
      }
    }
  triangleDataReady = true;
  }

asset::MaterialGroup PhysicVbo::materialId(size_t triangle) const {
  prepareTriangleData();
  if(triangle<triangleSegments.size() && triangleSegments[triangle]<segments.size())
    return segments[triangleSegments[triangle]].mat;
  return asset::MaterialGroup::UNDEFINED;
  }

const char* PhysicVbo::sectorName(size_t triangle) const {
  prepareTriangleData();
  if(triangle<triangleSegments.size() && triangleSegments[triangle]<segments.size())
    return segments[triangleSegments[triangle]].sector;
  return nullptr;
  }

bool PhysicVbo::triangleVertices(uint32_t triangle, JPH::Vec3 (&vertices)[3]) const {
  prepareTriangleData();
  if(triangle>=triangleSegments.size() || triangleSegments[triangle]>=segments.size())
    return false;
  const auto* index = id.data()+3*size_t(triangle);
  for(size_t i=0;i<3;++i)
    if(index[i]>=vert.size())
      return false;
  for(size_t i=0;i<3;++i)
    vertices[i] = vert[index[i]];
  return true;
  }

float PhysicVbo::rayTriangleFraction(uint32_t triangle, JPH::Vec3Arg from, JPH::Vec3Arg to, bool backfaces) const {
  JPH::Vec3 vertices[3];
  if(!triangleVertices(triangle,vertices))
    return 1.f;
  return rayPlaneFraction(vert,id,triangle,from,to,backfaces);
  }

bool PhysicVbo::rayTriangleNormal(uint32_t triangle, JPH::Vec3& normal) const {
  JPH::Vec3 vertices[3];
  if(!triangleVertices(triangle,vertices))
    return false;
  for(const auto& vertex:vertices)
    for(JPH::uint axis=0;axis<3;++axis)
      if(!std::isfinite(vertex[axis]))
        return false;

  const auto cross = scalarCross(vertices[1]-vertices[0],vertices[2]-vertices[0]);
  const float lengthSq = scalarDot(cross,cross);
  if(!std::isfinite(lengthSq) || lengthSq<=0.f)
    return false;
  // Preserve the scalar source plane and reciprocal-multiply normalization.
  // Component division changes otherwise identical float normals by one ULP.
  const float inverseLength = 1.f/std::sqrt(lengthSq);
  const auto unit = cross*inverseLength;
  for(JPH::uint axis=0;axis<3;++axis)
    if(!std::isfinite(unit[axis]))
      return false;
  normal = unit;
  return true;
  }

bool PhysicVbo::isRayEdge(uint32_t triangle, JPH::Vec3Arg point, float tolerance) const {
  JPH::Vec3 vertices[3];
  if(!triangleVertices(triangle,vertices))
    return false;
  double edge0[3], edge1[3], relative[3];
  for(JPH::uint axis=0;axis<3;++axis) {
    edge0[axis] = double(vertices[1][axis])-double(vertices[0][axis]);
    edge1[axis] = double(vertices[2][axis])-double(vertices[0][axis]);
    relative[axis] = double(point[axis])-double(vertices[0][axis]);
    }
  auto dot = [](const double (&a)[3], const double (&b)[3]) {
    return (a[0]*b[0]+a[1]*b[1])+a[2]*b[2];
    };
  const double normal[3] = {edge0[1]*edge1[2]-edge0[2]*edge1[1],
                            edge0[2]*edge1[0]-edge0[0]*edge1[2],
                            edge0[0]*edge1[1]-edge0[1]*edge1[0]};
  // The squared cross product avoids cancellation in the Gram determinant
  // for very thin triangles. Dot products project the point onto the plane.
  const double determinant = dot(normal,normal);
  if(!std::isfinite(determinant) || determinant<=0.)
    return false;
  const double area0[3] = {relative[1]*edge1[2]-relative[2]*edge1[1],
                           relative[2]*edge1[0]-relative[0]*edge1[2],
                           relative[0]*edge1[1]-relative[1]*edge1[0]};
  const double area1[3] = {edge0[1]*relative[2]-edge0[2]*relative[1],
                           edge0[2]*relative[0]-edge0[0]*relative[2],
                           edge0[0]*relative[1]-edge0[1]*relative[0]};
  const double u = dot(area0,normal)/determinant;
  const double v = dot(area1,normal)/determinant;
  const double w = 1.-u-v;
  if(!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(w))
    return false;
  // The input is already bounded to the native compressed face. A point just
  // outside its original face is also an edge ambiguity caused by compression.
  return std::min({u,v,w})<=double(tolerance);
  }

bool PhysicVbo::hasRayDuplicates(uint32_t triangle) const {
  prepareTriangleData();
  return duplicateGroup.find(triangle)!=duplicateGroup.end();
  }

uint32_t PhysicVbo::resolveRayTriangle(uint32_t triangle, JPH::Vec3Arg from, JPH::Vec3Arg to, bool backfaces) const {
  prepareTriangleData();
  const auto group = duplicateGroup.find(triangle);
  if(group==duplicateGroup.end())
    return triangle;
  float closest = 1.f;
  auto result = triangle;
  for(auto candidate:rayDuplicates[group->second]) {
    const float fraction = rayPlaneFraction(vert,id,candidate,from,to,backfaces);
    if(fraction<closest) {
      closest = fraction;
      result = candidate;
      }
    }
  return result;
  }

bool PhysicVbo::isEmpty() const {
  return segments.empty();
  }

JPH::ShapeRefC PhysicVbo::shape(bool splitLargeTriangles) const {
  auto& cached=splitLargeTriangles ? splitMeshShape : meshShape;
  if(cached!=nullptr || isEmpty())
    return cached;

  Physics::initialize();
  prepareTriangleData();

  size_t triangleCount = 0;
  for(auto& segment:segments)
    triangleCount += segment.size;

  MeshTriangles triangles;
  triangles.reserve(triangleCount);
  bool hasLongTriangles=false;
  for(size_t s=0;s<segments.size();++s) {
    auto& segment = segments[s];
    for(size_t t=0;t<segment.size;++t) {
      const size_t off = segment.off + 3*t;
      // Retain the source triangle ID across Jolt's tree reordering. The ID
      // resolves to its material and sector and to any exact cyclic copies.
      triangles.push_back({JPH::IndexedTriangle(id[off],id[off+1],id[off+2],0,uint32_t(off/3)),triangles.size()});
      hasLongTriangles=hasLongTriangles || longestEdgeSquared(vert,triangles.back().triangle)>maxTriangleEdge*maxTriangleEdge;
      }
    }

  std::vector<JPH::Vec3> splitVertices;
  const auto* vertices=&vert;
  if(splitLargeTriangles && hasLongTriangles) {
    // Subdivide only the native mesh. Source vertices, planes, material and
    // sector IDs remain exact for gameplay rays and legacy movement queries.
    splitVertices=vert;
    splitTriangles(splitVertices,triangles);
    vertices=&splitVertices;
    }

  if(meshBounds(*vertices,triangles,0,triangles.size()).GetSize().LengthSq()<=maxMeshDiagonal*maxMeshDiagonal) {
    cached=createMesh(*vertices,triangles,0,triangles.size(),JPH::Vec3::sZero());
    }
  else {
    JPH::StaticCompoundShapeSettings compound;
    addMeshChunks(*vertices,triangles,0,triangles.size(),compound);
    if(compound.mSubShapes.empty())
      return nullptr;
    const auto result=compound.Create();
    if(result.HasError())
      throw std::runtime_error("Unable to build compound physics mesh: " + std::string(result.GetError().c_str()));
    cached=result.Get();
    }
  if(!hasLongTriangles)
    meshShape=splitMeshShape=cached;
  return cached;
  }

std::string_view PhysicVbo::validateSectorName(std::string_view name) const {
  if(name.empty())
    return "";
  for(auto& i:segments)
    if(i.sector!=nullptr && name==i.sector)
      return i.sector;
  return "";
  }
