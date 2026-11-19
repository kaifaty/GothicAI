#include "npchelperdefinitions.h"

#include "definitionjson.h"

#include <cmath>
#include <limits>
#include <type_traits>

namespace {
using namespace NpcHelperDefinitions;
using namespace DefinitionJson;

const Json& array(const Json& value) {
  require(value.IsArray(),"expected an array");
  return value;
  }
std::string symbol(const Json& value) {
  const auto name=string(value);
  require(!name.empty(),"empty symbol name");
  return name;
  }
std::array<float,3> scale(const Json& value) {
  require(value.IsArray() && value.Size()==3,"expected three scale components");
  std::array<float,3> result;
  for(rapidjson::SizeType i=0;i<3;++i) {
    require(value[i].IsNumber(),"expected scale number");
    const auto number=value[i].GetDouble();
    require(std::isfinite(number) && std::abs(number)<=std::numeric_limits<float>::max(),"scale out of range");
    result[i]=float(number);
    }
  return result;
  }
AddFightSkill fight(const Json& value) {
  object(value,{"talents"});
  AddFightSkill result;
  for(const auto& entry:array(at(value,"talents")).GetArray()) {
    object(entry,{"talent","aivar","chance","thresholds"});
    FightTalent talent;
    talent.talent=integer(at(entry,"talent"));
    talent.aivar=integer(at(entry,"aivar"));
    talent.chance=integer(at(entry,"chance"));
    for(const auto& row:array(at(entry,"thresholds")).GetArray()) {
      object(row,{"chance","threshold","skill"});
      talent.thresholds.push_back({integer(at(row,"chance")),integer(at(row,"threshold")),integer(at(row,"skill"))});
      }
    result.talents.push_back(std::move(talent));
    }
  return result;
  }
SetNpcVisual visual(const Json& value) {
  object(value,{"gender_aivar","male","model","male_body","female_body","body_color","teeth",
                "female_texture_first","female_texture_last","female_texture_offset","strength",
                "thin_limit","strong_limit","thin_scale","strong_scale"});
  SetNpcVisual result;
  result.genderAivar=integer(at(value,"gender_aivar"));
  result.male=integer(at(value,"male"));
  result.model=bytes(at(value,"model"));
  result.maleBody=bytes(at(value,"male_body"));
  result.femaleBody=bytes(at(value,"female_body"));
  result.bodyColor=integer(at(value,"body_color"));
  result.teeth=integer(at(value,"teeth"));
  result.femaleTextureFirst=integer(at(value,"female_texture_first"));
  result.femaleTextureLast=integer(at(value,"female_texture_last"));
  result.femaleTextureOffset=integer(at(value,"female_texture_offset"));
  result.strength=integer(at(value,"strength"));
  result.thinLimit=integer(at(value,"thin_limit"));
  result.strongLimit=integer(at(value,"strong_limit"));
  result.thinScale=scale(at(value,"thin_scale"));
  result.strongScale=scale(at(value,"strong_scale"));
  return result;
  }
InventoryTable inventory(const Json& value) {
  object(value,{"helper","rolls"});
  InventoryTable result;
  result.helper=symbol(at(value,"helper"));
  for(const auto& row:array(at(value,"rolls")).GetArray()) {
    std::vector<InventoryItem> items;
    for(const auto& item:array(row).GetArray()) {
      object(item,{"item","count"});
      items.push_back({symbol(at(item,"item")),integer(at(item,"count"))});
      }
    result.rolls.push_back(std::move(items));
    }
  return result;
  }
AmbientInventory ambient(const Json& value) {
  object(value,{"random_bound","guilds","fallback"});
  AmbientInventory result;
  result.randomBound=integer(at(value,"random_bound"));
  require(result.randomBound>0,"random bound must be positive");
  for(const auto& row:array(at(value,"guilds")).GetArray()) {
    object(row,{"guild","table"});
    const auto guild=integer(at(row,"guild"));
    require(result.guilds.emplace(guild,inventory(at(row,"table"))).second,"duplicate guild table");
    }
  result.fallback=inventory(at(value,"fallback"));
  const auto valid=[&](const InventoryTable& table) {
    require(table.rolls.size()==size_t(result.randomBound),"inventory table roll count differs from random bound");
    };
  valid(result.fallback);
  for(const auto& [guild,table]:result.guilds) { (void)guild; valid(table); }
  return result;
  }
SkeletonLord skeleton(const Json& value) {
  object(value,{"model","overlays","visual","gender","face_texture","body_texture","head","armor"});
  SkeletonLord result;
  result.model=bytes(at(value,"model"));
  for(const auto& overlay:array(at(value,"overlays")).GetArray()) result.overlays.push_back(bytes(overlay));
  result.visual=visual(at(value,"visual"));
  result.gender=integer(at(value,"gender"));
  result.faceTexture=integer(at(value,"face_texture"));
  result.bodyTexture=integer(at(value,"body_texture"));
  result.head=bytes(at(value,"head"));
  result.armor=symbol(at(value,"armor"));
  return result;
  }
AttributesToChapter attributes(const Json& value) {
  object(value,{"talent","skill","chapters","experience_multiplier","experience_divisor"});
  AttributesToChapter result;
  result.talent=integer(at(value,"talent"));
  result.skill=integer(at(value,"skill"));
  result.experienceMultiplier=integer(at(value,"experience_multiplier"));
  result.experienceDivisor=integer(at(value,"experience_divisor"));
  require(result.experienceDivisor!=0,"zero experience divisor");
  for(const auto& row:array(at(value,"chapters")).GetArray()) {
    object(row,{"chapter","minimum","fields"});
    ChapterAttributes chapter;
    chapter.chapter=integer(at(row,"chapter"));
    require(at(row,"minimum").IsBool(),"chapter minimum must be a boolean");
    chapter.minimum=at(row,"minimum").GetBool();
    for(const auto& field:array(at(row,"fields")).GetArray()) {
      object(field,{"binding","index","value"});
      const auto name=symbol(at(field,"binding"));
      require(name.starts_with("C_NPC."),"expected NPC field binding");
      const auto index=integer(at(field,"index"));
      require(index>=0 && index<=UINT16_MAX,"expected uint16 field index");
      Value value;
      value.integer=integer(at(field,"value"));
      chapter.fields.push_back({name,uint16_t(index),value});
      }
    result.chapters.push_back(std::move(chapter));
    }
  return result;
  }

Json scaleJson(const std::array<float,3>& scale, Allocator& a) {
  Json value(rapidjson::kArrayType);
  for(float component:scale) value.PushBack(Json(double(component)),a);
  return value;
  }
Json parameters(const AddFightSkill& fight, Allocator& a) {
  Json value(rapidjson::kObjectType),talents(rapidjson::kArrayType);
  for(const auto& talent:fight.talents) {
    Json entry(rapidjson::kObjectType),thresholds(rapidjson::kArrayType);
    put(entry,"talent",Json(talent.talent),a);
    put(entry,"aivar",Json(talent.aivar),a);
    put(entry,"chance",Json(talent.chance),a);
    for(const auto& row:talent.thresholds) {
      Json threshold(rapidjson::kObjectType);
      put(threshold,"chance",Json(row.chance),a);
      put(threshold,"threshold",Json(row.threshold),a);
      put(threshold,"skill",Json(row.skill),a);
      thresholds.PushBack(threshold,a);
      }
    put(entry,"thresholds",std::move(thresholds),a);
    talents.PushBack(entry,a);
    }
  put(value,"talents",std::move(talents),a);
  return value;
  }
Json parameters(const SetFightSkills& fight, Allocator& a) { return parameters(fight.fight,a); }
Json parameters(const SetNpcVisual& visual, Allocator& a) {
  Json value(rapidjson::kObjectType);
  put(value,"gender_aivar",Json(visual.genderAivar),a);
  put(value,"male",Json(visual.male),a);
  put(value,"model",text(encode(visual.model),a),a);
  put(value,"male_body",text(encode(visual.maleBody),a),a);
  put(value,"female_body",text(encode(visual.femaleBody),a),a);
  put(value,"body_color",Json(visual.bodyColor),a);
  put(value,"teeth",Json(visual.teeth),a);
  put(value,"female_texture_first",Json(visual.femaleTextureFirst),a);
  put(value,"female_texture_last",Json(visual.femaleTextureLast),a);
  put(value,"female_texture_offset",Json(visual.femaleTextureOffset),a);
  put(value,"strength",Json(visual.strength),a);
  put(value,"thin_limit",Json(visual.thinLimit),a);
  put(value,"strong_limit",Json(visual.strongLimit),a);
  put(value,"thin_scale",scaleJson(visual.thinScale,a),a);
  put(value,"strong_scale",scaleJson(visual.strongScale,a),a);
  return value;
  }
Json inventoryJson(const InventoryTable& table, Allocator& a) {
  Json value(rapidjson::kObjectType),rolls(rapidjson::kArrayType);
  put(value,"helper",text(table.helper,a),a);
  for(const auto& items:table.rolls) {
    Json row(rapidjson::kArrayType);
    for(const auto& item:items) {
      Json entry(rapidjson::kObjectType);
      put(entry,"item",text(item.item,a),a);
      put(entry,"count",Json(item.count),a);
      row.PushBack(entry,a);
      }
    rolls.PushBack(row,a);
    }
  put(value,"rolls",std::move(rolls),a);
  return value;
  }
Json parameters(const AmbientInventory& inventory, Allocator& a) {
  Json value(rapidjson::kObjectType),guilds(rapidjson::kArrayType);
  put(value,"random_bound",Json(inventory.randomBound),a);
  for(const auto& [guild,table]:inventory.guilds) {
    Json row(rapidjson::kObjectType);
    put(row,"guild",Json(guild),a);
    put(row,"table",inventoryJson(table,a),a);
    guilds.PushBack(row,a);
    }
  put(value,"guilds",std::move(guilds),a);
  put(value,"fallback",inventoryJson(inventory.fallback,a),a);
  return value;
  }
Json parameters(const SkeletonLord& skeleton, Allocator& a) {
  Json value(rapidjson::kObjectType),overlays(rapidjson::kArrayType);
  put(value,"model",text(encode(skeleton.model),a),a);
  for(const auto& overlay:skeleton.overlays) overlays.PushBack(text(encode(overlay),a),a);
  put(value,"overlays",std::move(overlays),a);
  put(value,"visual",parameters(skeleton.visual,a),a);
  put(value,"gender",Json(skeleton.gender),a);
  put(value,"face_texture",Json(skeleton.faceTexture),a);
  put(value,"body_texture",Json(skeleton.bodyTexture),a);
  put(value,"head",text(encode(skeleton.head),a),a);
  put(value,"armor",text(skeleton.armor,a),a);
  return value;
  }
Json parameters(const AttributesToChapter& attributes, Allocator& a) {
  Json value(rapidjson::kObjectType),chapters(rapidjson::kArrayType);
  put(value,"talent",Json(attributes.talent),a);
  put(value,"skill",Json(attributes.skill),a);
  put(value,"experience_multiplier",Json(attributes.experienceMultiplier),a);
  put(value,"experience_divisor",Json(attributes.experienceDivisor),a);
  for(const auto& chapter:attributes.chapters) {
    Json row(rapidjson::kObjectType),fields(rapidjson::kArrayType);
    put(row,"chapter",Json(chapter.chapter),a);
    put(row,"minimum",Json(chapter.minimum),a);
    for(const auto& field:chapter.fields) {
      require(field.value.kind==Value::Kind::Int,"chapter field must contain an integer");
      Json entry(rapidjson::kObjectType);
      put(entry,"binding",text(field.binding,a),a);
      put(entry,"index",Json(field.index),a);
      put(entry,"value",Json(field.value.integer),a);
      fields.PushBack(entry,a);
      }
    put(row,"fields",std::move(fields),a);
    chapters.PushBack(row,a);
    }
  put(value,"chapters",std::move(chapters),a);
  return value;
  }
constexpr const char* templates[]={"set_fight_skills","set_npc_visual","ambient_inventory","add_fight_skill",
                                   "skeleton_lord","attributes_to_chapter"};
Definition definition(std::string_view name, const Json& value) {
  if(name==templates[0]) return SetFightSkills{fight(value)};
  if(name==templates[1]) return visual(value);
  if(name==templates[2]) return ambient(value);
  if(name==templates[3]) return fight(value);
  if(name==templates[4]) return skeleton(value);
  if(name==templates[5]) return attributes(value);
  throw std::runtime_error("unsupported NPC helper template "+std::string(name));
  }
}

