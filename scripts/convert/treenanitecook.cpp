#include "treenanitecook.h"

#include "gltfmesh.h"
#include "gltfmeshcolor.h"
#include "gothicglb.h"
#include "graphics/foliagetree.h"
#include "graphics/mesh/treenaniteasset.h"
#include "worldchunkbinary.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>

namespace {

using ClusterLod::Graph;
using ClusterLod::NoGroup;
using ClusterLod::Point;
using ClusterLod::Triangle;
using ClusterLod::Vertex;
using TreeNanite::Asset;
using TreeNanite::MaxLogicalTriangles;
using TreeNanite::MaxMeshletPrimitives;
using TreeNanite::MaxMeshletVertices;
using TreeNanite::Meshlet;
using TreeNanite::Bone;
using TreeNanite::Skin;

constexpr float Infinity = std::numeric_limits<float>::infinity();

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error("tree cluster DAG: " + message);
  }

uint32_t offset(size_t size) {
  require(size < NoGroup,"32-bit offset overflow");
  return uint32_t(size);
  }

struct Input {
  std::vector<Vertex>          vertices;
  std::vector<Triangle>        triangles;
  std::vector<uint8_t>         locks;
  std::vector<uint32_t>        wind;
  std::vector<Bone>            bones;
  std::vector<Skin>            skin;
  std::array<uint8_t,32>       sourceHash, materialsHash, rigHash;
  float                       height;
  };

rapidjson::Document readJson(const std::vector<uint8_t>& bytes, const char* error) {
  rapidjson::Document document;
  require(!bytes.empty() && !document.Parse(reinterpret_cast<const char*>(bytes.data()),bytes.size()).HasParseError() &&
          document.IsObject(),error);
  return document;
  }

void readCardMesh(Input& out, const std::filesystem::path& glb, const rapidjson::Document& document) {
  std::vector<uint8_t> source;
  const auto mesh = loadGltfMesh(glb,&source);
  const auto colors = loadGltfMeshColors(source,mesh);
  require(mesh.sub_meshes.size()==2 && mesh.sub_meshes[0].mat.name=="OWN_TREE_FIR_BARK" &&
          mesh.sub_meshes[1].mat.name=="OWN_TREE_FIR_BOUGHS" && colors.size()==2,
          "expected the two spruce A materials with wind weights");
  out.sourceHash = sha256Bytes(source);
  out.height = mesh.bbox.max.y;
  for(uint32_t material=0; material<mesh.sub_meshes.size(); ++material) {
    const auto& sub = mesh.sub_meshes[material];
    const auto& entry = gothicglb::member(document,sub.mat.texture.c_str());
    const auto& wind = gothicglb::member(entry,"foliage");
    require(gothicglb::jsonToString(gothicglb::member(wind,"anchor"),"anchor")=="tree",
            "expected tree wind on " + sub.mat.texture);
    const auto amplitude = [&](const char* name) {
      const auto value = gothicglb::jsonToFloat(gothicglb::member(wind,name),name);
      require(value>=0.f && value<=FoliageWind::MaxScale,"invalid wind amplitude on " + sub.mat.texture);
      return value;
      };
    out.wind.push_back(FoliageWind::pack({true,amplitude("wind"),amplitude("flutter"),0.f,
                                        FoliageWind::AnchorTree}, FoliageWind::Wind));

    const uint32_t start = offset(out.vertices.size());
    const bool bark = material==0;
    for(size_t i=0; i<sub.wedges.size(); ++i) {
      const auto& wedge = sub.wedges[i];
      const auto& p = mesh.positions.at(wedge.index);
      const auto& n = wedge.normal;
      const auto& uv = wedge.texture;
      out.vertices.push_back({{p.x,p.y,p.z},{n.x,n.y,n.z},{uv.x,uv.y},colors[material][i]});
      out.locks.push_back(uint8_t(bark));
      }
    for(const auto& t : sub.triangles)
      out.triangles.push_back({{start+t.wedges[0],start+t.wedges[1],start+t.wedges[2]},material,bark});
    }
  }

