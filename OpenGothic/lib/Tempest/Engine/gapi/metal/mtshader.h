#pragma once

#include <Tempest/AbstractGraphicsApi>

#include "gapi/shader.h"

#include <Metal/Metal.hpp>
#include "nsptr.h"

#include <string>

namespace Tempest {
namespace Detail {

class MtDevice;

class MtShader : public Detail::Shader {
  public:
    MtShader(MtDevice& dev, const void* source, size_t srcSize);
    ~MtShader();

    // Key of the on-disk MSL cache for this device; empty when the translator version is unknown (cache off).
    static std::string cacheIdentity(const MtDevice& dev);

    enum Bindings : uint32_t {
      MSL_BUFFER_LENGTH      = 29,
      MSL_BUFFER_LENGTH_SIZE = 30,
      };

    NsPtr<MTL::Library>  library;
    NsPtr<MTL::Function> impl;
    bool                 bufferSizeBuffer = false;
  };

}
}
