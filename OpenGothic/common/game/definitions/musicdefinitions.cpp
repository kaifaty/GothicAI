#include "musicdefinitions.h"

#include "basescripts.h"
#include "basestore.h"
#include "contentcensus.h"
#include "definitiontypes.h"
#include "gothic.h"
#include "programdata.h"
#include "resources.h"

#include <algorithm>
#include <cctype>

MusicDefinitions::MusicDefinitions() {
  const auto& index = baseProgramIndex();
  if(const auto* source = ProgramData::musicProgram(index);
     source!=nullptr && source->recordSha256==MusicDefinitionData::stockProgram) {
    const auto base = index.dir.parent_path();
    const auto section = MusicDefinitionData::section(base,source->recordSha256);
    try {
      if(std::filesystem::exists(section)) {
        themes = MusicDefinitionData::read(base,source->recordSha256);
        if(auto* census = Resources::contentCensus())
          census->replaced("Music.dat","base/"+section.lexically_relative(base).generic_string());
        return;
        }
      }
    catch(const std::runtime_error& error) {
      throw BaseStore::Error(section.string()+": "+error.what());
      }
    }

  vm = Gothic::inst().createPhoenixVm("Music.dat");

  vm->enumerate_instances_by_class_name("C_MusicTheme", [this](daedalus::DaedalusSymbol& s) {
    themes.push_back({s.index(),s.name(),vm->init_instance<VmDef::MusicTheme>(&s)->copy()});
    });
  }

MusicDefinitions::~MusicDefinitions() = default;

const Def::MusicTheme* MusicDefinitions::operator [](std::string_view name) const {
  if(vm) {
    const auto* symbol = vm->find_symbol_by_name(name);
    if(symbol==nullptr)
      return nullptr;
    for(const auto& theme : themes)
      if(theme.id==symbol->index())
        return &theme.value;
    return nullptr;
    }

  std::string upper(name);
  std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char c) {
    return char(std::toupper(c));
    });
  for(const auto& theme : themes)
    if(theme.name==upper)
      return &theme.value;
  return nullptr;
  }