void readNeedleMesh(Input& out, const std::filesystem::path& path) {
  // The pilot's rigid, opaque triangle stream has 32-bit indices rather than
  // the MRM restrictions of the GLB-to-game converter.
  struct FileVertex {
    Vertex vertex;
    uint16_t first, second;
    uint8_t weight, lock;
    std::array<uint8_t,2> padding;
    };
  using FileTriangle = std::array<uint32_t,4>;
  using Header = std::array<uint32_t,4>;
  static_assert(std::endian::native == std::endian::little && sizeof(FileVertex)==44);
  const auto bytes = gothicglb::readFile(path);
  require(bytes.size()>=sizeof(Header),"truncated spruce geometry");
  Header header;
  std::memcpy(header.data(),bytes.data(),sizeof(Header));
  const auto [magic,version,vertexCount,triangleCount] = header;
  require(magic==0x47524946 && version==1 && vertexCount>0 && triangleCount>0,
          "unsupported spruce geometry");
  require(sizeof(Header) + uint64_t(vertexCount)*sizeof(FileVertex) +
          uint64_t(triangleCount)*sizeof(FileTriangle)==bytes.size(),"invalid spruce geometry size");
  out.sourceHash = sha256Bytes(bytes);
  out.vertices.reserve(vertexCount);
  out.locks.reserve(vertexCount);
  out.skin.reserve(vertexCount);
  out.triangles.reserve(triangleCount);
  size_t cursor = sizeof(Header);
  for(uint32_t i=0; i<vertexCount; ++i) {
    FileVertex v;
    std::memcpy(&v,bytes.data()+cursor,sizeof(v));
    require(v.lock<=1 && v.padding==std::array<uint8_t,2>{},"invalid spruce vertex flags");
    out.vertices.push_back(v.vertex);
    out.locks.push_back(v.lock);
    out.skin.push_back({v.first,v.second,v.weight});
    out.height = std::max(out.height,v.vertex.position[1]);
    cursor += sizeof(v);
    }
  std::array<uint32_t,2> materialCounts{};
  for(uint32_t i=0; i<triangleCount; ++i) {
    FileTriangle t;
    std::memcpy(t.data(),bytes.data()+cursor,sizeof(t));
    const auto [a,b,c,material] = t;
    require(a<vertexCount && b<vertexCount && c<vertexCount && material<materialCounts.size(),
            "invalid spruce triangle");
    ++materialCounts[material];
    out.triangles.push_back({{a,b,c},material,material==0});
    cursor += sizeof(t);
    }
  require(materialCounts[0]>0 && materialCounts[1]>0,"expected spruce bark and needle triangles");
  }

void readRig(Input& out, const std::filesystem::path& path) {
  const auto bytes = gothicglb::readFile(path);
  const auto document = readJson(bytes,"invalid spruce rig JSON");
  require(gothicglb::jsonToInt(gothicglb::member(document,"version"),"version")==1,"unsupported spruce rig");
  out.rigHash = sha256Bytes(bytes);
  const auto& bones = gothicglb::member(document,"bones");
  require(bones.IsArray() && bones.Size()>0 && bones.Size()<=std::numeric_limits<uint16_t>::max(),
          "invalid spruce rig bone count");
  for(const auto& v : bones.GetArray()) {
    require(v.IsArray() && v.Size()==8,"expected bone parent, kind, pivot and axis");
    const auto integer = [&](unsigned i) { return gothicglb::jsonToInt(v[i],"bone integer"); };
    const auto vector = [&](unsigned i) {
      return Point{gothicglb::jsonToFloat(v[i],"bone vector"),gothicglb::jsonToFloat(v[i+1],"bone vector"),
                   gothicglb::jsonToFloat(v[i+2],"bone vector")};
      };
    const auto parent = integer(0), kind = integer(1);
    require(out.bones.empty() ? parent==-1 && kind==TreeNanite::RootBone :
            parent>=0 && uint64_t(parent)<out.bones.size() &&
            kind>=TreeNanite::TrunkBone && kind<=TreeNanite::ShootBone,
            "invalid spruce rig hierarchy");
    const auto axis = vector(5);
    const auto pivot = vector(2);
    require(std::abs(std::hypot(axis[0],axis[1],axis[2])-1.f)<1e-4f,"expected a unit bone axis");
    require(std::ranges::all_of(pivot,[](float value) { return std::isfinite(value); }),"invalid bone pivot");
    out.bones.push_back({pivot,axis,uint32_t(parent),uint32_t(kind)});
    }

  if(out.skin.empty()) {
    const auto& bindings = gothicglb::member(document,"vertices");
    require(bindings.IsArray() && bindings.Size()==out.vertices.size(),
            "the rig must contain one binding per GLB vertex");
    size_t vertex = 0;
    for(const auto& v : bindings.GetArray()) {
      require(v.IsArray() && v.Size()==4,"expected two joint IDs, weight and lock flag");
      const auto integer = [&](unsigned i) { return gothicglb::jsonToInt(v[i],"binding integer"); };
      const auto first = integer(0), second = integer(1), weight = integer(2), lock = integer(3);
      require(first>=0 && uint64_t(first)<out.bones.size() && second>=0 && uint64_t(second)<out.bones.size() &&
              weight>=0 && weight<=255 && lock>=0 && lock<=1,"invalid GLB vertex binding");
      out.skin.push_back({uint16_t(first),uint16_t(second),uint8_t(weight)});
      out.locks[vertex++] |= uint8_t(lock);
      }
    }
  for(const auto& v : out.skin)
    require(v.first<out.bones.size() && v.second<out.bones.size(),"joint outside spruce rig");
  }

