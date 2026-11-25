#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <filesystem>

namespace NativeBaseCatalog { struct Selection; }

namespace NativeWorldPlacement {

struct ShipCompanion {
  std::string_view flag, npc, partner = {};
  };
inline constexpr ShipCompanion captains[] = {
  {"JORGENISCAPTAIN", "VLK_4250_JORGEN_DI"},
  {"TORLOFISCAPTAIN", "SLD_801_TORLOF_DI"},
  {"JACKISCAPTAIN",   "VLK_444_JACK_DI"},
  };
inline constexpr ShipCompanion crew[] = {
  {"LEE_ISONBOARD",      "SLD_800_LEE_DI"},
  {"MILTENNW_ISONBOARD", "PC_MAGE_DI",        "LESTER_ISONBOARD"},
  {"LESTER_ISONBOARD",   "PC_PSIONIC_DI",     "MILTENNW_ISONBOARD"},
  {"MARIO_ISONBOARD",    "NONE_101_MARIO_DI"},
  {"WOLF_ISONBOARD",     "SLD_811_WOLF_DI"},
  {"VATRAS_ISONBOARD",   "VLK_439_VATRAS_DI"},
  {"BENNET_ISONBOARD",   "SLD_809_BENNET_DI"},
  {"DIEGO_ISONBOARD",    "PC_THIEF_DI",       "LARES_ISONBOARD"},
  {"GORN_ISONBOARD",     "PC_FIGHTER_DI"},
  {"LARES_ISONBOARD",    "VLK_449_LARES_DI",  "DIEGO_ISONBOARD"},
  {"BIFF_ISONBOARD",     "DJG_713_BIFF_DI"},
  {"ANGAR_ISONBOARD",    "DJG_705_ANGAR_DI"},
  {"GIRION_ISONBOARD",   "PAL_207_GIRION_DI"},
  };

struct Spawn {
  uint32_t instance;
  std::string point;
  bool npc;
  };

struct SetFlag { std::string name; int32_t value; };
struct SetTime { int32_t hour, minute; };
struct PlayVideo { std::string name; };
struct KillNpc { uint32_t instance; };
struct InitLanguage {};
using StartupStep = std::variant<Spawn,SetFlag,SetTime,PlayVideo,KillNpc,InitLanguage>;

struct Room {
  std::string label;
  int32_t guild;
  };

struct RoomChoice {
  std::string flag;
  int32_t expected;
  Room equal, otherwise;
  };

struct MobRoutine {
  int32_t hour, minute;
  std::string name;
  int32_t state;
  };

struct RoomInitializer {
  std::vector<MobRoutine> mobs;
  std::vector<std::variant<Room,RoomChoice>> rooms;
  };

struct Definitions {
  std::map<std::string,std::vector<StartupStep>,std::less<>> startup;
  std::vector<Spawn> dragonIsland, addonChapter4;
  std::map<std::string,RoomInitializer,std::less<>> rooms;
  std::string dragonArrival;

  static std::filesystem::path section(const std::filesystem::path& base,std::string_view programSha);
  static Definitions load(const std::filesystem::path& base,std::string_view programSha,
                          const NativeBaseCatalog::Selection& catalog);
  };

}
