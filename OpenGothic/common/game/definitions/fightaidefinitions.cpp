#include "fightaidefinitions.h"

#include "basescripts.h"
#include "fightdefinitioncapture.h"
#include "gothic.h"
#include "programdata.h"

#include <stdexcept>

FightAi::FightAi() {
  const auto& index = baseProgramIndex();
  const auto* source = ProgramData::fightProgram(index);
  if(source==nullptr)
    throw std::runtime_error("fight definitions: no source in program base index");
  const auto base = index.dir.parent_path();
  if(std::filesystem::exists(FightDefinitions::section(base,source->recordSha256))) {
    fAi = FightDefinitions::read(base,source->recordSha256);
    } else {
    // Extra script programs can call engine externals while building their tactics.
    auto vm = Gothic::inst().createPhoenixVm("Fight.dat");
    fAi = FightDefinitions::capture(*vm);
    }
  }

const FightAi::FA& FightAi::operator[](size_t i) const {
  if(i<fAi.size())
    return fAi[i];
  static FA tmp;
  return tmp;
  }
