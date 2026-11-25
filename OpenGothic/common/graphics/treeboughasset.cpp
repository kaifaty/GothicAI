#include "treeboughasset.h"

#include <Tempest/Log>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>

#include <rapidjson/document.h>

#include "graphics/foliagetree.h"
#include "graphics/mesh/submesh/staticmesh.h"
#include "meshlevels.h"
#include "settings.h"
#include "treeboughglb.h"
#include "utils/fileext.h"

using namespace Tempest;

namespace TreeBough {

namespace {

constexpr float BoundsTolerance = .01f;
constexpr uint32_t MaxDepth = 64;

using Json = rapidjson::Value;

struct Box {
  Vec3 min, max;

  bool contains(const Vec3& v) const {
    return min.x-BoundsTolerance<=v.x && min.y-BoundsTolerance<=v.y && min.z-BoundsTolerance<=v.z &&
           max.x+BoundsTolerance>=v.x && max.y+BoundsTolerance>=v.y && max.z+BoundsTolerance>=v.z;
    }

  bool contains(const Box& box) const {
    return contains(box.min) && contains(box.max);
    }
  };

struct Geometry {
  uint32_t primitive = 0;
  uint32_t firstVertex = 0, vertexCount = 0;
  uint32_t firstIndex = 0, indexCount = 0;
  };

struct Record {
  Box sourceBox;
  Node node;
  uint32_t id = 0, sourceCards = 0;
  std::vector<uint32_t> children;
  Geometry geometry;
  bool seen = false;
  };

[[noreturn]] void reject(const std::string& reason) {
  throw std::runtime_error(reason);
  }

const Json& member(const Json& value, const char* name) {
  if(!value.IsObject() || !value.HasMember(name))
    reject(std::string("expected field ")+name);
  return value[name];
  }

uint32_t uintField(const Json& value, const char* name) {
  const auto& entry = member(value,name);
  if(!entry.IsUint())
    reject(std::string("expected unsigned ")+name);
  return entry.GetUint();
  }

std::string_view stringField(const Json& value, const char* name) {
  const auto& entry = member(value,name);
  if(!entry.IsString())
    reject(std::string("expected string ")+name);
  return {entry.GetString(),entry.GetStringLength()};
  }

float number(const Json& value) {
  const auto n = value.IsNumber() ? value.GetDouble() : std::numeric_limits<double>::infinity();
  if(!std::isfinite(n) || std::abs(n)>1e6)
    reject("invalid numeric value in hierarchy");
  return float(n);
  }

Vec3 point(const Json& point) {
  if(!point.IsArray() || point.Size()<3)
    reject("expected a three-component position");
  // Generator Z-up metres -> glTF Y-up -> mirrored X engine centimetres.
  return Vec3(-100.f*number(point[0]),100.f*number(point[2]),-100.f*number(point[1]));
  }

Box boxField(const Json& value, const char* name) {
  const auto& array = member(value,name);
  if(!array.IsArray() || array.Size()!=2)
    reject(std::string("expected minimum and maximum in ")+name);
  const Vec3 lo = point(array[0]);
  const Vec3 hi = point(array[1]);
  Box box {Vec3(hi.x,lo.y,hi.z),Vec3(lo.x,hi.y,lo.z)};
  if(box.min.x>box.max.x || box.min.y>box.max.y || box.min.z>box.max.z)
    reject(std::string("inverted box in ")+name);
  return box;
  }

std::vector<uint32_t> ids(const Json& value) {
  if(!value.IsArray())
    reject("expected node IDs");
  std::vector<uint32_t> result;
  result.reserve(value.Size());
  for(const auto& id:value.GetArray()) {
    if(!id.IsUint() || id.GetUint()==0)
      reject("expected positive node IDs");
    result.push_back(id.GetUint());
    }
  return result;
  }

Record readRecord(const Json& entry) {
  Record record;
  record.id = uintField(entry,"id");
  record.sourceCards = uintField(entry,"source_cards");
  record.children = ids(member(entry,"children"));
  record.sourceBox = boxField(entry,"box");
  const auto bounds = boxField(entry,"cull_box");
  if(!bounds.contains(record.sourceBox))
    reject("culling box does not cover the original foliage");

  auto& node = record.node;
  node.boxMin = bounds.min;
  node.boxMax = bounds.max;
  node.diameter = (record.sourceBox.max-record.sourceBox.min).length();
  const auto& sphere = member(entry,"sphere");
  if(!sphere.IsArray() || sphere.Size()!=4)
    reject("expected sphere centre and radius");
  const auto center = point(sphere);
  const auto radius = 100.f*number(sphere[3]);
  const auto extent = Vec3(std::max(std::abs(bounds.min.x-center.x),std::abs(bounds.max.x-center.x)),
                           std::max(std::abs(bounds.min.y-center.y),std::abs(bounds.max.y-center.y)),
                           std::max(std::abs(bounds.min.z-center.z),std::abs(bounds.max.z-center.z)));
  if(radius<=0.f || radius+BoundsTolerance<extent.length() || node.diameter<=0.f)
    reject("sphere does not cover the culling box");
  node.sphere = Vec4(center.x,center.y,center.z,std::max(radius,extent.length()));

  node.thresholdPx = number(member(entry,"threshold_px"));
  node.lowPx = number(member(entry,"low_px"));
  node.highPx = number(member(entry,"high_px"));
  // max_px records the measured size before the content producer applies its selected tolerance.
  if(number(member(entry,"max_px"))<0.f || (node.thresholdPx==0.f ? node.lowPx!=0.f || node.highPx!=0.f :
     !(node.lowPx>0.f && node.lowPx<node.thresholdPx && node.thresholdPx<node.highPx)))
    reject("invalid transition band");

  const auto& geometry = member(entry,"geometry");
  if(!geometry.IsNull()) {
    auto& out = record.geometry;
    out.primitive = uintField(geometry,"primitive");
    out.firstVertex = uintField(geometry,"first_vertex");
    out.vertexCount = uintField(geometry,"vertex_count");
    out.firstIndex = uintField(geometry,"first_index");
    out.indexCount = uintField(geometry,"index_count");
    if(out.primitive==0 || out.vertexCount<3 || out.vertexCount>MaxVertices || out.indexCount==0 ||
       out.indexCount>MaxIndices || (out.firstIndex%3)!=0 || (out.indexCount%3)!=0)
      reject("unsupported compact geometry range");
    }
  if(record.children.empty()) {
    if(record.geometry.primitive!=1 || node.highPx!=0.f)
      reject("a leaf must address the original foliage and have no band");
    }
  else if((record.geometry.primitive==0 && node.highPx!=0.f) ||
          (record.geometry.primitive!=0 && (record.geometry.primitive<2 ||
            record.geometry.vertexCount*3!=record.geometry.indexCount*2)))
    reject("a merged node needs one or more distinct quads or a disabled band");

  return record;
  }

struct Forest {
  Asset& asset;
  std::vector<Record> records;
  std::vector<uint32_t> order;

