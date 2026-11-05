#include "sounddefinitions.h"

#include <Tempest/Log>
#include <gothic.h>

using namespace Tempest;

SoundDefinitions::SoundDefinitions() {
  auto vm = Gothic::inst().createPhoenixVm("Sfx.dat");

  vm->enumerate_instances_by_class_name("C_SFX", [this, &vm](daedalus::DaedalusSymbol& s) {
    try {
      this->sfx[s.name()] = vm->init_instance<Def::SoundEffect>(&s);
      }
    catch(const daedalus::DaedalusScriptError&) {
      // There was an error during initialization. Ignore it.
      }
  });
  }

const Def::SoundEffect& SoundDefinitions::operator[](std::string_view name) const {
  std::string buf(name); // FIXME
  auto i = sfx.find(buf);
  if(i!=sfx.end())
    return *i->second;
  static const Def::SoundEffect s {};
  return s;
  }

