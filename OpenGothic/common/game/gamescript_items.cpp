#include "gamescript.h"

#include "game/definitions/baseprogram.h"
#include "game/definitions/basescripts.h"
#include "gothic.h"
#include "loadprobe.h"
#include "programdata.h"
#include "settings.h"

daedalus::DaedalusVm GameScript::createVm(Gothic& gothic, ItemDefinitionRuntime::Definitions& items) {
  const auto lang=Settings::getI("GAME", "language");
  auto script=loadBaseProgram(gothic.defaultGameDatFile(),lang);
  items=ItemDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script);
  const auto flags=daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS;
  return daedalus::DaedalusVm(std::move(script),flags);
  }

void GameScript::initializeInstanceItem(const std::shared_ptr<daedalus::IItem>& item, size_t instance) {
  LoadProbe::Vobs::Scope probe("script_item");
  LoadProbe::Startup::Scope itemProbe(LoadProbe::Startup::Script,false,"placed-items");
  auto* symbol=vm.find_symbol_by_index(uint32_t(instance));
  if(symbol==nullptr) {
    Tempest::Log::e("Cannot initialize item ",instance,": Symbol not found.");
    return;
    }
  ItemDefinitionRuntime::initialize(vm,item,symbol,itemDefinitions);
  }
