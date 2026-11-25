#include "nativebasecatalogimport.h"

#include "daedalus/script.h"
#include "definitionjson.h"
#include "game/definitions/cameradefinitiondata.h"
#include "game/definitions/dialoginfodata.h"
#include "game/definitions/effectdefinitiondata.h"
#include "game/definitions/fightdefinitiondata.h"
#include "game/definitions/menudefinitions.h"
#include "game/definitions/musicdefinitiondata.h"
#include "game/definitions/sfxdefinitiondata.h"
#include "game/definitions/stockstoryprograms.h"
#include "itemdefinitions.h"
#include "npcdefinitions.h"
#include "outputunits.h"
#include "programdata.h"
#include "routinedefinitions.h"

#include <algorithm>
#include <set>
#include <type_traits>

namespace {

const ProgramData::Source* programAt(const ProgramData::Index& index, std::string_view name, int language) {
  const auto row = std::ranges::find_if(index.selections,[&](const auto& entry) {
    return entry.request==name && entry.language==language && entry.source &&
           index.sources.at(*entry.source).skipped.empty();
    });
  return row!=index.selections.end() ? &index.sources.at(*row->source) : nullptr;
  }

template<class Captured, class Uncaptured>
void verifyArchetypes(const std::vector<NativeBaseCatalog::Identity>& identities,
                      const Captured& captured, const Uncaptured& uncaptured, std::string_view family) {
  using DefinitionJson::require;
  require(identities.size()==captured.size()+uncaptured.size(),
          "native "+std::string(family)+" identity count differs from prepared definitions");
  for(const auto& identity : identities)
    require(captured.contains(identity.name) || uncaptured.contains(identity.name),
            "native "+std::string(family)+" has no prepared definition for "+identity.name);
  }

void verifyIdentities(const std::filesystem::path& base, const NativeBaseCatalog::Identities& identities) {
  const auto npcs = NpcDefinitions::read(base,StockStoryPrograms::stockProgram);
  verifyArchetypes(identities.npcs,npcs.npcs,npcs.uncaptured,"NPC");
  const auto items = ItemDefinitions::read(base,StockStoryPrograms::stockProgram);
  verifyArchetypes(identities.items,items.items,items.uncaptured,"item");
  // The prepared dialog records already own the fields and their numeric keys.
  const auto dialogs = DialogInfoData::load(base);
  DefinitionJson::require(dialogs && dialogs->size()==identities.dialogs.size(),
                          "native dialog identity count differs from prepared definitions");
  for(const auto& identity : identities.dialogs)
    DefinitionJson::require(dialogs->contains(identity.id),"no prepared dialog definition for "+identity.name);
  }

}

NativeBaseCatalog::Selection NativeBaseCatalog::prepare(const ProgramData::Index& programs,
                                                         const OutputUnits::Index& outputUnits) {
  using DefinitionJson::require;
  int language = -1;
  for(int candidate=-1; candidate<8; ++candidate) {
    if(programAt(programs,"GOTHIC.DAT",candidate)==nullptr)
      continue;
    const auto hasOutput = std::ranges::any_of(outputUnits.selections,[&](const auto& row) {
      return row.language==candidate && row.source && outputUnits.sources.at(*row.source).skipped.empty();
      });
    if(hasOutput) {
      language = candidate;
      break;
      }
    }

  const auto* gameplay = programAt(programs,"GOTHIC.DAT",language);
  require(gameplay!=nullptr && gameplay->recordSha256==StockStoryPrograms::stockProgram,
          "native base needs the stock game catalog for the selected language");
  const auto* menu = programAt(programs,"MENU.DAT",language);
  require(menu!=nullptr && menu->recordSha256==MenuDefinitions::stockProgram,
          "native base needs the stock menu catalog for the selected language");
  const auto* fight = ProgramData::fightProgram(programs);
  require(fight!=nullptr,"native base needs a fight catalog");
  Selection selection;
  selection.fight=fight->recordSha256;
  selection.language=language;
  return selection;
  }

uint32_t NativeBaseCatalog::prepareDirectorFirstId(const daedalus::DaedalusScript& program) {
  static_assert(daedalus::DaedalusScript::MAX_ADDED_SYMBOLS==directorCapacity);
  const auto& symbols = program.symbols();
  DefinitionJson::require(symbols.size()<uint32_t(INT32_MAX)-directorCapacity,
                          "stock program does not leave room for director IDs");
  DefinitionJson::require(std::ranges::none_of(symbols,[](const auto& symbol) { return symbol.name().starts_with("DIR_"); }),
                          "stock program uses reserved director names");
  // v57 allocates one scratch slot immediately after the authored program.
  return uint32_t(symbols.size())+1;
  }

