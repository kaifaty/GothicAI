#pragma once

#include "itemdefinitions.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "game/rpgvocabulary.h"

#include <optional>
#include <unordered_map>
#include <variant>

namespace ItemDefinitionRuntime {
struct RestoreChange {
  Attribute attribute;
  std::variant<uint32_t,int32_t> amount; // Symbol index or literal dose.
  };
struct RestoreEffect {
  RestoreChange first;
  std::optional<RestoreChange> second;
  };
struct FullRestoreEffect {
  Attribute attribute, maximum;
  };
struct PermanentAttributeEffect {
  RestoreChange increase;
  std::optional<Attribute> refill;
  };
struct ArmorSetEffect {
  uint32_t armorFlag;
  uint32_t beltFlag;
  bool     equip;
  };
struct FightSkillEffect {
  Talent  talent;
  int32_t delta;
  bool    playerOnly;
  };
struct Definitions {
  std::unordered_map<uint32_t,ItemRecord> items;
  std::unordered_map<uint32_t,RestoreEffect> restoreEffects;
  std::unordered_map<uint32_t,FullRestoreEffect> fullRestoreEffects;
  std::unordered_map<uint32_t,PermanentAttributeEffect> permanentAttributeEffects;
  std::unordered_map<uint32_t,ArmorSetEffect> armorSetEffects;
  std::unordered_map<uint32_t,FightSkillEffect> fightSkillEffects;
  };
Definitions resolve(const ItemDefinitions::Library& library, const daedalus::DaedalusScript& script);
// Select the section by the verified program-record SHA, applying the game's environment switch.
Definitions load(const std::filesystem::path& base, const daedalus::DaedalusScript& program,
                 std::string_view sha256);
// Register effects only after verifying the stock NotR program SHA.
void registerStockRestoreEffects(Definitions& definitions, const daedalus::DaedalusScript& program);
void registerStockArmorSetEffects(Definitions& definitions, const daedalus::DaedalusScript& program);
void registerStockFightSkillEffects(Definitions& definitions, const daedalus::DaedalusScript& program);
// Both the game and its contract use this complete choice of initialization path.
// Returns true when a definition supplied the fields, false after a VM constructor.
bool initialize(daedalus::DaedalusVm& vm, const std::shared_ptr<daedalus::IItem>& item,
                daedalus::DaedalusSymbol* symbol, const Definitions& definitions);
}