Input readInput(const std::filesystem::path& geometry, const std::filesystem::path& materials,
                const std::filesystem::path& rig) {
  Input out = {};
  const auto materialBytes = gothicglb::readFile(materials);
  const auto document = readJson(materialBytes,"invalid materials JSON");
  const bool opaque = geometry.extension()==".firg";
  if(opaque) {
    require(!rig.empty(),"opaque spruce geometry needs a bone hierarchy");
    readNeedleMesh(out,geometry);
    }
  else {
    readCardMesh(out,geometry,document);
    }
  if(!rig.empty())
    readRig(out,rig);
  out.materialsHash = sha256Bytes(materialBytes);
  require(out.height>0 && !out.triangles.empty(),"expected spruce triangles with an up-Y trunk");
  return out;
  }

using Position = std::array<uint32_t,3>;
using Edge = std::array<Position,2>;

uint32_t bits(float v) {
  return v==0.f ? 0u : std::bit_cast<uint32_t>(v);
  }

Position key(const Point& p) {
  return {bits(p[0]),bits(p[1]),bits(p[2])};
  }

using VertexKey = std::array<uint32_t,11>;

VertexKey key(const Vertex& v, Skin skin) {
  return {bits(v.position[0]),bits(v.position[1]),bits(v.position[2]),
          bits(v.normal[0]),bits(v.normal[1]),bits(v.normal[2]),bits(v.uv[0]),bits(v.uv[1]),v.color,
          uint32_t(skin.first) | (uint32_t(skin.second)<<16),skin.weight};
  }

VertexKey key(const auto& mesh, uint32_t id) {
  return key(mesh.vertices.at(id),mesh.skin.empty() ? Skin{} : mesh.skin.at(id));
  }

std::vector<uint32_t> clusterTriangles(const Graph& graph, const ClusterLod::Cluster& cluster) {
  std::vector<uint32_t> ids;
  const auto count = size_t(cluster.triangleCount)*3;
  require(cluster.indexOffset<=graph.indices.size() && count<=graph.indices.size()-cluster.indexOffset,
          "cluster range lies outside the index stream");
  require(cluster.vertexOffset<=graph.vertexIndices.size() &&
          cluster.vertexCount<=graph.vertexIndices.size()-cluster.vertexOffset,
          "cluster range lies outside the vertex stream");
  const auto local = std::span(graph.indices).subspan(cluster.indexOffset,count);
  ids.reserve(local.size());
  for(uint8_t index : local) {
    require(index < cluster.vertexCount,"cluster corner lies outside the local vertex range");
    ids.push_back(graph.vertexIndices.at(cluster.vertexOffset+index));
    }
  return ids;
  }

struct Topology {
  using Wedge = std::pair<uint32_t,VertexKey>; // material and all source wedge attributes
  std::map<Position,std::set<Wedge>> wedges;
  std::map<Edge,int> edges; // positive is in key order
  size_t clusters = 0;

