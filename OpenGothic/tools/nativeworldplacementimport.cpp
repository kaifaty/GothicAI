#include "nativeworldplacementimport.h"

#include "daedalus/script.h"
#include "placementdefinitions.h"

using namespace DefinitionJson;
using daedalus::DaedalusDataType;

rapidjson::Document NativeWorldPlacement::prepare(const std::filesystem::path& base,std::string_view programSha,
                                                const daedalus::DaedalusScript& script) {
  auto data = PlacementDefinitions::read(base,programSha);
  rapidjson::Document out(rapidjson::kObjectType);
  auto& alloc = out.GetAllocator();
  const auto byId = [&](uint32_t id) -> const daedalus::DaedalusSymbol& {
    const auto* symbol = script.find_symbol_by_index(id);
    require(symbol!=nullptr,"missing world placement input "+std::to_string(id));
    return *symbol;
    };
  const auto byName = [&](std::string_view name) -> const daedalus::DaedalusSymbol& {
    const auto* symbol = script.find_symbol_by_name(name);
    require(symbol!=nullptr,"missing world placement input "+std::string(name));
    return *symbol;
    };
  const auto stringValue = [&](const daedalus::DaedalusSymbol& symbol) {
    require(symbol.type()==DaedalusDataType::STRING && symbol.is_const(),"invalid world placement string "+symbol.name());
    return text(encode(symbol.get_string()),alloc);
    };
  const auto intValue = [](const daedalus::DaedalusSymbol& symbol) {
    require(symbol.type()==DaedalusDataType::INT && symbol.is_const(),"invalid world placement constant "+symbol.name());
    return symbol.get_int();
    };
  const auto spawns = [&](const auto& rows) {
    Json list(rapidjson::kArrayType);
    for(const auto& row : rows) {
      const auto& external = byId(row.external);
      require(external.is_external() && (external.name()=="WLD_INSERTNPC" || external.name()=="WLD_INSERTITEM"),
              "invalid world insertion operation "+external.name());
      Json spawn(rapidjson::kObjectType);
      put(spawn,"instance",Json(row.instance),alloc);
      put(spawn,"point",stringValue(byId(row.point)),alloc);
      put(spawn,"npc",Json(external.name()=="WLD_INSERTNPC"),alloc);
      list.PushBack(spawn,alloc);
      }
    return list;
    };
  const auto room = [&](const daedalus::DaedalusSymbol& label,int32_t guild) {
    Json ret(rapidjson::kObjectType);
    put(ret,"label",stringValue(label),alloc);
    put(ret,"guild",Json(guild),alloc);
    return ret;
    };
  const auto roomRow = [&](const PlacementDefinitions::RoomAssignment& row) {
    return room(byId(row.label),row.guild ? intValue(byId(*row.guild)) : -1);
    };
  const auto choice = [&](const daedalus::DaedalusSymbol& flag,int32_t expected,Json equal,Json otherwise) {
    require(flag.type()==DaedalusDataType::INT && !flag.is_const(),"invalid world room condition "+flag.name());
    Json ret(rapidjson::kObjectType);
    put(ret,"flag",text(flag.name(),alloc),alloc);
    put(ret,"expected",Json(expected),alloc);
    put(ret,"equal",std::move(equal),alloc);
    put(ret,"otherwise",std::move(otherwise),alloc);
    return ret;
    };
  Json regions(rapidjson::kObjectType);
  const auto region = [&](const char* name,Json rooms,Json mobs) {
    Json ret(rapidjson::kObjectType);
    put(ret,"rooms",std::move(rooms),alloc);
    put(ret,"mobs",std::move(mobs),alloc);
    put(regions,name,std::move(ret),alloc);
    };
  for(const auto& [name,entry] : data.roomInitializers) {
    Json rooms(rapidjson::kArrayType);
    Json mobs(rapidjson::kArrayType);
    for(const auto& row : entry.rooms) {
      if(const auto* fixed = std::get_if<PlacementDefinitions::RoomAssignment>(&row)) {
        rooms.PushBack(roomRow(*fixed),alloc);
        } else {
        const auto& branch = std::get<PlacementDefinitions::RoomChoice>(row);
        rooms.PushBack(choice(byId(branch.flag),intValue(byId(branch.expected)),roomRow(branch.equal),
                              roomRow(branch.otherwise)),alloc);
        }
      }
    for(const auto& row : entry.mobs) {
      Json mob(rapidjson::kObjectType);
      put(mob,"hour",Json(row.hour),alloc);
      put(mob,"minute",Json(row.minute),alloc);
      put(mob,"name",stringValue(byId(row.name)),alloc);
      put(mob,"state",Json(row.state),alloc);
      mobs.PushBack(mob,alloc);
      }
    region(name.c_str(),std::move(rooms),std::move(mobs));
    }

  const auto publicRoom = intValue(byName("GIL_PUBLIC"));
  Json farm(rapidjson::kArrayType);
  farm.PushBack(room(byName("$54786"),publicRoom),alloc);
  farm.PushBack(room(byName("$54787"),publicRoom),alloc);
  farm.PushBack(choice(byName("LOBART_KLEIDUNG_VERKAUFT"),intValue(byName("TRUE")),
                      room(byName("$54788"),intValue(byName("GIL_NONE"))),room(byName("$54789"),publicRoom)),alloc);
  region("INIT_SUB_NEWWORLD_PART_FARM_01",std::move(farm),Json(rapidjson::kArrayType));

  const auto seekers = intValue(byName("GIL_DMT"));
  Json surface(rapidjson::kArrayType);
  surface.PushBack(room(byName("$54426"),seekers),alloc);
  surface.PushBack(room(byName("$54427"),seekers),alloc);
  region("INIT_SUB_SURFACE",std::move(surface),Json(rapidjson::kArrayType));

  put(out,"version",Json(1),alloc);
  put(out,"program_sha256",text(programSha,alloc),alloc);
  put(out,"dragon_island",spawns(data.dragonIsland),alloc);
  put(out,"addon_chapter_4",spawns(data.addonChapter4),alloc);
  put(out,"rooms",std::move(regions),alloc);
  put(out,"dragon_arrival",stringValue(byName("$53061")),alloc);
  return out;
  }
