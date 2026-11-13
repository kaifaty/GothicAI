#pragma once

#include "worlddata.h"
#include "graphics/voblight.h"
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

// Slot-local bundles accept only add entries, using the same validation as a world layer.
// Ignores runtime flags and replaces tree only when the whole file is valid.
Result readTree(std::vector<std::shared_ptr<asset::VirtualObject>>& tree,
                const std::filesystem::path& baseRoot, const std::filesystem::path& file,
                const std::filesystem::path& contentRoot = {},
                const std::map<std::string,std::filesystem::path>* ownMeshes = nullptr);

} // namespace VobLayer
