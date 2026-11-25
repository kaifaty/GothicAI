#pragma once

#include "itemdefinitions.h"
#include "game/rpgvocabulary.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace NativeBaseCatalog { struct Selection; }

namespace ItemDefinitionRuntime {
struct RestoreChange {
  Attribute attribute;
  int32_t amount;
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
struct EquipmentAttributeWrite {
  Attribute attribute;
  std::optional<int32_t> minimum{};
  };
struct EquipmentStatChange {
  std::variant<Protection,Attribute,EquipmentAttributeWrite> stat;
  int32_t amount;
  };
struct EquipmentStatEffect {
  std::vector<EquipmentStatChange> changes;
  bool equip;
  };
struct DocumentCall {
  std::string function;
  std::vector<ItemDefinitions::DocumentArgument> arguments;
  };
struct Document {
  uint32_t handle;
  bool map;
  std::vector<DocumentCall> calls;
  };
struct Definitions {
  std::unordered_map<uint32_t,ItemRecord> items;
  std::unordered_set<uint32_t> itemUsePrograms;
  std::unordered_map<uint32_t,Document> documents;
  std::optional<uint32_t> brokenEye;
  std::unordered_map<uint32_t,RestoreEffect> restoreEffects;
  std::unordered_map<uint32_t,FullRestoreEffect> fullRestoreEffects;
  std::unordered_map<uint32_t,PermanentAttributeEffect> permanentAttributeEffects;
  std::unordered_map<uint32_t,ArmorSetEffect> armorSetEffects;
  std::unordered_map<uint32_t,FightSkillEffect> fightSkillEffects;
  std::unordered_map<uint32_t,EquipmentStatEffect> equipmentStatEffects;
  };
Definitions resolveNative(const ItemDefinitions::Library& library, const NativeBaseCatalog::Selection& catalog);
Definitions loadNative(const std::filesystem::path& base, const NativeBaseCatalog::Selection& catalog,
                       std::string_view sha256);
void registerStockEffects(Definitions& definitions, const NativeBaseCatalog::Selection& catalog);
}
