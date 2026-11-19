#include "drawclusters.h"
#include "shaders.h"

#include <cassert>
#include <cstddef>
#include <cstring>

#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/mesh/submesh/animmesh.h"

using namespace Tempest;

DrawClusters::DrawClusters() {
  scratch.header.reserve(1024);
  scratch.patch .reserve(1024);
  }

DrawClusters::~DrawClusters() {
  }

uint32_t DrawClusters::alloc(const PackedMesh::Cluster* cx, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId,
                             uint16_t commandId, std::span<const PackedMesh::ClusterLevel> levels, uint32_t instanceId) {
  if(commandId==uint16_t(-1))
    return uint32_t(-1);

  const auto ret = implAlloc(meshletCount);
  assert(levels.empty() || levels.size()==meshletCount);
  assert(cx!=nullptr || instanceId!=uint32_t(-1));
  if(!levels.empty())
    allocLevels(ret, levels);
  for(size_t i=0; i<meshletCount; ++i) {
    Cluster c;
    // Instanced clusters get their world bounds when the instance matrix is set.
    if(cx!=nullptr) {
      c.pos = cx[i].pos;
      c.r   = cx[i].r;
      }
    c.bucketId     = bucketId;
    c.commandId    = commandId;
    c.firstMeshlet = uint32_t(firstMeshlet + i);
    c.meshletCount = 1;
    c.instanceId   = instanceId;

    clusters[ret+i] = c;
    }

  clustersDurty.resize((clusters.size() + 32 - 1)/32);
  markClusters(ret, meshletCount);
  return uint32_t(ret);
  }

uint32_t DrawClusters::alloc(const Bucket& bucket, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId, uint16_t commandId) {
  const auto ret = implAlloc(1);

  Cluster& c = clusters[ret];
  if(bucket.staticMesh!=nullptr)
    c.r = bucket.staticMesh->drawn.rConservative + bucket.mat.waveMaxAmplitude +
          FoliageTree::travel(bucket.mat.foliage, bucket.staticMesh->bbox.bbox[0], bucket.staticMesh->bbox.bbox[1]); else
    c.r = bucket.animMesh->bbox.rConservative;
  c.bucketId     = bucketId;
  c.commandId    = commandId;
  c.firstMeshlet = uint32_t(firstMeshlet);
  c.meshletCount = uint32_t(meshletCount);
  c.instanceId   = uint32_t(-1);

  clustersDurty.resize((clusters.size() + 32 - 1)/32);
  markClusters(ret, 1);
  return uint32_t(ret);
  }

void DrawClusters::free(uint32_t id, uint32_t numCluster) {
  freeLevels(id, numCluster);
  for(size_t i=0; i<numCluster; ++i) {
    clusters[id + i]              = Cluster();
    clusters[id + i].r            = -1;
    clusters[id + i].meshletCount = 0;
    markClusters(id + i);
    }

  Range r = {id, id+numCluster};
  auto at = std::lower_bound(freeList.begin(),freeList.end(),r,[](const Range& l, const Range& r){
    return l.begin<r.begin;
    });
  at = freeList.insert(at,r);
  auto next = at+1;
  if(next!=freeList.end() && at->end==next->begin) {
    next->begin = at->begin;
    at = freeList.erase(at);
    }
  if(at!=freeList.begin() && at->begin==(at-1)->end) {
    auto prev = (at-1);
    prev->end = at->end;
    at = freeList.erase(at);
    }
  }

bool DrawClusters::commit(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  commitLevels(cmd, fId);
  if(!clustersDurtyBit)
    return false;
  clustersDurtyBit = false;

  std::atomic_thread_fence(std::memory_order_acquire);
  size_t csize = clusters.size()*sizeof(clusters[0]);
  csize = (csize + 0xFFF) & size_t(~0xFFF);

  auto& device = Resources::device();
  if(clustersGpu.byteSize() == csize) {
    patchClusters(cmd, fId);
    return false;
    }

  Resources::recycle(std::move(clustersGpu));
  clustersGpu  = device.ssbo(Tempest::Uninitialized, csize);
  clustersGpu.updateAsync(clusters.data(),0,clusters.size()*sizeof(clusters[0]));
  std::fill(clustersDurty.begin(), clustersDurty.end(), 0x0);
  return true;
  }

void DrawClusters::allocLevels(size_t first, std::span<const PackedMesh::ClusterLevel> source) {
  if(const auto end = first+source.size(); end>levelIndices.size()) {
    // Round the addressable map up so growing the live set does not replace
    // the Metal/Vulkan buffer every time a new piece of the world streams in.
    levelIndices.resize((end+1023) & ~size_t(1023));
    }
  for(const auto& level : source) {
    if((level.flags & 1u)!=0) {
      uint32_t index = uint32_t(levels.size());
      if(freeLevelsIds.empty()) {
        levels.push_back(level);
        } else {
        index = freeLevelsIds.back();
        freeLevelsIds.pop_back();
        levels[index] = level;
        }
      levelIndices[first] = index+1;
      dirtyLevelIndices.begin = std::min(dirtyLevelIndices.begin, first);
      dirtyLevelIndices.end   = std::max(dirtyLevelIndices.end, first+1);
      dirtyLevels.begin       = std::min(dirtyLevels.begin, size_t(index));
      dirtyLevels.end         = std::max(dirtyLevels.end, size_t(index)+1);
      }
    ++first;
    }
  }

void DrawClusters::freeLevels(uint32_t first, uint32_t count) {
  const auto end = std::min(size_t(first)+count, levelIndices.size());
  for(size_t i=first; i<end; ++i) {
    const auto id = levelIndices[i];
    if(id==0)
      continue;
    freeLevelsIds.push_back(id-1);
    levelIndices[i] = 0;
    dirtyLevelIndices.begin = std::min(dirtyLevelIndices.begin, i);
    dirtyLevelIndices.end   = std::max(dirtyLevelIndices.end, i+1);
    }
  }

