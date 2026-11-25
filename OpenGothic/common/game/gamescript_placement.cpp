#include "gamescript.h"

#include "game/definitions/basescripts.h"
#include "game/definitions/nativebasecatalog.h"
#include "game/definitions/stockstoryprograms.h"
#include "resources.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace {
constexpr storygraph::NativeInstance operationActor = 1;

struct ShipCompanion {
  std::string_view flag, npc, partner = {};
  };
constexpr ShipCompanion captains[] = {
  {"JORGENISCAPTAIN", "VLK_4250_JORGEN_DI"},
  {"TORLOFISCAPTAIN", "SLD_801_TORLOF_DI"},
  {"JACKISCAPTAIN",   "VLK_444_JACK_DI"},
  };
constexpr ShipCompanion crew[] = {
  {"LEE_ISONBOARD",      "SLD_800_LEE_DI"},
  {"MILTENNW_ISONBOARD", "PC_MAGE_DI",        "LESTER_ISONBOARD"},
  {"LESTER_ISONBOARD",   "PC_PSIONIC_DI",     "MILTENNW_ISONBOARD"},
  {"MARIO_ISONBOARD",    "NONE_101_MARIO_DI"},
  {"WOLF_ISONBOARD",     "SLD_811_WOLF_DI"},
  {"VATRAS_ISONBOARD",   "VLK_439_VATRAS_DI"},
  {"BENNET_ISONBOARD",   "SLD_809_BENNET_DI"},
  {"DIEGO_ISONBOARD",    "PC_THIEF_DI",       "LARES_ISONBOARD"},
  {"GORN_ISONBOARD",     "PC_FIGHTER_DI"},
  {"LARES_ISONBOARD",    "VLK_449_LARES_DI",  "DIEGO_ISONBOARD"},
  {"BIFF_ISONBOARD",     "DJG_713_BIFF_DI"},
  {"ANGAR_ISONBOARD",    "DJG_705_ANGAR_DI"},
  {"GIRION_ISONBOARD",   "PAL_207_GIRION_DI"},
  };
}

using storygraph::NativeValue;

void GameScript::loadNativeWorldEntry() {
  const auto* catalog = baseNativeCatalog();
  if(catalog==nullptr)
    throw std::runtime_error("Missing native world placement catalog");
  bindRuntimeVariable("CURRENTLEVEL",currentStoryWorld);
  static constexpr std::pair<std::string_view,int32_t GhostProgress::*> ghostFlags[]={
    {"GHOST_SCKNOWSHOW2GETINADANOSTEMPEL",&GhostProgress::knowsTempleEntrance},
    {"SC_SUMMONEDANCIENTGHOST",&GhostProgress::summonedAncientGhost},
    {"B_REMOVEQUARHODRON_ONETIME",&GhostProgress::removedQuarhodron},
    {"ADW_ADANOSTEMPEL_STONEGRD_TRIGG_FUNC_01_ONETIME",&GhostProgress::stoneGuardianTrigger},
    {"SC_TOOKRHADEMESTRAP",&GhostProgress::tookRhademesTrap},
    {"SC_TALKEDTORHADEMAFTER",&GhostProgress::talkedToRhademesAfter},
    {"B_REMOVERHADEMES_ONETIME",&GhostProgress::removedRhademes},
    {"GHOSTATTACKWARN",&GhostProgress::attackWarn},
    };
  for(const auto& [name,field]:ghostFlags)
    bindRuntimeVariable(name,ghostProgress.*field);
  nativeWorldPlacement = NativeWorldPlacement::Definitions::load(Resources::baseDir(),
                                                                 StockStoryPrograms::stockProgram,*catalog);
  }

NativeValue GameScript::nativeWorldOperation(std::string_view name,std::initializer_list<NativeValue> args,
                                            const ScriptVars::Object& actor) {
  const auto found = nativeExternals.find(name);
  if(found==nativeExternals.end())
    throw std::runtime_error("Missing world operation "+std::string(name));
  const auto& operation = found->second;
  if(!std::ranges::equal(operation.signature.parameters,args,{},std::identity{},&NativeValue::type))
    throw std::runtime_error("Invalid world operation arguments for "+std::string(name));
  const auto resolve = [&actor](storygraph::NativeInstance id) { return id==operationActor ? actor : nullptr; };
  return operation.call(args,resolve,{}).value;
  }

