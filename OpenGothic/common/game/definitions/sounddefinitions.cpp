#include "sounddefinitions.h"
#include "basescripts.h"
#include "basestore.h"
#include "contentcensus.h"
#include "programdata.h"
#include "resources.h"
#include "sfxdefinitiondata.h"

SoundDefinitions::SoundDefinitions() {
  constexpr auto program = SfxDefinitionData::stockProgram;
  if(baseNativeCatalog()==nullptr) {
    const auto* source = ProgramData::sfxProgram(baseProgramIndex());
    if(source==nullptr || source->recordSha256!=program)
      throw BaseStore::Error("sound effects require the supported prepared catalog");
    }

  const auto base = Resources::baseDir();
  const auto section = SfxDefinitionData::section(base,program);
  try {
    auto presets = SfxDefinitionData::read(base,program);
    sfx.reserve(presets.size());
    for(auto& [name,effect] : presets)
      sfx.emplace(std::move(name),std::make_shared<Def::SoundEffect>(std::move(effect)));
    if(auto* census = Resources::contentCensus())
      census->replaced("Sfx.dat","base/"+section.lexically_relative(base).generic_string());
    }
  catch(const std::runtime_error& error) {
    throw BaseStore::Error(section.string()+": "+error.what());
      }
  }

const Def::SoundEffect& SoundDefinitions::operator[](std::string_view name) const {
  std::string buf(name); // FIXME
  auto i = sfx.find(buf);
  if(i!=sfx.end())
    return *i->second;
  static const Def::SoundEffect s {};
  return s;
  }

