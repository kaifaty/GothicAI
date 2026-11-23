#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Tempest {
namespace Detail {

// On-disk cache of the SPIR-V -> MSL translation (SPIRV-Cross), one file per set of translator options.
// The file is keyed by the options, the SPIRV-Cross and translator sources (hashed at configure time) and a
// format constant; entries inside by two hashes and the size of the SPIR-V. A miss or a damaged file means a
// live translation; a write keeps the shaders of this session (temp + rename).
class MtShaderCache {
  public:
    struct Key {
      uint64_t h0 = 0, h1 = 0;
      uint32_t size = 0;
      bool operator==(const Key& o) const { return h0==o.h0 && h1==o.h1 && size==o.size; }
      };
    // Final binding table of MtShader: entry k comes from reflection binding `src` with these MSL slots.
    struct Bind {
      uint32_t src = 0, mslBinding = 0, mslBinding2 = 0, mslSize = 0;
      };
    struct Entry {
      std::string       msl;
      bool              bufferSizeBuffer = false;
      uint32_t          reflected = 0; // size of the reflection table before merging
      std::vector<Bind> lay;
      };

    MtShaderCache() = default;
    ~MtShaderCache();

    void init(const std::string& identity);
    bool enabled() const { return !file.empty(); }
    bool verify()  const { return verifyAll; }

    static Key key(const void* spirv, size_t size);
    bool find(const Key& k, Entry& out);
    void put (const Key& k, Entry e);
    void verified(bool ok);

    void save(const char* reason = "request");
    void saveAsync();

  private:
    struct KeyHash { size_t operator()(const Key& k) const { return size_t(k.h0); } };
    struct Slot {
      Entry entry;
      bool  used = false;
      };

    bool load();

    std::mutex                               sync;
    std::unordered_map<Key,Slot,KeyHash>     entries;
    std::filesystem::path                    file;
    std::string                              identity;
    bool                                     verifyAll = false;
    bool                                     dirty     = false;
    std::atomic<uint32_t>                    hits{0}, misses{0}, checked{0};
    std::mutex                               saveSync;
    std::thread                              saver;
  };

}
}
