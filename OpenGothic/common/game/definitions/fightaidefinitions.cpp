#include "fightaidefinitions.h"

#include "basescripts.h"
#include "basestore.h"
#include "fightdefinitiondata.h"
#include "programdata.h"

FightAi::FightAi() {
  const auto& index = baseProgramIndex();
  const auto* source = ProgramData::fightProgram(index);
  if(source==nullptr)
    throw BaseStore::Error("fight tactics require a prepared catalog");

  const auto base = index.dir.parent_path();
  const auto section = FightDefinitions::section(base,source->recordSha256);
  try {
    fAi = FightDefinitions::read(base,source->recordSha256);
    }
  catch(const std::runtime_error& error) {
    throw BaseStore::Error(section.string()+": "+error.what());
    }
  }

const FightAi::FA& FightAi::operator[](size_t i) const {
  if(i<fAi.size())
    return fAi[i];
  static FA tmp;
  return tmp;
  }
