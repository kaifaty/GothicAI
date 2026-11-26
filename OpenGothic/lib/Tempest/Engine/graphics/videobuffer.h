#pragma once

#include <Tempest/AbstractGraphicsApi>
#include "../utility/dptr.h"

#include <memory>

namespace Tempest {

class Device;
class CommandBuffer;
class DescriptorSet;
template<class T>
class Encoder;

namespace Detail {

class VideoBuffer {
  public:
    VideoBuffer()=default;
    VideoBuffer(VideoBuffer&&) noexcept;
    // Copies share the ref-counted allocation: a published snapshot keeps the buffer alive after its owner is gone
    VideoBuffer(const VideoBuffer&);
    ~VideoBuffer();
    VideoBuffer& operator=(VideoBuffer&&) noexcept;
    VideoBuffer& operator=(const VideoBuffer&);

    void   update(const void* data, size_t offset, size_t size);
    void   updateAsync(const void* data, size_t offset, size_t size);
    size_t size() const { return sz; }

  private:
    VideoBuffer(AbstractGraphicsApi::PBuffer &&impl, size_t size);

    Detail::DSharedPtr<AbstractGraphicsApi::Buffer*> impl;
    size_t                                           sz=0;
    // the live-buffer probe counts the allocation once, as Texture2d's payload accounting: copies share this token
    struct BufferAccounting;
    std::shared_ptr<BufferAccounting>                accounting;

  friend class Tempest::Device;
  friend class Tempest::CommandBuffer;
  friend class Tempest::DescriptorSet;
  friend class Tempest::Encoder<Tempest::CommandBuffer>;
  };

}

}

