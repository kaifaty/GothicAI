#pragma once

#include "npcdefinitions.h"

#include <array>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace NpcHelperDefinitions {
using Value = NpcDefinitions::Value;

struct SkillThreshold {
  int32_t chance = 0;
  int32_t threshold = 0;
  int32_t skill = 0;
  bool operator==(const SkillThreshold&) const = default;
  };
struct FightTalent {
  int32_t talent = 0;
  int32_t aivar = 0;
  int32_t chance = 0;
  std::vector<SkillThreshold> thresholds;
  bool operator==(const FightTalent&) const = default;
  };
struct AddFightSkill {
  std::vector<FightTalent> talents;
  bool operator==(const AddFightSkill&) const = default;
  };
struct SetFightSkills {
  AddFightSkill fight;
  bool operator==(const SetFightSkills&) const = default;
  };
struct SetNpcVisual {
  int32_t genderAivar = 0, male = 0;
  std::string model, maleBody, femaleBody;
  int32_t bodyColor = 0, teeth = 0;
  int32_t femaleTextureFirst = 0, femaleTextureLast = 0, femaleTextureOffset = 0;
  int32_t strength = 0, thinLimit = 0, strongLimit = 0;
  std::array<float,3> thinScale{}, strongScale{};
  bool operator==(const SetNpcVisual&) const = default;
  };
struct InventoryItem {
  std::string item;
  int32_t count = 0;
  bool operator==(const InventoryItem&) const = default;
  };
struct InventoryTable {
  std::string helper;
  std::vector<std::vector<InventoryItem>> rolls;
  bool operator==(const InventoryTable&) const = default;
  };
struct AmbientInventory {
  int32_t randomBound = 0;
  std::map<int32_t,InventoryTable> guilds;
  InventoryTable fallback;
  bool operator==(const AmbientInventory&) const = default;
  };
struct SkeletonLord {
  std::string model;
  std::vector<std::string> overlays;
  SetNpcVisual visual;
  int32_t gender = 0, faceTexture = 0, bodyTexture = 0;
  std::string head, armor;
  bool operator==(const SkeletonLord&) const = default;
  };
struct ChapterAttributes {
  int32_t chapter = 0;
  bool minimum = false;
  std::vector<NpcDefinitions::Field> fields;
  bool operator==(const ChapterAttributes&) const = default;
  };
struct AttributesToChapter {
  int32_t talent = 0, skill = 0;
  std::vector<ChapterAttributes> chapters;
  int32_t experienceMultiplier = 0, experienceDivisor = 0;
  bool operator==(const AttributesToChapter&) const = default;
  };
using Definition = std::variant<SetFightSkills,SetNpcVisual,AmbientInventory,AddFightSkill,SkeletonLord,AttributesToChapter>;
struct Library {
  std::map<std::string,Definition> helpers;
  std::map<std::string,std::string> uncaptured;
  bool operator==(const Library&) const = default;
  };

// The template sees the same field accesses, engine calls and random draw as the script.
// local() preserves helper parameters and locals stored with the VM in a save.
class Host {
  public:
    virtual ~Host() = default;
    virtual int32_t get(std::string_view binding, uint16_t index = 0) = 0;
    virtual void set(std::string_view binding, uint16_t index, int32_t value) = 0;
    virtual void external(std::string_view function, std::span<const Value> arguments) = 0;
    virtual int32_t random(int32_t bound) = 0;
    virtual void local(std::string_view name, const Value& value) = 0;
  };

// args retain the declared function order, including the NPC represented as Value::Self.
void run(const Definition& definition, std::span<const Value> args, Host& host);
}
