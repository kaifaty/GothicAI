#include "nativebasecatalog.h"

#include "definitionjson.h"

#include <map>

namespace {
using namespace DefinitionJson;

constexpr std::string_view format = "gothic-native-base-catalog";
uint32_t number(const Json& value) {
  require(value.IsUint(),"invalid state identity");
  return value.GetUint();
  }

std::optional<uint32_t> phase(const Json& value) {
  return value.IsNull() ? std::nullopt : std::optional(number(value));
  }

void validate(const NativeBaseCatalog::Selection& selection) {
  require(selection.fight.size()==64 && std::ranges::all_of(selection.fight,[](char c) {
    return (c>='0' && c<='9') || (c>='a' && c<='f');
    }),"invalid fight catalog identity");
  require(selection.language>=-1 && selection.language<8,"invalid base language");
  const auto& states = selection.states;
  require(!states.empty(),"missing NPC states");
  std::map<std::string_view,uint32_t,std::less<>> names;
  std::optional<uint32_t> previous;
  for(const auto& state : states) {
    require((!previous || *previous<state.id) && state.name.starts_with("ZS_") && names.emplace(state.name,state.id).second,
            "invalid or duplicate NPC state "+state.name);
    previous=state.id;
    }
  for(const auto& state : states) {
    const auto check = [&](std::optional<uint32_t> id, const char* suffix) {
      const auto name=state.name+suffix;
      const auto entry=names.find(name);
      require(entry==names.end() ? !id : id==entry->second,"invalid NPC state phase "+name);
      };
    check(state.loop,"_LOOP");
    check(state.end,"_END");
    }
  }
}

std::filesystem::path NativeBaseCatalog::section(const std::filesystem::path& base) {
  return base/"definitions"/"native-catalog.json";
  }

NativeBaseCatalog::Selection NativeBaseCatalog::read(const std::filesystem::path& base) {
  const auto file = section(base);
  try {
    const auto doc = DefinitionJson::read(file);
    object(doc,{"format","version","fight_catalog","language","npc_states"});
    require(string(doc["format"])==format && integer(doc["version"])==catalogVersion,"unsupported native base catalog");
    Selection selection{string(doc["fight_catalog"]),integer(doc["language"]),{}};
    const auto& states = doc["npc_states"];
    require(states.IsArray(),"invalid NPC state catalog");
    selection.states.reserve(states.Size());
    for(const auto& row : states.GetArray()) {
      require(row.IsArray() && row.Size()==4,"invalid NPC state row");
      selection.states.push_back({number(row[0]),bytes(row[1]),phase(row[2]),phase(row[3])});
      }
    validate(selection);
    return selection;
    }
  catch(const std::runtime_error& error) {
    throw std::runtime_error(file.string()+": "+error.what());
    }
  }

void NativeBaseCatalog::write(const std::filesystem::path& base, const Selection& selection) {
  validate(selection);
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text(format,alloc),alloc);
  put(doc,"version",Json(catalogVersion),alloc);
  put(doc,"fight_catalog",text(selection.fight,alloc),alloc);
  put(doc,"language",Json(selection.language),alloc);
  Json states(rapidjson::kArrayType);
  states.Reserve(rapidjson::SizeType(selection.states.size()),alloc);
  for(const auto& state : selection.states) {
    Json row(rapidjson::kArrayType);
    row.PushBack(state.id,alloc).PushBack(text(encode(state.name),alloc),alloc);
    row.PushBack(state.loop ? Json(*state.loop) : Json(),alloc);
    row.PushBack(state.end ? Json(*state.end) : Json(),alloc);
    states.PushBack(row,alloc);
    }
  put(doc,"npc_states",std::move(states),alloc);
  const auto file = section(base);
  std::filesystem::create_directories(file.parent_path());
  DefinitionJson::write(file,doc);
  }
