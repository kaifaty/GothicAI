#pragma once

#include "worlddata.h"
#include <map>

namespace VobLayer {

struct Result {
  size_t added = 0, removed = 0, moved = 0;
  std::vector<std::string> errors;
  };

// Runtime-only own records identify additions without changing the base schema.
bool isAdded(const asset::VirtualObject& vob);
const std::string& key(const asset::VirtualObject& vob);

// Sorted *.json files commit individually; a rejected file leaves the world untouched.
Result apply(WorldData::Data& data, const std::filesystem::path& baseWorldDir,
             const std::filesystem::path& worldContentDir,
             const std::map<std::string,std::filesystem::path>* ownMeshes = nullptr);
// Includes the whole replacement and earlier valid files; ignores runtime flags.
Result check(const std::filesystem::path& baseWorldDir, const std::filesystem::path& file,
             const std::filesystem::path& contentRoot = {});

} // namespace VobLayer
