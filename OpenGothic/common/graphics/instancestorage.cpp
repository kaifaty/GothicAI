#include "instancestorage.h"
#include "shaders.h"
#include "utils/workers.h"
#include "frameprofile.h"

#include <Tempest/Log>
#include <algorithm>
#include <cstdint>
#include <atomic>

using namespace Tempest;

static uint32_t nextPot(uint32_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v;
  }

static uint32_t alignAs(uint32_t sz, uint32_t alignment) {
  return ((sz+alignment-1)/alignment)*alignment;
  }

static void bitSet(std::vector<uint32_t>& b, size_t id) {
  static_assert(sizeof(std::atomic<uint32_t>)==sizeof(uint32_t));
  auto& bits = b[id/32];
  id %= 32;
  reinterpret_cast<std::atomic<uint32_t>&>(bits).fetch_or(1u << id, std::memory_order_relaxed);
  }

static void bitSetRange(std::vector<uint32_t>& b, size_t first, size_t count) {
  static_assert(sizeof(std::atomic<uint32_t>)==sizeof(uint32_t));
  while(count>0) {
    const size_t offset = first%32;
    const size_t span = std::min(count,32-offset);
    const uint32_t mask = (UINT32_MAX >> (32-span)) << offset;
    auto& bits = b[first/32];
    reinterpret_cast<std::atomic<uint32_t>&>(bits).fetch_or(mask, std::memory_order_relaxed);
    first += span;
    count -= span;
    }
  }

static bool bitAt(std::vector<uint32_t>& b, size_t id) {
  auto bits = b[id/32];
  id %= 32;
  return bits & (1u << id);
  }

// Calls run(begin,end) for every run of set bits among the first `count`;
// word(w) gives the w-th 32-bit word of the bit set.
template<class Word, class Run>
static void scanRuns(size_t count, Word word, Run run) {
  size_t begin = 0;
  bool   open  = false;
  for(size_t w=0; w*32<count; ++w) {
    const uint32_t bits = word(w);
    if(bits==0 && !open)
      continue;
    if(bits==UINT32_MAX && open)
      continue;
    const size_t end = std::min(count, w*32+32);
    for(size_t i=w*32; i<end; ++i) {
      const bool set = (bits >> (i%32)) & 1u;
      if(set && !open) {
        begin = i;
        open  = true;
        }
      else if(!set && open) {
        run(begin, i);
        open = false;
        }
      }
    }
  if(open)
    run(begin, count);
  }

using namespace Tempest;

InstanceStorage::Id::Id(Id&& other) noexcept
  :owner(other.owner), rgn(other.rgn) {
  other.owner = nullptr;
  }

InstanceStorage::Id& InstanceStorage::Id::operator = (Id&& other) noexcept {
  std::swap(owner, other.owner);
  std::swap(rgn,   other.rgn);
  return *this;
  }

InstanceStorage::Id::~Id() {
  if(owner!=nullptr)
    owner->free(rgn);
  }

void InstanceStorage::Id::set(const Tempest::Matrix4x4* mat) {
  if(owner==nullptr)
    return;

  auto data = reinterpret_cast<Matrix4x4*>(owner->dataCpu.data() + rgn.begin);
  const bool identical = rgn.asize==0 || std::memcmp(data,mat,rgn.asize)==0;
  if(FrameProfile::enabled()) {
    FrameProfile::add(FrameProfile::Amount::PaletteCalls,1);
    FrameProfile::add(FrameProfile::Amount::PaletteBytes,rgn.asize);
    if(identical) {
      FrameProfile::add(FrameProfile::Amount::IdenticalCalls,1);
      FrameProfile::add(FrameProfile::Amount::IdenticalBytes,rgn.asize);
      }
    }
  if(identical)
    return;
  std::memcpy(data, mat, rgn.asize);

  bitSetRange(owner->durty,rgn.begin/blockSz,(rgn.asize+blockSz-1)/blockSz);
  }

