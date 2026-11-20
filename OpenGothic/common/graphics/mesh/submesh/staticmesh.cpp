#include "staticmesh.h"
#include "loadprobe.h"

#include <cassert>
#include <algorithm>
#include <atomic>
#include <limits>

#include "graphics/foliagetree.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/meshlevels.h"
#include "graphics/drawcommands.h"
#include "graphics/shaders.h"

struct LandscapePages::State {
  static constexpr size_t PageMeshlets = 4096;
  static constexpr size_t VertexBytes = PackedMesh::MaxVert*sizeof(StaticMesh::Vertex);
  static constexpr size_t IndexBytes = PackedMesh::MaxPrim*4;
  // With the ray scene a page keeps the meshlets' triangles for rays as well: 32-bit indices of the page's vertices,
  // which BLAS of chunks are built from and hit shaders read (rt_common.glsl)
  static constexpr size_t RtIndexBytes = PackedMesh::MaxInd*sizeof(uint32_t);

  // the bytes of a meshlet in a page: its vertices and 8-bit indices, and with the ray scene its 32-bit indices
  const size_t MeshletBytes = VertexBytes+IndexBytes+(Shaders::options().doRtScene ? RtIndexBytes : 0);

  struct Span {
    size_t first = 0;
    size_t count = 0;
    };
  struct Page {
    Tempest::StorageBuffer vbo;
    Tempest::StorageBuffer ibo;
    Tempest::StorageBuffer rtIbo;  // only with the ray scene
    std::vector<Span> free;
    size_t capacity = 0;
    };

  std::vector<std::unique_ptr<Page>> pages;
  // Old-world retirement can overlap main-thread frame recycling.
  std::atomic_size_t occupied{0};
  std::atomic_size_t retired{0};

  void release(Page& page, Span span) {
    auto at=std::lower_bound(page.free.begin(),page.free.end(),span.first,
                            [](const Span& left,size_t first) { return left.first<first; });
    at=page.free.insert(at,span);
    if(at+1!=page.free.end() && at->first+at->count==(at+1)->first) {
      at->count+=(at+1)->count;
      page.free.erase(at+1);
      }
    if(at!=page.free.begin() && (at-1)->first+(at-1)->count==at->first) {
      (at-1)->count+=at->count;
      page.free.erase(at);
      }
    }
  };

class LandscapeRange final {
  public:
    std::shared_ptr<LandscapePages::State> owner;
    LandscapePages::State::Page* page = nullptr;
    LandscapePages::State::Span span;
    bool retired = false;

    ~LandscapeRange() {
      if(page==nullptr)
        return;
      const auto bytes=span.count*owner->MeshletBytes;
      if(retired)
        owner->retired-=bytes;
      else
        owner->occupied-=bytes;
      owner->release(*page,span);
      }

    void retire() {
      assert(!retired);
      const auto bytes=span.count*owner->MeshletBytes;
      owner->occupied-=bytes;
      owner->retired+=bytes;
      retired=true;
      }
  };

LandscapePages::LandscapePages():state(std::make_shared<State>()) {
  }

LandscapePages::~LandscapePages() = default;

LandscapePages::Stats LandscapePages::stats() const {
  Stats ret;
  ret.occupiedBytes=state->occupied.load();
  ret.retiredBytes=state->retired.load();
  ret.pageCount=state->pages.size();
  for(const auto& page:state->pages)
    ret.freeBytes+=page->capacity*state->MeshletBytes;
  ret.freeBytes-=ret.occupiedBytes+ret.retiredBytes;
  return ret;
  }

