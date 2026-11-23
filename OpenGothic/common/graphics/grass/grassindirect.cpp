#include "grassfield.h"

#include <Tempest/Device>

#include <cstdlib>

#include "graphics/grass/grasscull.h"
#include "resources.h"

using namespace Tempest;

const StorageBuffer* GrassField::uploadIndirect(const std::vector<Draw>& draws, uint8_t fId, uint32_t pack) {
  static const bool enabled = [] {
    const char* env = std::getenv("OPENGOTHIC_GRASS_INDIRECT");
    return env!=nullptr && env[0]=='1' && env[1]=='\0';
    }();
  const bool cull = Grass::gpuCullEnabled();
  if(!enabled && !cull)
    return nullptr;

  std::vector<DrawIndexedIndirectCommand> records;
  records.reserve(draws.size());
  for(const auto& draw:draws) {
    DrawIndexedIndirectCommand record;
    record.indexCount    = uint32_t(Grass::packIndexCount(draw.links, pack));
    record.instanceCount = cull ? 0u : draw.instances;
    record.firstIndex    = uint32_t(Grass::packIndexOffset(draw.links, pack));
    records.push_back(record);
    }

  auto&        buffer = indirectGpu[fId];
  const size_t bytes  = records.size()*sizeof(records.front());
  if(buffer.byteSize()<bytes) {
    Resources::recycle(std::move(buffer));
    buffer = Resources::device().ssbo(BufferHeap::Upload, Uninitialized, std::max<size_t>(bytes+bytes/2, 4096));
    }
  buffer.update(records.data(), 0, bytes);
  return &buffer;
  }
