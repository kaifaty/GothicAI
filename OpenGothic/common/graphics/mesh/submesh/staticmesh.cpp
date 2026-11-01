#include "staticmesh.h"
#include "loadprobe.h"

#include <cassert>
#include <algorithm>
#include <atomic>
#include "gothic.h"

#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/shaders.h"

struct LandscapePages::State {
  static constexpr size_t PageMeshlets = 4096;
  static constexpr size_t VertexBytes = PackedMesh::MaxVert*sizeof(StaticMesh::Vertex);
  static constexpr size_t IndexBytes = PackedMesh::MaxPrim*4;
  static constexpr size_t MeshletBytes = VertexBytes+IndexBytes;

  struct Span {
    size_t first = 0;
    size_t count = 0;
    };
  struct Page {
    Tempest::StorageBuffer vbo;
    Tempest::StorageBuffer ibo;
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
      const auto bytes=span.count*LandscapePages::State::MeshletBytes;
      if(retired)
        owner->retired-=bytes;
      else
        owner->occupied-=bytes;
      owner->release(*page,span);
      }

    void retire() {
      assert(!retired);
      const auto bytes=span.count*LandscapePages::State::MeshletBytes;
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
    ret.freeBytes+=page->capacity*State::MeshletBytes;
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
  state->occupied+=count*State::MeshletBytes;

  // These ranges are either new or have passed the frame recycle fence.
  // Uploads precede consuming draws without waiting for the GPU on the CPU.
  page->vbo.updateAsync(mesh.vertices.data(),range->span.first*State::VertexBytes,count*State::VertexBytes);
  page->ibo.updateAsync(mesh.indices8.data(),range->span.first*State::IndexBytes,count*State::IndexBytes);
  return range;
  }

StaticMesh::StaticMesh(const PackedMesh& mesh) {
  const Vertex* vert=mesh.vertices.data();
  if(Resources::hasGraphicsDevice()) {
    LoadProbe::Vobs::Scope probe("mesh_gpu");
    vbo  = Resources::vbo<Vertex>  (vert,mesh.vertices.size());
    ibo  = Resources::ibo<uint32_t>(mesh.indices.data(),mesh.indices.size());
    ibo8 = Resources::ssbo(mesh.indices8.data(),mesh.indices8.size());
    }

  initSubMeshes(mesh);

  if(Resources::hasGraphicsDevice() && Shaders::options().doRtScene) {
    LoadProbe::Vobs::Scope probe("mesh_gpu");
    for(size_t i=0;i<mesh.subMeshes.size();++i) {
      sub[i].blas = Resources::blas(vbo, ibo, sub[i].iboOffset, sub[i].iboLength);
      }
    }
  }

StaticMesh::StaticMesh(const PackedMesh& mesh, LandscapePages& pages)
  :pageRange(pages.allocate(mesh)) {
  assert(!Shaders::options().doRtScene);
  // Landscape takes materials from PackedMesh; RT keeps the whole-mesh path.
  bbox.assign(mesh.bbox());
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
  pageRange=std::move(other.pageRange);
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

void StaticMesh::initSubMeshes(const PackedMesh& mesh) {
  sub.resize(mesh.subMeshes.size());
  for(size_t i=0;i<mesh.subMeshes.size();++i) {
    sub[i].texName   = mesh.subMeshes[i].material.texture;
    sub[i].material  = Resources::loadMaterial(mesh.subMeshes[i].material,
                                             mesh.isUsingAlphaTest && mesh.subMeshes[i].alphaTest,
                                             mesh.subMeshes[i].alphaTestRequired);
    sub[i].iboOffset = mesh.subMeshes[i].iboOffset;
    sub[i].iboLength = mesh.subMeshes[i].iboLength;
    }
  bbox.assign(mesh.bbox());
  }

const Tempest::AccelerationStructure* StaticMesh::blas(size_t iboOffset, size_t iboLen) const {
  for(auto& i:sub)
    if(i.iboOffset==iboOffset && i.iboLength==iboLen && !i.blas.isEmpty()) {
      return &i.blas;
      }
  return nullptr;
  }

StaticMesh::StaticMesh(const Material& mat, std::vector<Resources::Vertex> cvbo, std::vector<uint32_t> cibo) {
  assert(cvbo.size()<=PackedMesh::MaxVert);
  assert(cibo.size()<=PackedMesh::MaxInd);

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
    //sub[i].iboLength = Gothic::options().doMeshShading ? PackedMesh::MaxInd : ibo.size();
    sub[i].iboLength = PackedMesh::MaxInd;
    if(Resources::hasGraphicsDevice() && Shaders::options().doRtScene) {
      LoadProbe::Vobs::Scope probe("mesh_gpu");
      sub[i].blas = Resources::blas(vbo, ibo, 0, ibo.size());
      }
    }
  bbox.assign(cvbo);
  }
