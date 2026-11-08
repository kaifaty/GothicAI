#include "rtscene.h"

#include <Tempest/Log>

#include <algorithm>
#include <cstring>

#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/shaders.h"
#include "frameprofile.h"

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <unordered_set>
#endif

using namespace Tempest;

namespace {
constexpr size_t MirrorBlock = 64;   // dirty granularity of a mirror, bytes
constexpr size_t PatchChunk  = 512;  // bytes one workgroup of the patch shader copies

bool sameMatrix(const Matrix4x4& a, const Matrix4x4& b) {
  return std::memcmp(&a, &b, sizeof(a))==0;
  }

bool sameInstance(const RtInstance& a, const RtInstance& b) {
  return a.blas==b.blas && a.mask==b.mask && a.flags==b.flags && sameMatrix(a.mat, b.mat);
  }

bool sameDesc(const RtScene::RtObjectDesc& a, const RtScene::RtObjectDesc& b) {
  return a.instanceId==b.instanceId && a.firstPrimitive==b.firstPrimitive && a.bits==b.bits;
  }
}

size_t RtScene::BucketHash::operator()(const BucketKey& k) const {
  const auto h = [](const void* p) { return std::hash<const void*>()(p); };
  size_t ret = h(k.tex);
  ret ^= h(k.vbo) + 0x9e3779b97f4a7c15ull + (ret<<6) + (ret>>2);
  ret ^= h(k.ibo) + 0x9e3779b97f4a7c15ull + (ret<<6) + (ret>>2);
  return ret;
  }

void RtScene::Mirror::resize(size_t bytes) {
  const size_t prev = cpu.size();
  cpu.resize(bytes, 0);
  dirty.resize((bytes+MirrorBlock*32-1)/(MirrorBlock*32), 0);
  if(bytes>prev)
    mark(prev, bytes-prev); // the GPU copy of the new bytes is uninitialized
  }

void RtScene::Mirror::mark(size_t offset, size_t size) {
  if(size==0)
    return;
  const size_t last = (offset+size-1)/MirrorBlock;
  for(size_t b=offset/MirrorBlock; b<=last; ++b)
    dirty[b/32] |= 1u << (b%32);
  anyDirty = true;
  }

void RtScene::Mirror::commit(Encoder<CommandBuffer>& cmd, uint8_t fId, StorageBuffer& gpu) {
  auto& device = Resources::device();
  const size_t size = cpu.size();
  if(gpu.byteSize()<size) {
    // the frames in flight keep the old buffer
    Resources::recycle(std::move(gpu));
    const size_t cap = ((size + size/2 + MirrorBlock-1)/MirrorBlock)*MirrorBlock;
    gpu = device.ssbo(BufferHeap::Device, Uninitialized, cap);
    mark(0, size);
    }
  if(!anyDirty)
    return;

  struct Path {
    uint32_t dst, src, size;
    };
  thread_local std::vector<Path> path;
  path.clear();
  size_t payload = 0;
  const size_t blocks = (size+MirrorBlock-1)/MirrorBlock;
  for(size_t b=0; b<blocks; ) {
    if(b%32==0 && dirty[b/32]==0) {
      b += 32;
      continue;
      }
    if((dirty[b/32] & (1u << (b%32)))==0) {
      ++b;
      continue;
      }
    size_t e = b+1;
    while(e<blocks && (dirty[e/32] & (1u << (e%32)))!=0)
      ++e;
    const size_t begin = b*MirrorBlock;
    const size_t end   = std::min(e*MirrorBlock, size);
    for(size_t at=begin; at<end; at+=PatchChunk) {
      const size_t sz = std::min(PatchChunk, end-at);
      path.push_back({uint32_t(at), uint32_t(payload), uint32_t(sz)});
      payload += sz;
      }
    b = e;
    }
  std::fill(dirty.begin(), dirty.end(), 0u);
  anyDirty = false;
  if(path.empty())
    return;

  // header (offsets in uints, as the shader reads them) and payload, as InstanceStorage::commit
  const size_t header = path.size()*sizeof(Path);
  patchCpu.resize(header + payload);
  for(auto& p:path) {
    std::memcpy(patchCpu.data()+header+p.src, cpu.data()+p.dst, p.size);
    p.src  = uint32_t((header+p.src)/4);
    p.dst /= 4;
    p.size/= 4;
    }
  std::memcpy(patchCpu.data(), path.data(), header);

  auto& stage = patch[fId];
  if(stage.byteSize()<patchCpu.size()) {
    Resources::recycle(std::move(stage));
    stage = device.ssbo(BufferHeap::Upload, Uninitialized, patchCpu.size() + patchCpu.size()/2);
    }
  // host-visible memory of this frame: the frame that used it last has completed
  stage.update(patchCpu.data(), 0, patchCpu.size());

  cmd.setFramebuffer({});
  cmd.setBinding(0, gpu);
  cmd.setBinding(1, stage);
  cmd.setPipeline(Shaders::inst().patch);
  cmd.dispatch(path.size());
  }