  uint32_t visit(uint32_t id, uint32_t parent, float upper, uint32_t depth) {
    if(id==0 || id>records.size())
      reject("unknown node ID");
    if(depth>MaxDepth)
      reject("hierarchy exceeds 64 levels");
    auto& record = records[id-1];
    if(record.seen)
      reject("a node occurs more than once in the forest");
    record.seen = true;
    const uint32_t index = uint32_t(asset.nodes.size());
    if(parent!=NoNode) {
      const auto& sourceParent = records[order[parent]-1].sourceBox;
      const auto& p = asset.nodes[parent];
      if(!sourceParent.contains(record.sourceBox) || !Box{p.boxMin,p.boxMax}.contains(Box{record.node.boxMin,record.node.boxMax}))
        reject("a parent box does not contain its child");
      }
    if(record.node.highPx>0.f && record.node.lowPx<upper)
      reject("overlapping transition bands along an ancestor chain");
    asset.nodes.push_back(record.node);
    asset.nodes[index].parent = parent;
    order.push_back(id);
    uint32_t sourceCards = 1;
    if(!record.children.empty()) {
      sourceCards = 0;
      for(const auto child:record.children)
        sourceCards += visit(child,index,std::max(upper,record.node.highPx),depth+1);
      }
    if(record.sourceCards!=sourceCards)
      reject("wrong number of source cards in node "+std::to_string(id));
    asset.nodes[index].subtreeEnd = uint32_t(asset.nodes.size());
    return sourceCards;
    }
  };

std::filesystem::path glbPath(const std::filesystem::path& directory, std::string_view name) {
  const std::filesystem::path relative(name);
  if(name.find('\0')!=std::string_view::npos || relative.is_absolute() || relative.has_root_name() ||
     !FileExt::hasExt(name,"GLB"))
    reject("expected a relative GLB path without '..'");
  for(const auto& part:relative)
    if(part=="..")
      reject("expected a relative GLB path without '..'");
  return directory/relative;
  }

float vertexTravel(const Resources::Vertex& v, uint32_t wind, float reach) {
  const float height = std::clamp(v.pos[1]/reach, 0.f, 1.f);
  const float branch = (v.color & 255u)==0 ? 0.f :
                       std::min(1.f,std::hypot(v.pos[0],v.pos[2])/(reach*FoliageTree::BranchReach));
  const float leaf = float((v.color >> 16u) & 255u)/255.f;
  return FoliageTree::travel(wind,reach,{height*height,branch,leaf});
  }

void packGeometry(Asset& asset, const Forest& forest, std::vector<GlbPage> source, const StaticMesh& mesh) {
  std::vector<std::vector<uint32_t>> pageNodes(source.size());
  for(uint32_t i=0; i<forest.order.size(); ++i) {
    const auto& geometry = forest.records[forest.order[i]-1].geometry;
    if(geometry.primitive>=source.size())
      reject("unknown geometry primitive");
    if(geometry.primitive!=0)
      pageNodes[geometry.primitive].push_back(i);
    }

  asset.pages.resize(source.size());
  asset.ranges.resize(asset.nodes.size());
  for(size_t primitive=1; primitive<source.size(); ++primitive) {
    auto& geometry = source[primitive];
    auto& nodes = pageNodes[primitive];
    const auto data = [&](uint32_t i) -> const Record& { return forest.records[forest.order[i]-1]; };
    std::ranges::sort(nodes,[&](uint32_t a, uint32_t b) { return data(a).geometry.firstIndex<data(b).geometry.firstIndex; });

    auto& page = asset.pages[primitive];
    ::asset::Material material;
    material.name = geometry.texture;
    material.texture = geometry.texture;
    material.color = {255,255,255,255};
    material.alpha_func = ::asset::AlphaFunction::DEFAULT;
    page.material = Resources::loadMaterial(material,true,true);
    if(!FoliageTree::isTree(page.material.foliage) || page.material.alpha!=Material::AlphaTest ||
       page.material.tex==nullptr || !page.material.hasMaterialMaps())
      reject("missing alpha-tested tree material "+geometry.texture);
    const auto& bounds = mesh.bbox.bbox;
    const float reach = FoliageTree::reach(page.material.foliage,bounds[0],bounds[1]);
    const auto wind = FoliageWind::pack(page.material.foliage,FoliageWind::Wind);
    std::vector<uint32_t> indices;
    indices.reserve(geometry.indices.size()+nodes.size());
    size_t nextIndex = 0, nextVertex = 0;
    for(const auto i:nodes) {
      const auto& record = data(i);
      const auto& range = record.geometry;
      if(range.firstIndex!=nextIndex || range.firstVertex!=nextVertex ||
         range.indexCount>geometry.indices.size()-nextIndex || range.vertexCount>geometry.vertices.size()-nextVertex)
        reject("geometry ranges must partition their primitive's indices and vertices");
      if(range.firstVertex>UINT16_MAX)
        reject("vertex range cannot be encoded in a compact header");
      const auto first = uint32_t(indices.size());
      if(first>=(1u<<FirstIndexBits))
        reject("index range cannot be encoded in a compact draw");
      indices.push_back((range.firstVertex<<16u) | range.vertexCount);
      nextIndex += range.indexCount;
      nextVertex += range.vertexCount;
      for(size_t index=range.firstIndex; index<nextIndex; ++index) {
        const auto vertex = geometry.indices[index];
        if(vertex<range.firstVertex || vertex>=nextVertex)
          reject("index escapes its node's vertex range");
        indices.push_back(vertex);
        }
      const auto& node = asset.nodes[i];
      for(size_t vertex=range.firstVertex; vertex<nextVertex; ++vertex) {
        const auto& v = geometry.vertices[vertex];
        const auto& p = v.pos;
        if(!Box{node.boxMin,node.boxMax}.contains(Vec3(p[0],p[1],p[2])))
          reject("culling box does not cover its geometry");
        const float margin = std::min({p[0]-node.boxMin.x,p[1]-node.boxMin.y,p[2]-node.boxMin.z,
                                       node.boxMax.x-p[0],node.boxMax.y-p[1],node.boxMax.z-p[2]});
        asset.cullPadding = std::max(asset.cullPadding,vertexTravel(v,wind,reach)-margin);
        if(primitive==1 && geometry.cardIds[vertex]!=record.id)
          reject("_CARD_ID does not match the source node");
        }
      if(primitive==1)
        asset.sourceTriangles += range.indexCount/3;
      if(primitive==1 || node.thresholdPx>0.f)
        asset.ranges[i] = {first,range.indexCount,range.primitive,primitive==1 ? 1u : range.indexCount/6u,
                           range.vertexCount};
      }
    if(nextIndex!=geometry.indices.size() || nextVertex!=geometry.vertices.size())
      reject("primitive has unused geometry");

    page.vbo = Resources::ssbo(geometry.vertices.data(),geometry.vertices.size()*sizeof(Resources::Vertex));
    page.ibo = Resources::ssbo(indices.data(),indices.size()*sizeof(uint32_t));
    }
  }

std::unique_ptr<Asset> load(const StaticMesh& mesh, const std::filesystem::path& file) {
  std::ifstream in(file,std::ios::binary);
  if(!in)
    reject("could not open the sidecar");
  const std::string source(std::istreambuf_iterator<char>(in),{});
  rapidjson::Document document;
  document.Parse<rapidjson::kParseValidateEncodingFlag>(source.data(),source.size());
  if(document.HasParseError() || !document.IsObject())
    reject("invalid JSON");
  if(stringField(document,"axes")!="Z-up metres" || uintField(document,"wood_primitive")!=0)
    reject("expected generator axes and wood in primitive 0");

  auto asset = std::make_unique<Asset>();
  uint32_t sourceSub = NoNode;
  for(size_t i=0; i<mesh.sub.size(); ++i) {
    const auto& mat = mesh.sub[i].material;
    if(mat.alpha==Material::AlphaTest && FoliageTree::isTree(mat.foliage)) {
      if(sourceSub!=NoNode)
        reject("more than one canopy submesh on the original mesh");
      sourceSub = uint32_t(i);
      }
    }
  if(sourceSub==NoNode)
    reject("no alpha-tested tree canopy submesh on the original mesh");
  asset->sourceSub = sourceSub;

  const auto& nodes = member(document,"nodes");
  if(!nodes.IsArray() || nodes.Empty() || nodes.Size()>=DescendantSide)
    reject("expected between 1 and 32767 nodes");
  Forest forest {*asset,std::vector<Record>(nodes.Size()),{}};
  asset->nodes.reserve(nodes.Size());
  forest.order.reserve(nodes.Size());
  for(const auto& entry:nodes.GetArray()) {
    auto node = readRecord(entry);
    const auto id = node.id;
    if(id==0 || id>forest.records.size() || forest.records[id-1].id!=0)
      reject("node IDs must be unique integers from 1 to the node count");
    forest.records[id-1] = std::move(node);
    }

  const auto roots = ids(member(document,"roots"));
  asset->roots.reserve(roots.size());
  for(const auto root:roots) {
    asset->roots.push_back(uint32_t(asset->nodes.size()));
    asset->sourceCards += forest.visit(root,NoNode,0.f,0);
    }
  if(forest.order.size()!=nodes.Size())
    reject("at least one node is not reachable from a root");
  if(std::ranges::none_of(asset->nodes,[](const Node& node) { return node.highPx>0.f; }))
    reject("no active proxy nodes");
  auto geometry = loadGlb(glbPath(file.parent_path(),stringField(document,"glb")));
  if(geometry[1].texture!=mesh.sub[sourceSub].texName)
    reject("source foliage uses a different material from the original mesh");
  packGeometry(*asset,forest,std::move(geometry),mesh);
  // Successive accepted boxes can each miss their child's box by BoundsTolerance.
  asset->cullPadding += float(MaxDepth)*BoundsTolerance;
  return asset;
  }

void selectNode(const Asset& asset, uint32_t i, const Projection& view, uint32_t transition, float weight,
                std::vector<Selected>& out) {
  const auto& node = asset.nodes[i];
  const bool leaf = node.subtreeEnd==i+1;
  if(leaf || (transition!=0u && node.highPx>0.f)) {
    out.push_back({i,transition,weight});
    return;
    }
  if(transition==0u && node.highPx>0.f) {
    weight = parentWeight(node,screenSize(node,view));
    if(weight>=1.f) {
      out.push_back({i,0u,0.f});
      return;
      }
    if(weight>0.f) {
      out.push_back({i,i+1u,weight});
      transition = DescendantSide | (i+1u);
      }
    }
  for(auto child=i+1; child<node.subtreeEnd; child=asset.nodes[child].subtreeEnd)
    selectNode(asset,child,view,transition,weight,out);
  }

}

bool enabled() {
  static const bool on = [] {
    const char* env = std::getenv("OPENGOTHIC_TREE_BOUGH_LOD");
    const bool ini = Settings::exists() && Settings::getI("INTERNAL","treeBoughLod")!=0;
    const bool value = MeshLevels::parse(env!=nullptr ? env : "",ini) && MeshLevels::enabled();
    Log::i("render: tree bough LOD = ",value ? "1" : "0");
    return value;
    }();
  return on;
  }

bool cpuEnabled() {
  static const bool on = Settings::exists() && Settings::getI("INTERNAL","treeBoughLodCpu")!=0;
  return on;
  }

void attach(StaticMesh& mesh, const std::filesystem::path& file) {
  if(!Resources::hasGraphicsDevice())
    return;
  try {
    auto asset = load(mesh,file);
    for(const auto root:asset->roots) {
      const auto& node = asset->nodes[root];
      Bounds bounds;
      bounds.assign(std::make_pair(node.boxMin,node.boxMax));
      mesh.drawn.assign(mesh.drawn,bounds);
      }
    Log::i("meshes: hierarchy \"",file.string(),"\": ",asset->nodes.size()," nodes, ",asset->sourceCards," source cards");
    mesh.treeBough = std::move(asset);
    }
  catch(const std::exception& error) {
    Log::e("meshes: hierarchy \"",file.string(),"\" is not used: ",error.what());
    }
  }

float screenSize(const Node& node, const Projection& view) {
  const auto& forward = view.forwardLocal;
  const Vec3 nearest(forward.x<0.f ? node.boxMax.x : node.boxMin.x,
                     forward.y<0.f ? node.boxMax.y : node.boxMin.y,
                     forward.z<0.f ? node.boxMax.z : node.boxMin.z);
  const float depth = view.originDepth+Vec3::dotProduct(forward,nearest);
  return depth>view.nearZ ? node.diameter*view.focalScale/depth : std::numeric_limits<float>::infinity();
  }

template<class Float>
static Float fadeWeight(Float low, Float high, Float px) {
  const auto t = std::clamp((px-low)/(high-low),Float(0),Float(1));
  return Float(1)-t*t*(Float(3)-Float(2)*t);
  }

float parentWeight(const Node& node, float px) {
  return node.highPx>node.lowPx ? fadeWeight(node.lowPx,node.highPx,px) : 0.f;
  }

float impostorJointWeight(float threshold, float px) {
  // Float intermediates can reverse the curve by one ULP near the upper edge.
  const double scale = double(threshold)/49.0;
  return float(fadeWeight(46.0*scale,56.0*scale,double(px)));
  }

uint32_t impostorJointFade(float weight) {
  // The high bit identifies a static instance's joint in the animation slot (tree_bough_dissolve.glsl).
  return weight>0.f && weight<1.f ? std::bit_cast<uint32_t>(weight) | 0x80000000u : 0u;
  }

void select(const Asset& asset, const Projection& view, std::vector<Selected>& out) {
  out.clear();
  for(const auto root:asset.roots)
    selectNode(asset,root,view,0u,0.f,out);
  }

}
