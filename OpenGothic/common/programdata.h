#pragma once

// P12: our own record of a compiled Daedalus program (docs/asset-migration.md) and the
// programs/programs.json index that maps the engine's and the server's original choices to it.

#include "baseimport.h"

#include <zenkit/DaedalusScript.hh>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ProgramData {

// One physical origin of a .DAT that the import looked at.
struct Source {
  std::string origin;          // "host" | "archive"
  std::string path;            // host: relative to the installation ('/'); archive: full VFS path
  std::string archive;         // upper VDF name when origin=="archive"
  uint64_t    size = 0;
  std::string sha256;          // original bytes: imported provenance only
  std::string program;         // record name (<name>.json); empty when skipped
  std::string recordSha256;    // identity of that record (JSON bytes then BIN bytes)
  uint64_t    trailingBytes = 0;
  std::string skipped;         // reason when this is not an importable program
  };

// The import's run of Gothic::loadScript for a canonical request and language (evidence).
struct Selection {
  std::string request;
  int32_t     language = -1;
  std::string step;            // census branch name
  std::string path;            // VFS path or host relative path; empty when nothing was found
  std::optional<size_t> source;
  };

struct Index {
  std::filesystem::path              dir;
  BaseImport::Files                  archives;    // (upper name, size), sorted
  BaseImport::Files                  looseFiles;  // (relative path, size) of every host source
  std::map<std::string,std::string>  programs;    // record name -> record identity
  std::vector<Source>                sources;
  std::map<std::string,size_t>       vfs;         // ASCII-upper basename -> source
  std::map<std::string,size_t>       host;        // relative path -> source
  std::vector<Selection>             selections;
  std::optional<size_t>              serverGameDat;

  // Gothic::loadScript's two VFS steps, as zenkit::Vfs::find matches names: trailing whitespace
  // trimmed, ASCII case ignored; first the whole request, then the part after the last '\\' or
  // '/'. *step = "vfs_find_request" | "vfs_find_basename". nullptr when neither matches.
  const Source* findVfs(std::string_view request, std::string_view* step = nullptr) const;
  // The host file Gothic::loadScript's probes chose, relative to the installation: exact, else
  // the only ASCII-case-insensitive match. nullptr when not imported.
  const Source* findHost(std::string_view relativePath) const;
  // The physical GOTHIC.DAT of the server and storygraph; nullptr when the installation has none.
  const Source* server() const;
  // The record file of a source that is a program.
  std::filesystem::path path(const Source& source) const;
  // findVfs, findHost and server throw std::runtime_error when the matched source is skipped.
  };

// Throws std::runtime_error("program data: <file>: <reason>") on missing, corrupt or
// unsupported data.
Index readIndex(const std::filesystem::path& dir);   // dir/programs.json

struct Encoded {
  std::string          name;    // hex SHA-256 of the version byte followed by bin
  std::string          json;
  std::vector<uint8_t> bin;
  std::string          sha256;  // record identity: json bytes then bin bytes
  };
Encoded encodeProgram(const zenkit::DaedalusProgramRecord& program);
// Writes dir/<name>.json and dir/<name>.bin; returns the record identity, sets *name.
std::string writeProgram(const zenkit::DaedalusProgramRecord& program, const std::filesystem::path& dir,
                         std::string* name = nullptr);
// `file` is <name>.json. With actualSha256, the identity of the bytes that were decoded.
zenkit::DaedalusProgramRecord readProgram(const std::filesystem::path& file, std::string* actualSha256 = nullptr);
std::string recordSha256(const std::filesystem::path& file);
// Empty when equal: version, symbol count and order, every field, value arrays (float bits), bytecode.
std::string compare(const zenkit::DaedalusProgramRecord& expected, const zenkit::DaedalusProgramRecord& actual);

}
