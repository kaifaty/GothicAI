#pragma once

#include <Metal/Metal.hpp>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

#include "nsptr.h"

namespace Tempest {
namespace Detail {

// One MTLBinaryArchive per device. It keeps the GPU binaries of render and compute pipeline states,
// so a warm start skips the backend compilation. The file is keyed by GPU, OS build and archive format;
// inside the archive Metal matches functions by their compiled hash, so a changed shader simply misses.
class MtPipelineCache {
  public:
    MtPipelineCache() = default;
    ~MtPipelineCache();

    void init(MTL::Device& dev, const std::string& identity);
    void save(const char* reason = "request");
    // Serializes on a background thread; the destructor joins it and writes the final state.
    void saveAsync();

    MTL::RenderPipelineState*  render (MTL::Device& dev, MTL::RenderPipelineDescriptor& desc, NS::Error** err);
    MTL::ComputePipelineState* compute(MTL::Device& dev, MTL::ComputePipelineDescriptor& desc, NS::Error** err);

  private:
    template<class Desc, class State, class Make, class Add>
    State* create(Desc& desc, NS::Error** err, Make make, Add add);

    std::shared_mutex          sync;
    NsPtr<MTL::BinaryArchive>  archive;
    NsPtr<NS::Array>           list;
    std::filesystem::path      file;
    std::mutex                 saveSync;
    std::atomic<bool>          dirty{false};
    std::atomic<uint32_t>      hits{0}, misses{0}, failures{0};
    std::thread                saver;
  };

}
}