void GameScript::nativeWorldFunction(std::string_view name,const std::vector<storygraph::StoryArgument>& args) {
  const auto [self,other] = storyActors();
  const auto result = callStoryFunction(name,self,other,args);
  if(!result.ok)
    throw std::runtime_error("World function "+std::string(name)+": "+result.error);
  }

int32_t GameScript::nativeWorldNumber(std::string_view name) const {
  if(const auto value = globalIntSymbol(name))
    return *value;
  throw std::runtime_error("Missing world state "+std::string(name));
  }

void GameScript::nativeWorldFlag(std::string_view name,int32_t value) {
  if(!setGlobalIntSymbol(name,value))
    throw std::runtime_error("Missing writable world state "+std::string(name));
  }

int32_t GameScript::nativeWorldActorId(std::string_view name,bool npc) {
  const auto value = npc ? storyNpcId(name) : findItemInstance(name);
  if(!value)
    throw std::runtime_error("Missing placement identity "+std::string(name));
  return int32_t(*value);
  }

void GameScript::nativeWorldSpawn(const NativeWorldPlacement::Spawn& row) {
  nativeWorldOperation(row.npc ? "WLD_INSERTNPC" : "WLD_INSERTITEM",
                      {NativeValue::integer(int32_t(row.instance)),NativeValue::str(row.point)});
  }

void GameScript::invokeWorldTrigger(std::string_view name) {
  const auto function = storyScriptFunctionId(name);
  int32_t result = 0;
  bool failed = false;
  if(!function || !invokeStockProgram(*function,result,&failed) || failed)
    throw std::runtime_error("Cannot execute world trigger "+std::string(name));
  }

void GameScript::runWorldFirstEntryTrigger(bool oldWorld) {
  invokeWorldTrigger(oldWorld ? "ENTER_OLDWORLD_FIRSTTIME_TRIGGER" : "ENTER_ADDONWORLD_FIRSTTIME_TRIGGER_FUNC");
  }

void GameScript::runStartup(std::string_view name) {
  const auto key = storygraph::nativeOperationName(name);
  if(key=="STARTUP_DRAGONISLAND") {
    const auto aboard = [&](std::string_view npc) {
      nativeWorldSpawn({uint32_t(nativeWorldActorId(npc,true)),"SHIP_DECK_01",true});
      };
    const auto success = nativeWorldNumber("LOG_SUCCESS");
    const auto yes = nativeWorldNumber("TRUE");
    for(const auto& row : captains)
      if(nativeWorldNumber(row.flag)==yes)
        aboard(row.npc);
    for(const auto& row : crew) {
      if(nativeWorldNumber(row.flag)!=success)
        continue;
      aboard(row.npc);
      if(!row.partner.empty() && nativeWorldNumber(row.partner)!=success) {
        auto* companion = findNpcById(uint32_t(nativeWorldActorId(row.npc,true)));
        nativeWorldFunction("B_STARTOTHERROUTINE",{companion,std::string("SittingShipDI")});
        }
      }
    aboard("NOV_600_PEDRO_DI");
    for(const auto* point : {"FP_ITEM_SHIP_03","FP_ITEM_SHIP_07"})
      nativeWorldSpawn({uint32_t(nativeWorldActorId("ITMI_FLASK",false)),point,false});
    for(const auto& row : nativeWorldPlacement.dragonIsland)
      nativeWorldSpawn(row);

    nativeWorldOperation("PLAYVIDEO",{NativeValue::str("SHIP.BIK")});
    const auto* topic = NativeBaseCatalog::findConstant(*baseNativeCatalog(),"TOPIC_HALLENVONIRDORATH");
    const auto* text = topic==nullptr ? nullptr : std::get_if<std::vector<std::string>>(&topic->values);
    if(text==nullptr || text->size()!=1)
      throw std::runtime_error("Missing Dragon Island journal topic");
    const auto title = NativeValue::str(text->front());
    nativeWorldOperation("LOG_CREATETOPIC",{title,NativeValue::integer(nativeWorldNumber("LOG_MISSION"))});
    nativeWorldOperation("LOG_SETTOPICSTATUS",{title,NativeValue::integer(nativeWorldNumber("LOG_RUNNING"))});
    nativeWorldOperation("LOG_ADDENTRY",{title,NativeValue::str(nativeWorldPlacement.dragonArrival)});
    nativeWorldFunction("B_KAPITELWECHSEL",{int32_t(6),nativeWorldNumber("DRAGONISLAND_ZEN")});
    return;
    }

  const auto found = nativeWorldPlacement.startup.find(key);
  if(found==nativeWorldPlacement.startup.end())
    throw std::runtime_error("Missing world startup "+key);
  using namespace NativeWorldPlacement;
  for(const auto& step : found->second) {
    std::visit([this](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr(std::is_same_v<T,Spawn>)
        nativeWorldSpawn(value);
      else if constexpr(std::is_same_v<T,SetFlag>)
        nativeWorldFlag(value.name,value.value);
      else if constexpr(std::is_same_v<T,KillNpc>)
        nativeWorldFunction("B_KILLNPC",{int32_t(value.instance)});
      else if constexpr(std::is_same_v<T,InitLanguage>)
        nativeWorldOperation("GAME_INITGERMAN");
      else if constexpr(std::is_same_v<T,PlayVideo>)
        nativeWorldOperation("PLAYVIDEO",{NativeValue::str(value.name)});
      else if constexpr(std::is_same_v<T,SetTime>)
        nativeWorldOperation("WLD_SETTIME",{NativeValue::integer(value.hour),NativeValue::integer(value.minute)});
      },step);
    }
  }

