#include "gamescript.h"

#include "game/definitions/baseprogram.h"
#include "game/definitions/basescripts.h"
#include "gothic.h"
#include "loadprobe.h"
#include "programdata.h"
#include "settings.h"

daedalus::DaedalusVm GameScript::createVm(Gothic& gothic, ItemDefinitionRuntime::Definitions& items,
                                         RoutineDefinitionRuntime::Definitions& routines,
                                         NpcDefinitionRuntime::Definitions& npcs, PlacementDefinitionRuntime::Definitions& placements,
                                         ActivityDefinitionRuntime::Definitions& activities, QuietSoundHeight& quietSound) {
  const auto lang=Settings::getI("GAME", "language");
  std::string sha256;
  auto script=loadBaseProgram(gothic.defaultGameDatFile(),lang,&sha256);
  items=ItemDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  routines=RoutineDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  npcs=NpcDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  placements=PlacementDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  activities=ActivityDefinitionRuntime::load(baseProgramIndex().dir.parent_path(),script,sha256);
  // This stock NotR callback returns at the height check, before any room or story effects.
  static constexpr std::string_view quietSoundProgram="a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c";
  if(sha256==quietSoundProgram) {
    const auto* callback=script.find_symbol_by_name("B_ASSESSQUIETSOUND");
    const auto* height=script.find_symbol_by_name("PERC_DIST_HEIGHT");
    quietSound={ScriptFn(callback->index()),height->get_int(0)};
    }
  const auto flags=daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS;
  daedalus::DaedalusVm vm(std::move(script),flags);
  NpcDefinitionRuntime::observeCalls(vm,npcs,baseProgramIndex().dir.parent_path(),sha256);
  PlacementDefinitionRuntime::observeCalls(vm,baseProgramIndex().dir.parent_path(),sha256);
  return vm;
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
