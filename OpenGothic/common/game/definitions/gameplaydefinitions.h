#pragma once

#include "definitionjson.h"
#include "world/records/focusrecord.h"
#include "world/records/guildrecord.h"
#include "world/records/spellrecord.h"

#include <filesystem>
#include <map>
#include <memory>
#include <utility>
#include <vector>

class GameplayDefinitions final {
  public:
    static constexpr std::string_view stockProgram = "a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c";
    static constexpr std::string_view format = "gothic-gameplay-definitions";
    static constexpr int version = 1;
    static constexpr std::string_view focusNames[] = {"FOCUS_NORMAL","FOCUS_MELEE","FOCUS_RANGED","FOCUS_MAGIC"};

    static std::filesystem::path section(const std::filesystem::path& base) {
      return base/"definitions"/stockProgram/"gameplay.json";
      }

    static std::shared_ptr<const GameplayDefinitions> load(const std::filesystem::path& base);

    std::vector<std::pair<std::string,SpellRecord>> spells() const;
    FocusRecord focus(std::string_view name) const;
    GuildRecord guild() const;
    std::string_view speech(int voice, std::string_view member) const;

  private:
    explicit GameplayDefinitions(rapidjson::Document rows) : rows(std::move(rows)) {}

    using Voice = std::map<std::string,std::string,std::less<>>;

    rapidjson::Document rows;
    mutable std::map<int,Voice> voices;
  };