std::shared_ptr<LandscapeRange> LandscapePages::allocate(const PackedMesh& mesh) {
  assert(mesh.vertices.size()%PackedMesh::MaxVert==0);
  const auto count=mesh.vertices.size()/PackedMesh::MaxVert;
  assert(mesh.indices8.size()==count*State::IndexBytes);
  if(count==0 || !Resources::hasGraphicsDevice())
    return {};

  State::Page* page=nullptr;
  size_t index=0, bestCount=size_t(-1);
  for(auto& candidate:state->pages)
    for(size_t i=0;i<candidate->free.size();++i)
      if(candidate->free[i].count>=count && candidate->free[i].count<bestCount) {
        page=candidate.get();
        index=i;
        bestCount=candidate->free[i].count;
        }

  if(page==nullptr) {
    auto next=std::make_unique<State::Page>();
    next->capacity=std::max(State::PageMeshlets,count);
    auto& device=Resources::device();
    next->vbo=device.ssbo(Tempest::Uninitialized,next->capacity*State::VertexBytes);
    next->ibo=device.ssbo(Tempest::Uninitialized,next->capacity*State::IndexBytes);
    if(Shaders::options().doRtScene)
      next->rtIbo=device.ssbo(Tempest::Uninitialized,next->capacity*State::RtIndexBytes);
    next->free.push_back({0,next->capacity});
    page=next.get();
    state->pages.push_back(std::move(next));
    }

  auto range=std::make_shared<LandscapeRange>();
  range->owner=state;
  range->page=page;
  range->span={page->free[index].first,count};
  page->free[index].first+=count;
  page->free[index].count-=count;
  if(page->free[index].count==0)
    page->free.erase(page->free.begin()+ptrdiff_t(index));
  state->occupied+=count*state->MeshletBytes;

  // These ranges are either new or have passed the frame recycle fence.
  // Uploads precede consuming draws without waiting for the GPU on the CPU.
  page->vbo.updateAsync(mesh.vertices.data(),range->span.first*State::VertexBytes,count*State::VertexBytes);
  page->ibo.updateAsync(mesh.indices8.data(),range->span.first*State::IndexBytes,count*State::IndexBytes);
  if(!page->rtIbo.isEmpty()) {
    // the chunk's indices count from its first vertex, the page's from the page's first one
    assert(mesh.indices.size()==count*PackedMesh::MaxInd);
    const auto base=uint32_t(range->span.first*PackedMesh::MaxVert);
    thread_local std::vector<uint32_t> indices;
    indices.resize(mesh.indices.size());
    for(size_t i=0;i<indices.size();++i)
      indices[i]=mesh.indices[i]+base;
    page->rtIbo.updateAsync(indices.data(),range->span.first*State::RtIndexBytes,count*State::RtIndexBytes);
    }
  return range;
  }

StaticMesh::StaticMesh(const PackedMesh& mesh) :clusterLevels(mesh.clusterLevels) {
  const Vertex* vert=mesh.vertices.data();
  if(Resources::hasGraphicsDevice()) {
    LoadProbe::Vobs::Scope probe("mesh_gpu");
    vbo  = Resources::vbo<Vertex>  (vert,mesh.vertices.size());
    ibo  = Resources::ibo<uint32_t>(mesh.indices.data(),mesh.indices.size());
    ibo8 = Resources::ssbo(mesh.indices8.data(),mesh.indices8.size());
    }

  initSubMeshes(mesh);

  if(!mesh.vertices.empty() && mesh.meshletBounds.size()*PackedMesh::MaxVert==mesh.vertices.size()) {
    meshletSpheres.reserve(mesh.meshletBounds.size());
    for(auto& b:mesh.meshletBounds)
      meshletSpheres.emplace_back(b.pos.x, b.pos.y, b.pos.z, b.r);
    }

  if(Resources::hasGraphicsDevice() && Shaders::options().doRtScene) {
    LoadProbe::Vobs::Scope probe("mesh_gpu");
    for(size_t i=0;i<mesh.subMeshes.size();++i) {
      if(sub[i].material.strand)
        continue; // ribbons have no width until the vertex shader: nothing for a ray to hit
      sub[i].blas = Resources::blas(vbo, ibo, sub[i].rtOffset, sub[i].rtLength);
      }
    }
  }