std::vector<NativeBaseCatalog::State> NativeBaseCatalog::prepareStates(const daedalus::DaedalusScript& program) {
  const auto isState = [](const daedalus::DaedalusSymbol& fn) {
    return fn.type()==daedalus::DaedalusDataType::FUNCTION && fn.is_const() && !fn.is_external() &&
           fn.name().starts_with("ZS_");
    };
  std::vector<State> states;
  for(const auto& init : program.symbols()) {
    if(!isState(init))
      continue;
    const auto phase = [&](const char* suffix) -> std::optional<uint32_t> {
      const auto name = init.name()+suffix;
      if(const auto* fn=program.find_symbol_by_name(name)) {
        DefinitionJson::require(isState(*fn),"invalid stock NPC state phase "+name);
        return fn->index();
        }
      return std::nullopt;
      };
    states.push_back({init.index(),init.name(),phase("_LOOP"),phase("_END")});
    }
  return states;
  }

std::vector<NativeBaseCatalog::InitialRow> NativeBaseCatalog::prepareInitialState(const daedalus::DaedalusScript& program) {
  using Type = daedalus::DaedalusDataType;
  const auto* npcClass=program.find_symbol_by_name("C_NPC");
  const auto* itemClass=program.find_symbol_by_name("C_ITEM");
  DefinitionJson::require(npcClass && itemClass,"missing stock actor or item class for initial state");

  std::vector<InitialRow> rows;
  for(const auto& symbol : program.symbols()) {
    const auto& name=symbol.name();
    if(symbol.is_member() || symbol.is_const() || name=="MIS_BENNET_BRINGORE" || name=="BENNET_TEACHSMITH" ||
       name=="$PHOENIX_FAKE_STRINGS" || name=="$INSTANCE_HELP")
      continue;
    const auto type=symbol.type();
    if(type==Type::INSTANCE) {
      const auto* parent=program.find_symbol_by_index(uint32_t(symbol.parent()));
      if(parent==npcClass || parent==itemClass)
        rows.push_back({symbol.index(),name,parent==npcClass ? Alias::Npc : Alias::Item});
      continue;
      }
    if(symbol.count()==0)
      continue;
    const auto values = [&]<class Getter>(Getter get) {
      using T = std::decay_t<decltype(get(uint16_t{}))>;
      std::vector<T> result;
      result.reserve(symbol.count());
      for(uint16_t i=0; i<symbol.count(); ++i)
        result.push_back(get(i));
      return result;
      };
    switch(type) {
      case Type::INT:    rows.push_back({symbol.index(),name,values([&](uint16_t i) { return symbol.get_int(i); })}); break;
      case Type::FLOAT:  rows.push_back({symbol.index(),name,values([&](uint16_t i) { return symbol.get_float(i); })}); break;
      case Type::STRING: rows.push_back({symbol.index(),name,values([&](uint16_t i) { return symbol.get_string(i); })}); break;
      default: break;
      }
    }
  return rows;
  }

