#pragma once

#include <Tempest/AbstractGraphicsApi>

namespace Tempest {

class Texture2d;

class DescriptorArray {
  public:
    DescriptorArray() = default;
    DescriptorArray(DescriptorArray&&);
    ~DescriptorArray();
    DescriptorArray& operator=(DescriptorArray&&);

    bool isEmpty() const { return impl.handler==nullptr; }

    // The caller must finish GPU use of this array before changing a slot.
    void set(size_t index, const Texture2d* texture);

  private:
    DescriptorArray(Device& dev, AbstractGraphicsApi::DescArray* desc);

    Detail::DPtr<AbstractGraphicsApi::DescArray*> impl;

  friend class Tempest::Device;
  friend class Tempest::Encoder<Tempest::CommandBuffer>;
  };

}
