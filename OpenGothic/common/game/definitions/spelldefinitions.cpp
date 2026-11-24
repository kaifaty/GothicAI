#include "spelldefinitions.h"

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "gameplaydefinitions.h"
#include "utils/string_frm.h"

#include <Tempest/Log>
#include <cctype>

using namespace Tempest;

SpellDefinitions::SpellDefinitions(daedalus::DaedalusVm& vm) {
  vm.enumerate_instances_by_class_name("C_Spell",[&](daedalus::DaedalusSymbol& sym) {
    spl.emplace(sym.name(),vm.init_instance<daedalus::ISpell>(&sym));
    });
  }

SpellDefinitions::SpellDefinitions(const GameplayDefinitions& records) {
  for(auto& [name,spell] : records.spells())
    spl.emplace(std::move(name),std::make_shared<SpellRecord>(spell));
  }

const SpellRecord& SpellDefinitions::find(std::string_view instanceName) const {
  string_frm name("SPELL_",instanceName);
  for(auto& ch : name)
    ch = char(std::toupper(static_cast<unsigned char>(ch)));

  if(const auto spell = spl.find(std::string_view(name)); spell!=spl.end())
    return *spell->second;
  Log::d("invalid spell [",instanceName.data(),"]");
  static constexpr SpellRecord szero = {};
  return szero;
  }