NativeBaseCatalog::Constants NativeBaseCatalog::prepareConstants(const daedalus::DaedalusScript& program) {
  using enum daedalus::DaedalusDataType;
  Constants result;
  for(const auto& symbol : program.symbols()) {
    if(!symbol.is_const() || symbol.is_member() || symbol.is_generated() || symbol.count()==0)
      continue;
    const auto values = [&]<class T>(auto get) {
      std::vector<T> array;
      array.reserve(symbol.count());
      for(uint16_t index=0;index<symbol.count();++index)
        array.emplace_back(get(index));
      return array;
      };
    InitialValue data;
    switch(symbol.type()) {
      case INT: data=values.operator()<int32_t>([&](uint16_t i) { return symbol.get_int(i); }); break;
      case FLOAT: data=values.operator()<float>([&](uint16_t i) { return symbol.get_float(i); }); break;
      case STRING: data=values.operator()<std::string>([&](uint16_t i) { return symbol.get_string(i); }); break;
      default: continue;
      }
    DefinitionJson::require(result.emplace(symbol.name(),Constant{symbol.index(),std::move(data)}).second,
                            "duplicate named constant "+symbol.name());
    }
  // These stock literals have native consumers outside the prepared story programs.
  static constexpr std::pair<std::string_view,std::string_view> literals[] = {
    {"AWAKENED_STONE_GOLEM_NAME", "$27152"},
    {"ITEM_BROKEN_EYE_NAME", "$15687"},
    {"ITEM_BROKEN_EYE_VISUAL", "$15688"},
    {"HERO_MANA_POTION_MISSING", "$18825"},
    {"HERO_MANA_POTION_FULL", "$18826"},
    {"HERO_HEALTH_POTION_MISSING", "$18827"},
    {"HERO_HEALTH_POTION_FULL", "$18828"},
    {"HERO_WEAPON_DAMAGE_SUFFIX", "$18925"},
    {"HERO_CAST_CIRCLE_SUFFIX", "$27336"},
    {"HERO_EQUIP_UNKNOWN_REASON", "$27337"},
    {"HERO_EQUIP_ATTRIBUTE_SUFFIX", "$27338"},
    {"HERO_LOCK_UNLOCK_SOUND", "$27339"},
    {"HERO_LOCK_SUCCESS_SOUND", "$27340"},
    {"HERO_LOCK_BROKEN_SOUND", "$27341"},
    {"HERO_LOCK_FAILURE_SOUND", "$27342"},
    {"ITEM_HEALTH_SET_VISUAL", "$12935"},
    {"ITEM_HEALTH_SET_SOUND", "$12936"},
    {"ITEM_MANA_SET_VISUAL", "$12937"},
    {"ITEM_MANA_SET_SOUND", "$12938"},
    {"ITEM_STRENGTH_SET_VISUAL", "$12939"},
    {"ITEM_STRENGTH_SET_SOUND", "$12940"},
    {"ITEM_EYE_CHARGED_VISUAL", "$15676"},
    {"ITEM_EYE_CHARGED_GLOW", "$15677"},
    {"ITEM_EYE_CHARGED_QUAKE", "$15678"},
    {"ITEM_EYE_CHARGED_ANIMATION", "$15679"},
    {"ITEM_EYE_CHARGED_SOUND", "$15680"},
    {"ITEM_EYE_CHARGED_MESSAGE", "$15681"},
    {"ITEM_EYE_DISCHARGED_VISUAL", "$15685"},
    {"ITEM_EYE_DISCHARGED_SOUND", "$15686"},
    {"ITEM_DEATH_AMULET_LIGHT", "$16181"},
    {"ITEM_DEATH_AMULET_QUAKE", "$16182"},
    {"ITEM_DEATH_AMULET_VISUAL", "$16183"},
    {"ITEM_DEATH_AMULET_ANIMATION", "$16184"},
    {"ITEM_DEATH_AMULET_SOUND", "$16185"},
    {"ITEM_WISP_SPAWN_WAYPOINT", "$16475"},
    {"ITEM_WISP_VISUAL", "$16476"},
    {"ITEM_WISP_EQUIP_SOUND", "$16477"},
    {"ITEM_WISP_UNEQUIP_SOUND", "$16478"},
    {"ITEM_WISP_REMOVE_WAYPOINT", "$16479"},
    {"ITEM_WISP_RETURN_WAYPOINT", "$16480"},
    };
  for(const auto& [key,name] : literals) {
    const auto* symbol=program.find_symbol_by_name(name);
    DefinitionJson::require(symbol && symbol->is_const() && symbol->is_generated() &&
                            symbol->type()==STRING && symbol->count()==1,
                            "invalid stock literal for "+std::string(key));
    DefinitionJson::require(result.emplace(key,Constant{symbol->index(),std::vector<std::string>{symbol->get_string()}}).second,
                            "duplicate stock literal "+std::string(key));
    }
  return result;
  }

NativeBaseCatalog::Identities NativeBaseCatalog::prepareIdentities(daedalus::DaedalusScript& program,
                                                                    const std::filesystem::path& base) {
  const auto declarations = [&](std::string_view cls) {
    std::vector<Identity> result;
    program.enumerate_instances_by_class_name(cls,[&](const auto& symbol) {
      DefinitionJson::require(!symbol.is_member() && !symbol.is_generated(),"invalid authored identity "+symbol.name());
      result.push_back({symbol.index(),symbol.name()});
      });
    return result;
    };
  Identities result{declarations("C_NPC"),declarations("C_ITEM"),declarations("C_INFO")};
  verifyIdentities(base,result);
  return result;
  }

