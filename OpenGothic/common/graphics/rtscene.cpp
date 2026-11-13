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
  return a.blas==b.blas && a.id==b.id && a.mask==b.mask && a.flags==b.flags && sameMatrix(a.mat, b.mat);
  }

bool sameDesc(const RtScene::RtObjectDesc& a, const RtScene::RtObjectDesc& b) {
  return a.instanceId==b.instanceId && a.firstPrimitive==b.firstPrimitive && a.bits==b.bits;
  }

// The arrays of the buckets with at least 1024 entries, as DrawBuckets: Tempest gives such an array a capacity of 8192
// in the pipeline layout, so a new bucket (a vob's mesh, a chunk's texture in a page) does not make every ray query
// pipeline compile anew for the next array size. The tail is never read (bucket ids are below the real count).
template<class T>
std::vector<const T*> padded(const std::vector<const T*>& v) {
  constexpr size_t Rounding = 1024;
  auto ret = v;
  ret.resize((v.size()+Rounding-1)/Rounding*Rounding, nullptr);
  return ret;
  }

RtInstanceFlags instanceFlags(Material::AlphaFunc alpha) {
  const auto flags = (alpha==Material::Solid || alpha==Material::Water) ? RtInstanceFlags::Opaque : RtInstanceFlags::NonOpaque;
  return flags | RtInstanceFlags::CullFlip;
  }
}

uint32_t RtScene::Ranges::alloc(uint32_t count) {
  auto it = bySize.lower_bound(std::make_pair(count, uint32_t(0)));
  if(it==bySize.end()) {
    const uint32_t first = end;
    end += count;
    return first;
    }
  const auto [size, first] = *it;
  bySize.erase(it);
  byFirst.erase(first);
  if(size>count) {
    byFirst.emplace(first+count, size-count);
    bySize.emplace(size-count, first+count);
    }
  return first;
  }

