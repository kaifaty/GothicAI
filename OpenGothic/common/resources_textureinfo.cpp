#include "resources.h"

std::vector<Resources::TextureInfo> Resources::textureSnapshot() {
  if(inst==nullptr)
    return {};
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  std::vector<TextureInfo> result;
  result.reserve(inst->textureRecords.size());
  for(const auto& [texture,record]:inst->textureRecords) {
    (void)texture;
    result.push_back({record.source, record.firstMip, record.reloads, record.resident});
    }
  return result;
  }