std::vector<NativeBaseCatalog::Identity> NativeBaseCatalog::prepareRoutines(const daedalus::DaedalusScript& program,
                                                                         const std::filesystem::path& base) {
  using DefinitionJson::require;
  const auto library=RoutineDefinitions::read(base,StockStoryPrograms::stockProgram);
  require(library.uncaptured.empty(),"native routine catalog has uncaptured routines");
  std::vector<Identity> routines;
  routines.reserve(library.routines.size());
  for(const auto& [name,rows] : library.routines) {
    const auto* function=program.find_symbol_by_name(name);
    require(function && function->type()==daedalus::DaedalusDataType::FUNCTION && !function->is_external() &&
            function->count()==0 && !function->has_return(),"invalid native routine function "+name);
    routines.push_back({function->index(),name});
    }
  std::ranges::sort(routines,{},&Identity::id);
  return routines;
  }

std::vector<NativeBaseCatalog::Identity> NativeBaseCatalog::prepareItemCallbacks(const daedalus::DaedalusScript& program,
                                                                                const std::filesystem::path& base) {
  using DefinitionJson::require;
  const auto library=ItemDefinitions::read(base,StockStoryPrograms::stockProgram);
  std::set<std::string,std::less<>> names;
  for(const auto& [item,definition] : library.items)
    for(const auto& [field,references] : definition.symbols)
      if(field!="owner")
        for(const auto& name : references)
          if(!name.empty())
            names.insert(name);
  if(library.uncaptured.contains("ITMI_INNOSEYE_BROKEN_MIS"))
    names.emplace("EQUIP_ITMI_INNOSEYE_BROKEN_MIS");

  std::vector<Identity> result;
  result.reserve(names.size());
  for(const auto& name : names) {
    const auto* symbol=program.find_symbol_by_name(name);
    require(symbol && symbol->type()==daedalus::DaedalusDataType::FUNCTION && symbol->is_const() && !symbol->is_external(),
            "invalid native item callback "+name);
    result.push_back({symbol->index(),name});
    }
  std::ranges::sort(result,{},&Identity::id);
  return result;
  }

std::vector<NativeBaseCatalog::Identity> NativeBaseCatalog::prepareNpcCallbacks(const daedalus::DaedalusScript& program) {
  // These names are assigned to perception slots or started as native NPC phases.
  static constexpr std::string_view names[] = {
    "B_ASSESSDAMAGE", "B_ASSESSDEFEAT", "B_ASSESSDRAWWEAPON", "B_ASSESSENEMY",
    "B_ASSESSENTERROOM", "B_ASSESSFIGHTSOUND", "B_ASSESSFOLLOWFIGHTSOUND", "B_ASSESSFOLLOWPLAYER",
    "B_ASSESSGUIDEFIGHTSOUND", "B_ASSESSGUIDEPLAYER", "B_ASSESSMAGIC", "B_ASSESSMAGICSLEEPTALK",
    "B_ASSESSMURDER", "B_ASSESSPLAYER", "B_ASSESSPORTALCOLLISION", "B_ASSESSQUIETSOUND",
    "B_ASSESSSLEEPTALK", "B_ASSESSSURPRISE", "B_ASSESSTALK", "B_ASSESSTHEFT", "B_ASSESSTHREAT",
    "B_ASSESSUSEMOB", "B_ASSESSWARN", "B_CHEERFIGHT", "B_CLEARROOMTALK", "B_EVENT_PORTAL_EARTHQUAKE",
    "B_EXITIFROOMLEFT", "B_GOLEMRISE", "B_MM_ASSESSBODY", "B_MM_ASSESSDAMAGE", "B_MM_ASSESSENEMY",
    "B_MM_ASSESSOTHERSDAMAGE", "B_MM_ASSESSPLAYER", "B_MM_ASSESSQUIETSOUND_SLEEP", "B_MM_ASSESSSURPRISE",
    "B_MM_ASSESSWARN", "B_MOVEMOB", "B_PAL_ZOMBIE_RISE", "B_RESTARTBURN", "B_RESTARTBURNSHORT",
    "B_RESTARTFREEZE", "B_RESTARTGREENTENTACLE", "B_RESTARTINFLATE", "B_RESTARTSUCKENERGY",
    "B_RESTARTSWARM", "B_RESTARTWHIRLWIND", "B_STOPMAGICBURN", "B_STOPMAGICBURNSHORT",
    "B_STOPMAGICFLEE", "B_STOPMAGICSLEEP", "B_STOPPYRO", "B_STOPSHORTZAPPED", "B_STOPZAPPED",
    "B_SUMMONEDASSESSTALK",
    };
  std::vector<Identity> result;
  result.reserve(std::size(names));
  for(const auto name : names) {
    const auto* function=program.find_symbol_by_name(name);
    DefinitionJson::require(function && function->type()==daedalus::DaedalusDataType::FUNCTION &&
                             function->is_const() && !function->is_external() && function->count()==0,
                             "invalid native NPC callback "+std::string(name));
    result.push_back({function->index(),std::string(name)});
    }
  std::ranges::sort(result,{},&Identity::id);
  return result;
  }

