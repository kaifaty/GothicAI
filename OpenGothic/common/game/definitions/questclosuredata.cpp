#include "questclosuredata.h"

#include "definitionjson.h"
#include "gameplaydefinitions.h"

namespace {

using namespace DefinitionJson;
using namespace QuestClosureData;

uint32_t id(const Json& value) {
  require(value.IsUint(),"expected an unsigned quest symbol ID");
  return value.GetUint();
  }

Json number(const Number& value, Allocator& alloc) {
  Json out(rapidjson::kObjectType);
  if(const auto* literal = std::get_if<int32_t>(&value))
    put(out,"literal",Json(*literal),alloc);
  else
    put(out,"symbol",Json(std::get<uint32_t>(value)),alloc);
  return out;
  }

Number number(const Json& value) {
  if(value.IsObject() && value.HasMember("literal")) {
    object(value,{"literal"});
    return integer(value["literal"]);
    }
  object(value,{"symbol"});
  return id(value["symbol"]);
  }

}

std::filesystem::path QuestClosureData::section(const std::filesystem::path& base) {
  return base/"definitions"/GameplayDefinitions::stockProgram/"quest-closures.json";
  }

QuestClosureData::Groups QuestClosureData::read(const std::filesystem::path& base) {
  const auto path = section(base);
  try {
    const auto data = DefinitionJson::read(path);
    object(data,{"format","version","program_sha256","groups"});
    require(string(data["format"])=="gothic-quest-closures" && integer(data["version"])==1 &&
            string(data["program_sha256"])==GameplayDefinitions::stockProgram,"invalid quest closure header");
    const auto& groups = data["groups"];
    require(groups.IsArray() && groups.Size()==names.size(),"invalid quest closure groups");
    Groups result;
    for(rapidjson::SizeType i=0;i<groups.Size();++i) {
      const auto& group = groups[i];
      object(group,{"name","rows","guild"});
      require(string(group["name"])==names[i] && group["rows"].IsArray() &&
              group["rows"].Size()==counts[i],"invalid quest closure group order or row count");
      for(const auto& row : group["rows"].GetArray()) {
        object(row,{"topic","values"});
        const auto& values = row["values"];
        require(values.IsArray() && values.Size()==3,"invalid quest closure call");
        result[i].rows.push_back({id(row["topic"]),{number(values[0]),number(values[1]),number(values[2])}});
        }
      const auto& guild = group["guild"];
      if(i+1<groups.Size()) {
        require(guild.IsNull(),"unexpected quest closure guild rule");
        continue;
        }
      object(guild,{"before","topic","status","excluded"});
      const auto& excluded = guild["excluded"];
      require(id(guild["before"])<counts[i] && excluded.IsArray() && excluded.Size()==3,
              "invalid quest closure guild rule");
      result[i].guildRule = GuildRule{id(guild["before"]),id(guild["topic"]),id(guild["status"]),
                                      {id(excluded[0]),id(excluded[1]),id(excluded[2])}};
      }
    return result;
    }
  catch(const std::runtime_error& error) {
    throw std::runtime_error(path.string()+": "+error.what());
    }
  }

void QuestClosureData::write(const std::filesystem::path& base, const Groups& groups) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text("gothic-quest-closures",alloc),alloc);
  put(doc,"version",Json(1),alloc);
  put(doc,"program_sha256",text(GameplayDefinitions::stockProgram,alloc),alloc);
  Json array(rapidjson::kArrayType);
  for(size_t i=0;i<groups.size();++i) {
    Json group(rapidjson::kObjectType);
    put(group,"name",text(names[i],alloc),alloc);
    Json rows(rapidjson::kArrayType);
    for(const auto& row : groups[i].rows) {
      Json encoded(rapidjson::kObjectType);
      put(encoded,"topic",Json(row.topic),alloc);
      Json values(rapidjson::kArrayType);
      for(const auto& n : row.values)
        values.PushBack(number(n,alloc),alloc);
      put(encoded,"values",std::move(values),alloc);
      rows.PushBack(std::move(encoded),alloc);
      }
    put(group,"rows",std::move(rows),alloc);
    Json guild;
    if(const auto& rule = groups[i].guildRule) {
      guild.SetObject();
      put(guild,"before",Json(rule->before),alloc);
      put(guild,"topic",Json(rule->topic),alloc);
      put(guild,"status",Json(rule->status),alloc);
      Json excluded(rapidjson::kArrayType);
      for(auto symbol : rule->excluded)
        excluded.PushBack(Json(symbol),alloc);
      put(guild,"excluded",std::move(excluded),alloc);
      }
    put(group,"guild",std::move(guild),alloc);
    array.PushBack(std::move(group),alloc);
    }
  put(doc,"groups",std::move(array),alloc);
  std::filesystem::create_directories(section(base).parent_path());
  DefinitionJson::write(section(base),doc);
  }
