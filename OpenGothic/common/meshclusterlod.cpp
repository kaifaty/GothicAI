#include "meshclusterlod.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <type_traits>

#include <miniz.h>

#include "worldchunkcook.h"

namespace MeshClusterLod {

namespace {

using ClusterLod::Cluster;
using ClusterLod::Group;
using ClusterLod::NoGroup;
using ClusterLod::Sphere;
using ClusterLod::Vertex;

constexpr size_t SectionCount = 5;
struct Header {
  std::array<char,4>                  magic = {'G','C','L','D'};
  uint32_t                           version = 2;
  // Bump the relevant revision when GLB decoding or graph construction changes the cooked output.
  // The content set validator in scripts/content/building_set.py checks the same revisions.
  uint32_t                           loaderVersion = 1;
  uint32_t                           builderVersion = 1;
  uint32_t                           glbSize = 0;
  uint32_t                           glbCrc32 = 0;
  uint32_t                           payloadCrc32 = 0;
  std::array<uint32_t,SectionCount>   counts = {};
  };

static_assert(std::endian::native==std::endian::little);
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(Header)==48 && sizeof(Vertex)==36 && sizeof(Cluster)==48 && sizeof(Group)==20);
constexpr size_t Strides[SectionCount] = {sizeof(Vertex),sizeof(uint32_t),sizeof(uint8_t),sizeof(Cluster),sizeof(Group)};

std::filesystem::path pathFor(const std::filesystem::path& glb) {
  auto path = glb;
  return path.replace_extension(".clod");
  }

uint32_t narrow(size_t size) {
  if(size>std::numeric_limits<uint32_t>::max())
    throw std::runtime_error("mesh cluster LOD: array or GLB exceeds 32-bit size");
  return uint32_t(size);
  }

template<class T>
void put(uint8_t*& out, const std::vector<T>& data) {
  static_assert(std::is_trivially_copyable_v<T>);
  if(data.empty())
    return;
  const size_t size = data.size()*sizeof(T);
  std::memcpy(out,data.data(),size);
  out += size;
  }

template<class T>
void get(const uint8_t*& in, std::vector<T>& data, size_t count) {
  static_assert(std::is_trivially_copyable_v<T>);
  if(count==0)
    return;
  data.resize(count);
  const size_t size = count*sizeof(T);
  std::memcpy(data.data(),in,size);
  in += size;
  }

bool finite(std::span<const float> values) {
  return std::all_of(values.begin(),values.end(),[](float value) { return std::isfinite(value); });
  }

bool valid(const Sphere& sphere) {
  return finite(sphere.center) && std::isfinite(sphere.radius) && sphere.radius>=0;
  }

bool valid(const Graph& graph, const asset::StaticMesh& mesh, std::span<const uint8_t> twoSided) {
  if(twoSided.size()!=mesh.sub_meshes.size())
    return false;
  for(const auto& v:graph.vertices)
    if(!finite(v.position) || !finite(v.normal) || !finite(v.uv))
      return false;
  for(auto i:graph.vertexIndices)
    if(i>=graph.vertices.size())
      return false;
  for(const auto& group:graph.groups)
    if(!valid(group.bounds) || !(group.errorCm>=0))
      return false;

  std::vector<size_t> sourceTriangles(mesh.sub_meshes.size());
  for(const auto& c:graph.clusters) {
    if(!valid(c.bounds) || c.material>=mesh.sub_meshes.size() || c.parentGroup>=graph.groups.size() ||
       c.vertexCount==0 || c.vertexCount>ClusterLod::MaxVertices ||
       c.triangleCount==0 || c.triangleCount>ClusterLod::MaxTriangles ||
       size_t(c.vertexOffset)+c.vertexCount>graph.vertexIndices.size() ||
       size_t(c.indexOffset)+c.triangleCount*3>graph.indices.size())
      return false;
    for(uint32_t i=0; i<c.triangleCount*3; ++i)
      if(graph.indices[c.indexOffset+i]>=c.vertexCount)
        return false;

    const auto& parent = graph.groups[c.parentGroup];
    if(locked(mesh.sub_meshes[c.material].mat,twoSided[c.material]!=0) && (c.level!=0 || !std::isinf(parent.errorCm)))
      return false;
    if(c.level==0) {
      if(c.refinedGroup!=NoGroup)
        return false;
      sourceTriangles[c.material] += c.triangleCount;
      } else {
      if(c.refinedGroup>=c.parentGroup || !std::isfinite(graph.groups[c.refinedGroup].errorCm) ||
         graph.groups[c.refinedGroup].errorCm>parent.errorCm)
        return false;
      }
    }
  for(size_t i=0; i<sourceTriangles.size(); ++i)
    if(sourceTriangles[i]!=mesh.sub_meshes[i].triangles.size())
      return false;
  return true;
  }

}

