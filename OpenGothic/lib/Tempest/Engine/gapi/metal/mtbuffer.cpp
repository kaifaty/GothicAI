#if defined(TEMPEST_BUILD_METAL)

#include "mtbuffer.h"
#include "mtdevice.h"

#include "utility/compiller_hints.h"

#include <unistd.h>

#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
#include "gapi/metalapi.h"
#endif

using namespace Tempest::Detail;

MtBuffer::MtBuffer() {
  }

MtBuffer::MtBuffer(MtDevice& dev, const void* data, size_t size, MTL::ResourceOptions f)
  :dev(&dev), size(size) {
  const size_t roundSize = ((size+64-1)/64)*64; // for uniforms/ssbo structures in msl

  const MTL::ResourceOptions flg = f | MTL::HazardTrackingModeDefault;
  if(data==nullptr) {
    impl = NsPtr<MTL::Buffer>(dev.impl->newBuffer(roundSize,flg));
    if(impl==nullptr)
      throw std::system_error(GraphicsErrc::OutOfVideoMemory);
    return;
    }

  if(roundSize==size && 0==(flg & MTL::ResourceStorageModePrivate)) {
    impl = NsPtr<MTL::Buffer>(dev.impl->newBuffer(data,roundSize,flg));
    if(impl==nullptr)
      throw std::system_error(GraphicsErrc::OutOfVideoMemory);
    return;
    }

  impl = NsPtr<MTL::Buffer>(dev.impl->newBuffer(roundSize,flg));
  if(impl==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);
  if(impl->storageMode()==MTL::StorageModePrivate)
    dev.uploadBuffer(*impl,data,0,size,dev.syncBufferUploads); else
    implUpdate(data,0,size);
  }

MtBuffer::~MtBuffer() {
  }

MtBuffer& MtBuffer::operator =(MtBuffer&& other) {
  assert(other.counter.load()==0); // internal use only
  assert(this->counter.load()==0);
  std::swap(dev,  other.dev);
  std::swap(size, other.size);
  std::swap(impl, other.impl);
  return *this;
  }

void MtBuffer::update(const void *data, size_t off, size_t size) {
  if(T_LIKELY(impl->storageMode()!=MTL::StorageModePrivate)) {
    implUpdate(data,off,size);
    return;
    }
  dev->uploadBuffer(*impl,data,off,size,true);
  }

void MtBuffer::updateAsync(const void* data, size_t off, size_t size) {
  if(T_LIKELY(impl->storageMode()!=MTL::StorageModePrivate)) {
    implUpdate(data,off,size);
    return;
    }
  dev->uploadBuffer(*impl,data,off,size,false);
  }

void MtDevice::uploadBuffer(MTL::Buffer& target, const void* data, size_t off, size_t size, bool wait) {
  auto pool = NsPtr<NS::AutoreleasePool>::init();
  // Metal can wait for a free command-buffer slot; completion handlers need
  // uploadSync to retire previous uploads while that wait is in progress.
  auto cmd = queue->commandBuffer();
  auto guard = lockUpload(size);
  MTL::ResourceOptions opt   = MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined;
  auto                 stage = NsPtr<MTL::Buffer>(impl->newBuffer(size,opt));
  if(stage==nullptr)
    throw std::system_error(GraphicsErrc::OutOfVideoMemory);
  probeMetalUploadBuffer(stage.get());
  std::memcpy(stage->contents(), data, size);

  // Retained references keep both buffers alive after the staging owner exits.
  // Consuming draws commit later on this same queue and see the completed copy.
  auto enc = cmd->blitCommandEncoder();
  enc->copyFromBuffer(stage.get(),0, &target,off, size);
  enc->endEncoding();
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  profileGpu(cmd);
#endif
  submitUpload(*cmd,stage->allocatedSize());
  guard.unlock();
  if(wait)
    cmd->waitUntilCompleted();
  }

std::unique_lock<std::mutex> MtDevice::lockUpload(size_t size) {
  constexpr size_t maxStagingBytes = 128u*1024u*1024u;
  // Standalone Metal allocations can round the payload/heap size to a VM page.
  const size_t pageSize = size_t(getpagesize());
  size = ((size+pageSize-1)/pageSize)*pageSize;
  std::unique_lock<std::mutex> guard(uploadSync);
  // Retire the oldest copies before allocating more staging. A single resource
  // larger than the budget is allowed only when all previous copies finished.
  uploadCv.wait(guard,[&] {
    return uploads.empty() || size<=maxStagingBytes-std::min(uploadBytes,maxStagingBytes);
    });
  return guard;
  }

void MtDevice::submitUpload(MTL::CommandBuffer& cmd, size_t bytes) {
  // The caller holds uploadSync from staging allocation through commit.
  cmd.retain();
  uploads.push_back({NsPtr<MTL::CommandBuffer>(&cmd),bytes});
  uploadBytes += bytes;
  onSubmit();
  auto* device = this;
  cmd.addCompletedHandler(^(MTL::CommandBuffer*) {
    {
    std::lock_guard<std::mutex> guard(device->uploadSync);
    while(!device->uploads.empty() &&
          device->uploads.front().command->status()>=MTL::CommandBufferStatusCompleted) {
      device->uploadBytes -= device->uploads.front().bytes;
      device->uploads.pop_front();
      }
    device->uploadCv.notify_all();
    }
    device->onFinish();
    });
  cmd.commit();
  }

void MtBuffer::read(void *data, size_t off, size_t size) {
  auto pool = NsPtr<NS::AutoreleasePool>::init();

  MTL::ResourceOptions opt   = MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeDefaultCache |
                               MTL::ResourceHazardTrackingModeTracked;
  auto                 stage = NsPtr<MTL::Buffer>(dev->impl->newBuffer(size,opt));
  probeMetalUploadBuffer(stage.get());
  auto cmd = dev->queue->commandBuffer();
  auto enc = cmd->blitCommandEncoder();
  enc->copyFromBuffer(impl.get(),off,stage.get(),0, size);
  enc->endEncoding();
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
  profileGpu(cmd);
#endif
  cmd->commit();

  cmd->waitUntilCompleted();
  std::memcpy(data,stage->contents(),size);
  }

void MtBuffer::implUpdate(const void *data, size_t off, size_t size) {
  auto ptr = reinterpret_cast<uint8_t*>(impl->contents());

  std::memcpy(ptr+off, data, size);
  if(impl->storageMode()!=MTL::StorageModeManaged)
    return;

  impl->didModifyRange(NS::Range(off,size));
  }

#endif
