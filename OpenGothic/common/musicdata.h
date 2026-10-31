#pragma once

// P16: adaptive music (SGT/STY/DLS) as own parsed records for both existing providers, and the
// music/music.json index mapping each provider's original selection to them.
//
// Records: music/<projection>/<kind>/<BIN_SHA>.json + .bin, projection "gk" (what GothicKit's parser
// produced, see common/musicnative.h) or "dx8" (what the Dx8 Riff constructors produced, encoded here),
// kind "segment" | "style" | "dls". Record key "<projection>/<kind>/<BIN_SHA>"; record identity =
// SHA-256(JSON bytes || BIN bytes). The imported original SHA-256 is provenance only.

#include "baseimport.h"
#include "dmusic/dlscollection.h"
#include "dmusic/segment.h"
#include "dmusic/style.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MusicData {

// One provider's import outcome for a source: exactly one of a record key or a failure.
// GK failure = the DmResult name the old parser returned (never "SUCCESS"); Dx8 failure = the old
// constructor's exception text (an empty text is still a failure).
struct Outcome {
  std::string                record;     // "<projection>/<kind>/<BIN_SHA>", empty on failure
  std::string                identity;   // record identity, empty on failure
  std::optional<std::string> failure;
  };

struct Source {
  std::string origin;          // "host" | "archive"
  std::string path;            // host: relative to the installation ('/'); archive: full VFS path
  std::string archive;         // upper VDF name when origin=="archive"
  uint64_t    size = 0;
  std::string sha256;          // original bytes: imported provenance only
  std::string kind;            // "segment" | "style" | "dls"
  Outcome     gk, dx8;
  };

struct Index {
  std::filesystem::path             dir;
  BaseImport::Files                 archives;    // (upper name, size), sorted
  BaseImport::Files                 looseFiles;  // (relative path, size) of every host source
  std::map<std::string,std::string> records;     // record key -> identity
  std::vector<Source>               sources;
  std::map<std::string,size_t>      gk;          // ASCII-upper VFS basename -> source
  std::map<std::string,size_t>      dx8;         // installation-relative host path -> source

  // The GothicKit resolver's lookup (Resources' gothicAssets.find), as zenkit::Vfs::find matches names:
  // trailing whitespace trimmed, ASCII case ignored, exact name. nullptr = not found.
  const Source* findGk(std::string_view name) const;
  // A Dx8 host file the caller's openSourceFile-equivalent loop selected, relative to the installation:
  // exact, else the only ASCII-case-insensitive match (ambiguity throws). nullptr = not in the base.
  const Source* findDx8(std::string_view relativePath) const;
  };

// dir/music.json only (no record is opened). Strict members, exclusive outcomes, record keys present in
// "records", host paths relative without "..". Throws std::runtime_error("music data: <file>: <reason>").
Index readIndex(const std::filesystem::path& dir);

// One read of the record's JSON and BIN; requires the JSON's projection, kind and fixed sibling buffer to
// match `key`, byte_length == BIN size and SHA-256(JSON||BIN) == index.records[key] before returning the
// BIN bytes. Throws std::runtime_error("music data: <key>: <reason>").
std::vector<uint8_t> readRecord(const Index& index, std::string_view key);

// Dx8 projection codec: every parsed field memberwise, owning string lengths and raw UTF-16 units kept.
// Decoders require exact consumption and throw std::runtime_error("music data: <reason>").
std::vector<uint8_t>       encodeDx8(const Dx8::Segment& segment);
std::vector<uint8_t>       encodeDx8(const Dx8::Style& style);
std::vector<uint8_t>       encodeDx8(const Dx8::DlsCollection::Parsed& dls);
Dx8::Segment               decodeDx8Segment(const std::vector<uint8_t>& bin);
Dx8::Style                 decodeDx8Style(const std::vector<uint8_t>& bin);
Dx8::DlsCollection::Parsed decodeDx8Dls(const std::vector<uint8_t>& bin);

}
