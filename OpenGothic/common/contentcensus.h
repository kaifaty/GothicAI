#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AudioData { struct Sound; }
namespace MusicData { struct Source; struct Outcome; }

// Session-only accounting. Resources creates this object only with OPENGOTHIC_CENSUS.
class ContentCensus final {
  public:
    // Where the import found a game record of the base (imported_source): its index source, by value.
    struct ImportedSource {
      std::string_view origin, path, archive, sha256;
      uint64_t         size = 0;
      };

    explicit ContentCensus(const char* output);

    void opened(std::string_view name, std::string_view source) noexcept;
    void replaced(std::string_view name, std::string_view replacement) noexcept;
    void missing(std::string_view name, std::string_view reason) noexcept;
    void profile(std::string_view game, std::string_view outputUnits, int language) noexcept;
    void program(std::string_view consumer, std::string_view request, int language,
                 std::string_view branch, std::string_view record, std::string_view sha256,
                 const ImportedSource& source) noexcept;
    void outputUnits(std::string_view consumer, std::string_view request, int language,
                     std::string_view branch, std::string_view record, std::string_view sha256,
                     const ImportedSource& source) noexcept;
    void audioLoaded(std::string_view request, const AudioData::Sound& source) noexcept;
    void musicLoaded(std::string_view provider, std::string_view request,
                     const MusicData::Source& source, const MusicData::Outcome& outcome) noexcept;
    bool write() noexcept;

  private:
    using Key = std::pair<std::string, std::string>;
    std::filesystem::path output;
    std::mutex sync;
    std::map<Key, uint64_t> files;
    std::map<Key, uint64_t> replacements;
    std::map<Key, uint64_t> misses;
    std::string game, outputUnitsPrefix;
    int language = -1;
    bool hasProfile = false;
    struct ProgramSelection {
      std::string consumer, request, branch, record, sha256;
      std::string sourceOrigin, sourcePath, sourceArchive, sourceSha256;
      int language = -1;
      uint64_t sourceSize = 0;
      };
    std::vector<ProgramSelection> programs;
    std::vector<ProgramSelection> outputUnitsSelections;
    struct AudioSelection {
      std::string request, cls, payloadFile, payloadSha256;
      std::string sourceOrigin, sourcePath, sourceArchive, sourceSha256;
      uint64_t size = 0;
      };
    std::vector<AudioSelection> audioSelections;
    struct MusicSelection {
      std::string provider, request, kind, record, sha256;
      std::string sourceOrigin, sourcePath, sourceArchive, sourceSha256;
      uint64_t sourceSize = 0;
      };
    std::vector<MusicSelection> musicSelections;
  };
