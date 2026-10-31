#pragma once

// P15: original sounds and speech as exact copies of the selected WAV/OGG bytes in the base
// (audio/<two hex>/<sha256>.<ext>), indexed by audio/audio.json under the engine's own request names.
// The engine keeps decoding them with the unchanged Tempest::Sound.

#include "baseimport.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace AudioData {

struct Sound {
  std::string payload;       // SHA-256 of the payload bytes (verified at every read)
  std::string file;          // "<two hex>/<sha256>.<wav|ogg>", relative to the family directory
  uint64_t    size = 0;      // payload bytes
  std::string cls;           // "speech" | "sound"
  std::string origin;        // "host" | "archive"
  std::string path;          // host: relative to the installation; archive: full VFS path
  std::string archive;       // upper VDF name when origin=="archive"
  std::string sourceSha256;  // imported provenance of the original bytes
  };

struct Index {
  std::filesystem::path       dir;
  BaseImport::Files           archives;    // (upper name, size), sorted
  BaseImport::Files           looseFiles;  // (relative path, size) of every host source
  std::map<std::string,Sound> sounds;      // ASCII-upper request name -> sound

  // The engine's sound lookup, as zenkit::Vfs::find matches names: trailing std::isspace trimmed,
  // ASCII case ignored, exact name; no basename fallback. nullptr = not an imported sound.
  const Sound* find(std::string_view request) const;
  };

// dir/audio.json only; no payload file is opened or stat-ed. Strict members, hex payload names, payload
// file names "<first two hex>/<sha256>.<wav|ogg>", every sound's payload present with the same size.
// Throws std::runtime_error("audio data: <file>: <reason>").
Index readIndex(const std::filesystem::path& dir);

// Reads the payload file once into `out`, then requires out.size()==s.size and SHA-256(out)==s.payload.
// Throws std::runtime_error("audio data: <request> (<file>): missing" | "size <n>, expected <m>" |
// "sha256 differs"); `out` is empty after a failure.
void readPayload(const Index& index, const Sound& s, std::string_view request, std::vector<uint8_t>& out);

}