StaticMesh::StaticMesh(const PackedMesh& mesh, LandscapePages& pages)
  :pageRange(pages.allocate(mesh)) {
  // Landscape takes materials from PackedMesh. The ray scene finds the BLAS of a submesh by its slice (blas()), so
  // with it the submeshes keep their slices: Landscape builds the BLAS from the page (rtGeometry).
  if(Shaders::options().doRtScene) {
    sub.resize(mesh.subMeshes.size());
    for(size_t i=0;i<mesh.subMeshes.size();++i) {
      sub[i].iboOffset = mesh.subMeshes[i].iboOffset;
      sub[i].iboLength = mesh.subMeshes[i].iboLength;
      sub[i].rtOffset  = sub[i].iboOffset;
      sub[i].rtLength  = sub[i].iboLength;
      }
    }
  bbox.assign(mesh.bbox());
  drawn = bbox;
  }

StaticMesh::~StaticMesh() {
  retireRange();
  }

StaticMesh& StaticMesh::operator=(StaticMesh&& other) {
  retireRange();
  vbo=std::move(other.vbo);
  ibo=std::move(other.ibo);
  ibo8=std::move(other.ibo8);
  morph=other.morph;
  sub=std::move(other.sub);
  bbox=other.bbox;
  drawn=other.drawn;
  levels=std::move(other.levels);
  impostor=std::move(other.impostor);
  treeBough=std::move(other.treeBough);
  meshletSpheres=std::move(other.meshletSpheres);
  clusterLevels=std::move(other.clusterLevels);
  ownMesh=other.ownMesh;
  pageRange=std::move(other.pageRange);
  treeHiZBoxes=std::move(other.treeHiZBoxes);
  shadowProbeTriangleEnds=std::move(other.shadowProbeTriangleEnds);
  return *this;
  }

void StaticMesh::retireRange() {
  if(!pageRange)
    return;
  pageRange->retire();
  Resources::recycle(std::move(pageRange));
  }

const Tempest::StorageBuffer& StaticMesh::vboBuffer() const {
  if(pageRange)
    return pageRange->page->vbo;
  return vbo;
  }

const Tempest::StorageBuffer& StaticMesh::ibo8Buffer() const {
  return pageRange ? pageRange->page->ibo : ibo8;
  }

size_t StaticMesh::firstMeshlet() const {
  return pageRange ? pageRange->span.first : 0;
  }

const Tempest::StorageBuffer& StaticMesh::rtIbo() const {
  if(pageRange)
    return pageRange->page->rtIbo;
  return ibo;
  }

size_t StaticMesh::rtFirstIndex(size_t iboOffset) const {
  return firstMeshlet()*PackedMesh::MaxInd + iboOffset;
  }

Tempest::RtGeometry StaticMesh::rtGeometry(size_t iboOffset, size_t iboLength) const {
  if(pageRange)
    return Tempest::RtGeometry(vboBuffer(),sizeof(Vertex),rtIbo(),Tempest::Detail::IndexClass::i32,
                               rtFirstIndex(iboOffset),iboLength);
  return Tempest::RtGeometry(vbo,ibo,iboOffset,iboLength);
  }

void StaticMesh::initSubMeshes(const PackedMesh& mesh) {
  sub.resize(mesh.subMeshes.size());
  for(size_t i=0;i<mesh.subMeshes.size();++i) {
    sub[i].texName   = mesh.subMeshes[i].material.texture;
    sub[i].material  = Resources::loadMaterial(mesh.subMeshes[i].material,
                                             mesh.isUsingAlphaTest && mesh.subMeshes[i].alphaTest,
                                             mesh.subMeshes[i].alphaTestRequired);
    sub[i].iboOffset = mesh.subMeshes[i].iboOffset;
    sub[i].iboLength = mesh.subMeshes[i].iboLength;
    sub[i].texel     = mesh.subMeshes[i].texel;
    sub[i].rtOffset  = sub[i].iboOffset;
    sub[i].rtLength  = clusterLevels.empty() ? sub[i].iboLength : mesh.subMeshes[i].clusterL0Length;
    }
  bbox.assign(mesh.bbox());
  drawn = bbox;
  // The levels of detail (meshlevels.h): the rays take the coarsest one at any distance, the culling keeps them all
  levels.resize(mesh.levels.size());
  for(size_t i=0;i<mesh.levels.size();++i) {
    levels[i].px  = mesh.levels[i].px;
    levels[i].sub = mesh.levels[i].sub;
    Bounds level;
    level.assign(mesh.levels[i].bbox);
    drawn.assign(drawn,level);
    }
  if(!levels.empty())
    for(size_t i=0;i<sub.size() && i<levels.back().sub.size();++i) {
      sub[i].rtOffset = levels.back().sub[i].first;
      sub[i].rtLength = levels.back().sub[i].second;
      }
  strandWidth = mesh.strandWidth;
  initTreeHiZBoxes(mesh);
  }

