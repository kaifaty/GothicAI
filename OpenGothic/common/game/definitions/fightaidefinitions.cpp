#include "fightaidefinitions.h"

#include "basescripts.h"
#include "basestore.h"
#include "fightdefinitiondata.h"
#include "resources.h"

FightAi::FightAi() {
  const auto catalog = baseFightCatalog();
  if(catalog.empty())
    throw BaseStore::Error("fight tactics require a prepared catalog");

  const auto base = Resources::baseDir();
  const auto section = FightDefinitions::section(base,catalog);
  try {
    fAi = FightDefinitions::read(base,catalog);
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
