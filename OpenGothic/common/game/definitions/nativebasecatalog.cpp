#include "nativebasecatalog.h"

#include "definitionjson.h"

#include <algorithm>
#include <bit>
#include <cctype>
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

void validateValues(const NativeBaseCatalog::InitialValue& values,bool allowAlias) {
  std::visit([allowAlias](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr(std::is_same_v<T,NativeBaseCatalog::Alias>) {
      require(allowAlias && (value==NativeBaseCatalog::Alias::Npc || value==NativeBaseCatalog::Alias::Item),
              "invalid native catalog alias");
      }
    else {
      require(!value.empty() && value.size()<=maxStateValues,"invalid native catalog values");
      if constexpr(std::is_same_v<T,std::vector<float>>)
        require(std::ranges::all_of(value,[](float f) { return std::isfinite(f); }),"non-finite native catalog float");
      }
    },values);
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
    require((!previous || *previous<row.id) && ids.insert(row.id).second && !row.name.empty() &&
            names.emplace(row.name,row.id).second,"invalid, duplicate or unordered initial state identity "+row.name);
    previous=row.id;
    validateValues(row.value,true);
    }

  require(std::ranges::all_of(selection.routines,[](const auto& row) { return row.name.starts_with("RTN_"); }),
          "invalid native routine name");
  for(const auto* rows : {&selection.identities.npcs,&selection.identities.items,&selection.identities.dialogs,
                          &selection.routines,&selection.itemCallbacks}) {
    require(!rows->empty(),"missing content identities");
    previous.reset();
    for(const auto& [id,name] : *rows) {
      require((!previous || *previous<id) && ids.insert(id).second && !name.empty() && names.emplace(name,id).second,
              "invalid or duplicate content identity "+name);
      previous=id;
      }
    }

  require(!selection.constants.empty(),"missing named constants");
  for(const auto& [name,constant] : selection.constants) {
    require(!name.empty() && ids.insert(constant.id).second && names.emplace(name,constant.id).second,
            "invalid or duplicate constant identity "+name);
    validateValues(constant.values,false);
    }
  require(selection.directorFirstId>1 && selection.directorFirstId<=uint32_t(INT32_MAX)-NativeBaseCatalog::directorCapacity &&
          *ids.rbegin()<selection.directorFirstId-1,"invalid first director ID");
  require(std::ranges::none_of(names,[](const auto& row) { return row.first.starts_with("DIR_"); }),
          "stock content uses reserved director names");
  }

Json writeValueRow(uint32_t id,std::string_view name,const NativeBaseCatalog::InitialValue& data,Allocator& alloc) {
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
    },data);
  Json result(rapidjson::kArrayType);
  result.PushBack(id,alloc).PushBack(text(encode(name),alloc),alloc);
  result.PushBack(text(kind,alloc),alloc).PushBack(std::move(values),alloc);
  return result;
  }
}

const NativeBaseCatalog::Identity* NativeBaseCatalog::findIdentity(std::span<const Identity> rows, uint32_t id) {
  const auto found = std::ranges::lower_bound(rows,id,{},&Identity::id);
  return found!=rows.end() && found->id==id ? &*found : nullptr;
  }

const NativeBaseCatalog::Identity* NativeBaseCatalog::findIdentity(std::span<const Identity> rows, std::string_view name) {
  const auto toUpper = [](unsigned char ch) { return char(std::toupper(ch)); };
  const auto found = std::ranges::find_if(rows,[&](const auto& row) {
    return std::ranges::equal(row.name,name,std::equal_to{},std::identity{},toUpper);
    });
  return found!=rows.end() ? &*found : nullptr;
  }

std::filesystem::path NativeBaseCatalog::section(const std::filesystem::path& base) {
  return base/"definitions"/"native-catalog.json";
  }

NativeBaseCatalog::Selection NativeBaseCatalog::read(const std::filesystem::path& base) {
  const auto file = section(base);
  try {
    const auto doc = DefinitionJson::read(file);
    object(doc,{"format","version","fight_catalog","language","director_first_id","npc_states","initial_session_state",
                "npc_archetypes","item_archetypes","dialog_infos","named_constants","routines","item_callbacks"});
    require(string(doc["format"])==format && integer(doc["version"])==catalogVersion,"unsupported native base catalog");
    Selection selection{string(doc["fight_catalog"]),integer(doc["language"]),number(doc["director_first_id"]),{}, {}, {}, {}, {}, {}};
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
      require(row.IsArray() && row.Size()==4,"invalid initial state row");
      selection.initialState.push_back({number(row[0]),bytes(row[1]),readInitialValue(string(row[2]),row[3])});
      }
    selection.identities={
      readIdentities(doc["npc_archetypes"]),
      readIdentities(doc["item_archetypes"]),
      readIdentities(doc["dialog_infos"]),
      };
    const auto& constants = doc["named_constants"];
    require(constants.IsArray(),"invalid named constants");
    for(const auto& row : constants.GetArray()) {
      require(row.IsArray() && row.Size()==4,"invalid named constant row");
      auto name=bytes(row[1]);
      Constant value{number(row[0]),readInitialValue(string(row[2]),row[3])};
      require(selection.constants.emplace(std::move(name),std::move(value)).second,"duplicate named constant");
      }
    selection.routines=readIdentities(doc["routines"]);
    selection.itemCallbacks=readIdentities(doc["item_callbacks"]);
    validate(selection);
    return selection;
    }
  catch(const std::runtime_error& error) {
    throw std::runtime_error(file.string()+": "+error.what());
    }
  }

const NativeBaseCatalog::Constant* NativeBaseCatalog::findConstant(const Selection& selection,std::string_view name) {
  const auto& constants=selection.constants;
  if(const auto it=constants.find(name); it!=constants.end())
    return &it->second;
  std::string upper(name);
  std::ranges::transform(name,upper.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  const auto it=constants.find(upper);
  return it==constants.end() ? nullptr : &it->second;
  }

void NativeBaseCatalog::write(const std::filesystem::path& base, const Selection& selection) {
  validate(selection);
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text(format,alloc),alloc);
  put(doc,"version",Json(catalogVersion),alloc);
  put(doc,"fight_catalog",text(selection.fight,alloc),alloc);
  put(doc,"language",Json(selection.language),alloc);
  put(doc,"director_first_id",Json(selection.directorFirstId),alloc);
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
    initialState.PushBack(writeValueRow(row.id,row.name,row.value,alloc),alloc);
  put(doc,"initial_session_state",std::move(initialState),alloc);
  put(doc,"npc_archetypes",writeIdentities(selection.identities.npcs,alloc),alloc);
  put(doc,"item_archetypes",writeIdentities(selection.identities.items,alloc),alloc);
  put(doc,"dialog_infos",writeIdentities(selection.identities.dialogs,alloc),alloc);
  Json constants(rapidjson::kArrayType);
  constants.Reserve(rapidjson::SizeType(selection.constants.size()),alloc);
  for(const auto& [name,constant] : selection.constants)
    constants.PushBack(writeValueRow(constant.id,name,constant.values,alloc),alloc);
  put(doc,"named_constants",std::move(constants),alloc);
  put(doc,"routines",writeIdentities(selection.routines,alloc),alloc);
  put(doc,"item_callbacks",writeIdentities(selection.itemCallbacks,alloc),alloc);
  const auto file = section(base);
  std::filesystem::create_directories(file.parent_path());
  DefinitionJson::write(file,doc);
  }
