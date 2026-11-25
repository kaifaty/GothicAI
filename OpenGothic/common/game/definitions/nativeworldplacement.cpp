#include "nativeworldplacement.h"

#include "definitionjson.h"
#include "game/definitions/nativebasecatalog.h"
#include "placementdefinitions.h"

#include <algorithm>
#include <iterator>

using namespace NativeWorldPlacement;

std::filesystem::path Definitions::section(const std::filesystem::path& base,std::string_view programSha) {
  return base/"definitions"/programSha/"world-placement.json";
  }

Definitions Definitions::load(const std::filesystem::path& base,std::string_view programSha,
                              const NativeBaseCatalog::Selection& catalog) {
  using namespace DefinitionJson;
  const auto file = section(base,programSha);
  try {
    Definitions ret;
    auto raw = PlacementDefinitions::read(base,programSha);
    std::map<std::string_view,uint32_t,std::less<>> npcIds, itemIds;
    for(const auto& npc : catalog.identities.npcs)
      npcIds.emplace(npc.name,npc.id);
    for(const auto& item : catalog.identities.items)
      itemIds.emplace(item.name,item.id);
    const auto npcOrItem = [&](std::string_view name,bool npc) {
      const auto& rows = npc ? npcIds : itemIds;
      const auto found = rows.find(name);
      require(found!=rows.end(),"unknown startup identity "+std::string(name));
      return found->second;
      };
    using Kind = PlacementDefinitions::Value::Kind;
    const auto startupStep = [&](const PlacementDefinitions::Step& step) -> StartupStep {
      if(const auto* write = std::get_if<PlacementDefinitions::GlobalWrite>(&step)) {
        const auto state = std::ranges::find(catalog.initialState,write->symbol,&NativeBaseCatalog::InitialRow::name);
        require(write->index==0 && write->value.kind==Kind::Int && state!=catalog.initialState.end() &&
                std::holds_alternative<std::vector<int32_t>>(state->value),"invalid startup state "+write->symbol);
        return SetFlag{write->symbol,write->value.integer};
        }
      const auto& call = std::get<PlacementDefinitions::Call>(step);
      const auto& args = call.args;
      const auto namedPoint = args.size()==2 && args[0].kind==Kind::Symbol && args[1].kind==Kind::String;
      if(!call.deferred && (call.function=="WLD_INSERTNPC" || call.function=="WLD_INSERTITEM") && namedPoint) {
        const auto npc = call.function=="WLD_INSERTNPC";
        return Spawn{npcOrItem(args[0].text,npc),args[1].text,npc};
        }
      if(call.deferred && call.function=="B_KILLNPC" && args.size()==1 && args[0].kind==Kind::Symbol)
        return KillNpc{npcOrItem(args[0].text,true)};
      if(!call.deferred && call.function=="GAME_INITGERMAN" && args.empty())
        return InitLanguage{};
      if(!call.deferred && call.function=="PLAYVIDEO" && args.size()==1 && args[0].kind==Kind::String)
        return PlayVideo{args[0].text};
      if(!call.deferred && call.function=="WLD_SETTIME" && args.size()==2 &&
         args[0].kind==Kind::Int && args[1].kind==Kind::Int)
        return SetTime{args[0].integer,args[1].integer};
      throw std::runtime_error("unsupported startup operation "+call.function);
      };
    for(const auto* name : {"STARTUP_GLOBAL","STARTUP_NEWWORLD","STARTUP_OLDWORLD","STARTUP_ADDONWORLD"}) {
      const auto found = raw.placements.find(name);
      require(found!=raw.placements.end() && !found->second.steps.empty(),std::string("missing startup ")+name);
      auto& startup = ret.startup[name];
      startup.reserve(found->second.steps.size());
      std::ranges::transform(found->second.steps,std::back_inserter(startup),startupStep);
      }

    const auto doc = read(file);
    object(doc,{"version","program_sha256","dragon_island","addon_chapter_4","rooms","dragon_arrival"});
    require(integer(at(doc,"version"))==1 && string(at(doc,"program_sha256"))==programSha,"invalid version or source");

    const auto spawns = [&](const Json& rows,size_t count,bool onlyNpc) {
      require(rows.IsArray() && rows.Size()==count,"invalid spawn list");
      std::vector<Spawn> out;
      for(const auto& row : rows.GetArray()) {
        object(row,{"instance","point","npc"});
        const auto& id = at(row,"instance");
        const auto& npc = at(row,"npc");
        require(id.IsUint() && npc.IsBool() && (!onlyNpc || npc.GetBool()),"invalid spawn identity");
        const auto& identities = npc.GetBool() ? catalog.identities.npcs : catalog.identities.items;
        require(NativeBaseCatalog::findIdentity(identities,id.GetUint())!=nullptr,"unknown spawn identity");
        auto point = bytes(at(row,"point"));
        require(!point.empty(),"empty spawn point");
        out.push_back({id.GetUint(),std::move(point),npc.GetBool()});
        }
      return out;
      };
    ret.dragonIsland = spawns(at(doc,"dragon_island"),raw.dragonIsland.size(),false);
    ret.addonChapter4 = spawns(at(doc,"addon_chapter_4"),PlacementDefinitions::Library::addonChapter4Count,true);
    require(!ret.dragonIsland.empty(),"missing Dragon Island placement");
    ret.dragonArrival = bytes(at(doc,"dragon_arrival"));
    require(!ret.dragonArrival.empty(),"missing Dragon Island arrival text");

    const auto room = [](const Json& value) {
      object(value,{"label","guild"});
      Room ret{bytes(at(value,"label")),integer(at(value,"guild"))};
      require(!ret.label.empty(),"empty room label");
      return ret;
      };
    const auto& regions = at(doc,"rooms");
    require(regions.IsObject() && regions.MemberCount()==std::size(PlacementDefinitions::roomInitializerShapes)+2,
            "invalid room regions");
    for(auto entry=regions.MemberBegin();entry!=regions.MemberEnd();++entry) {
      const auto name = string(entry->name);
      object(entry->value,{"rooms","mobs"});
      const auto& rooms = at(entry->value,"rooms");
      const auto& mobs  = at(entry->value,"mobs");
      require(rooms.IsArray() && mobs.IsArray(),"invalid room initializer");
      auto [row, inserted] = ret.rooms.try_emplace(name);
      require(inserted,"duplicate room region "+name);
      auto& region = row->second;
      for(const auto& mob : mobs.GetArray()) {
        object(mob,{"hour","minute","name","state"});
        region.mobs.push_back({integer(at(mob,"hour")),integer(at(mob,"minute")),bytes(at(mob,"name")),
                               integer(at(mob,"state"))});
        }
      for(const auto& value : rooms.GetArray()) {
        if(value.IsObject() && value.HasMember("flag")) {
          object(value,{"flag","expected","equal","otherwise"});
          const auto flag = string(at(value,"flag"));
          const auto binding = std::ranges::find(catalog.initialState,flag,&NativeBaseCatalog::InitialRow::name);
          require(binding!=catalog.initialState.end() && std::holds_alternative<std::vector<int32_t>>(binding->value),
                  "unknown room condition "+flag);
          region.rooms.emplace_back(RoomChoice{flag,integer(at(value,"expected")),room(at(value,"equal")),
                                               room(at(value,"otherwise"))});
          } else {
          region.rooms.emplace_back(room(value));
          }
        }
      }
    for(const auto& shape : PlacementDefinitions::roomInitializerShapes) {
      const auto got = ret.rooms.find(shape.name);
      const auto isChoice = [](const auto& value) { return std::holds_alternative<RoomChoice>(value); };
      require(got!=ret.rooms.end() && got->second.rooms.size()==shape.rooms && got->second.mobs.size()==shape.mobs &&
              std::ranges::count_if(got->second.rooms,isChoice)==std::ptrdiff_t(shape.choices),
              std::string("invalid room shape ")+shape.name);
      }
    require(ret.rooms.contains("INIT_SUB_NEWWORLD_PART_FARM_01") && ret.rooms.contains("INIT_SUB_SURFACE"),
            "missing farm or surface rooms");
    return ret;
    }
  catch(const std::exception& e) {
    throw std::runtime_error("world placement "+file.string()+": "+e.what());
    }
  }
