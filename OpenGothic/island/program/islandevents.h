#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

class ChunkResidency;

namespace island {
class Hero;

class Events final {
  public:
    explicit Events(const std::filesystem::path& file);
    ~Events();
    void watch(ChunkResidency& residency, const Hero& hero, const uint64_t& time);
    void emit(const char* kind, uint64_t time, const Hero& hero,
              std::initializer_list<std::pair<const char*,std::string>> fields = {});
    void observe(uint64_t time, const Hero& hero);
    void finish(uint64_t time, const Hero& hero, uint64_t frames, const std::vector<uint32_t>& frameMs,
                int code, bool completed);

  private:
    void census(uint64_t time, const Hero& hero);
    std::ofstream output;
    ChunkResidency* residency = nullptr;
    uint64_t subscription = 0;
    std::map<std::string,std::pair<uint32_t,uint32_t>> textures;
  };
}
