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

VideoBuffer::VideoBuffer(AbstractGraphicsApi::PBuffer&& impl, size_t size)
  :impl(std::move(impl)),sz(size) {
  if(sz!=0)
    liveBuffers.fetch_add(1,std::memory_order_relaxed);
  }

VideoBuffer::VideoBuffer(VideoBuffer &&other)
  :impl(std::move(other.impl)),sz(other.sz) {
  other.sz = 0;
  }

VideoBuffer::~VideoBuffer(){
  if(sz!=0)
    liveBuffers.fetch_sub(1,std::memory_order_relaxed);
  }

VideoBuffer &VideoBuffer::operator=(VideoBuffer &&other) {
  std::swap(impl,other.impl);
  std::swap(sz,  other.sz);
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