  void add(const Graph& graph, const ClusterLod::Cluster& cluster) {
    ++clusters;
    const auto vertices = clusterTriangles(graph,cluster);
    for(size_t i=0; i<vertices.size(); i+=3) {
      std::array<Position,3> positions;
      for(size_t j=0; j<3; ++j) {
        const auto& v = graph.vertices.at(vertices[i+j]);
        positions[j] = key(v.position);
        wedges[positions[j]].emplace(cluster.material,key(graph,vertices[i+j]));
        }
      for(size_t j=0; j<3; ++j) {
        const auto a = positions[j], b = positions[(j+1)%3];
        if(a!=b)
          edges[{std::min(a,b),std::max(a,b)}] += a<b ? 1 : -1;
        }
      }
    }
  };

bool encloses(const ClusterLod::Sphere& outer, const ClusterLod::Sphere& inner) {
  const double distance = std::hypot(double(outer.center[0])-inner.center[0],
                                     double(outer.center[1])-inner.center[1],
                                     double(outer.center[2])-inner.center[2]);
  return distance+inner.radius<=outer.radius;
  }

size_t verifyTransitions(const Input& input, const Graph& graph) {
  using Side = std::vector<const ClusterLod::Cluster*>;
  struct Transition { Side fine, coarse; };
  std::vector<Transition> transitions(graph.groups.size());
  Side frontier;
  for(const auto& c : graph.clusters) {
    transitions.at(c.parentGroup).fine.push_back(&c);
    if(c.refinedGroup==NoGroup) {
      require(c.level==0,"unrefined cluster is not at level zero");
      frontier.push_back(&c);
      }
    else {
      require(c.refinedGroup < c.parentGroup,"cluster group link is not topological");
      transitions[c.refinedGroup].coarse.push_back(&c);
      const auto& child = graph.groups[c.refinedGroup];
      const auto& parent = graph.groups[c.parentGroup];
      require(child.errorCm<=parent.errorCm && encloses(parent.bounds,child.bounds),
              "parent error or sphere does not cover the refined group");
      }
    }

  for(size_t id=0; id<transitions.size(); ++id) {
    const auto& [fine,coarse] = transitions[id];
    const auto& group = graph.groups[id];
    require(!fine.empty() && group.errorCm>=0 && std::isinf(group.errorCm)==coarse.empty(),
            "invalid terminal or finite group " + std::to_string(id));
    for(const auto* cluster : fine)
      require(encloses(group.bounds,cluster->bounds),"group sphere misses its fine side");
    for(const auto* cluster : coarse)
      require(encloses(group.bounds,cluster->bounds),"group sphere misses its coarse side");
    }

  // Walk simultaneous hierarchy rounds while terminal groups stay in the cut.
  // Check shared vertices even when the neighbour uses a different UV chart.
  std::set<Position> attachments;
  for(size_t i=0; i<input.locks.size(); ++i)
    if(input.locks[i])
      attachments.insert(key(input.vertices[i].position));
  size_t verified = 0;
  for(;;) {
    std::map<uint32_t,Topology> current;
    std::map<Position,size_t> positionUses;
    std::map<Edge,size_t> edgeUses;
    for(const auto* cluster : frontier)
      current[cluster->parentGroup].add(graph,*cluster);
    for(const auto& [group,topology] : current) {
      for(const auto& [position,wedges] : topology.wedges)
        ++positionUses[position];
      for(const auto& [edge,winding] : topology.edges)
        ++edgeUses[edge];
      }

    Side next;
    const size_t previous = verified;
    for(const auto& [id,fine] : current) {
      const auto& [fullFine,coarse] = transitions[id];
      require(fine.clusters==fullFine.size(),"fractional transition frontier");
      if(coarse.empty()) {
        next.insert(next.end(),fullFine.begin(),fullFine.end());
        continue;
        }
      Topology replacement;
      for(const auto* cluster : coarse)
        replacement.add(graph,*cluster);
      for(const auto& [position,wedges] : fine.wedges) {
        if(positionUses.at(position)>1 || wedges.size()>1 || attachments.contains(position)) {
          const auto found = replacement.wedges.find(position);
          require(found!=replacement.wedges.end() && found->second==wedges,
                  "shared position, attachment, UV seam or wind weights changed at group " + std::to_string(id));
          }
        }
      for(const auto& [edge,winding] : fine.edges) {
        if(winding!=0 && edgeUses.at(edge)>1) {
          const auto found = replacement.edges.find(edge);
          require(found!=replacement.edges.end() && found->second==winding,
                  "shared edge changed at group " + std::to_string(id));
          }
        }
      ++verified;
      next.insert(next.end(),coarse.begin(),coarse.end());
      }
    if(verified==previous)
      break;
    frontier.swap(next);
    }
  require(verified==size_t(std::ranges::count_if(transitions,[](const auto& g) { return !g.coarse.empty(); })),
          "some transitions are unreachable");
  return verified;
  }

