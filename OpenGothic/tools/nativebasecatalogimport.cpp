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

#include <algorithm>
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
  return {fight->recordSha256,language,{}, {}, {}};
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

void NativeBaseCatalog::verify(const std::filesystem::path& base, const Selection& expected) {
  using DefinitionJson::require;
  require(!std::filesystem::exists(base/"programs"),"native catalog base contains the program directory");
  const auto selected = read(base);
  require(selected.fight==expected.fight && selected.language==expected.language && selected.states==expected.states &&
          selected.initialState==expected.initialState && selected.identities==expected.identities,
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