void GameScript::runNativeWorldRooms(std::string_view name) {
  const auto& region = nativeWorldPlacement.rooms.at(std::string(name));
  for(const auto& mob : region.mobs)
    nativeWorldOperation("WLD_SETMOBROUTINE",{NativeValue::integer(mob.hour),NativeValue::integer(mob.minute),
                                           NativeValue::str(mob.name),NativeValue::integer(mob.state)});
  const auto assign = [&](const NativeWorldPlacement::Room& room) {
    nativeWorldOperation("WLD_ASSIGNROOMTOGUILD",{NativeValue::str(room.label),NativeValue::integer(room.guild)});
    };
  for(const auto& room : region.rooms) {
    if(const auto* fixed = std::get_if<NativeWorldPlacement::Room>(&room)) {
      assign(*fixed);
      } else {
      const auto& choice = std::get<NativeWorldPlacement::RoomChoice>(room);
      assign(nativeWorldNumber(choice.flag)==choice.expected ? choice.equal : choice.otherwise);
      }
    }
  }

void GameScript::runNativeAddonChapterEntry() {
  nativeWorldOperation("B_INITNPCGLOBALS");
  const auto yes = nativeWorldNumber("TRUE");
  const auto no = nativeWorldNumber("FALSE");
  const auto remove = [&](std::string_view actor) {
    nativeWorldFunction("B_REMOVENPC",{nativeWorldActorId(actor,true)});
    };
  if(storyChapter>=1) {
    if(nativeWorldNumber("RAVENISDEAD")==yes)
      remove("KDW_14030_ADDON_MYXIR_ADW");
    if(nativeWorldNumber("REMOVESKLAVEN")==no && nativeWorldNumber("SKLAVEN_WEG")==yes) {
      for(const auto* actor : {"STRF_1118_ADDON_PATRICK","STRF_1119_ADDON_MONTY","STRF_1120_ADDON_TONAK",
                               "STRF_1121_ADDON_TELBOR","STRF_1122_ADDON_PARDOS"})
        remove(actor);
      nativeWorldFlag("REMOVESKLAVEN",yes);
      }
    }
  constexpr std::array chapters = {"ENTERADW_KAPITEL2","ENTERADW_KAPITEL3","ENTERADW_KAPITEL4",
                                   "ENTERADW_KAPITEL5","ENTERADW_KAPITEL6"};
  for(size_t i=0;i<chapters.size();++i) {
    const int32_t chapter = int32_t(i)+2;
    if(storyChapter<chapter || nativeWorldNumber(chapters[i])!=no)
      continue;
    if(chapter==4)
      for(const auto& row : nativeWorldPlacement.addonChapter4)
        nativeWorldSpawn(row);
    nativeWorldFlag(chapters[i],yes);
    }
  currentStoryWorld = nativeWorldNumber("ADDONWORLD_ZEN");
  nativeWorldOperation("B_INITNPCGLOBALS");
  }

