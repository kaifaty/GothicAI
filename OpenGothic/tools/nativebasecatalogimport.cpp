#include "nativebasecatalogimport.h"

#include "daedalus/script.h"
#include "definitionjson.h"
#include "game/definitions/cameradefinitiondata.h"
#include "game/definitions/effectdefinitiondata.h"
#include "game/definitions/fightdefinitiondata.h"
#include "game/definitions/menudefinitions.h"
#include "game/definitions/musicdefinitiondata.h"
#include "game/definitions/sfxdefinitiondata.h"
#include "game/definitions/stockstoryprograms.h"
#include "outputunits.h"
#include "programdata.h"

#include <algorithm>

namespace {

const ProgramData::Source* programAt(const ProgramData::Index& index, std::string_view name, int language) {
  const auto row = std::ranges::find_if(index.selections,[&](const auto& entry) {
    return entry.request==name && entry.language==language && entry.source &&
           index.sources.at(*entry.source).skipped.empty();
    });
  return row!=index.selections.end() ? &index.sources.at(*row->source) : nullptr;
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
  return {fight->recordSha256,language,{}};
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

void NativeBaseCatalog::verify(const std::filesystem::path& base, const Selection& expected) {
  using DefinitionJson::require;
  require(!std::filesystem::exists(base/"programs"),"native catalog base contains the program directory");
  const auto selected = read(base);
  require(selected.fight==expected.fight && selected.language==expected.language && selected.states==expected.states,
          "native catalog selection differs from its source");
  require(!FightDefinitions::read(base,selected.fight).empty(),"native catalog base has no fight tactics");
  require(MenuDefinitions::read(base,MenuDefinitions::stockProgram).menus.contains("MENU_MAIN"),
          "native catalog base has no main menu");
  (void)CameraDefinitionData::read(base,CameraDefinitionData::stockProgram);
  (void)SfxDefinitionData::read(base,SfxDefinitionData::stockProgram);
  (void)MusicDefinitionData::read(base,MusicDefinitionData::stockProgram);
  (void)EffectDefinitionData::readParticles(base,EffectDefinitionData::stockParticleProgram);
  (void)EffectDefinitionData::readVisuals(base,EffectDefinitionData::stockVisualProgram);
  }
