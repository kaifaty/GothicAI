#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace island {
class Hero;
class View;

class Events final {
  public:
    explicit Events(const std::filesystem::path& file);
    void emit(const char* kind, uint64_t time, const Hero& hero,
              std::initializer_list<std::pair<const char*,std::string>> fields = {});
    void observe(uint64_t time, const Hero& hero, const View& view);
    void finish(uint64_t time, const Hero& hero, uint64_t frames, const std::vector<uint32_t>& frameMs,
                int code, bool completed);

  private:
    void census(uint64_t time, const Hero& hero, bool final);
    std::ofstream output;
    uint64_t published = 0, released = 0, sampled = 0;
    std::set<std::string> textures;
  };
}