float vertexTravel(const Vertex& v, uint32_t wind, float reach) {
  const float height = std::clamp(v.position[1]/reach,0.f,1.f);
  const float branch = (v.color & 255u)==0 ? 0.f :
                       std::min(1.f,std::hypot(v.position[0],v.position[2])/(reach*FoliageTree::BranchReach));
  const float leaf = float((v.color>>16)&255u)/255.f;
  return FoliageTree::travel(wind,reach,{height*height,branch,leaf});
  }

template<class T>
std::array<T,3> orientedFace(T a, T b, T c) {
  using Face = std::array<T,3>;
  return std::min({Face{a,b,c},Face{b,c,a},Face{c,a,b}});
  }

using Face = std::array<uint32_t,3>;

void verifySource(const Input& input, const Graph& graph) {
  using Signature = std::pair<uint32_t,std::array<VertexKey,3>>;
  const auto face = [](uint32_t mat, const auto& mesh, uint32_t a, uint32_t b, uint32_t c) -> Signature {
    return {mat,orientedFace(key(mesh,a),key(mesh,b),key(mesh,c))};
    };
  std::vector<Signature> source, leaf;
  for(const auto& t : input.triangles)
    source.push_back(face(t.material,input,t.vertices[0],t.vertices[1],t.vertices[2]));
  for(const auto& cluster : graph.clusters) {
    if(cluster.refinedGroup!=NoGroup)
      continue;
    const auto indices = clusterTriangles(graph,cluster);
    for(size_t i=0; i<indices.size(); i+=3)
      leaf.push_back(face(cluster.material,graph,indices[i],indices[i+1],indices[i+2]));
    }
  std::ranges::sort(source);
  std::ranges::sort(leaf);
  require(source==leaf,"leaf geometry or attributes differ from the source triangles");
  }

void appendMeshlet(Asset& asset, const Input& input, std::span<const float> travel, uint32_t material,
                   std::span<const uint32_t> vertices,
                   std::span<const uint8_t> triangles, std::vector<Face>& faces) {
  require(!vertices.empty() && vertices.size()<=MaxMeshletVertices && !triangles.empty() &&
          triangles.size()<=3*MaxMeshletPrimitives,
          "physical meshlet exceeds its raster limits");
  Meshlet meshlet = {{Infinity,Infinity,Infinity},{-Infinity,-Infinity,-Infinity},
                     offset(asset.vertexIds.size()),offset(asset.primitives.size()),
                     offset(vertices.size()),offset(triangles.size()/3)};
  for(auto id : vertices) {
    const auto& v = asset.vertices.at(id);
    const float radius = travel.empty() ? vertexTravel(v,input.wind.at(material),input.height) : travel[id];
    for(size_t axis=0; axis<v.position.size(); ++axis) {
      const float margin = 8.f*std::numeric_limits<float>::epsilon()*std::max(input.height,std::abs(v.position[axis]));
      meshlet.boxMin[axis] = std::min(meshlet.boxMin[axis],std::nextafter(v.position[axis]-radius-margin,-Infinity));
      meshlet.boxMax[axis] = std::max(meshlet.boxMax[axis],std::nextafter(v.position[axis]+radius+margin,Infinity));
      }
    }
  asset.vertexIds.insert(asset.vertexIds.end(),vertices.begin(),vertices.end());
  for(size_t i=0; i<triangles.size(); i+=3) {
    const auto a = triangles[i], b = triangles[i+1], c = triangles[i+2];
    require(a<vertices.size() && b<vertices.size() && c<vertices.size(),"physical meshlet corner out of range");
    asset.primitives.push_back(uint32_t(a) | uint32_t(b)<<8 | uint32_t(c)<<16);
    faces.push_back(orientedFace(vertices[a],vertices[b],vertices[c]));
    }
  asset.meshlets.push_back(meshlet);
  }

