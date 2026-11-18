#include "meshletspheres.h"

#include <algorithm>
#include <cstring>

#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/shaders.h"

using namespace Tempest;

namespace {
constexpr uint32_t PatchChunk = 1024; // uints one workgroup of patch.comp copies
constexpr uint32_t MinCapacity = 16*1024;
}

MeshletSpheres::MeshletSpheres() {
  }

MeshletSpheres::~MeshletSpheres() {
  }

uint32_t MeshletSpheres::place(const StaticMesh& mesh) {
  if(mesh.meshletSpheres.empty())
    return None;
  if(auto it = ranges.find(&mesh); it!=ranges.end())
    return it->second.first;
  const auto count = uint32_t(mesh.meshletSpheres.size());
  const Range r = {alloc(count), count};
  ranges[&mesh] = r;
  pending.push_back(&mesh);
  return r.first;
  }

void MeshletSpheres::forget(const StaticMesh& mesh) {
  auto it = ranges.find(&mesh);
  if(it==ranges.end())
    return;
  release(it->second);
  ranges.erase(it);
  pending.erase(std::remove(pending.begin(), pending.end(), &mesh), pending.end());
  }

uint32_t MeshletSpheres::alloc(uint32_t count) {
  for(size_t i=0; i<freeList.size(); ++i) {
    auto& f = freeList[i];
    if(f.count<count)
      continue;
    const uint32_t ret = f.first;
    f.first += count;
    f.count -= count;
    if(f.count==0)
      freeList.erase(freeList.begin()+ptrdiff_t(i));
    return ret;
    }
  const uint32_t ret = end;
  end += count;
  return ret;
  }

void MeshletSpheres::release(Range r) {
  auto at = std::lower_bound(freeList.begin(), freeList.end(), r, [](const Range& a, const Range& b) {
    return a.first<b.first;
    });
  at = freeList.insert(at, r);
  if(at+1!=freeList.end() && at->first+at->count==(at+1)->first) {
    at->count += (at+1)->count;
    freeList.erase(at+1);
    }
  if(at!=freeList.begin() && (at-1)->first+(at-1)->count==at->first) {
    (at-1)->count += at->count;
    freeList.erase(at);
    }
  }

void MeshletSpheres::commit(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  auto& device = Resources::device();
  if(gpu.isEmpty() || capacity<end) {
    // the frames in flight keep the old array; the new one gets every range
    Resources::recycle(std::move(gpu));
    capacity = std::max(MinCapacity, end + end/2);
    gpu      = device.ssbo(BufferHeap::Device, Uninitialized, size_t(capacity)*sizeof(Vec4));
    pending.clear();
    for(auto& [mesh,r]:ranges)
      pending.push_back(mesh);
    }
  if(pending.empty())
    return;
  upload(cmd, fId);
  pending.clear();
  }

void MeshletSpheres::upload(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  struct Path {
    uint32_t dst, src, size;
    };
  std::vector<Path> path;
  uint32_t payload = 0; // uints
  for(auto* mesh:pending) {
    const Range& r = ranges[mesh];
    for(uint32_t at=0; at<r.count*4; at+=PatchChunk) {
      const uint32_t sz = std::min(PatchChunk, r.count*4-at);
      path.push_back({r.first*4+at, payload, sz});
      payload += sz;
      }
    }

  // patch.comp: the paths (offsets in uints), then the data they copy
  const size_t header = path.size()*sizeof(Path);
  patchCpu.resize(header + size_t(payload)*sizeof(uint32_t));
  uint8_t* data = patchCpu.data()+header;
  for(auto* mesh:pending) {
    const size_t bytes = mesh->meshletSpheres.size()*sizeof(Vec4);
    std::memcpy(data, mesh->meshletSpheres.data(), bytes);
    data += bytes;
    }
  for(auto& p:path)
    p.src += uint32_t(header/sizeof(uint32_t));
  std::memcpy(patchCpu.data(), path.data(), header);

  auto& device = Resources::device();
  auto& st     = stage[fId];
  if(st.byteSize()<patchCpu.size()) {
    Resources::recycle(std::move(st));
    st = device.ssbo(BufferHeap::Upload, Uninitialized, patchCpu.size() + patchCpu.size()/2);
    }
  // host-visible memory of this frame: the frame that used it last has completed
  st.update(patchCpu.data(), 0, patchCpu.size());

  cmd.setFramebuffer({});
  cmd.setBinding(0, gpu);
  cmd.setBinding(1, st);
  cmd.setPipeline(Shaders::inst().patch);
  cmd.dispatch(path.size());
  }