void GameScript::runWorldInit(std::string_view name) {
  const auto key = storygraph::nativeOperationName(name);
  if(key=="INIT_GLOBAL") {
    nativeWorldOperation("GAME_INITGERMAN");
    return;
    }
  const bool oldWorld = key=="INIT_OLDWORLD";
  const bool addonWorld = key=="INIT_ADDONWORLD";
  const bool dragonIsland = key=="INIT_DRAGONISLAND";
  if(!oldWorld && !addonWorld && !dragonIsland && key!="INIT_NEWWORLD")
    throw std::runtime_error("Missing world initialization "+key);

  if(oldWorld) {
    runNativeWorldRooms("INIT_SUB_OLDCAMP");
    runNativeWorldRooms("INIT_SUB_SURFACE");
    nativeWorldFunction("B_ENTER_OLDWORLD");
    }
  if(addonWorld)
    runNativeWorldRooms("INIT_SUB_ADDON_PART_BANDITSCAMP_01");
  nativeWorldOperation("B_INITMONSTERATTITUDES");
  nativeWorldOperation("B_INITNPCGLOBALS");
  if(addonWorld) {
    runNativeAddonChapterEntry();
    return;
    }
  if(dragonIsland) {
    currentStoryWorld = nativeWorldNumber("DRAGONISLAND_ZEN");
    const auto& player = storyActorSlot("HERO");
    const auto actor = NativeValue::instance(player ? operationActor : 0);
    const auto item = NativeValue::integer(nativeWorldActorId("ITKE_SHIP_LEVELCHANGE_MIS",false));
    if(nativeWorldOperation("NPC_HASITEMS",{actor,item},player).i!=0)
      nativeWorldOperation("NPC_REMOVEINVITEMS",{actor,item,NativeValue::integer(1)},player);
    nativeWorldOperation("B_INITNPCGLOBALS");
    return;
    }

  nativeWorldFunction(oldWorld ? "B_ENTER_OLDWORLD" : "B_ENTER_NEWWORLD");
  const auto yes = nativeWorldNumber("TRUE");
  const auto no = nativeWorldNumber("FALSE");
  const auto advance = [&](int32_t chapter,std::string_view latch,std::string_view ready) {
    if(nativeWorldNumber(latch)==no && nativeWorldNumber(ready)==yes) {
      nativeWorldFunction("B_KAPITELWECHSEL",{chapter,nativeWorldNumber(oldWorld ? "OLDWORLD_ZEN" : "NEWWORLD_ZEN")});
      nativeWorldFlag(latch,yes);
      }
    };
  if(oldWorld) {
    advance(4,"B_CHAPTER4_ONETIME","MIS_READYFORCHAPTER4");
    return;
    }

  constexpr auto thievesCleared = "ANDRE_DIEBESGILDE_AUFGERAEUMT";
  if(nativeWorldNumber(thievesCleared)!=yes && nativeWorldNumber("DIEBESGILDE_VERRATEN")!=0) {
    const auto alive = [&](std::string_view alias) {
      const auto& npc = storyActorSlot(alias);
      return nativeWorldOperation("NPC_ISDEAD",{NativeValue::instance(npc ? operationActor : 0)},npc).i==0;
      };
    const bool ramirez = alive("RAMIREZ");
    const bool jesper = alive("JESPER");
    const bool cassia = alive("CASSIA");
    if(ramirez || jesper || cassia) {
      for(const auto* actor : {"VLK_447_CASSIA","VLK_446_JESPER","VLK_445_RAMIREZ"})
        nativeWorldFunction("B_KILLNPC",{nativeWorldActorId(actor,true)});
      nativeWorldFlag(thievesCleared,yes);
      }
    }
  for(const auto* region : {"INIT_SUB_NEWWORLD_PART_CITY_01","INIT_SUB_NEWWORLD_PART_FARM_01",
                            "INIT_SUB_NEWWORLD_PART_MONASTERY_01","INIT_SUB_NEWWORLD_PART_GREATPEASANT_01"})
    runNativeWorldRooms(region);
  advance(3,"B_CHAPTER3_ONETIME","MIS_READYFORCHAPTER3");
  advance(5,"B_CHAPTER5_ONETIME","MIS_ALLDRAGONSDEAD");
  }
