#pragma once

#include "baseidentity.h"
#include <memory>
#include <atomic>
#include <future>
#include <stdexcept>
#include <string_view>

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
bool metadataAuditEnabled() noexcept;
std::filesystem::path cacheRoot();
// A nonempty OPENGOTHIC_BASE selects and validates an explicit base.
// Otherwise OPENGOTHIC_BASE_FINGERPRINT selects a ready cache/store base without an installation.
// Empty has the same meaning as unset: automatic resolution.
// Import-only callers provide these hooks through basestoreimport.h; the game leaves them empty.
using Import = std::function<void(const std::filesystem::path&)>;
using IdentifyVideos = std::function<VideoData::Identity()>;
struct Profile {
  // An empty value keeps the compatibility selection rules.
  std::string_view stockProgram;
  // A runtime profile never substitutes a combined source base.
  bool             projectedOnly=false;
  void           (*validateProgramMetadata)(const std::filesystem::path&, std::string_view)=nullptr;
  // A production catalog profile selects ready data only. Converter callers can supply a projection recipe.
  int              catalogVersion=0;
  };
inline constexpr int nativeProjectionVersion=2;
bool isNativeProjection(const std::filesystem::path& base);
// Lease and verify an explicit prepared base for asset readers, without selecting a game profile.
Selection openPreparedBase(const std::filesystem::path& base, BaseImport::Identity& identity);
// Legacy definition artifacts are kept only in offline bases.
bool isFullSymbolFile(const std::filesystem::path& relative);
bool isLegacyScriptFile(const std::filesystem::path& relative);
// Derive offline v2 or runtime v3 identities from an already verified combined base.
BaseImport::Identity nativeProjectionIdentity(const std::filesystem::path& combined);
BaseImport::Identity nativeProjectionIdentity(const BaseImport::Identity& combined, std::string_view manifestSha256);
BaseImport::Identity nativeCatalogIdentity(const std::filesystem::path& combined, int catalogVersion);
BaseImport::Identity nativeCatalogIdentity(const BaseImport::Identity& combined, std::string_view manifestSha256,
                                          int catalogVersion);
// A projector uses the existing import/cache path with its derived identity.
Selection resolve(const std::filesystem::path& gothic, const BaseImport::Files& mounted,
                  const BaseImport::Progress& log, const Import& import={}, Profile profile={},
                  const BaseImport::Identity* projected=nullptr, const std::filesystem::path& explicitSelection={});
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
