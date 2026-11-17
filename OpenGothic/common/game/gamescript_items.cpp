#include "gamescript.h"

#include "game/definitions/baseprogram.h"
#include "game/definitions/basescripts.h"
#include "gothic.h"
#include "loadprobe.h"
#include "programdata.h"
#include "settings.h"

daedalus::DaedalusVm GameScript::createVm(Gothic& gothic, ItemDefinitionRuntime::Definitions& items) {
  const auto lang=Settings::getI("GAME", "language");
  std::string sha256;
  auto script=loadBaseProgram(gothic.defaultGameDatFile(),lang,&sha256);
  items=ItemDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  const auto flags=daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS;
  return daedalus::DaedalusVm(std::move(script),flags);
  }

auto GameScript::createItemForQuery(size_t instance) -> std::shared_ptr<ItemRecord> {
  auto item=std::make_shared<daedalus::IItem>();
  ItemDefinitionRuntime::initialize(vm,item,vm.find_symbol_by_index(uint32_t(instance)),itemDefinitions);
  return item;
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
