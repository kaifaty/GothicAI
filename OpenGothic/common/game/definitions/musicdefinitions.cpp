#include "musicdefinitions.h"

#include "basescripts.h"
#include "basestore.h"
#include "contentcensus.h"
#include "programdata.h"
#include "resources.h"

#include <algorithm>
#include <cctype>

MusicDefinitions::MusicDefinitions() {
  constexpr auto program = MusicDefinitionData::stockProgram;
  if(baseNativeCatalog()==nullptr) {
    const auto* source = ProgramData::musicProgram(baseProgramIndex());
    if(source==nullptr || source->recordSha256!=program)
      throw BaseStore::Error("music requires the supported prepared catalog");
    }

  const auto base = Resources::baseDir();
  const auto section = MusicDefinitionData::section(base,program);
  try {
    themes = MusicDefinitionData::read(base,program);
    if(auto* census = Resources::contentCensus())
      census->replaced("Music.dat","base/"+section.lexically_relative(base).generic_string());
    }
  catch(const std::runtime_error& error) {
    throw BaseStore::Error(section.string()+": "+error.what());
    }
  }

const Def::MusicTheme* MusicDefinitions::operator [](std::string_view name) const {
  std::string upper(name);
  std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char c) {
    return char(std::toupper(c));
    });
  for(const auto& theme : themes)
    if(theme.name==upper)
      return &theme.value;
  return nullptr;
  }