void InstanceStorage::Id::set(const Tempest::Matrix4x4& obj, size_t offset) {
  if(owner==nullptr)
    return;

  auto data = reinterpret_cast<Matrix4x4*>(owner->dataCpu.data() + rgn.begin);
  if(data[offset] == obj)
    return;
  data[offset] = obj;
  bitSet(owner->durty, (rgn.begin+offset*sizeof(Matrix4x4))/blockSz);
  }

void InstanceStorage::Id::set(const void* data, size_t offset, size_t size) {
  if(owner==nullptr)
    return;

  auto src = reinterpret_cast<const uint8_t*>(data);
  auto dst = (owner->dataCpu.data() + rgn.begin + offset);

  if(std::memcmp(src, dst, size)==0)
    return;

  if((offset%blockSz)==0) {
    for(size_t i=0; i<size; i+=blockSz) {
      const size_t sz = std::min(blockSz, size-i);
      std::memcpy(dst+i, src+i, sz);
      bitSet(owner->durty, (rgn.begin + offset + i)/blockSz);
      }
    } else {
    for(size_t i=0; i<size; ++i) {
      dst[i] = src[i];
      bitSet(owner->durty, (rgn.begin + offset + i)/blockSz);
      }
    }
  }


InstanceStorage::InstanceStorage() {
  dataCpu.reserve(131072);
  dataCpu.resize(sizeof(Matrix4x4)); // also avoid null-ssbo
  reinterpret_cast<Matrix4x4*>(dataCpu.data())->identity();

  patchCpu.reserve(4*1024*1024);
  patchBlock.reserve(16*1024);

  uploadTh = std::thread([this](){ uploadMain(); });
  }

InstanceStorage::~InstanceStorage() {
  {
    std::unique_lock<std::mutex> lck(sync);
    uploadFId = Resources::MaxFramesInFlight;
  }
  uploadCnd.notify_one();
  uploadTh.join();
  }

size_t InstanceStorage::gpuBytes() const {
  return (dataCpu.size() + 0xFFF) & ~size_t(0xFFF);
  }

void InstanceStorage::setHistory(bool enabled) {
  if(history==enabled)
    return;
  history      = enabled;
  historyValid = false;
  if(enabled) {
    // nothing of the frames before is known: the next commit copies all of cur
    durtyPrev.assign(durty.size(), 0);
    fresh    .assign(durty.size(), 0);
    } else {
    Resources::recycle(std::move(dataGpuPrev));
    std::vector<uint32_t>().swap(durtyPrev);
    std::vector<uint32_t>().swap(fresh);
    std::vector<Copy>().swap(saveBlock);
    std::vector<Copy>().swap(freshBlock);
    }
  }

const Tempest::StorageBuffer& InstanceStorage::ssboPrev() const {
  if(history && historyValid && !dataGpuPrev.isEmpty() && dataGpuPrev.byteSize()==dataGpu.byteSize())
    return dataGpuPrev;
  return dataGpu;
  }

void InstanceStorage::pushCopies(std::vector<Copy>& out, size_t firstBlock, size_t endBlock) {
  // one workgroup of patch_copy.comp per Copy
  static constexpr uint32_t chunkSz = 1024;
  uint32_t at   = uint32_t(firstBlock*blockSz);
  uint32_t size = uint32_t((endBlock-firstBlock)*blockSz);
  while(size>0) {
    Copy c;
    c.at   = at;
    c.size = std::min(size, chunkSz);
    out.push_back(c);
    at   += c.size;
    size -= c.size;
    }
  }

