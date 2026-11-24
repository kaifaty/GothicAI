#include "nativebasecatalog.h"

#include "definitionjson.h"

#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <type_traits>

namespace {
using namespace DefinitionJson;

constexpr std::string_view format = "gothic-native-base-catalog";
uint32_t number(const Json& value) {
  require(value.IsUint(),"expected a native catalog uint32");
  return value.GetUint();
  }

std::optional<uint32_t> phase(const Json& value) {
  return value.IsNull() ? std::nullopt : std::optional(number(value));
  }

constexpr uint32_t maxStateValues = UINT16_MAX;

template<class Decode>
auto readArray(const Json& value, Decode decode) {
  require(value.IsArray() && !value.Empty() && value.Size()<=maxStateValues,"invalid initial state array");
  std::vector<decltype(decode(value[0]))> result;
  result.reserve(value.Size());
  for(const auto& entry : value.GetArray())
    result.push_back(decode(entry));
  return result;
  }

NativeBaseCatalog::InitialValue readInitialValue(std::string_view kind, const Json& value) {
  using NativeBaseCatalog::Alias;
  if(kind=="int")    return readArray(value,integer);
  if(kind=="float")  return readArray(value,[](const Json& entry) { return std::bit_cast<float>(number(entry)); });
  if(kind=="string") return readArray(value,bytes);
  require((kind=="npc" || kind=="item") && value.IsNull(),"invalid initial state alias");
  return kind=="npc" ? Alias::Npc : Alias::Item;
  }

std::vector<NativeBaseCatalog::Identity> readIdentities(const Json& value) {
  require(value.IsArray(),"invalid content identity list");
  std::vector<NativeBaseCatalog::Identity> names;
  names.reserve(value.Size());
  for(const auto& row : value.GetArray()) {
    require(row.IsArray() && row.Size()==2,"invalid content identity row");
    names.push_back({number(row[0]),bytes(row[1])});
    }
  return names;
  }

Json writeIdentities(const std::vector<NativeBaseCatalog::Identity>& names, Allocator& alloc) {
  Json rows(rapidjson::kArrayType);
  rows.Reserve(rapidjson::SizeType(names.size()),alloc);
  for(const auto& [id,name] : names) {
    Json row(rapidjson::kArrayType);
    row.PushBack(id,alloc).PushBack(text(encode(name),alloc),alloc);
    rows.PushBack(row,alloc);
    }
  return rows;
  }

void validate(const NativeBaseCatalog::Selection& selection) {
  require(selection.fight.size()==64 && std::ranges::all_of(selection.fight,[](char c) {
    return (c>='0' && c<='9') || (c>='a' && c<='f');
    }),"invalid fight catalog identity");
  require(selection.language>=-1 && selection.language<8,"invalid base language");
  const auto& states = selection.states;
  require(!states.empty(),"missing NPC states");
  std::map<std::string_view,uint32_t,std::less<>> names;
  std::set<uint32_t> ids;
  std::optional<uint32_t> previous;
  for(const auto& state : states) {
    require((!previous || *previous<state.id) && state.name.starts_with("ZS_") && names.emplace(state.name,state.id).second,
            "invalid or duplicate NPC state "+state.name);
    ids.insert(state.id);
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

  require(!selection.initialState.empty(),"missing initial session state");
  previous.reset();
  for(const auto& row : selection.initialState) {
    require((!previous || *previous<row.id) && ids.insert(row.id).second,
            "duplicate or unordered initial state identity");
    previous=row.id;
    std::visit([](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr(std::is_same_v<T,NativeBaseCatalog::Alias>) {
        require(value==NativeBaseCatalog::Alias::Npc || value==NativeBaseCatalog::Alias::Item,"invalid initial state alias");
        }
      else {
        require(!value.empty() && value.size()<=maxStateValues,"invalid initial state array");
        if constexpr(std::is_same_v<T,std::vector<float>>)
          require(std::ranges::all_of(value,[](float f) { return std::isfinite(f); }),"non-finite initial state float");
        }
      },row.value);
    }

  for(const auto* rows : {&selection.identities.npcs,&selection.identities.items,&selection.identities.dialogs}) {
    require(!rows->empty(),"missing content identities");
    previous.reset();
    for(const auto& [id,name] : *rows) {
      require((!previous || *previous<id) && ids.insert(id).second && !name.empty() && names.emplace(name,id).second,
              "invalid or duplicate content identity "+name);
      previous=id;
      }
    }
  }

Json writeInitialRow(const NativeBaseCatalog::InitialRow& row, Allocator& alloc) {
  using NativeBaseCatalog::Alias;
  Json values;
  const auto kind = std::visit([&](const auto& value) -> std::string_view {
    using T = std::decay_t<decltype(value)>;
    if constexpr(std::is_same_v<T,Alias>) {
      return value==Alias::Npc ? "npc" : "item";
      }
    else {
      values.SetArray();
      values.Reserve(rapidjson::SizeType(value.size()),alloc);
      if constexpr(std::is_same_v<T,std::vector<int32_t>>) {
        for(auto entry : value) values.PushBack(entry,alloc);
        return "int";
        }
      else if constexpr(std::is_same_v<T,std::vector<float>>) {
        for(auto entry : value) values.PushBack(std::bit_cast<uint32_t>(entry),alloc);
        return "float";
        }
      else {
        for(const auto& entry : value) values.PushBack(text(encode(entry),alloc),alloc);
        return "string";
        }
      }
    },row.value);
  Json result(rapidjson::kArrayType);
  result.PushBack(row.id,alloc).PushBack(text(kind,alloc),alloc).PushBack(std::move(values),alloc);
  return result;
  }
}

std::filesystem::path NativeBaseCatalog::section(const std::filesystem::path& base) {
  return base/"definitions"/"native-catalog.json";
  }

NativeBaseCatalog::Selection NativeBaseCatalog::read(const std::filesystem::path& base) {
  const auto file = section(base);
  try {
    const auto doc = DefinitionJson::read(file);
    object(doc,{"format","version","fight_catalog","language","npc_states","initial_session_state",
                "npc_archetypes","item_archetypes","dialog_infos"});
    require(string(doc["format"])==format && integer(doc["version"])==catalogVersion,"unsupported native base catalog");
    Selection selection{string(doc["fight_catalog"]),integer(doc["language"]),{}, {}, {}};
    const auto& states = doc["npc_states"];
    require(states.IsArray(),"invalid NPC state catalog");
    selection.states.reserve(states.Size());
    for(const auto& row : states.GetArray()) {
      require(row.IsArray() && row.Size()==4,"invalid NPC state row");
      selection.states.push_back({number(row[0]),bytes(row[1]),phase(row[2]),phase(row[3])});
      }
    const auto& initialState = doc["initial_session_state"];
    require(initialState.IsArray(),"invalid initial session state");
    selection.initialState.reserve(initialState.Size());
    for(const auto& row : initialState.GetArray()) {
      require(row.IsArray() && row.Size()==3,"invalid initial state row");
      selection.initialState.push_back({number(row[0]),readInitialValue(string(row[1]),row[2])});
      }
    selection.identities={
      readIdentities(doc["npc_archetypes"]),
      readIdentities(doc["item_archetypes"]),
      readIdentities(doc["dialog_infos"]),
      };
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
  Json initialState(rapidjson::kArrayType);
  initialState.Reserve(rapidjson::SizeType(selection.initialState.size()),alloc);
  for(const auto& row : selection.initialState)
    initialState.PushBack(writeInitialRow(row,alloc),alloc);
  put(doc,"initial_session_state",std::move(initialState),alloc);
  put(doc,"npc_archetypes",writeIdentities(selection.identities.npcs,alloc),alloc);
  put(doc,"item_archetypes",writeIdentities(selection.identities.items,alloc),alloc);
  put(doc,"dialog_infos",writeIdentities(selection.identities.dialogs,alloc),alloc);
  const auto file = section(base);
  std::filesystem::create_directories(file.parent_path());
  DefinitionJson::write(file,doc);
  }