void StaticMesh::initTreeHiZBoxes(const PackedMesh& mesh) {
  if(!clusterLevels.empty() ||
     std::ranges::none_of(sub, [](const auto& s) { return FoliageTree::isTree(s.material.foliage); }))
    return;

  if(!levels.empty() && DrawCommands::vsmFrameStatsEnabled() && MeshLevels::shadowProbePx()) {
    constexpr size_t stride = PackedMesh::MaxPrim*4;
    const size_t meshlets = mesh.indices8.size()/stride;
    shadowProbeTriangleEnds.resize(meshlets+1);
    for(size_t i=0; i<meshlets; ++i) {
      // As in processMeshlet: the last packed triangle encodes the count when there is padding.
      const auto* tail = &mesh.indices8[(i+1)*stride-4];
      const uint32_t triangles = tail[0]==tail[1] ? uint32_t(tail[2]) : PackedMesh::MaxPrim;
      shadowProbeTriangleEnds[i+1] = shadowProbeTriangleEnds[i]+triangles;
      }
    }

  const auto addBox = [&](size_t offset, size_t length) {
    constexpr float inf = std::numeric_limits<float>::infinity();
    TreeHiZBounds bounds {Tempest::Vec3(inf), uint32_t(offset/PackedMesh::MaxInd),
                          Tempest::Vec3(-inf), uint32_t(length/PackedMesh::MaxInd)};
    for(size_t i=offset; i<offset+length; i+=3) {
      const auto a = mesh.indices[i], b = mesh.indices[i+1], c = mesh.indices[i+2];
      // The meshlet's padding is a repeated index and can refer to an unused zero vertex.
      if(a==b || b==c || a==c)
        continue;
      for(auto id : {a,b,c}) {
        const auto& p = mesh.vertices[id].pos;
        bounds.min.x = std::min(bounds.min.x,p[0]);
        bounds.min.y = std::min(bounds.min.y,p[1]);
        bounds.min.z = std::min(bounds.min.z,p[2]);
        bounds.max.x = std::max(bounds.max.x,p[0]);
        bounds.max.y = std::max(bounds.max.y,p[1]);
        bounds.max.z = std::max(bounds.max.z,p[2]);
        }
      }
    if(bounds.min.x!=inf)
      treeHiZBoxes.push_back(bounds);
    };

  for(const auto& s : sub)
    addBox(s.iboOffset,s.iboLength);
  for(const auto& level : levels)
    for(const auto& [offset,length] : level.sub)
      addBox(offset,length);
  }

uint32_t StaticMesh::shadowProbeTriangles(uint32_t first, uint32_t count) const {
  if(shadowProbeTriangleEnds.empty())
    return 0;
  return shadowProbeTriangleEnds[size_t(first)+count]-shadowProbeTriangleEnds[first];
  }

const StaticMesh::TreeHiZBounds* StaticMesh::treeHiZBox(uint32_t firstMeshlet, uint32_t meshletCount) const {
  const auto it = std::ranges::find_if(treeHiZBoxes, [&](const auto& box) {
    return box.firstMeshlet==firstMeshlet && box.meshletCount==meshletCount;
    });
  return it==treeHiZBoxes.end() ? nullptr : &*it;
  }

const Tempest::AccelerationStructure* StaticMesh::blas(size_t iboOffset, size_t iboLen) const {
  for(auto& i:sub)
    if(i.iboOffset==iboOffset && i.iboLength==iboLen && !i.blas.isEmpty()) {
      return &i.blas;
      }
  return nullptr;
  }

