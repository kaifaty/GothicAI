#include "gamescript.h"

#include "game/definitions/baseprogram.h"
#include "game/definitions/gameplaydefinitions.h"
#include "game/definitions/basescripts.h"
#include "gothic.h"
#include "loadprobe.h"
#include "programdata.h"
#include "settings.h"

daedalus::DaedalusVm GameScript::createVm(Gothic& gothic, ItemDefinitionRuntime::Definitions& items,
                                         RoutineDefinitionRuntime::Definitions& routines,
                                         NpcDefinitionRuntime::Definitions& npcs, PlacementDefinitionRuntime::Definitions& placements,
                                         ActivityDefinitionRuntime::Definitions& activities, QuietSoundReaction& quietSound,
                                         bool& nativeRoutineProgram) {
  const auto lang=Settings::getI("GAME", "language");
  std::string sha256;
  auto script=loadBaseProgram(gothic.defaultGameDatFile(),lang,&sha256);
  items=ItemDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  routines=RoutineDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  npcs=NpcDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  placements=PlacementDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  activities=ActivityDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  // Native behavior is verified against this particular stock Night of the Raven program.
  nativeRoutineProgram=sha256==GameplayDefinitions::stockProgram;
  if(nativeRoutineProgram) {
    if(!items.items.empty())
      items.brokenEye=script.find_symbol_by_name("ITMI_INNOSEYE_BROKEN_MIS")->index();
    ItemDefinitionRuntime::registerStockRestoreEffects(items,script);
    ItemDefinitionRuntime::registerStockArmorSetEffects(items,script);
    ItemDefinitionRuntime::registerStockFightSkillEffects(items,script);
    ItemDefinitionRuntime::registerStockEquipmentStatEffects(items,script);
    const auto* callback=script.find_symbol_by_name("B_ASSESSQUIETSOUND");
    const auto* height=script.find_symbol_by_name("PERC_DIST_HEIGHT");
    const auto* indoorHeight=script.find_symbol_by_name("PERC_DIST_INDOOR_HEIGHT");
    quietSound={ScriptFn(callback->index()),height->get_int(0),indoorHeight->get_int(0)};
    }
  const auto flags=daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS;
  daedalus::DaedalusVm vm(std::move(script),flags);
  Gothic::enableScriptAudit(vm,gothic.defaultGameDatFile());
  NpcDefinitionRuntime::observeCalls(vm,npcs,baseProgramIndex().dir.parent_path(),sha256);
  PlacementDefinitionRuntime::observeCalls(vm,baseProgramIndex().dir.parent_path(),sha256);
  return vm;
  }

auto GameScript::createItemForQuery(size_t instance) -> std::shared_ptr<ItemRecord> {
  auto item=std::make_shared<daedalus::IItem>();
  ItemDefinitionRuntime::initialize(vm,item,vm.find_symbol_by_index(uint32_t(instance)),itemDefinitions);
  return item;
  }

std::optional<std::string> GameScript::itemNameForQuery(size_t instance) {
  auto* symbol = findSymbol(instance);
  if(symbol==nullptr || symbol->type()!=daedalus::DaedalusDataType::INSTANCE)
    return std::nullopt;

  // A query may use a legacy initializer. Preserve the gameplay instance the symbol refers to.
  const auto previous = symbol->get_instance();
  std::optional<std::string> result;
  try {
    result = createItemForQuery(instance)->name;
    }
  catch(const std::exception&) {
    }
  symbol->set_instance(previous);
  return result;
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

bool GameScript::invokeStockItemProgram(const daedalus::DaedalusSymbol& function) {
  if(!itemDefinitions.itemUsePrograms.contains(function.index()))
    return false;
  int32_t result=0;
  return ItemDefinitionRuntime::showDocument(vm,function.index(),itemDefinitions) || invokeStockProgram(function,result);
  }
