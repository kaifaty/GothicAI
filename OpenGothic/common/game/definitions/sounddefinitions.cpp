#include "sounddefinitions.h"
#include "basescripts.h"
#include "basestore.h"
#include "contentcensus.h"
#include "definitiontypes.h"
#include "gothic.h"
#include "programdata.h"
#include "resources.h"
#include "sfxdefinitiondata.h"

SoundDefinitions::SoundDefinitions() {
  const auto& index = baseProgramIndex();
  if(const auto* source = ProgramData::sfxProgram(index);
     source!=nullptr && source->recordSha256==SfxDefinitionData::stockProgram) {
    const auto base = index.dir.parent_path();
    const auto section = SfxDefinitionData::section(base,source->recordSha256);
    try {
      if(std::filesystem::exists(section)) {
        auto presets = SfxDefinitionData::read(base,source->recordSha256);
        sfx.reserve(presets.size());
        for(auto& [name,effect] : presets)
          sfx.emplace(std::move(name),std::make_shared<Def::SoundEffect>(std::move(effect)));
        if(auto* census = Resources::contentCensus())
          census->replaced("Sfx.dat","base/"+section.lexically_relative(base).generic_string());
        return;
        }
      }
    catch(const std::runtime_error& error) {
      throw BaseStore::Error(section.string()+": "+error.what());
      }
    }

  auto vm = Gothic::inst().createPhoenixVm("Sfx.dat");

  vm->enumerate_instances_by_class_name("C_SFX", [this, &vm](daedalus::DaedalusSymbol& s) {
    try {
      this->sfx[s.name()] = std::make_shared<Def::SoundEffect>(vm->init_instance<VmDef::SoundEffect>(&s)->copy());
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