RtScene::RtScene() {
  }

RtScene::~RtScene() {
  }

bool RtScene::isSupported(const Material& mat, Category cat) {
  if(cat!=RtScene::Landscape && cat!=RtScene::Static)
    return false; // not supported
  if(mat.alpha!=Material::Solid && mat.alpha!=Material::AlphaTest && mat.alpha!=Material::Water)
    return false; // not supported
  return true;
  }

bool RtScene::isMergedLandscape(const Material& mat, Category cat) {
  return cat==RtScene::Landscape && (mat.alpha==Material::Solid || mat.alpha==Material::AlphaTest);
  }

uint32_t RtScene::bucketId(const Material& mat, const StaticMesh& mesh) {
  const BucketKey key = {mat.tex, &mesh.vbo, &mesh.ibo};
  auto it = buckets.find(key);
  if(it!=buckets.end())
    return it->second;
  const uint32_t id = uint32_t(bucketTex.size());
  buckets.emplace(key, id);
  bucketTex.push_back(key.tex);
  bucketVbo.push_back(key.vbo);
  bucketIbo.push_back(key.ibo);
  return id;
  }

RtScene::RtObjectDesc RtScene::objectDesc(const Material& mat, const StaticMesh& mesh, size_t firstIndex) {
  RtObjectDesc desc = {};
  desc.instanceId     = bucketId(mat,mesh);
  desc.firstPrimitive = uint32_t(firstIndex/3) & 0x00FFFFFF; // 24 bit for primmitive + 8 for utility
  if(mat.alpha==Material::Solid)
    desc.bits |= 0x1;
  if(mat.alpha==Material::Water)
    desc.bits |= 0x2;
  return desc;
  }

uint32_t RtScene::allocSlot() {
  uint32_t slot = 0;
  if(!freeSlots.empty()) {
    slot = freeSlots.top();
    freeSlots.pop();
    } else {
    slot = uint32_t(slots.size());
    slots.emplace_back();
    }
  auto& s = slots[slot];
  s.used = true;
  s.inst = RtInstance();
  s.inst.blas = nullptr; // inactive until set
  markSlot(slot);
  return slot;
  }

void RtScene::freeSlot(uint32_t slot) {
  auto& s = slots[slot];
  s.used      = false;
  s.inst      = RtInstance();
  s.inst.blas = nullptr;
  s.triangles = 0;
  markSlot(slot);
  freeSlots.push(slot);
  }

void RtScene::setSlot(uint32_t slot, const Matrix4x4& pos, const AccelerationStructure& blas,
                      const Material& mat, const StaticMesh& mesh, size_t firstIndex, size_t iboLength, bool visible) {
  RtInstance ix;
  ix.mat  = pos;
  ix.blas = &blas;
  if(mat.alpha==Material::Water)
    ix.flags = RtInstanceFlags::Opaque;
  else if(mat.alpha!=Material::Solid)
    ix.flags = RtInstanceFlags::NonOpaque;
  else
    ix.flags = RtInstanceFlags::Opaque;
  ix.flags = ix.flags | RtInstanceFlags::CullFlip;

  if(!visible)
    ix.mask = 0;
  else if(mat.alpha==Material::Solid)
    ix.mask = CullMask::CM_Opaque;
  else if(mat.alpha==Material::AlphaTest)
    ix.mask = CullMask::CM_Transparent;
  else if(mat.alpha==Material::Water)
    ix.mask = CullMask::CM_Water;
  else
    ix.mask = CullMask::CM_Unknown;

  const RtObjectDesc desc = objectDesc(mat, mesh, firstIndex);
  auto& s = slots[slot];
  s.triangles = uint32_t(iboLength/3);
  if(sameInstance(s.inst, ix) && sameDesc(s.desc, desc))
    return;
  s.inst = ix;
  s.desc = desc;
  markSlot(slot);
  }