std::pair<uint32_t,uint32_t> StaticMesh::levelMeshlets(size_t iboOffset, size_t iboLen, size_t level) const {
  std::pair<size_t,size_t> slice = {iboOffset,iboLen};
  if(level>0 && level<=levels.size())
    for(size_t i=0;i<sub.size() && i<levels[level-1].sub.size();++i)
      if(sub[i].iboOffset==iboOffset && sub[i].iboLength==iboLen) {
        slice = levels[level-1].sub[i];
        break;
        }
  return {uint32_t(slice.first/PackedMesh::MaxInd), uint32_t(slice.second/PackedMesh::MaxInd)};
  }

MeshLevels::Selection StaticMesh::pickLevel(uint8_t previous, float height, bool allowImpostor) const {
  const size_t glbs = std::min(levels.size(), MeshLevels::MaxLevels);
  const bool card = allowImpostor && glbs>0 && impostor.px>0.f;
  float px[MeshLevels::MaxLevels+1] = {};
  for(size_t i=0; i<glbs; ++i)
    px[i] = levels[i].px;
  px[glbs] = impostor.px;
  const auto stage = MeshLevels::pick(px, glbs+size_t(card), previous, height);
  return {stage, uint8_t(std::min<size_t>(stage, glbs))};
  }

std::pair<size_t,size_t> StaticMesh::rtSlice(size_t iboOffset, size_t iboLen) const {
  for(auto& i:sub)
    if(i.iboOffset==iboOffset && i.iboLength==iboLen)
      return {i.rtOffset,i.rtLength};
  return {iboOffset,iboLen};
  }

StaticMesh::StaticMesh(const Material& mat, std::vector<Resources::Vertex> cvbo, std::vector<uint32_t> cibo) {
  assert(cvbo.size()<=PackedMesh::MaxVert);
  assert(cibo.size()<=PackedMesh::MaxInd);
  const auto texel = TexelDensity::measure(cvbo.data(),cvbo.size(),cibo.data(),0,cibo.size());

  {
    const size_t vert = cvbo.size();
    const size_t prim = cibo.size()/3;

    cvbo.resize(PackedMesh::MaxVert);
    std::vector<uint8_t> ibo(PackedMesh::MaxPrim*4);
    for(size_t i=0; i<cibo.size(); i+=3) {
      size_t at = (i/3)*4;
      ibo[at+0] = uint8_t(cibo[i+0]);
      ibo[at+1] = uint8_t(cibo[i+1]);
      ibo[at+2] = uint8_t(cibo[i+2]);
      ibo[at+3] = 0;
      }
    size_t at = (PackedMesh::MaxPrim)*4 - 4;
    ibo[at + 0] = uint8_t(0);
    ibo[at + 1] = uint8_t(0);
    ibo[at + 2] = uint8_t(prim);
    ibo[at + 3] = uint8_t(vert);

    if(Resources::hasGraphicsDevice()) {
      LoadProbe::Vobs::Scope probe("mesh_gpu");
      ibo8 = Resources::ssbo(ibo.data(),ibo.size());
      }
    }
  if(Resources::hasGraphicsDevice()) {
    LoadProbe::Vobs::Scope probe("mesh_gpu");
    vbo  = Resources::vbo<Vertex>(cvbo.data(),cvbo.size());
    ibo  = Resources::ibo(cibo.data(),cibo.size());
    }

  sub.resize(1);
  for(size_t i=0;i<1;++i) {
    sub[i].texName   = "";
    sub[i].material  = mat;
    sub[i].iboLength = PackedMesh::MaxInd;
    sub[i].rtLength  = PackedMesh::MaxInd;
    sub[i].texel     = texel;
    if(Resources::hasGraphicsDevice() && Shaders::options().doRtScene) {
      LoadProbe::Vobs::Scope probe("mesh_gpu");
      sub[i].blas = Resources::blas(vbo, ibo, 0, ibo.size());
      }
    }
  bbox.assign(cvbo);
  drawn = bbox;
  }
