#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ScriptSymbolCatalog {

enum class Kind : uint8_t {
  Other    = 0,
  Npc      = 1,
  Item     = 2,
  Info     = 3,
  Constant = 4,
  Variable = 5,
  Function = 6,
  };

struct Entry {
  std::string name;
  Kind        kind = Kind::Other;
  };

class Catalog final {
  public:
    explicit Catalog(std::vector<Entry> entries);
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    const Entry* at(size_t id) const;
    std::optional<uint32_t> find(std::string_view name) const;

  private:
    const std::vector<Entry>                       entries;
    std::unordered_map<std::string_view,uint32_t>   names;
  };

// Offline importer and contracts. The program identity selects the catalog.
std::unique_ptr<Catalog> load(const std::filesystem::path& base, std::string_view program);
void write(const std::filesystem::path& base, std::string_view program, std::span<const Entry> entries);

}