void RtScene::markSlot(uint32_t slot) {
  auto& s = slots[slot];
  if(s.dirty)
    return;
  s.dirty = true;
  dirtySlots.push_back(slot);
  }

void RtScene::beginLandscape() {
  for(auto& m:landscape) {
    m.geom.clear();
    m.desc.clear();
    }
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  landscapeTriangles = 0;
#endif
  landscapeDirty = true;
  }

void RtScene::addLandscape(const Material& mat, const StaticMesh& mesh, size_t firstIndex, size_t iboLength) {
  auto& m = landscape[mat.alpha==Material::Solid ? 0 : 1];
  m.geom.push_back({mesh.vbo, mesh.ibo, firstIndex, iboLength});
  m.desc.push_back(objectDesc(mat, mesh, firstIndex));
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  landscapeTriangles += iboLength/3;
#endif
  }

void RtScene::buildLandscape() {
  auto& device = Resources::device();
  for(auto& m:landscape) {
    // the frames in flight keep the old BLAS; the instance that held it lets it go when packed anew
    Resources::recycle(std::move(m.blas));
    if(!m.geom.empty())
      m.blas = device.blas(m.geom);
    }
  prefix = uint32_t(landscape[0].desc.size() + landscape[1].desc.size());
  landscapeDirty = false;
  }

void RtScene::packInstance(size_t index, const RtInstance& inst) {
  const size_t sz = Resources::device().rtInstanceSize();
  Resources::device().packRtInstance(tlas, inst, instanceCpu.cpu.data()+index*sz);
  instanceCpu.mark(index*sz, sz);
  }

