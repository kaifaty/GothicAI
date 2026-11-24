#include "musicdefinitions.h"

#include "basescripts.h"
#include "basestore.h"
#include "contentcensus.h"
#include "programdata.h"
#include "resources.h"

#include <algorithm>
#include <cctype>

MusicDefinitions::MusicDefinitions() {
  const auto& index = baseProgramIndex();
  const auto* source = ProgramData::musicProgram(index);
  if(source==nullptr || source->recordSha256!=MusicDefinitionData::stockProgram)
    throw BaseStore::Error("music requires the supported prepared catalog");

  const auto base = index.dir.parent_path();
  const auto section = MusicDefinitionData::section(base,source->recordSha256);
  try {
    themes = MusicDefinitionData::read(base,source->recordSha256);
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