void DrawClusters::commitLevels(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  static_assert(sizeof(PackedMesh::ClusterLevel)==48);
  static_assert(offsetof(PackedMesh::ClusterLevel, errorCm)==32);
  auto& device = Resources::device();
  if(noLevelsGpu.isEmpty()) {
    const PackedMesh::ClusterLevel none {};
    noLevelsGpu = device.ssbo(&none, sizeof(none));
    }
  const auto upload = [&]<class T>(StorageBuffer& dst, const std::vector<T>& src, Range& dirty, StorageBuffer& staging) {
    if(src.empty())
      return;
    if(dst.byteSize()<src.size()*sizeof(T)) {
      Resources::recycle(std::move(dst));
      dst = device.ssbo(Tempest::Uninitialized, ((src.size()+1023) & ~size_t(1023))*sizeof(T));
      dst.updateAsync(src.data(), 0, src.size()*sizeof(T));
      }
    else if(dirty.begin<dirty.end) {
      // Device memory can be CPU-visible on Vulkan. Apply changes on the GPU,
      // after earlier frames have consumed the previous index/record pair.
      constexpr size_t wordsPerBlock = 1024;
      const size_t words = (dirty.end-dirty.begin)*sizeof(T)/sizeof(uint32_t);
      const size_t dstWord = dirty.begin*sizeof(T)/sizeof(uint32_t);
      const size_t blocks = (words+wordsPerBlock-1)/wordsPerBlock;
      const size_t headerWords = blocks*3;
      levelPatchScratch.resize(headerWords+words);
      for(size_t i=0; i<blocks; ++i) {
        const size_t offset = i*wordsPerBlock;
        levelPatchScratch[i*3+0] = uint32_t(dstWord+offset);
        levelPatchScratch[i*3+1] = uint32_t(headerWords+offset);
        levelPatchScratch[i*3+2] = uint32_t(std::min(words-offset,wordsPerBlock));
        }
      std::memcpy(levelPatchScratch.data()+headerWords, src.data()+dirty.begin, words*sizeof(uint32_t));
      if(staging.byteSize()<levelPatchScratch.size()*sizeof(uint32_t))
        staging = device.ssbo(BufferHeap::Upload, levelPatchScratch);
      else
        staging.update(levelPatchScratch);
      cmd.setFramebuffer({});
      cmd.setBinding(0, dst);
      cmd.setBinding(1, staging);
      cmd.setPipeline(Shaders::inst().patch);
      cmd.dispatch(blocks);
      }
    dirty = {size_t(-1), 0};
    };
  upload(levelIndicesGpu, levelIndices, dirtyLevelIndices, levelPatch[fId][0]);
  upload(levelsGpu, levels, dirtyLevels, levelPatch[fId][1]);
  }

size_t DrawClusters::implAlloc(size_t count) {
  size_t bestFit = size_t(-1), bfDiff = size_t(-1);
  for(size_t i=0; i<freeList.size(); ++i) {
    auto f  = freeList[i];
    auto sz = (f.end - f.begin);
    if(sz==count) {
      freeList.erase(freeList.begin()+int(i));
      return f.begin;
      }
    if(sz<count)
      continue;
    if(sz-count < bfDiff)
      bestFit = i;
    }

  if(bestFit != size_t(-1)) {
    auto& f = freeList[bestFit];
    f.end -= count;
    return f.end;
    }

  size_t ret = clusters.size();
  clusters.resize(clusters.size() + count);
  return ret;
  }

void DrawClusters::patchClusters(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  std::vector<uint32_t>& header = scratch.header;
  std::vector<Cluster>&  patch  = scratch.patch;

  header.clear();
  patch.clear();

  for(size_t i=0; i<clustersDurty.size(); ++i) {
    if(clustersDurty[i]==0x0)
      continue;
    const uint32_t mask = clustersDurty[i];
    clustersDurty[i] = 0x0;

    for(size_t r=0; r<32; ++r) {
      if((mask & (1u<<r))==0)
        continue;
      size_t idx = i*32 + r;
      if(idx>=clusters.size())
        continue;
      patch.push_back(clusters[idx]);
      header.push_back(uint32_t(idx));
      }
    }

  if(header.empty())
    return;

  auto& device = Resources::device();
  auto& p = this->patch[fId];

  if(header.size()*sizeof(header[0]) <= p.indices.byteSize()) {
    p.indices.update(header);
    } else {
    p.indices = device.ssbo(BufferHeap::Upload, header);
    }

  if(patch.size()*sizeof(patch[0]) <= p.data.byteSize()) {
    p.data.update(patch);
    } else {
    p.data = device.ssbo(BufferHeap::Upload, patch);
    }

  const uint32_t count = uint32_t(header.size());
  cmd.setFramebuffer({});
  cmd.setBinding(0, clustersGpu);
  cmd.setBinding(1, p.data);
  cmd.setBinding(2, p.indices);
  cmd.setPushData(count);
  cmd.setPipeline(Shaders::inst().clusterPatch);
  cmd.dispatchThreads(count);
  }

void DrawClusters::markClusters(size_t id, size_t count) {
  for(size_t i=0; i<count; ++i) {
    static_assert(sizeof(std::atomic<uint32_t>)==sizeof(uint32_t));
    auto& bits = clustersDurty[id/32];
    reinterpret_cast<std::atomic<uint32_t>&>(bits).fetch_or(1u << (id%32), std::memory_order_relaxed);
    id++;
    }
  clustersDurtyBit.store(true);
  }