NpcHelperDefinitions::Library NpcHelperDefinitions::read(const std::filesystem::path& base, std::string_view programSha256) {
  return readFile(base/"definitions"/programSha256/"helpers.json",programSha256);
  }

NpcHelperDefinitions::Library NpcHelperDefinitions::readFile(const std::filesystem::path& file, std::string_view programSha256) {
  try {
    const auto document=DefinitionJson::read(file);
    object(document,{"format","version","program_name","program_sha256","string_encoding","records"});
    require(string(at(document,"format"))=="gothic-npc-helper-definitions","unsupported format");
    require(integer(at(document,"version"))==1,"unsupported version");
    require(!string(at(document,"program_name")).empty(),"empty program name");
    require(string(at(document,"program_sha256"))==programSha256,"program SHA differs");
    require(string(at(document,"string_encoding"))=="unicode-byte-mapping","unsupported string encoding");
    Library library;
    for(const auto& record:array(at(document,"records")).GetArray()) {
      const auto name=symbol(at(record,"symbol"));
      require(!library.helpers.contains(name) && !library.uncaptured.contains(name),"duplicate helper symbol "+name);
      try {
        const auto status=string(at(record,"status"));
        if(status=="uncaptured") {
          object(record,{"symbol","status","reason","detail"});
          library.uncaptured.emplace(name,symbol(at(record,"reason"))+": "+bytes(at(record,"detail")));
          }
        else {
          require(status=="captured","unsupported capture status");
          object(record,{"symbol","status","template","parameters"});
          library.helpers.emplace(name,definition(string(at(record,"template")),at(record,"parameters")));
          }
        }
      catch(const std::exception& error) { throw std::runtime_error(name+": "+error.what()); }
      }
    return library;
    }
  catch(const std::exception& error) { throw std::runtime_error("NPC helper definitions: "+file.string()+": "+error.what()); }
  }

