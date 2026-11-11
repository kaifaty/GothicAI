#include "musicdefinitions.h"
#include "definitiontypes.h"

#include <Tempest/Log>

#include "gothic.h"

using namespace Tempest;

MusicDefinitions::MusicDefinitions() {
  vm = Gothic::inst().createPhoenixVm("Music.dat");

  vm->enumerate_instances_by_class_name("C_MusicTheme", [this](daedalus::DaedalusSymbol& s) {
    themes.emplace_back(s.index(),vm->init_instance<VmDef::MusicTheme>(&s)->copy());
    });
  }

MusicDefinitions::~MusicDefinitions() {
  }

const Def::MusicTheme* MusicDefinitions::operator [](std::string_view name) const {
  if(!vm)
    return nullptr;

  auto id = vm->find_symbol_by_name(name);
  if(id==nullptr)
    return nullptr;
  for(auto& i:themes) {
    if(i.first == id->index())
      return &i.second;
    }
  return nullptr;
  }