bool alphaTest(const asset::Material& material) {
  return material.alpha_func==asset::AlphaFunction::DEFAULT;
  }

bool locked(const asset::Material& material, bool twoSided) {
  return twoSided || material.alpha_func!=asset::AlphaFunction::NONE || material.wave_mode!=asset::WaveMode::NONE;
  }

Graph build(const asset::StaticMesh& mesh, const GltfMeshColors& colors, std::span<const uint8_t> twoSided) {
  if(twoSided.size()!=mesh.sub_meshes.size() || (!colors.empty() && colors.size()!=mesh.sub_meshes.size()))
    throw std::invalid_argument("mesh cluster LOD: expected material flags and optional colors for each submesh");
  std::vector<Vertex> vertices;
  std::vector<ClusterLod::Triangle> triangles;
  for(size_t i=0; i<mesh.sub_meshes.size(); ++i) {
    const auto& sub = mesh.sub_meshes[i];
    const uint32_t start = narrow(vertices.size());
    const bool isLocked = locked(sub.mat,twoSided[i]!=0);
    if(!colors.empty() && colors[i].size()!=sub.wedges.size())
      throw std::invalid_argument("mesh cluster LOD: expected one color per wedge");
    for(size_t w=0; w<sub.wedges.size(); ++w) {
      const auto& wedge = sub.wedges[w];
      const auto& p = mesh.positions.at(wedge.index);
      const auto& n = wedge.normal;
      const auto& uv = wedge.texture;
      vertices.push_back({{p.x,p.y,p.z},{n.x,n.y,n.z},{uv.x,uv.y},colors.empty() ? 0xffffffffu : colors[i][w]});
      }
    for(const auto& t:sub.triangles)
      triangles.push_back({{start+t.wedges[0],start+t.wedges[1],start+t.wedges[2]},narrow(i),isLocked});
    }
  return ClusterLod::build(vertices,triangles,std::vector<uint8_t>(vertices.size()));
  }

void write(const std::filesystem::path& glb, size_t glbSize, uint32_t glbCrc32, const Graph& graph) {
  Header header;
  header.glbSize  = narrow(glbSize);
  header.glbCrc32 = glbCrc32;
  header.counts   = {narrow(graph.vertices.size()),narrow(graph.vertexIndices.size()),narrow(graph.indices.size()),
                     narrow(graph.clusters.size()),narrow(graph.groups.size())};
  std::vector<uint8_t> bytes(sizeof(Header)+graph.byteSize());
  auto* p = bytes.data()+sizeof(Header);
  put(p,graph.vertices);
  put(p,graph.vertexIndices);
  put(p,graph.indices);
  put(p,graph.clusters);
  put(p,graph.groups);
  header.payloadCrc32 = uint32_t(mz_crc32(0,bytes.data()+sizeof(Header),graph.byteSize()));
  std::memcpy(bytes.data(),&header,sizeof(Header));
  writeCookedFile(pathFor(glb),bytes);
  }

std::optional<Graph> read(const std::filesystem::path& glb, size_t glbSize, uint32_t glbCrc32,
                          const asset::StaticMesh& mesh, std::span<const uint8_t> twoSided) {
  std::ifstream file(pathFor(glb),std::ios::binary|std::ios::ate);
  if(!file)
    return std::nullopt;
  const auto size = file.tellg();
  if(size<std::streamoff(sizeof(Header)) || !file.seekg(0))
    return std::nullopt;
  Header h;
  const Header expected;
  if(!file.read(reinterpret_cast<char*>(&h),sizeof(h)) || h.magic!=expected.magic || h.version!=expected.version ||
     h.loaderVersion!=expected.loaderVersion || h.builderVersion!=expected.builderVersion ||
     h.glbSize!=glbSize || h.glbCrc32!=glbCrc32)
    return std::nullopt;
  uint64_t payloadSize = 0;
  for(size_t i=0; i<h.counts.size(); ++i)
    payloadSize += uint64_t(h.counts[i])*Strides[i];
  const auto end = uint64_t(size);
  if(payloadSize!=end-sizeof(Header) || payloadSize>std::numeric_limits<size_t>::max())
    return std::nullopt;
  std::vector<uint8_t> payload((size_t(payloadSize)));
  if(!file.read(reinterpret_cast<char*>(payload.data()),std::streamsize(payload.size())) ||
     uint32_t(mz_crc32(0,payload.data(),payload.size()))!=h.payloadCrc32)
    return std::nullopt;
  Graph graph;
  const auto* p = payload.data();
  get(p,graph.vertices,h.counts[0]);
  get(p,graph.vertexIndices,h.counts[1]);
  get(p,graph.indices,h.counts[2]);
  get(p,graph.clusters,h.counts[3]);
  get(p,graph.groups,h.counts[4]);
  if(!valid(graph,mesh,twoSided))
    return std::nullopt;
  return graph;
  }

}