NativeBaseCatalog::NpcConstructorBindings NativeBaseCatalog::prepareNpcConstructor(const daedalus::DaedalusScript& program,
                                                                                    const std::filesystem::path& base) {
  using DefinitionJson::require;
  using Type = daedalus::DaedalusDataType;
  const auto tagged=[](Type type) {
    switch(type) {
      case Type::INT:      return NpcConstructorType::Integer;
      case Type::FLOAT:    return NpcConstructorType::Float;
      case Type::STRING:   return NpcConstructorType::String;
      case Type::INSTANCE: return NpcConstructorType::Instance;
      case Type::FUNCTION: return NpcConstructorType::Function;
      default: throw std::runtime_error("unsupported NPC constructor binding type");
      }
    };
  const auto library=NpcDefinitions::read(base,StockStoryPrograms::stockProgram);
  NpcConstructorBindings result;
  for(const auto& [name,npc] : library.npcs) {
    for(const auto& step : npc.steps) {
      if(const auto* field=std::get_if<NpcDefinitions::Field>(&step)) {
        if(result.members.contains(field->binding))
          continue;
        const auto* binding=program.find_symbol_by_name(field->binding);
        require(binding && binding->is_member() && binding->name().starts_with("C_NPC.") &&
                binding->count()>0 && binding->count()<=UINT16_MAX,
                "invalid NPC constructor member "+field->binding);
        result.members.emplace(field->binding,NpcConstructorMember{binding->index(),uint16_t(binding->count()),tagged(binding->type())});
        }
      else if(const auto* call=std::get_if<NpcDefinitions::Call>(&step)) {
        if(result.calls.contains(call->function))
          continue;
        const auto* function=program.find_symbol_by_name(call->function);
        require(function && function->type()==Type::FUNCTION && function->is_external()!=call->deferred &&
                !function->has_return(),"invalid NPC constructor call "+call->function);
        NpcConstructorCall value{function->index(),call->deferred,{}};
        for(const auto& parameter : program.find_parameters_for_function(function))
          value.parameters.push_back(tagged(parameter.type()));
        result.calls.emplace(call->function,std::move(value));
        }
      }
    }
  return result;
  }

void NativeBaseCatalog::verify(const std::filesystem::path& base, const Selection& expected) {
  using DefinitionJson::require;
  require(!std::filesystem::exists(base/"programs"),"native catalog base contains the program directory");
  const auto selected = read(base);
  require(selected.fight==expected.fight && selected.language==expected.language &&
          selected.directorFirstId==expected.directorFirstId && selected.states==expected.states &&
          selected.initialState==expected.initialState && selected.identities==expected.identities &&
          selected.constants==expected.constants && selected.routines==expected.routines &&
          selected.itemCallbacks==expected.itemCallbacks && selected.npcCallbacks==expected.npcCallbacks &&
          selected.npcConstructor==expected.npcConstructor,
          "native catalog selection differs from its source");
  verifyIdentities(base,selected.identities);
  require(!FightDefinitions::read(base,selected.fight).empty(),"native catalog base has no fight tactics");
  require(MenuDefinitions::read(base,MenuDefinitions::stockProgram).menus.contains("MENU_MAIN"),
          "native catalog base has no main menu");
  (void)CameraDefinitionData::read(base,CameraDefinitionData::stockProgram);
  (void)SfxDefinitionData::read(base,SfxDefinitionData::stockProgram);
  (void)MusicDefinitionData::read(base,MusicDefinitionData::stockProgram);
  (void)EffectDefinitionData::readParticles(base,EffectDefinitionData::stockParticleProgram);
  (void)EffectDefinitionData::readVisuals(base,EffectDefinitionData::stockVisualProgram);
  }
