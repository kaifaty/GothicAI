#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace NativeBaseCatalog {

inline constexpr int projectionVersion = 3;
inline constexpr int catalogVersion = 5;

struct State {
  uint32_t id = 0;
  std::string name;
  std::optional<uint32_t> loop, end;
  bool operator==(const State&) const = default;
  };

enum class Alias : uint8_t { Npc, Item };

using InitialValue = std::variant<std::vector<int32_t>,std::vector<float>,std::vector<std::string>,Alias>;

// The ID identifies the same state in a v57 save. Aliases start without a live object.
struct InitialRow {
  uint32_t id = 0;
  std::string name;
  InitialValue value;
  bool operator==(const InitialRow&) const = default;
  };

struct Identity {
  uint32_t id = 0;
  std::string name;
  bool operator==(const Identity&) const = default;
  };

struct Identities {
  std::vector<Identity> npcs, items, dialogs;
  bool operator==(const Identities&) const = default;
  };

struct Constant {
  uint32_t id = 0;
  InitialValue values;
  bool operator==(const Constant&) const = default;
  };

using Constants = std::map<std::string,Constant,std::less<>>;

struct Selection {
  std::string fight;
  int language = -1;
  std::vector<State> states;
  std::vector<InitialRow> initialState;
  Identities identities;
  Constants constants;
  };

const Identity* findIdentity(std::span<const Identity> rows, uint32_t id);
const Identity* findIdentity(std::span<const Identity> rows, std::string_view name);

std::filesystem::path section(const std::filesystem::path& base);
Selection read(const std::filesystem::path& base);
const Constant* findConstant(const Selection& selection,std::string_view name);
void write(const std::filesystem::path& base, const Selection& selection);

}
