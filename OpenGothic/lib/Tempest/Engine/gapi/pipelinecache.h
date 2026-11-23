#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Tempest {
namespace Detail {

// Shared file handling of the on-disk pipeline caches (Vulkan VkPipelineCache, Metal MTLBinaryArchive).
// The application chooses the directory before the device is created; an empty directory turns the cache off.
namespace PipelineCacheFile {
  void                  setDirectory(const std::filesystem::path& dir);
  std::filesystem::path directory();

  // SPIR-V -> MSL translation cache (Metal). Its own directory and switch; verify translates live on every
  // hit and stops the process on any difference.
  void                  setShaderDirectory(const std::filesystem::path& dir, bool verify);
  std::filesystem::path shaderDirectory();
  bool                  shaderVerify();

  uint64_t              hash(const void* data, size_t size, uint64_t seed = 0xcbf29ce484222325ull);
  std::string           hex(uint64_t v);
  // Word-at-a-time hash for large blobs (cache keys and payload checks); stable across builds and platforms.
  uint64_t              hashWords(const void* data, size_t size, uint64_t seed);

  // Whole file, or false when it is missing or larger than the limit.
  bool                  read(const std::filesystem::path& file, std::vector<uint8_t>& out, size_t maxSize);
  // Temporary file in the same directory, then rename: readers never see a partial file.
  bool                  writeAtomic(const std::filesystem::path& file, const void* data, size_t size);
  std::filesystem::path temporaryPath(const std::filesystem::path& file);
  }

}
}
