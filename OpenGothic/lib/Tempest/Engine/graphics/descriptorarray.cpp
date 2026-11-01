#include "descriptorarray.h"
#include "texture2d.h"

#include <stdexcept>

using namespace Tempest;

DescriptorArray::DescriptorArray(Device&, AbstractGraphicsApi::DescArray *desc)
  : impl(desc) {
  }

DescriptorArray::DescriptorArray(DescriptorArray&& u)
  : impl(std::move(u.impl)) {
  }

DescriptorArray::~DescriptorArray() {
  delete impl.handler;
  }

DescriptorArray& DescriptorArray::operator=(DescriptorArray&& u) {
  impl=std::move(u.impl);
  return *this;
  }

void DescriptorArray::set(size_t index, const Texture2d* texture) {
  if(impl.handler==nullptr)
    throw std::out_of_range("descriptor array is empty");
  impl.handler->setTexture(index,texture!=nullptr ? texture->impl.handler : nullptr);
  }