InstanceStorage::Plan InstanceStorage::planCommit(size_t gpuCapacity) {
  Plan plan;
  if(gpuCapacity!=gpuBytes()) {
    // cur (and prev) are recreated from dataCpu: nothing is pending, no motion in this frame
    plan.grow = true;
    std::memset(durty.data(), 0, durty.size()*sizeof(uint32_t));
    if(history) {
      std::fill(durtyPrev.begin(), durtyPrev.end(), 0);
      std::fill(fresh.begin(),     fresh.end(),     0);
      historyValid = true;
      }
    return plan;
    }

  patchBlock.clear();
  size_t payloadSize = 0;
  for(size_t i = 0; i<blockCnt; ++i) {
    if(i%32==0 && durty[i/32]==0) {
      i+=31;
      continue;
      }
    if(!bitAt(durty,i))
      continue;
    auto begin = i; ++i;
    while(i<blockCnt) {
      if(!bitAt(durty,i))
        break;
      ++i;
      }

    uint32_t size    = uint32_t((i-begin)*blockSz);
    uint32_t chunkSz = 256;

    Path p = {};
    p.dst  = uint32_t(begin*blockSz);
    p.src  = uint32_t(payloadSize);
    while(size>0) {
      p.size       = std::min<uint32_t>(size, chunkSz);
      size        -= p.size;
      patchBlock.push_back(p);

      payloadSize += p.size;
      p.dst       += p.size;
      p.src       += p.size;
      }
    }
  if(history) {
    // durtyPrev and fresh follow durty in alloc; a mismatch would be a bug, keep it harmless
    durtyPrev.resize(durty.size(), 0);
    fresh    .resize(durty.size(), 0);
    saveBlock.clear();
    freshBlock.clear();
    if(!historyValid) {
      // first commit with history: after the patch prev becomes a full copy of cur
      pushCopies(freshBlock, 0, (dataCpu.size()+blockSz-1)/blockSz);
      } else {
      // 1. save: dirty now or in the previous commit; cur still holds the previous state
      scanRuns(blockCnt, [this](size_t w){ return durty[w] | durtyPrev[w]; }, [this](size_t b, size_t e){
        pushCopies(saveBlock, b, e);
        });
      // 3. fresh: a new Id's past is its present
      scanRuns(blockCnt, [this](size_t w){ return fresh[w]; }, [this](size_t b, size_t e){
        pushCopies(freshBlock, b, e);
        });
      }
    std::copy(durty.begin(), durty.end(), durtyPrev.begin());
    std::fill(fresh.begin(), fresh.end(), 0);
    historyValid = true;
    }
  std::memset(durty.data(), 0, durty.size()*sizeof(durty[0]));

  const bool copies = history && (saveBlock.size()>0 || freshBlock.size()>0);
  if(patchBlock.size()==0 && !copies)
    return plan;

  const size_t headerSize = patchBlock.size()*sizeof(Path);
  const size_t saveSize   = copies ? saveBlock .size()*sizeof(Copy) : 0;
  const size_t freshSize  = copies ? freshBlock.size()*sizeof(Copy) : 0;
  patchCpu.resize(headerSize + payloadSize + saveSize + freshSize);
  for(auto& i:patchBlock) {
    i.src += uint32_t(headerSize);
    std::memcpy(patchCpu.data()+i.src, dataCpu.data() + i.dst, i.size);

    // uint's in shader
    i.src  /= 4;
    i.dst  /= 4;
    i.size /= 4;
    }
  if(headerSize>0)
    std::memcpy(patchCpu.data(), patchBlock.data(), headerSize);

  plan.upload     = true;
  plan.patchCount = uint32_t(patchBlock.size());
  if(copies) {
    auto pack = [this](std::vector<Copy>& list, size_t at) {
      for(auto& i:list) {
        // uint's in shader
        i.at   /= 4;
        i.size /= 4;
        }
      if(list.size()>0)
        std::memcpy(patchCpu.data()+at, list.data(), list.size()*sizeof(Copy));
      return uint32_t(at/4);
      };
    plan.saveBase   = pack(saveBlock,  headerSize + payloadSize);
    plan.saveCount  = uint32_t(saveBlock.size());
    plan.freshBase  = pack(freshBlock, headerSize + payloadSize + saveSize);
    plan.freshCount = uint32_t(freshBlock.size());
    }
  return plan;
  }

