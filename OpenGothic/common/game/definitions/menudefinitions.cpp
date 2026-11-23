#include "menudefinitions.h"

#include "definitionjson.h"

#include <cmath>
#include <type_traits>

namespace {
namespace fs = std::filesystem;
using namespace DefinitionJson;
using namespace MenuDefinitions;

constexpr std::string_view format = "gothic-menu-definitions";
constexpr int version = 1;

template<class T>
void fields(T& menu, auto&& field) requires std::is_same_v<std::remove_const_t<T>,Menu> {
  field("back_pic",menu.back_pic);
  field("back_world",menu.back_world);
  field("pos_x",menu.pos_x);
  field("pos_y",menu.pos_y);
  field("dim_x",menu.dim_x);
  field("dim_y",menu.dim_y);
  field("alpha",menu.alpha);
  field("music_theme",menu.music_theme);
  field("event_timer_msec",menu.event_timer_msec);
  field("items",menu.items);
  field("flags",menu.flags);
  field("default_outgame",menu.default_outgame);
  field("default_ingame",menu.default_ingame);
  }

template<class T>
void fields(T& item, auto&& field) requires std::is_same_v<std::remove_const_t<T>,MenuItem> {
  field("fontname",item.fontname);
  field("text",item.text);
  field("backpic",item.backpic);
  field("alphamode",item.alphamode);
  field("alpha",item.alpha);
  field("type",item.type);
  field("on_sel_action",item.on_sel_action);
  field("on_sel_action_s",item.on_sel_action_s);
  field("on_chg_set_option",item.on_chg_set_option);
  field("on_chg_set_option_section",item.on_chg_set_option_section);
  field("on_event_action",item.on_event_action);
  field("pos_x",item.pos_x);
  field("pos_y",item.pos_y);
  field("dim_x",item.dim_x);
  field("dim_y",item.dim_y);
  field("size_start_scale",item.size_start_scale);
  field("flags",item.flags);
  field("open_delay_time",item.open_delay_time);
  field("open_duration",item.open_duration);
  field("user_float",item.user_float);
  field("user_string",item.user_string);
  field("frame_posx",item.frame_posx);
  field("frame_posy",item.frame_posy);
  field("frame_sizex",item.frame_sizex);
  field("frame_sizey",item.frame_sizey);
  field("hide_if_option_section_set",item.hide_if_option_section_set);
  field("hide_if_option_set",item.hide_if_option_set);
  field("hide_on_value",item.hide_on_value);
  }

template<class T>
void decode(const Json& value, T& field) {
  if constexpr(std::is_same_v<T,std::string>) {
    field = bytes(value);
    } else if constexpr(std::is_same_v<T,float>) {
    require(value.IsNumber() && std::isfinite(value.GetFloat()),"invalid menu float");
    field = value.GetFloat();
    } else if constexpr(std::is_enum_v<T>) {
    require(value.IsUint(),"invalid menu flag or type");
    field = T(value.GetUint());
    } else if constexpr(std::is_array_v<T>) {
    require(value.IsArray() && value.Size()==std::extent_v<T>,"invalid menu field count");
    for(rapidjson::SizeType i=0; i<value.Size(); ++i)
      decode(value[i],field[i]);
    } else {
    static_assert(std::is_same_v<T,int32_t>);
    field = integer(value);
    }
  }

template<class T>
Json encodeValue(const T& field, Allocator& allocator) {
  if constexpr(std::is_same_v<T,std::string>) {
    return text(DefinitionJson::encode(field),allocator);
    } else if constexpr(std::is_enum_v<T>) {
    return Json(uint32_t(field));
    } else if constexpr(std::is_array_v<T>) {
    Json values(rapidjson::kArrayType);
    for(const auto& value : field)
      values.PushBack(encodeValue(value,allocator),allocator);
    return values;
    } else {
    static_assert(std::is_same_v<T,int32_t> || std::is_same_v<T,float>);
    return Json(field);
    }
  }

template<class Map>
void readRows(Map& map, const Json& rows) {
  require(rows.IsObject(),"invalid menu table");
  for(auto it=rows.MemberBegin(); it!=rows.MemberEnd(); ++it) {
    const auto [entry,inserted] = map.try_emplace(bytes(it->name));
    require(inserted && it->value.IsObject(),"invalid or duplicate menu row");
    size_t fieldCount = 0;
    fields(entry->second,[&](const char* key, auto& field) {
      decode(at(it->value,key),field);
      ++fieldCount;
      });
    require(fieldCount==it->value.MemberCount(),"unexpected menu fields");
    }
  }

template<class Map>
Json writeRows(const Map& map, Allocator& allocator) {
  Json rows(rapidjson::kObjectType);
  for(const auto& [name,record] : map) {
    Json row(rapidjson::kObjectType);
    fields(record,[&](const char* key, const auto& field) {
      put(row,key,encodeValue(field,allocator),allocator);
      });
    rows.AddMember(text(DefinitionJson::encode(name),allocator),row,allocator);
    }
  return rows;
  }

const char* actionName(Action::Kind kind) {
  switch(kind) {
    case Action::Kind::Video:                   return "video";
    case Action::Kind::ApplyVideoOptions:       return "apply_video_options";
    case Action::Kind::ApplyPerformanceOptions: return "apply_performance_options";
    }
  throw std::runtime_error("invalid menu action");
  }

Action readAction(const Json& value) {
  object(value,{"kind","video"});
  const auto name = string(value["kind"]);
  const auto video = bytes(value["video"]);
  if(name==actionName(Action::Kind::Video) && !video.empty())
    return {Action::Kind::Video,video};
  if(name==actionName(Action::Kind::ApplyVideoOptions) && video.empty())
    return {Action::Kind::ApplyVideoOptions,{}};
  if(name==actionName(Action::Kind::ApplyPerformanceOptions) && video.empty())
    return {Action::Kind::ApplyPerformanceOptions,{}};
  throw std::runtime_error("invalid menu action: "+name);
  }
}

