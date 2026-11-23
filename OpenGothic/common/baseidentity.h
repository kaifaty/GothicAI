#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <cstdint>

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

std::string sha256(std::string_view bytes);
std::string sha256File(const std::filesystem::path& file);
// Every nonempty file under Data (Resources::detectVdf) in the engine's mount order,
// .mod files included and flagged; the caller applies the profile's filter.
std::vector<RuntimeArchive> runtimeArchives(const std::filesystem::path& gothic);
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
}