void NpcHelperDefinitions::write(const Library& library, const std::filesystem::path& file,
                                 std::string_view programName, std::string_view programSha256) {
  rapidjson::Document storage;
  auto& a=storage.GetAllocator();
  Json document(rapidjson::kObjectType),records(rapidjson::kArrayType);
  put(document,"format",text("gothic-npc-helper-definitions",a),a);
  put(document,"version",Json(1),a);
  put(document,"program_name",text(programName,a),a);
  put(document,"program_sha256",text(programSha256,a),a);
  put(document,"string_encoding",text("unicode-byte-mapping",a),a);
  std::map<std::string,const Definition*> names;
  for(const auto& [name,value]:library.helpers) names.emplace(name,&value);
  for(const auto& [name,reason]:library.uncaptured) {
    (void)reason;
    require(names.emplace(name,nullptr).second,"helper is both captured and uncaptured: "+name);
    }
  for(const auto& [name,value]:names) {
    Json record(rapidjson::kObjectType);
    put(record,"symbol",text(name,a),a);
    put(record,"status",text(value ? "captured" : "uncaptured",a),a);
    if(value) {
      put(record,"template",text(templates[value->index()],a),a);
      put(record,"parameters",std::visit([&](const auto& helper) { return parameters(helper,a); },*value),a);
      }
    else {
      const auto& failure=library.uncaptured.at(name);
      const auto separator=failure.find(": ");
      require(separator!=std::string::npos,"uncaptured helper must have reason and detail: "+name);
      put(record,"reason",text(failure.substr(0,separator),a),a);
      put(record,"detail",text(encode(failure.substr(separator+2)),a),a);
      }
    records.PushBack(record,a);
    }
  put(document,"records",std::move(records),a);
  std::filesystem::create_directories(file.parent_path());
  DefinitionJson::write(file,document);
  }
