#include "baseimport.h"

#include "game/definitions/fightdefinitioncapture.h"
#include "programdata.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace {
struct FightNeedsRuntime {};
}

void BaseImport::convertFightDefinitions(const std::filesystem::path& base) {
  const auto index = ProgramData::readIndex(base/"programs");
  const auto* source = ProgramData::fightProgram(index);
  if(source==nullptr)
    return;
  std::string sha;
  const auto program = ProgramData::readProgram(index.path(*source),&sha);
  if(sha!=source->recordSha256)
    throw std::runtime_error("fight definitions: program record differs from index");

  daedalus::DaedalusVm vm(daedalus::DaedalusScript::from_program(program),daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  if(auto* type = vm.find_symbol_by_name("C_FIGHTAI"))
    vm.register_as_opaque(type);
  // Only store tactics that can be resolved without the live game's engine callbacks.
  vm.observe_externals([](const auto&) { throw FightNeedsRuntime{}; });
  std::vector<FightTactic> tactics;
  try {
    tactics = FightDefinitions::capture(vm);
    }
  catch(const FightNeedsRuntime&) {
    return;
    }
  FightDefinitions::write(base,sha,tactics);
  const auto roundTrip = FightDefinitions::read(base,sha);
  if(roundTrip.size()!=tactics.size())
    throw std::runtime_error("fight definitions: tactic count differs");
  for(size_t i=0; i<tactics.size(); ++i) {
    for(const auto& [prefix, field] : FightDefinitions::slots) {
      const auto& actual = (roundTrip[i].*field).move;
      const auto& expected = (tactics[i].*field).move;
      if(!std::equal(std::begin(actual),std::end(actual),std::begin(expected)))
        throw std::runtime_error("fight definitions: move row differs for "+std::string(prefix)+std::to_string(i));
      }
    }
  }
