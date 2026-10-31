#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <zenkit/Logger.hh>

namespace zenkit { class Vfs; }

namespace BaseImport {
using Files = std::vector<std::pair<std::string,uintmax_t>>;
using Progress = std::function<void(std::string_view)>;

struct Identity {
  std::string fingerprint;
  std::string json;
  };

// One file of Resources::loadVdfs's scan of Data, in its mount order.
struct RuntimeArchive {
  std::filesystem::path path;
  std::string           name; // upper case file name, as the engine reports mounted archives
  uintmax_t             size = 0;
  int64_t               timestamp = -1; // VDF header value Resources::vdfTimestamp sorts by
  bool                  mod = false;
  };

// Forward the application's ZenKit log callback here. Only active import reads
// consume errors; normal archive loading keeps its existing behavior.
void onZenkitLog(zenkit::LogLevel level, const char* component, const char* message);

int convertWorld(const std::filesystem::path& gothic, const std::string& name, const std::filesystem::path& out);
int verifyWorld(const std::filesystem::path& gothic, const std::string& name, const std::filesystem::path& out,
                const std::filesystem::path& overrides = {});
int convertZen(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
int convertTextures(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
int convertMeshes(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
int convertModels(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// P11: fonts/fonts.json and one record per selected FNT basename (common/fontdata.h),
// selected through the engine's runtime VFS (mountRuntime).
int convertFonts(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// P12: programs/programs.json and one record per distinct program (common/programdata.h) for every
// .DAT of the runtime VFS, below the nine _compiled directories and the server's GOTHIC.DAT.
// An installation without programs gives an empty index.
int convertPrograms(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// P14: outputunits/outputunits.json and one record per distinct cutscene library (common/outputunits.h)
// for every .BIN/.DAT of the runtime VFS, the engine's host and language candidates and the server's
// and storygraph's choices. Fails when the server's native message view differs from its original
// scanner. An installation without libraries gives an empty index.
int convertOutputUnits(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// One external OU.BIN/OU.DAT to a native record at `record` (import only; prints both SHA-256).
int convertOutputUnitsFile(const std::filesystem::path& source, const std::filesystem::path& record);
// P15: audio/audio.json and one exact copy per distinct payload (common/audiodata.h) of every .WAV/.OGG
// request of the runtime VFS and its Vfs::find winner. An installation without sounds gives an empty index.
int convertAudio(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// P16: music/music.json and the GothicKit and Dx8 records (common/musicdata.h) of every SGT/STY/DLS either
// provider selects: each music basename of the runtime VFS and its Vfs::find winner, and every music file
// directly in the six Dx8 search directories. Stops on a GothicKit string its parser cannot convert exactly
// and on a reference naming a file of another kind. An installation without music gives an empty index.
int convertMusic(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// P10: sources/sources.json, the selected inputs of the not yet migrated families
// (FNT, audio, BIK, SGT/STY/DLS, the eight DATs, OU, server readers) with origin and SHA-256.
int convertSources(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);

std::string sha256(std::string_view bytes);
std::string sha256File(const std::filesystem::path& file);
// Every nonempty file under Data (Resources::detectVdf) in the engine's mount order,
// .mod files included and flagged; the caller applies the profile's filter.
std::vector<RuntimeArchive> runtimeArchives(const std::filesystem::path& gothic);
// Resources::loadVdfs without a mod ini, then Resources::mountWork: the engine's VFS of the
// supported Gothic II installation. Returns the mounted archives in mount order and sets
// `work` to the game's _work path (mounted with NONE only when it is a directory).
std::vector<RuntimeArchive> mountRuntime(zenkit::Vfs& vfs, const std::filesystem::path& gothic,
                                         std::filesystem::path& work);
// The identity binds the format, the content and mount order of the mounted archives
// and the content of every loose _work input. `mounted` is the engine's list.
Identity identify(const std::filesystem::path& gothic);
Identity identify(const std::filesystem::path& gothic, Files mounted);
// The same source membership/order as identify, with sizes and modification times only.
std::string snapshot(const std::filesystem::path& gothic);
std::string snapshot(const std::filesystem::path& gothic, Files mounted);
// Read a current importer identity without accessing the source installation.
Identity readIdentity(const std::filesystem::path& dir);
// base.json binds the source census and base-files.json by size and SHA-256.
// Written last; the native manifest lists every base file with size and SHA-256.
void writeBaseMetadata(const std::filesystem::path& dir, const Identity& identity);
// Startup hashes JSON indexes and checks every payload size. Publication also hashes payloads.
bool matches(const std::filesystem::path& dir, const Identity& identity, bool payloadHashes=false,
             std::string* reason=nullptr);
void build(const std::filesystem::path& gothic, const std::filesystem::path& out, const Progress& progress = {});
}