fs::path MenuDefinitions::section(const fs::path& base, std::string_view program) {
  return base/"definitions"/program/"menus.json";
  }

Catalog MenuDefinitions::read(const fs::path& base, std::string_view program) {
  const auto path = section(base,program);
  const auto doc = DefinitionJson::read(path);
  object(doc,{"format","version","program_sha256","info_position","menus","items","execute"});
  require(string(doc["format"])==format && integer(doc["version"])==version &&
          string(doc["program_sha256"])==program,"invalid menu format or program in "+path.string());

  Catalog result;
  const auto& position = doc["info_position"];
  if(!position.IsNull()) {
    require(position.IsArray() && position.Size()==2,"invalid menu information position");
    result.infoPosition = {integer(position[0]),integer(position[1])};
    }
  readRows(result.menus,doc["menus"]);
  readRows(result.items,doc["items"]);
  const auto& actions = doc["execute"];
  require(actions.IsArray(),"invalid menu actions");
  for(const auto& row : actions.GetArray()) {
    object(row,{"symbol","actions"});
    const auto symbol = integer(row["symbol"]);
    const auto [entry,inserted] = result.actions.try_emplace(symbol);
    require(symbol>0 && inserted && row["actions"].IsArray(),"invalid or duplicate menu callback");
    for(const auto& action : row["actions"].GetArray())
      entry->second.push_back(readAction(action));
    }
  for(const auto& [name,item] : result.items) {
    const auto symbol = item.on_event_action[uint8_t(MenuItemSelectEvent::EXECUTE)];
    require(symbol<=0 || result.actions.contains(symbol),"missing execute callback for "+name);
    }
  return result;
  }

void MenuDefinitions::write(const fs::path& base, std::string_view program, const Catalog& catalog) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& allocator = doc.GetAllocator();
  put(doc,"format",text(format,allocator),allocator);
  put(doc,"version",Json(version),allocator);
  put(doc,"program_sha256",text(program,allocator),allocator);
  Json position;
  if(catalog.infoPosition) {
    position.SetArray();
    position.PushBack(Json(catalog.infoPosition->first),allocator);
    position.PushBack(Json(catalog.infoPosition->second),allocator);
    }
  put(doc,"info_position",std::move(position),allocator);
  put(doc,"menus",writeRows(catalog.menus,allocator),allocator);
  put(doc,"items",writeRows(catalog.items,allocator),allocator);
  Json callbacks(rapidjson::kArrayType);
  for(const auto& [symbol,actions] : catalog.actions) {
    Json row(rapidjson::kObjectType);
    Json list(rapidjson::kArrayType);
    put(row,"symbol",Json(symbol),allocator);
    for(const auto& action : actions) {
      Json entry(rapidjson::kObjectType);
      put(entry,"kind",text(actionName(action.kind),allocator),allocator);
      put(entry,"video",encodeValue(action.video,allocator),allocator);
      list.PushBack(entry,allocator);
      }
    put(row,"actions",std::move(list),allocator);
    callbacks.PushBack(row,allocator);
    }
  put(doc,"execute",std::move(callbacks),allocator);
  const auto path = section(base,program);
  fs::create_directories(path.parent_path());
  DefinitionJson::write(path,doc);
  }
