#include "npcworlddata.h"
#include "gameplaydefinitions.h"

namespace {
using namespace DefinitionJson;
constexpr std::string_view globalsFormat = "gothic-npc-world-globals";
constexpr std::string_view attitudesFormat = "gothic-monster-attitudes";
constexpr int version = 1;

std::filesystem::path section(const std::filesystem::path& base, std::string_view name) {
  return base/"definitions"/GameplayDefinitions::stockProgram/name;
  }

void validate(const Json& document, std::string_view format) {
  require(string(document["format"])==format && integer(document["version"])==version &&
          string(document["program_sha256"])==GameplayDefinitions::stockProgram,
          "NPC world data: invalid format or stock program");
  }

rapidjson::Document header(std::string_view format) {
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  put(document,"format",text(format,allocator),allocator);
  put(document,"version",Json(version),allocator);
  put(document,"program_sha256",text(GameplayDefinitions::stockProgram,allocator),allocator);
  return document;
  }
}

std::optional<NpcWorldData::Globals> NpcWorldData::loadGlobals(const std::filesystem::path& base) {
  const auto path = section(base,"npc-world-globals.json");
  if(!std::filesystem::exists(path))
    return std::nullopt;
  const auto document = read(path);
  object(document,{"format","version","program_sha256","unset_chapter","default_chapter","bindings"});
  validate(document,globalsFormat);
  require(document["bindings"].IsArray(),"NPC world globals: missing alias rows");
  Globals result;
  result.unsetChapter = integer(document["unset_chapter"]);
  result.defaultChapter = integer(document["default_chapter"]);
  for(const auto& row : document["bindings"].GetArray()) {
    require(row.IsArray() && row.Size()==2 && row[1].IsUint(),"NPC world globals: invalid alias row");
    result.bindings.emplace_back(integer(row[0]),row[1].GetUint());
    }
  require(result.bindings.size()==310,"NPC world globals: incomplete stock aliases");
  return result;
  }

std::optional<std::vector<NpcWorldData::AttitudeRule>> NpcWorldData::loadAttitudes(const std::filesystem::path& base) {
  const auto path = section(base,"monster-attitudes.json");
  if(!std::filesystem::exists(path))
    return std::nullopt;
  const auto document = read(path);
  object(document,{"format","version","program_sha256","rules"});
  validate(document,attitudesFormat);
  require(document["rules"].IsArray(),"Monster attitudes: missing rule rows");

  std::vector<AttitudeRule> result;
  for(const auto& row : document["rules"].GetArray()) {
    require(row.IsArray() && row.Size()==3,"Monster attitudes: invalid rule row");
    result.push_back({integer(row[0]),integer(row[1]),integer(row[2])});
    }
  require(result.size()==1932,"Monster attitudes: incomplete stock rules");
  return result;
  }

void NpcWorldData::write(const std::filesystem::path& base, const Globals& globals, const std::vector<AttitudeRule>& attitudes) {
  auto aliases = header(globalsFormat);
  auto& aliasAllocator = aliases.GetAllocator();
  Json bindings(rapidjson::kArrayType);
  for(const auto& [source,target] : globals.bindings) {
    Json row(rapidjson::kArrayType);
    row.PushBack(Json(source),aliasAllocator);
    row.PushBack(Json(target),aliasAllocator);
    bindings.PushBack(row,aliasAllocator);
    }
  put(aliases,"unset_chapter",Json(globals.unsetChapter),aliasAllocator);
  put(aliases,"default_chapter",Json(globals.defaultChapter),aliasAllocator);
  put(aliases,"bindings",std::move(bindings),aliasAllocator);

  auto monsters = header(attitudesFormat);
  auto& monsterAllocator = monsters.GetAllocator();
  Json rules(rapidjson::kArrayType);
  for(const auto& rule : attitudes) {
    Json row(rapidjson::kArrayType);
    row.PushBack(Json(rule.from),monsterAllocator);
    row.PushBack(Json(rule.attitude),monsterAllocator);
    row.PushBack(Json(rule.to),monsterAllocator);
    rules.PushBack(row,monsterAllocator);
    }
  put(monsters,"rules",std::move(rules),monsterAllocator);

  const auto aliasPath = section(base,"npc-world-globals.json");
  std::filesystem::create_directories(aliasPath.parent_path());
  DefinitionJson::write(aliasPath,aliases);
  DefinitionJson::write(section(base,"monster-attitudes.json"),monsters);
  }
