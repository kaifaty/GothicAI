#include "gamescript.h"

#include "basestore.h"
#include "game/definitions/baseprogram.h"
#include "game/definitions/gameplaydefinitions.h"
#include "game/definitions/basescripts.h"
#include "gothic.h"
#include "loadprobe.h"
#include "programdata.h"
#include "settings.h"
#include "storygraph/eval.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"

#include <cstdlib>

daedalus::DaedalusVm GameScript::createVm(Gothic& gothic, ItemDefinitionRuntime::Definitions& items,
                                         RoutineDefinitionRuntime::Definitions& routines,
                                         NpcDefinitionRuntime::Definitions& npcs, PlacementDefinitionRuntime::Definitions& placements,
                                         ActivityDefinitionRuntime::Definitions& activities, QuietSoundReaction& quietSound,
                                         bool& nativeRoutineProgram) {
  const auto lang=Settings::getI("GAME", "language");
  const auto& index=baseProgramIndex();
  const auto base=index.dir.parent_path();
#if defined(OPENGOTHIC_NATIVE_GAME)
  constexpr bool metadata=true;
#else
  const bool metadata=BaseStore::metadataAuditEnabled();
#endif
  std::string sha256;
  auto script=[&] {
    if(!metadata)
      return loadBaseProgram(gothic.defaultGameDatFile(),lang,&sha256);
    const auto selected=selectBaseProgram(index,gothic.defaultGameDatFile(),lang);
    if(selected.source.recordSha256!=GameplayDefinitions::stockProgram)
      throw BaseStore::Error("native game requires the verified stock GOTHIC.DAT program");
    const auto file=ProgramData::symbolMetadataPath(base,selected.source.recordSha256);
    auto symbols=ProgramData::readSymbolMetadata(file,selected.source.recordSha256);
    sha256=selected.source.recordSha256;
    return daedalus::DaedalusScript::from_program(std::move(symbols));
    }();
  items=ItemDefinitionRuntime::load(base,script,sha256);
  routines=RoutineDefinitionRuntime::load(base,script,sha256);
  npcs=NpcDefinitionRuntime::load(base,script,sha256);
  placements=PlacementDefinitionRuntime::load(base,script,sha256);
  activities=ActivityDefinitionRuntime::load(base,script,sha256);
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
#if defined(OPENGOTHIC_NATIVE_GAME)
  auto vm=daedalus::DaedalusVm::without_legacy_dispatcher(std::move(script),flags);
#else
  auto vm=metadata ? daedalus::DaedalusVm::without_legacy_dispatcher(std::move(script),flags)
                   : daedalus::DaedalusVm(std::move(script),flags);
#endif
  Gothic::enableScriptAudit(vm,gothic.defaultGameDatFile());
  NpcDefinitionRuntime::observeCalls(vm,npcs,base,sha256);
  PlacementDefinitionRuntime::observeCalls(vm,base,sha256);
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

void GameScript::setupDirectorNpcEquipment(Npc& npc, const DirectorNpcLook* look,
                                          std::span<const std::pair<uint32_t,int32_t>> items) {
  using storygraph::Value;
  struct RestoreSelf {
    daedalus::DaedalusSymbol& self;
    std::shared_ptr<daedalus::DaedalusInstance> previous;
    ~RestoreSelf() { self.set_instance(previous); }
    } restore{*vm.global_self(),vm.global_self()->get_instance()};

  const auto actor = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  restore.self.set_instance(actor);
  const std::string id(symbolName(npc.instanceSymbol()));
  const auto call = [&](const char* function, std::vector<Value> args) {
    std::string error;
    const auto* symbol = vm.find_symbol_by_name(function);
    if(symbol==nullptr || !storygraph::callFunction(vm,*symbol,args,error))
      Tempest::Log::e("[story] ",id,": ",function," failed: ",symbol==nullptr ? "no such function" : error);
    };

  if(look!=nullptr) {
    // Remove the template's armor before the script dresses the new actor.
    if(auto* previous = npc.currentArmor()) {
      const auto instance = previous->clsId();
      npc.unequipItem(instance);
      npc.delItem(instance,1);
      }
    call("B_SETNPCVISUAL",{Value::instance(actor),Value::integer(look->gender),Value::str(look->head),
                          Value::integer(look->face),Value::integer(look->skin),Value::integer(look->armor)});
    }
  for(const auto& [item,count] : items)
    call("CREATEINVITEMS",{Value::instance(actor),Value::integer(int32_t(item)),Value::integer(count)});
  }

bool GameScript::isStockDirectorActor(const Npc& npc) const {
  const auto* symbol = vm.find_symbol_by_index(npc.instanceSymbol());
  return symbol!=nullptr && symbol->type()==daedalus::DaedalusDataType::INSTANCE && !symbol->is_generated();
  }

Npc* GameScript::boundStockDirectorNpc(std::string_view name) {
  auto* symbol = vm.find_symbol_by_name(name);
  if(symbol==nullptr || symbol->type()!=daedalus::DaedalusDataType::INSTANCE || symbol->is_generated())
    return nullptr;
  auto record = std::dynamic_pointer_cast<daedalus::INpc>(symbol->get_instance());
  return findNpc(static_cast<NpcRecord*>(record.get()));
  }

bool GameScript::tryDirectorGiveItems(Npc& giver, Npc& taker, std::string_view item, int32_t count) {
  if(!nativeRoutineProgram || count<=0 || !isStockDirectorActor(giver) || !isStockDirectorActor(taker))
    return false;
  const auto* enabled = std::getenv("GOTHIC_AI_STORYEXEC");
  if(enabled!=nullptr && std::string_view(enabled)=="0")
    return false;
  const auto* function = vm.find_symbol_by_name("B_GIVEINVITEMS");
  if(function==nullptr || !vm.has_active_override(*function))
    return false;
  const auto id = findItemInstance(item);
  if(!id || !itemDefinitions.items.contains(*id))
    return false;

  giveInventoryItems(std::static_pointer_cast<daedalus::INpc>(giver.handlePtr()),
                    std::static_pointer_cast<daedalus::INpc>(taker.handlePtr()),int32_t(*id),count);
  return true;
  }

bool GameScript::tryDirectorCreateItems(Npc& npc, std::string_view item, int32_t count) {
  if(!nativeRoutineProgram || count<=0 || !isStockDirectorActor(npc))
    return false;
  const auto id = findItemInstance(item);
  if(!id || !itemDefinitions.items.contains(*id))
    return false;

  createinvitems(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()),int32_t(*id),count);
  return true;
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
