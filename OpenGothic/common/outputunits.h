#pragma once

// P14: the dialog line library (OU.BIN/OU.DAT, a ZenGin cutscene library) as our own record, a
// WorldData::ZenFile of kind "cutscene_library" with its roots in archive order, and the
// outputunits/outputunits.json index that maps the engine's, the server's and storygraph's original
// choices to it. Each consumer derives its own view from the same record.

#include "baseimport.h"
#include "worlddata.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OutputUnits {

// One physical origin of a .BIN/.DAT the import looked at.
struct Source {
  std::string origin;          // "host" | "archive"
  std::string path;            // host: relative to the installation ('/'); archive: full VFS path
  std::string archive;         // upper VDF name when origin=="archive"
  uint64_t    size = 0;
  std::string sha256;          // original bytes: imported provenance only
  std::string record;          // record name (<record>.json); empty when skipped
  std::string recordSha256;    // identity of that record: SHA-256 of its JSON bytes
  std::string skipped;         // reason when this is not an importable cutscene library
  };

// The import's run of GameScript::loadDialogOU for the default prefix (evidence only).
struct Selection {
  int32_t     language = -1;
  std::string candidate;       // "OU.BIN" | "OU.DAT"
  std::string step;            // census branch name
  std::string path;            // VFS path or host relative path; empty when nothing was found
  std::optional<size_t> source;
  };

struct Index {
  std::filesystem::path             dir;
  BaseImport::Files                 archives;    // (upper name, size), sorted
  BaseImport::Files                 looseFiles;  // (relative path, size) of every host source
  std::map<std::string,std::string> records;     // record name -> identity
  std::vector<Source>               sources;
  std::map<std::string,size_t>      vfs;         // ASCII-upper basename of every VFS .BIN/.DAT -> source
  std::map<std::string,size_t>      host;        // relative path -> source
  std::vector<Selection>            selections;
  std::optional<size_t>             serverOuPySource;

  // GameScript::loadDialogOU's two VFS steps, as zenkit::Vfs::find matches names: trailing whitespace
  // trimmed, ASCII case ignored; first the whole request, then the part after the last '\\' or '/'.
  // *step = "vfs_find_request" | "vfs_find_basename". Skipped sources are returned too; nullptr only
  // when the name is not a VFS .BIN/.DAT at all.
  const Source* findVfs(std::string_view request, std::string_view* step = nullptr) const;
  // A host file relative to the installation: exact, else the only ASCII-case-insensitive match
  // (ambiguity throws). Skipped sources are returned too; nullptr when the file was not imported.
  const Source* findHost(std::string_view relativePath) const;
  // The file server/src/gothic_ai/gamedata/ou.py find_ou_file selects; nullptr when there is none.
  const Source* serverOuPy() const;
  // The record file of a library source; throws for a skipped source.
  std::filesystem::path path(const Source& source) const;
  };

// dir/outputunits.json. Throws std::runtime_error("output units: <file>: <reason>") on missing,
// corrupt or unsupported data.
Index readIndex(const std::filesystem::path& dir);

// A record: one read, *actualSha256 = SHA-256 of exactly the bytes then decoded. Kind must be
// "cutscene_library". Throws std::runtime_error("output units: <file>: <reason>").
WorldData::ZenFile readLibrary(const std::filesystem::path& file, std::string* actualSha256 = nullptr);
// Writes a library record to `file` and returns its identity.
std::string writeLibrary(const WorldData::ZenFile& library, const std::filesystem::path& file);

// The engine's and storygraph's view: the roots std::sort-ed by raw name with CutsceneLibrary::load's
// comparator (same algorithm, same input order: the same order as before, duplicates included).
std::vector<std::shared_ptr<asset::CutsceneBlock>> sortedBlocks(const WorldData::ZenFile& library);
// CutsceneLibrary::block_by_name: lower_bound over `sorted`, without an equality check, so an absent
// name returns its successor; nullptr past the end.
std::shared_ptr<asset::CutsceneBlock> blockByName(const std::vector<std::shared_ptr<asset::CutsceneBlock>>& sorted,
                                                  std::string_view name);
// CutsceneBlock::get_message: follows nested blocks; nullptr for a null nested block or a null message.
std::shared_ptr<asset::ConversationMessageEvent> message(const asset::CutsceneBlock& block);
// The server's view: every message object once (aliases visited once), depth-first from the roots in
// archive order, i.e. in archive definition order. Raw name/text bytes; consumers apply their own rules.
std::vector<std::shared_ptr<asset::ConversationMessageEvent>> messagesInArchiveOrder(const WorldData::ZenFile& library);

}
