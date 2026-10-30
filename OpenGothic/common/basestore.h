#pragma once

#include "baseimport.h"
#include <memory>
#include <atomic>
#include <future>
#include <stdexcept>

namespace VideoData { struct Identity; }

namespace BaseStore {
class Error : public std::runtime_error {
  public:
    explicit Error(const std::string& reason)
      : std::runtime_error("Game base is not ready: "+reason+
          "\nStart the game through the launcher or run content-convert resolve <game directory>.") {}
  };
class Lock;
struct Selection {
  std::filesystem::path path;
  // Keep the cache entry leased until its resource owner is destroyed.
  std::shared_ptr<Lock> lease;
  // Optional completion wait for callers that need the store copy, not shutdown.
  std::future<void> publication;
  };
// Off: a cache hit never starts the background copy to the store. The headless body runner
// forks its loaded world, which needs a single thread, and does not live long enough to own it.
void setPublication(bool enabled);
std::filesystem::path cacheRoot();
// A nonempty OPENGOTHIC_BASE selects and validates an explicit base.
// Otherwise OPENGOTHIC_BASE_FINGERPRINT selects a ready cache/store base without an installation.
// Empty has the same meaning as unset: automatic resolution.
// Import-only callers provide these hooks through basestoreimport.h; the game leaves them empty.
using Import = std::function<void(const std::filesystem::path&)>;
using IdentifyVideos = std::function<VideoData::Identity()>;
Selection resolve(const std::filesystem::path& gothic, const BaseImport::Files& mounted,
                  const BaseImport::Progress& log, const Import& import={});
// P17: the independently fingerprinted video part. OPENGOTHIC_BASE does not
// select this part; OPENGOTHIC_VIDEOS_FINGERPRINT selects a ready part without an installation.
// An explicitly empty video fingerprint disables selection; unset retains automatic selection.
// Its cache and store are below the normal cache/store roots.
// The game supplies the already selected main base; importer hooks do not use it.
Selection resolveVideos(const std::filesystem::path& gothic, const std::filesystem::path& mainBase,
                        const BaseImport::Progress& log,
                        std::shared_ptr<std::atomic_bool> cancelled={}, const Import& import={},
                        const IdentifyVideos& identify={});
}