bool InstanceStorage::commit(Encoder<CommandBuffer>& cmd, uint8_t fId) {
  FrameProfile::Scope timing(FrameProfile::Zone::StorageCommit);
  auto& device = Resources::device();

  std::atomic_thread_fence(std::memory_order_acquire);
  join();

  const size_t dataSize = gpuBytes();
  if(dataGpu.byteSize()!=dataSize) {
    FrameProfile::Scope growth(FrameProfile::Zone::StorageGrow);
    FrameProfile::add(FrameProfile::Amount::GrowBytes,dataCpu.size());
    planCommit(dataGpu.byteSize());
    Resources::recycle(std::move(dataGpu));
    dataGpu = device.ssbo(BufferHeap::Device,Tempest::Uninitialized,dataSize);
    dataGpu.update(dataCpu);
    if(history) {
      // prev from the same CPU data: one frame without motion
      Resources::recycle(std::move(dataGpuPrev));
      dataGpuPrev = device.ssbo(BufferHeap::Device,Tempest::Uninitialized,dataSize);
      dataGpuPrev.update(dataCpu);
      }
    return true;
    }

  FrameProfile::Scope packing(FrameProfile::Zone::StoragePack);
  const Plan plan = planCommit(dataGpu.byteSize());
  if(!plan.upload)
    return false;
  FrameProfile::add(FrameProfile::Amount::PatchCalls,1);
  FrameProfile::add(FrameProfile::Amount::PatchBytes,patchCpu.size());
  FrameProfile::add(FrameProfile::Amount::PatchBlocks,plan.patchCount);
  packing.finish();

  FrameProfile::Scope dispatch(FrameProfile::Zone::StorageDispatch);
  auto& path = patchGpu[fId];
  if(path.byteSize() < patchCpu.size()) {
    path = device.ssbo(BufferHeap::Upload, Uninitialized, patchCpu.size());
    }

  {
    std::unique_lock<std::mutex> lck(sync);
    uploadFId = fId;
  }
  uploadCnd.notify_one();
  //path.update(patchCpu);

  if(history && dataGpuPrev.byteSize()!=dataGpu.byteSize()) {
    // history was just turned on: the plan's fresh list covers the whole buffer
    Resources::recycle(std::move(dataGpuPrev));
    dataGpuPrev = device.ssbo(BufferHeap::Device,Tempest::Uninitialized,dataGpu.byteSize());
    }

  // Copy lists live in this frame's patch buffer, uploaded by uploadTh and joined
  // before submit, exactly like the patch headers.
  auto copy = [&](uint32_t base, uint32_t count) {
    cmd.setBinding(0, dataGpuPrev);
    cmd.setBinding(1, dataGpu);
    cmd.setBinding(2, path);
    cmd.setPushData(base);
    cmd.setPipeline(Shaders::inst().patchCopy);
    cmd.dispatch(count);
    };

  cmd.setFramebuffer({});
  if(plan.saveCount>0)
    copy(plan.saveBase, plan.saveCount);
  if(plan.patchCount>0) {
    cmd.setBinding(0, dataGpu);
    cmd.setBinding(1, path);
    cmd.setPipeline(Shaders::inst().patch);
    cmd.dispatch(plan.patchCount);
    }
  if(plan.freshCount>0)
    copy(plan.freshBase, plan.freshCount);
  return false;
  }

void InstanceStorage::join() {
  FrameProfile::Scope timing(FrameProfile::Zone::StorageJoin);
  while(true) {
    std::unique_lock<std::mutex> lck(sync);
    if(uploadFId<0)
      break;
    }
  }

