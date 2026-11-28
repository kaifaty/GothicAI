#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

class SaveArchive;

namespace modkit {

using ItemPropValue = std::variant<int64_t, double, bool, std::string>;

// Optional per-instance properties attached to an Item (quality, durability, temperature, custom mod fields).
// Vanilla items keep a null unique_ptr<ItemInstanceProps> for zero heap and save overhead.
struct ItemInstanceProps final {
  static constexpr uint16_t FormatVersion = 1;

  std::optional<float> quality;
  std::optional<float> durability;
  std::optional<float> maxDurability;
  std::optional<float> temperature;
  bool                 unique = false;
  std::map<std::string, ItemPropValue> custom;

  bool empty() const noexcept;
  uint64_t stackKey(uint32_t archetypeId) const noexcept;
  bool equalsCanonical(const ItemInstanceProps& other) const noexcept;

  static bool canStack(const ItemInstanceProps* a, const ItemInstanceProps* b) noexcept;

  std::vector<std::pair<std::string, std::string>> tooltipRows() const;

  void save(SaveArchive& fout) const;
  static ItemInstanceProps load(SaveArchive& fin);
  };

} // namespace modkit