Asset pack(const Input& input, const Graph& graph) {
  Asset asset;
  asset.sourceHash = input.sourceHash;
  asset.materialHash = input.materialsHash;
  asset.vertices = graph.vertices;
  asset.groups = graph.groups;
  asset.bones = input.bones;
  asset.skin = graph.skin;
  std::vector<float> travel(asset.skin.size());
  for(uint32_t index=0; index<travel.size(); ++index)
    travel[index] = TreeNanite::windTravel(asset,index);
  float windMargin = 0.f;
  if(travel.empty()) {
    for(auto wind : input.wind)
      windMargin = std::max(windMargin,FoliageTree::travel(wind,input.height));
    }
  else {
    windMargin = std::ranges::max(travel);
    }
  for(auto& group : asset.groups)
    group.bounds.radius = std::nextafter(group.bounds.radius+windMargin,Infinity);

  for(const auto& c : graph.clusters) {
    require(c.vertexCount<=256 && c.triangleCount<=MaxLogicalTriangles,"logical cluster exceeds the offline profile limits");
    const auto first = offset(asset.meshlets.size());
    const auto indices = clusterTriangles(graph,c);
    for(auto vertexId : std::span(graph.vertexIndices).subspan(c.vertexOffset,c.vertexCount)) {
      const auto& vertex = graph.vertices.at(vertexId);
      const float radius = travel.empty() ? vertexTravel(vertex,input.wind.at(c.material),input.height) : travel[vertexId];
      const ClusterLod::Sphere wind = {vertex.position,radius};
      require(encloses(asset.groups[c.parentGroup].bounds,wind),"fine wind envelope lies outside its group sphere");
      if(c.refinedGroup!=NoGroup)
        require(encloses(asset.groups[c.refinedGroup].bounds,wind),"coarse wind envelope lies outside its group sphere");
      }
    std::vector<Face> packed, original;
    for(size_t i=0; i<indices.size(); i+=3)
      original.push_back(orientedFace(indices[i],indices[i+1],indices[i+2]));

    if(c.vertexCount<=MaxMeshletVertices && c.triangleCount<=MaxMeshletPrimitives) {
      appendMeshlet(asset,input,travel,c.material,std::span(graph.vertexIndices).subspan(c.vertexOffset,c.vertexCount),
                   std::span(graph.indices).subspan(c.indexOffset,c.triangleCount*3),packed);
      }
    else {
      std::vector<meshopt_Meshlet> meshlets(meshopt_buildMeshletsBound(indices.size(),MaxMeshletVertices,
                                                                     MaxMeshletPrimitives));
      std::vector<uint32_t> vertexIds(indices.size());
      std::vector<uint8_t> local(indices.size());
      meshlets.resize(meshopt_buildMeshlets(meshlets.data(),vertexIds.data(),local.data(),indices.data(),indices.size(),
                       asset.vertices.front().position.data(),asset.vertices.size(),sizeof(Vertex),
                       MaxMeshletVertices,MaxMeshletPrimitives,0.f));
      for(const auto& m : meshlets) {
        meshopt_optimizeMeshlet(vertexIds.data()+m.vertex_offset,local.data()+m.triangle_offset,
                                m.triangle_count,m.vertex_count);
        appendMeshlet(asset,input,travel,c.material,std::span(vertexIds).subspan(m.vertex_offset,m.vertex_count),
                     std::span(local).subspan(m.triangle_offset,m.triangle_count*3),packed);
        }
      }
    std::ranges::sort(original);
    std::ranges::sort(packed);
    require(original==packed,"physical repack changed oriented triangles");
    asset.clusters.push_back({c.refinedGroup,c.parentGroup,c.material,first,offset(asset.meshlets.size())-first});
    if(c.refinedGroup!=NoGroup)
      require(encloses(asset.groups[c.parentGroup].bounds,asset.groups[c.refinedGroup].bounds),
              "wind-inflated parent sphere misses its refined sphere");
    }
  return asset;
  }

void distribution(std::string_view profile, size_t level, std::string_view metric, std::vector<size_t> values) {
  std::ranges::sort(values);
  if(values.empty())
    return;
  const auto percentile = [&](size_t p) { return values[(p*values.size()+99)/100-1]; };
  std::cout << profile << '\t' << level << '\t' << metric << '\t' << values.size()
            << '\t' << values.front() << '\t' << percentile(50) << '\t' << percentile(95)
            << '\t' << values.back() << '\n';
  }

