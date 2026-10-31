#pragma once

#include "basestore.h"

// The converter uses the same ready-base selection as the game, with import enabled on a miss.
namespace BaseStore {
Selection ensure(const std::filesystem::path& gothic, const BaseImport::Files& mounted,
                 const BaseImport::Progress& log);
Selection ensureVideos(const std::filesystem::path& gothic, const BaseImport::Progress& log,
                       std::shared_ptr<std::atomic_bool> cancelled={}, size_t workers=12);
}
