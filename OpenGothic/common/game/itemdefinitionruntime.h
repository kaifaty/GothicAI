#pragma once

#include "itemdefinitions.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "game/rpgvocabulary.h"

#include <unordered_map>

namespace ItemDefinitionRuntime {
struct RestoreEffect {
  Attribute attribute;
  uint32_t amountSymbol;
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