InstanceStorage::Id InstanceStorage::alloc(const size_t size) {
  if(size==0)
    return Id(*this,Range());

  const auto nsize = alignAs(nextPot(uint32_t(size)), alignment);
  for(size_t i=0; i<rgn.size(); ++i) {
    if(rgn[i].size==nsize) {
      auto ret = rgn[i];
      ret.asize = size;
      rgn.erase(rgn.begin()+intptr_t(i));
      markFresh(ret);
      return Id(*this,ret);
      }
    }
  size_t retId = size_t(-1);
  for(size_t i=0; i<rgn.size(); ++i) {
    if(rgn[i].size>nsize && (retId==size_t(-1) || rgn[i].size<rgn[retId].size)) {
      retId = i;
      }
    }
  if(retId!=size_t(-1)) {
    Range ret = rgn[retId];
    ret.size  = nsize;
    ret.asize = size;
    rgn[retId].begin += nsize;
    rgn[retId].size  -= nsize;
    markFresh(ret);
    return Id(*this,ret);
    }
  Range r;
  r.begin = dataCpu.size();
  r.size  = nsize;
  r.asize = size;

  dataCpu.resize(dataCpu.size() + nsize);

  blockCnt = (dataCpu.size()+blockSz-1)/blockSz;
  durty.resize((blockCnt+32-1)/32, 0);
  // GPU capacity is page-rounded and its unused tail is uninitialized. Upload
  // every newly reserved block, including padding, even if the first palette is
  // zero or a later realloc exposes padding without changing the CPU bytes.
  bitSetRange(durty,r.begin/blockSz,r.size/blockSz);
  if(history) {
    durtyPrev.resize(durty.size(), 0);
    fresh    .resize(durty.size(), 0);
    }
  markFresh(r);
  return Id(*this,r);
  }

void InstanceStorage::markFresh(const Range& r) {
  // alloc runs on the main thread only (see rgn), but the bit set shares words
  // with neighbours, so mark it the same atomic way as durty
  if(history)
    bitSetRange(fresh,r.begin/blockSz,r.size/blockSz);
  }

bool InstanceStorage::realloc(Id& id, const size_t size) {
  if(size==0) {
    if(id.isEmpty())
      return false;
    id = Id(*this,Range());
    return true;
    }

  if(size<=id.rgn.size) {
    id.rgn.asize = size;
    return false;
    }

  auto next = alloc(size);
  if(id.isEmpty()) {
    id = std::move(next);
    return true;
    }

  auto data = dataCpu.data();
  std::memcpy(data+next.rgn.begin, data+id.rgn.begin, id.rgn.asize);
  bitSetRange(durty,next.rgn.begin/blockSz,(id.rgn.asize+blockSz-1)/blockSz);
  id = std::move(next);
  return true;
  }

const Tempest::StorageBuffer& InstanceStorage::ssbo() const {
  return dataGpu;
  }

void InstanceStorage::free(const Range& r) {
  for(auto& i:rgn) {
    if(i.begin+i.size==r.begin) {
      i.size  += r.size;
      return;
      }
    if(r.begin+r.size==i.begin) {
      i.begin -= r.size;
      i.size  += r.size;
      return;
      }
    }
  auto at = std::lower_bound(rgn.begin(),rgn.end(),r,[](const Range& l, const Range& r){
    return l.begin<r.begin;
    });
  rgn.insert(at,r);
  }

void InstanceStorage::uploadMain() {
  Workers::setThreadName("InstanceStorage upload");
  while(true) {
    std::unique_lock<std::mutex> lck(sync);
    // a frame set between two waits is not lost: join() would spin on it forever
    uploadCnd.wait(lck,[this](){ return uploadFId>=0; });
    if(uploadFId==Resources::MaxFramesInFlight)
      break;

    {
    // Execution interval and payload size, deliberately NOT the originating
    // dispatch frame. An async upload may overlap a later render attempt.
    FrameProfile::Scope upload(FrameProfile::Zone::StorageUploadWorker);
    upload.add(FrameProfile::Amount::UploadBytes,patchCpu.size());
    patchGpu[uploadFId].update(patchCpu);
    }
    uploadFId = -1;
    }
  }