void RtScene::commit(Encoder<CommandBuffer>& cmd, uint8_t fId) {
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  const auto start = std::chrono::steady_clock::now();
#endif
  auto& device = Resources::device();
  const char* reason = nullptr;

  // every instance and entry anew: the landscape moves the slots' entries of rtDesc, a new TLAS its BLAS list (Metal)
  bool everything = false;
  if(landscapeDirty) {
    const uint32_t prevPrefix = prefix;
    buildLandscape();
    reason     = "landscape";
    everything = prefix!=prevPrefix;
    }
  const bool landscapeBuilt = reason!=nullptr;
  if(dirtySlots.empty() && !landscapeBuilt && !tlas.isEmpty())
    return;

  const size_t count = LandscapeInstances + slots.size();
  if(tlas.capacity()<count) {
    Resources::recycle(std::move(tlas));
    tlas = device.tlas(Tempest::Uninitialized, count + count/2);
    // the slots hold zeros for the new structure: inactive, holding no BLAS of it
    std::fill(instanceCpu.cpu.begin(), instanceCpu.cpu.end(), uint8_t(0));
    everything = true;
    }
  const size_t isz = device.rtInstanceSize();
  if(instanceCpu.cpu.size()<count*isz)
    instanceCpu.resize(count*isz);
  const size_t entries = std::max<size_t>(prefix + slots.size(), 1); // never an empty buffer to bind
  if(descCpu.cpu.size()<entries*sizeof(RtObjectDesc))
    descCpu.resize(entries*sizeof(RtObjectDesc));

  if(landscapeBuilt || everything) {
    // the landscape: identity, one entry of rtDesc per geometry from the start of the table
    uint32_t first = 0;
    for(size_t i=0; i<LandscapeInstances; ++i) {
      auto& m = landscape[i];
      RtInstance ix;
      ix.mat   = Matrix4x4::mkIdentity();
      ix.id    = first;
      ix.flags = (i==0 ? RtInstanceFlags::Opaque : RtInstanceFlags::NonOpaque) | RtInstanceFlags::CullFlip;
      ix.blas  = m.blas.isEmpty() ? nullptr : &m.blas;
      packInstance(i, ix);
      if(!m.desc.empty()) {
        std::memcpy(descCpu.cpu.data()+first*sizeof(RtObjectDesc), m.desc.data(), m.desc.size()*sizeof(RtObjectDesc));
        descCpu.mark(first*sizeof(RtObjectDesc), m.desc.size()*sizeof(RtObjectDesc));
        }
      first += uint32_t(m.desc.size());
      }
    }

  if(everything) {
    for(uint32_t i=0; i<slots.size(); ++i)
      markSlot(i);
    }
  const size_t dirtyCount = dirtySlots.size();
  for(auto i:dirtySlots) {
    auto& s = slots[i];
    s.dirty = false;
    RtInstance ix = s.inst;
    ix.id = prefix + i;
    packInstance(LandscapeInstances + i, ix);
    const size_t at = (prefix + i)*sizeof(RtObjectDesc);
    std::memcpy(descCpu.cpu.data()+at, &s.desc, sizeof(RtObjectDesc));
    descCpu.mark(at, sizeof(RtObjectDesc));
    }
  dirtySlots.clear();

  // the arrays of the buckets anew only when a bucket is new: once a frame, rarely after the world is loaded
  if(bucketsUploaded!=bucketTex.size()) {
    Resources::recycle(std::move(tex));
    Resources::recycle(std::move(vbo));
    Resources::recycle(std::move(ibo));
    tex = device.descriptors(bucketTex);
    vbo = device.descriptors(bucketVbo);
    ibo = device.descriptors(bucketIbo);
    textureGeneration = Resources::textureGeneration();
    bucketsUploaded   = bucketTex.size();
    if(reason==nullptr)
      reason = "bucket";
    }

  cmd.setDebugMarker("RT-TLAS");
  instanceCpu.commit(cmd, fId, instanceGpu);
  descCpu    .commit(cmd, fId, rtDesc);
  cmd.buildTlas(tlas, instanceGpu, count);
  cmd.setDebugMarker("");
  FrameProfile::add(FrameProfile::Amount::RtTlasBuilds, 1);
  FrameProfile::add(FrameProfile::Amount::RtDirtySlots, dirtyCount);

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  if(reason!=nullptr)
    logBuild(reason, std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
#else
  (void)reason;
#endif
  }

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
void RtScene::logBuild(const char* reason, double buildMs) const {
  // Size of the scene: instances that rays meet and unique BLAS among them, triangles summed over them;
  // bytes are -1 when the backend keeps no size (Metal).
  std::unordered_set<const AccelerationStructure*> unique;
  int64_t  blasBytes = 0;
  uint64_t triangles = 0;
  size_t   active    = 0;
  const auto add = [&](const AccelerationStructure* blas, uint64_t tri) {
    if(blas==nullptr)
      return;
    ++active;
    triangles += tri;
    if(!unique.insert(blas).second)
      return;
    const int64_t bytes = profileByteSize(*blas);
    blasBytes = (bytes<0 || blasBytes<0) ? -1 : blasBytes+bytes;
    };
  if(!landscape[0].blas.isEmpty() || !landscape[1].blas.isEmpty()) {
    // the merged landscape counted as before: its triangles once
    add(landscape[0].blas.isEmpty() ? nullptr : &landscape[0].blas, landscapeTriangles);
    add(landscape[1].blas.isEmpty() ? nullptr : &landscape[1].blas, 0);
    }
  for(auto& s:slots)
    if(s.used && s.inst.mask!=0)
      add(s.inst.blas, s.triangles);
  char line[320] = {};
  std::snprintf(line, sizeof(line),
                "render: rt scene reason=%s instances=%zu blas=%zu triangles=%" PRIu64 " tlas_bytes=%" PRId64
                " blas_bytes=%" PRId64 " build_ms=%.3f slots=%zu buckets=%zu",
                reason, active, unique.size(), triangles, profileByteSize(tlas), blasBytes, buildMs,
                slots.size(), bucketTex.size());
  Log::i(line);
  }
#endif

bool RtScene::refreshTextures() {
  const auto generation=Resources::textureGeneration();
  if(textureGeneration==generation || bucketsUploaded==size_t(-1))
    return false;
  Resources::recycle(std::move(tex));
  tex=Resources::device().descriptors(bucketTex);
  textureGeneration=generation;
  return true;
  }
