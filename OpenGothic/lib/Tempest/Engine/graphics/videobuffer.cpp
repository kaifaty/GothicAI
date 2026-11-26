#include "videobuffer.h"

#include <Tempest/Device>
#include <Tempest/Except>
#include <algorithm>
#include <atomic>

using namespace Tempest;
using namespace Tempest::Detail;

static std::atomic<size_t> liveBuffers{0};

size_t tempestProbeGpuBufferCount() {
  return liveBuffers.load(std::memory_order_relaxed);
  }

struct VideoBuffer::BufferAccounting {
  BufferAccounting()  { liveBuffers.fetch_add(1,std::memory_order_relaxed); }
  ~BufferAccounting() { liveBuffers.fetch_sub(1,std::memory_order_relaxed); }
  };

VideoBuffer::VideoBuffer(AbstractGraphicsApi::PBuffer&& impl, size_t size)
  :impl(std::move(impl)),sz(size) {
  if(sz!=0)
    accounting = std::make_shared<BufferAccounting>();
  }

VideoBuffer::VideoBuffer(VideoBuffer &&other) noexcept
  :impl(std::move(other.impl)),sz(other.sz),accounting(std::move(other.accounting)) {
  other.sz = 0;
  }

VideoBuffer::VideoBuffer(const VideoBuffer &other)
  :impl(other.impl),sz(other.sz),accounting(other.accounting) {
  }

VideoBuffer::~VideoBuffer() = default;

VideoBuffer &VideoBuffer::operator=(VideoBuffer &&other) noexcept {
  std::swap(impl,other.impl);
  std::swap(sz,  other.sz);
  std::swap(accounting,other.accounting);
  return *this;
  }

VideoBuffer &VideoBuffer::operator=(const VideoBuffer &other) {
  impl       = other.impl;
  sz         = other.sz;
  accounting = other.accounting;
  return *this;
  }

void VideoBuffer::update(const void* data, size_t offset, size_t size) {
  if(size==0)
    return;
  if(offset+size>sz)
    throw std::system_error(Tempest::GraphicsErrc::InvalidBufferUpdate);
  impl.handler->update(data,offset,size);
  }

void VideoBuffer::updateAsync(const void* data, size_t offset, size_t size) {
  if(size==0)
    return;
  if(offset+size>sz)
    throw std::system_error(Tempest::GraphicsErrc::InvalidBufferUpdate);
  impl.handler->updateAsync(data,offset,size);
  }