void RtScene::Ranges::free(uint32_t first, uint32_t count) {
  auto next = byFirst.lower_bound(first);
  if(next!=byFirst.end() && first+count==next->first) {
    count += next->second;
    bySize.erase(std::make_pair(next->second, next->first));
    next = byFirst.erase(next);
    }
  if(next!=byFirst.begin()) {
    auto prev = std::prev(next);
    if(prev->first+prev->second==first) {
      first  = prev->first;
      count += prev->second;
      bySize.erase(std::make_pair(prev->second, prev->first));
      byFirst.erase(prev);
      }
    }
  if(first+count==end) {
    end = first; // the tail is no range: a later one grows from here
    return;
    }
  byFirst.emplace(first, count);
  bySize.emplace(count, first);
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

uint32_t RtScene::bucketId(const Texture2d* t, const StorageBuffer* v, const StorageBuffer* i) {
  const BucketKey key = {t, v, i};
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

RtScene::RtObjectDesc RtScene::objectDesc(const Geometry& geom, Material::AlphaFunc alpha) {
  RtObjectDesc desc = {};
  desc.instanceId     = bucketId(geom.tex, geom.vbo, geom.ibo);
  desc.firstPrimitive = uint32_t(geom.firstIndex/3) & 0x00FFFFFF; // 24 bit for primmitive + 8 for utility
  if(alpha==Material::Solid)
    desc.bits |= 0x1;
  if(alpha==Material::Water)
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
  s.inst = RtInstance(); // no BLAS: inactive until set
  markSlot(slot);
  return slot;
  }

void RtScene::releaseRange(Slot& s) {
  // the entries stay in the copy as they were: no instance points at them any more
  if(s.descCount>0)
    entries.free(s.descFirst, s.descCount);
  s.descFirst = 0;
  s.descCount = 0;
  }

void RtScene::freeSlot(uint32_t slot) {
  auto& s = slots[slot];
  if(s.landscape) {
    --landscapeSlots;
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
    landscapeTriangles -= s.triangles;
    if(landscapeBytes>=0)
      landscapeBytes = s.blasBytes<0 ? -1 : landscapeBytes - s.blasBytes;
    landscapeChanged = true;
#endif
    }
  releaseRange(s);
  s.used      = false;
  s.landscape = false;
  s.inst      = RtInstance(); // no BLAS: inactive
  s.triangles = 0;
  s.blasBytes = 0;
  markSlot(slot);
  freeSlots.push(slot);
  }

void RtScene::writeSlot(uint32_t slot, RtInstance ix, Material::AlphaFunc alpha, const Geometry* geom, size_t count) {
  thread_local std::vector<RtObjectDesc> desc;
  desc.resize(count);
  uint64_t triangles = 0;
  for(size_t i=0; i<count; ++i) {
    desc[i]    = objectDesc(geom[i], alpha);
    triangles += geom[i].indices/3;
    }

  auto& s = slots[slot];
  s.triangles = triangles;
  bool changed = false;
  if(s.descCount!=count) {
    releaseRange(s);
    s.descFirst = entries.alloc(uint32_t(count));
    s.descCount = uint32_t(count);
    if(descCpu.cpu.size()<entries.end*sizeof(RtObjectDesc))
      descCpu.resize(entries.end*sizeof(RtObjectDesc));
    changed = true;
    }
  auto* at = reinterpret_cast<RtObjectDesc*>(descCpu.cpu.data()) + s.descFirst;
  for(size_t i=0; i<count && !changed; ++i)
    changed = !sameDesc(at[i], desc[i]);
  if(changed) {
    std::memcpy(at, desc.data(), count*sizeof(RtObjectDesc));
    descCpu.mark(s.descFirst*sizeof(RtObjectDesc), count*sizeof(RtObjectDesc));
    }

  ix.id = s.descFirst;
  if(!changed && sameInstance(s.inst, ix))
    return;
  s.inst = ix;
  markSlot(slot);
  }

void RtScene::setSlot(uint32_t slot, const Matrix4x4& pos, const AccelerationStructure& blas,
                      const Material& mat, const StaticMesh& mesh, size_t firstIndex, size_t iboLength, bool visible) {
  RtInstance ix;
  ix.mat   = pos;
  ix.blas  = &blas;
  ix.flags = instanceFlags(mat.alpha);

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

  const Geometry geom = {mat.tex, &mesh.vboBuffer(), &mesh.rtIbo(), mesh.rtFirstIndex(firstIndex), iboLength};
  writeSlot(slot, ix, mat.alpha, &geom, 1);
  }

void RtScene::setLandscapeSlot(uint32_t slot, const AccelerationStructure& blas, Material::AlphaFunc alpha,
                               const Geometry* geom, size_t count, bool visible) {
  auto& s = slots[slot];
  const bool added = !s.landscape;
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  const uint64_t prevTriangles = s.triangles;
  const int64_t  prevBytes     = s.blasBytes;
#endif
  RtInstance ix;
  ix.mat   = Matrix4x4::mkIdentity();
  ix.blas  = &blas;
  ix.flags = instanceFlags(alpha);
  ix.mask  = visible ? 0xFF : 0;
  writeSlot(slot, ix, alpha, geom, count);

  if(added) {
    s.landscape = true;
    ++landscapeSlots;
    }
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  s.blasBytes = profileByteSize(blas);
  if(!added) {
    landscapeTriangles -= prevTriangles;
    if(landscapeBytes>=0)
      landscapeBytes = prevBytes<0 ? -1 : landscapeBytes - prevBytes;
    }
  landscapeTriangles += s.triangles;
  if(landscapeBytes>=0)
    landscapeBytes = s.blasBytes<0 ? -1 : landscapeBytes + s.blasBytes;
  landscapeChanged = true;
#endif
  }

void RtScene::markSlot(uint32_t slot) {
  auto& s = slots[slot];
  if(s.dirty)
    return;
  s.dirty = true;
  dirtySlots.push_back(slot);
  }

void RtScene::packInstance(size_t index, const RtInstance& inst) {
  const size_t sz = Resources::device().rtInstanceSize();
  Resources::device().packRtInstance(tlas, inst, instanceCpu.cpu.data()+index*sz);
  instanceCpu.mark(index*sz, sz);
  }

RtScene::Stats RtScene::stats() const {
  Stats ret;
  ret.slots          = slots.size() - freeSlots.size();
  ret.landscapeSlots = landscapeSlots;
  ret.entries        = entries.end;
  for(auto& s:slots)
    if(s.used && s.landscape && s.inst.blas!=nullptr && !s.inst.blas->isEmpty() && s.inst.mask!=0)
      ++ret.landscapeShown;
  return ret;
  }

void RtScene::commit(Encoder<CommandBuffer>& cmd, uint8_t fId) {
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  const auto start = std::chrono::steady_clock::now();
  logLandscape();
#endif
  auto& device = Resources::device();
  const char* reason = nullptr;

  if(dirtySlots.empty() && !tlas.isEmpty())
    return;

  // an instance at least: no TLAS of nothing, the spare one holds zeros (inactive)
  const size_t count = std::max<size_t>(slots.size(), 1);
  bool everything = false;
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
  const size_t entriesCount = std::max<size_t>(entries.end, 1); // never an empty buffer to bind
  if(descCpu.cpu.size()<entriesCount*sizeof(RtObjectDesc))
    descCpu.resize(entriesCount*sizeof(RtObjectDesc));

  if(everything) {
    // a new TLAS: every instance anew, its BLAS list too (Metal)
    for(uint32_t i=0; i<slots.size(); ++i)
      markSlot(i);
    }
  const size_t dirtyCount = dirtySlots.size();
  for(auto i:dirtySlots) {
    auto& s = slots[i];
    s.dirty = false;
    packInstance(i, s.inst);
    }
  dirtySlots.clear();

  // the arrays of the buckets anew only when a bucket is new: once a frame, rarely after the world is loaded
  if(bucketsUploaded!=bucketTex.size()) {
    Resources::recycle(std::move(tex));
    Resources::recycle(std::move(vbo));
    Resources::recycle(std::move(ibo));
    tex = device.descriptors(padded(bucketTex));
    vbo = device.descriptors(padded(bucketVbo));
    ibo = device.descriptors(padded(bucketIbo));
    textureGeneration = Resources::textureGeneration();
    bucketsUploaded   = bucketTex.size();
    reason            = "bucket";
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
  for(auto& s:slots)
    if(s.used && s.inst.mask!=0)
      add(s.inst.blas, s.triangles);
  char line[400] = {};
  std::snprintf(line, sizeof(line),
                "render: rt scene reason=%s instances=%zu blas=%zu triangles=%" PRIu64 " tlas_bytes=%" PRId64
                " blas_bytes=%" PRId64 " build_ms=%.3f slots=%zu buckets=%zu landscape_slots=%zu"
                " landscape_triangles=%" PRIu64 " landscape_blas_bytes=%" PRId64 " entries=%u",
                reason, active, unique.size(), triangles, profileByteSize(tlas), blasBytes, buildMs,
                slots.size(), bucketTex.size(), landscapeSlots, landscapeTriangles, landscapeBytes, entries.end);
  Log::i(line);
  }

void RtScene::logLandscape() {
  // The landscape of the scene once it settles: at most a line a second while chunks come and go, and the last change
  // a second later at the latest (the frame calls commit with or without changes)
  using namespace std::chrono;
  if(!landscapeChanged)
    return;
  const auto now = steady_clock::now();
  if(now-landscapeLogged<seconds(1))
    return;
  landscapeChanged = false;
  landscapeLogged  = now;
  char line[300] = {};
  std::snprintf(line, sizeof(line),
                "render: rt landscape slots=%zu triangles=%" PRIu64 " blas_bytes=%" PRId64 " tlas_bytes=%" PRId64
                " instances=%zu entries=%u buckets=%zu",
                landscapeSlots, landscapeTriangles, landscapeBytes, profileByteSize(tlas),
                slots.size() - freeSlots.size(), entries.end, bucketTex.size());
  Log::i(line);
  }
#endif

bool RtScene::refreshTextures() {
  const auto generation=Resources::textureGeneration();
  if(textureGeneration==generation || bucketsUploaded==size_t(-1))
    return false;
  Resources::recycle(std::move(tex));
  tex=Resources::device().descriptors(padded(bucketTex));
  textureGeneration=generation;
  return true;
  }
