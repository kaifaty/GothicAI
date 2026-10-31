#pragma once

#include "baseimport.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// P17a: independently indexed lossless decoder planes and player PCM16. No runtime player changes.
namespace VideoData {
struct Source {
  std::string name;
  std::filesystem::path path; // selected VFS path, informational; identity uses name/size/SHA-256
  uint64_t size = 0;
  std::string sha256;
  };
struct Identity { std::string fingerprint, json; };
struct PlaneInfo { uint32_t stride=0, rows=0; };
struct Track { uint32_t sampleRate=0, channels=0; uint64_t samples=0; };
struct Info {
  uint32_t width=0, height=0, frames=0, fpsNum=0, fpsDen=0;
  std::array<PlaneInfo,4> planes;
  std::vector<Track> tracks;
  };
struct Frame {
  std::array<std::vector<uint8_t>,4> planes;
  std::vector<std::vector<int16_t>> audio; // interleaved samples, one vector for each original track
  };
struct Entry {
  Source source;
  std::string file, sha256;
  uint64_t size=0;
  Info info;
  };
class Reader {
  public:
    explicit Reader(const std::filesystem::path& file);
    const Info& info() const { return meta; }
    Frame readFrame(uint32_t number);
    Frame nextFrame();
    void verifyPayload(); // optional full payload pass; frame reads verify every record used for reconstruction
  private:
    struct Position { uint64_t offset=0, size=0; uint32_t flags=0; std::string sha256; };
    std::ifstream input;
    Info meta;
    std::vector<Position> index;
    std::array<std::vector<uint8_t>,4> previousPlanes;
    uint32_t next=0;
    bool havePrevious=false;
    uint64_t payloadStart=0, fileSize=0;
    std::string payloadSha256;
  };
std::string formatSha256();
std::vector<Source> sources(const std::filesystem::path& gothic);
Identity identify(const std::filesystem::path& gothic, const std::atomic_bool* cancelled=nullptr);
Identity identifyFromBase(const std::filesystem::path& base, const std::atomic_bool* cancelled=nullptr);
std::vector<Entry> readIndex(const std::filesystem::path& dir);
// Cache/store hits verify the index and file sizes; publication also verifies every file SHA-256.
bool matches(const std::filesystem::path& dir, const Identity& identity, bool verifyFiles=false,
             const std::atomic_bool* cancelled=nullptr);
void build(const std::filesystem::path& gothic, const std::filesystem::path& out,
           const BaseImport::Progress& progress = {}, const std::atomic_bool* cancelled=nullptr, size_t workers=12);
int convert(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly=false);
}