void report(std::string_view profile, const Graph& graph, const Asset& asset, const ClusterLod::BuildStats& stats,
            size_t verified) {
  std::map<uint32_t,std::vector<size_t>> levels;
  for(size_t id=0; id<graph.clusters.size(); ++id)
    levels[graph.clusters[id].level].push_back(id);
  std::cout << "profile\t" << profile << "\tverified_transitions\t" << verified << "\tterminal_groups\t"
            << graph.groups.size()-verified << '\n';
  std::cout << "profile\t" << profile << "\tdilated_groups\t"
            << std::ranges::count_if(stats.dilationCm,[](float distance) { return distance>0.f; })
            << "\tmax_dilation_cm\t" << std::ranges::max(stats.dilationCm) << '\n';
  constexpr std::array stopReasons = {"reduction","material","boundary","locked","area"};
  static_assert(stopReasons.size()==size_t(ClusterLod::StopReason::Count));
  for(size_t i=0; i<stopReasons.size(); ++i)
    std::cout << profile << "\tstop_" << stopReasons[i] << '\t' << stats.stoppedGroups[i]
              << "\ttriangles\t" << stats.stoppedTriangles[i] << '\n';
  std::cout << "profile\tlevel\tmetric\tcount\tmin\tp50\tp95\tmax\n";
  for(const auto& [level,ids] : levels) {
    std::vector<size_t> logicalVertices, logicalTriangles, meshletsPerCluster, meshletVertices, meshletTriangles;
    std::set<uint32_t> unique;
    std::array<size_t,2> materialTriangles{};
    size_t references = 0, triangles = 0;
    for(auto id : ids) {
      const auto& c = graph.clusters[id];
      const auto& packed = asset.clusters[id];
      logicalVertices.push_back(c.vertexCount);
      logicalTriangles.push_back(c.triangleCount);
      materialTriangles.at(c.material) += c.triangleCount;
      meshletsPerCluster.push_back(packed.meshletCount);
      for(const auto& m : std::span(asset.meshlets).subspan(packed.firstMeshlet,packed.meshletCount)) {
        meshletVertices.push_back(m.vertexCount);
        meshletTriangles.push_back(m.primitiveCount);
        references += m.vertexCount;
        triangles += m.primitiveCount;
        for(auto v : std::span(asset.vertexIds).subspan(m.firstVertex,m.vertexCount))
          unique.insert(v);
        }
      }
    distribution(profile,level,"logical_vertices",std::move(logicalVertices));
    distribution(profile,level,"logical_triangles",std::move(logicalTriangles));
    distribution(profile,level,"meshlets_per_cluster",std::move(meshletsPerCluster));
    const size_t meshletCount = meshletVertices.size();
    distribution(profile,level,"meshlet_vertices",std::move(meshletVertices));
    distribution(profile,level,"meshlet_triangles",std::move(meshletTriangles));
    std::cout << profile << '\t' << level << "\ttriangles\t" << triangles << "\tvertex_references\t" << references
              << "\tunique_vertices\t" << unique.size() << "\tduplicate_vertices\t" << references-unique.size() << '\n';
    if(!asset.skin.empty()) {
      std::set<uint16_t> bones;
      for(uint32_t id : unique) {
        const auto skin = asset.skin[id];
        if(skin.weight>0)
          bones.insert(skin.first);
        if(skin.weight<255)
          bones.insert(skin.second);
        }
      const size_t bytes = unique.size()*(sizeof(Vertex)+sizeof(Skin)) + ids.size()*sizeof(TreeNanite::Cluster) +
                           meshletCount*sizeof(Meshlet) + (references+triangles)*sizeof(uint32_t);
      std::cout << profile << '\t' << level << "\tbark_triangles\t" << materialTriangles[0]
                << "\tcrown_triangles\t" << materialTriangles[1] << "\tinfluencing_bones\t" << bones.size()
                << "\tgeometry_bytes\t" << bytes << '\n';
      }
    }
  if(!asset.skin.empty()) {
    size_t fine=0, terminal=0;
    for(const auto& cluster : asset.clusters)
      for(const auto& meshlet : std::span(asset.meshlets).subspan(cluster.firstMeshlet,cluster.meshletCount)) {
        if(cluster.refinedGroup==NoGroup)
          fine += meshlet.primitiveCount;
        if(std::isinf(asset.groups[cluster.parentGroup].errorCm))
          terminal += meshlet.primitiveCount;
        }
    std::cout << profile << "\tleaf_cut_triangles\t" << fine << "\tterminal_cut_triangles\t" << terminal << '\n';
    std::cout << profile << "\tbone_bytes\t" << asset.bones.size()*sizeof(Bone)
              << "\tskin_bytes\t" << asset.skin.size()*sizeof(Skin) << '\n';
    }
  }

void reportCuts(std::string_view profile, const Asset& asset, float height) {
  const float y = 1.f/std::tan(67.5f*std::numbers::pi_v<float>/360.f);
  const float x = y*(720.f/1280.f);
  const float eye = height*0.5f;
  constexpr float near = 10.f, far = 300000.f;
  std::cout << "profile\tdistance_cm\tcut_clusters\tcut_meshlets\tcut_triangles\tfrustum_meshlets\tfrustum_triangles\n";
  for(float distance : {300.f,3000.f,10000.f,30000.f,100000.f}) {
    const std::array<TreeNanite::Plane,6> frustum = {{
        {{ x,0.f,-1.f}, distance}, {{-x,0.f,-1.f}, distance},
        {{0.f, y,-1.f}, distance-y*eye}, {{0.f,-y,-1.f}, distance+y*eye},
        {{0.f,0.f,-1.f},distance-near}, {{0.f,0.f,1.f},far-distance}}};
    const TreeNanite::View view = {{0.f,eye,distance},{0.f,0.f,-1.f},720.f,y,near,far,frustum};
    size_t clusters = 0, meshlets = 0, triangles = 0, visibleMeshlets = 0, visibleTriangles = 0;
    for(const auto& c : asset.clusters) {
      if(!TreeNanite::clusterSelected(asset,c,view,{},1.f))
        continue;
      ++clusters;
      meshlets += c.meshletCount;
      for(const auto& m : std::span(asset.meshlets).subspan(c.firstMeshlet,c.meshletCount)) {
        triangles += m.primitiveCount;
        if(TreeNanite::meshletVisible(m,view,{},1.f)) {
          ++visibleMeshlets;
          visibleTriangles += m.primitiveCount;
          }
        }
      }
    std::cout << profile << '\t' << distance << '\t' << clusters << '\t' << meshlets << '\t' << triangles
              << '\t' << visibleMeshlets << '\t' << visibleTriangles << '\n';
    }
  }

void printHash(std::string_view name, const std::array<uint8_t,32>& hash) {
  std::cout << name << '\t' << std::hex << std::setfill('0');
  for(auto byte : hash)
    std::cout << std::setw(2) << unsigned(byte);
  std::cout << std::dec << std::setfill(' ') << '\n';
  }

}

void cookTreeNanite(const std::filesystem::path& geometry, const std::filesystem::path& materials,
                    const std::filesystem::path& output, const std::filesystem::path& rig) {
  if(std::filesystem::exists(output))
    require(!std::filesystem::equivalent(geometry,output) && !std::filesystem::equivalent(materials,output) &&
            (rig.empty() || !std::filesystem::equivalent(rig,output)),
            "output would replace an input");
  const auto input = readInput(geometry,materials,rig);
  std::cout << "format_version\t" << (input.skin.empty() ? TreeNanite::FormatVersion : TreeNanite::WindFormatVersion)
            << '\n';
  printHash(geometry.extension()==".firg" ? "firg_sha256" : "glb_sha256",input.sourceHash);
  printHash("materials_sha256",input.materialsHash);
  if(!input.skin.empty()) {
    printHash("rig_sha256",input.rigHash);
    std::cout << "bones\t" << input.bones.size() << '\n';
    }
  Asset result;
  for(const bool logical128 : {false,true}) {
    if(!logical128 && !input.skin.empty())
      continue;
    ClusterLod::BuildStats stats;
    const auto graph = ClusterLod::buildFoliage(input.vertices,input.triangles,input.locks,stats,logical128,input.skin);
    verifySource(input,graph);
    const auto verified = verifyTransitions(input,graph);
    auto packed = pack(input,graph);
    const char* profile = logical128 ? "logical_128" : "logical_64";
    report(profile,graph,packed,stats,verified);
    reportCuts(profile,packed,input.height);
    if(logical128)
      result = std::move(packed);
    }
  TreeNanite::write(output,result);
  std::cout << "asset_bytes\t" << std::filesystem::file_size(output) << '\n';
  }
