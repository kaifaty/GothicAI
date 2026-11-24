#include "gamescript.h"

#include "basestore.h"
#include "game/definitions/baseprogram.h"
#include "game/definitions/gameplaydefinitions.h"
#include "game/definitions/runtimebindings.h"
#include "game/definitions/basescripts.h"
#include "gothic.h"
#include "loadprobe.h"
#include "programdata.h"
#include "settings.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"

daedalus::DaedalusVm GameScript::createVm(Gothic& gothic, ItemDefinitionRuntime::Definitions& items,
                                         RoutineDefinitionRuntime::Definitions& routines,
                                         NpcDefinitionRuntime::Definitions& npcs, PlacementDefinitionRuntime::Definitions& placements,
                                         ActivityDefinitionRuntime::Definitions& activities, QuietSoundReaction& quietSound,
                                         bool& nativeRoutineProgram) {
  const auto lang=Settings::getI("GAME", "language");
  const auto& index=baseProgramIndex();
  const auto base=index.dir.parent_path();
  const auto selected=selectBaseProgram(index,gothic.defaultGameDatFile(),lang);
  const auto& sha256=selected.source.recordSha256;
  if(sha256!=GameplayDefinitions::stockProgram)
    throw BaseStore::Error("native game requires the prepared catalog for the verified stock program");
  auto script=daedalus::DaedalusScript::from_program(RuntimeBindings::read(base,sha256));
  items=ItemDefinitionRuntime::load(base,script,sha256);
  routines=RoutineDefinitionRuntime::load(base,script,sha256);
  npcs=NpcDefinitionRuntime::load(base,script,sha256);
  placements=PlacementDefinitionRuntime::load(base,script,sha256);
  activities=ActivityDefinitionRuntime::load(base,script,sha256);
  // Native behavior is verified against this particular stock Night of the Raven program.
  nativeRoutineProgram=true;
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
  const auto flags=daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS;
  auto vm=daedalus::DaedalusVm::without_legacy_dispatcher(std::move(script),flags);
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
  if(instance<=UINT32_MAX) {
    const auto definition = itemDefinitions.items.find(uint32_t(instance));
    if(definition!=itemDefinitions.items.end())
      return definition->second.name;
    if(itemDefinitions.brokenEye==instance)
      return brokenEyePrepared.name;
    }
  return std::nullopt;
  }

bool GameScript::hasDirectorNpcVisual() const {
  return directorVisual!=nullptr && NpcHelperRuntime::enabled() &&
         npcDefinitions.helpers.helpers.contains(directorVisual->index());
  }

void GameScript::setupDirectorNpcEquipment(Npc& npc, const DirectorNpcLook* look,
                                          std::span<const std::pair<uint32_t,int32_t>> items) {
  struct RestoreSelf {
    daedalus::DaedalusSymbol& self;
    std::shared_ptr<daedalus::DaedalusInstance> previous;
    ~RestoreSelf() { self.set_instance(previous); }
    } restore{*vm.global_self(),vm.global_self()->get_instance()};

  const auto actor = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  restore.self.set_instance(actor);

  if(look!=nullptr) {
    if(!hasDirectorNpcVisual()) {
      Tempest::Log::e("[story] ",storyNpcName(npc.instanceSymbol()),": no prepared visual helper");
      return;
      }
    // Remove the template's armor before dressing the new actor.
    if(auto* previous = npc.currentArmor()) {
      const auto instance = previous->clsId();
      npc.unequipItem(instance);
      npc.delItem(instance,1);
      }
    using Value=NpcHelperDefinitions::Value;
    const std::array arguments={
      Value{.kind=Value::Kind::Self},
      Value{.integer=look->gender},
      Value{.kind=Value::Kind::String,.text=look->head},
      Value{.integer=look->face},
      Value{.integer=look->skin},
      Value{.integer=look->armor},
      };
    invokeNpcHelper(actor,directorVisual->index(),arguments);
    }
  for(const auto& [item,count] : items)
    createinvitems(actor,int32_t(item),count);
  }

void GameScript::bindPreparedInventory() {
  if(!nativeRoutineProgram)
    return;

  // Keep the native item type and its alias ready for new inventory stacks.
  preparedItems.reserve(itemDefinitions.items.size()+1);
  const auto bind = [&](uint32_t id, const auto& prepare) {
    auto* symbol = vm.find_symbol_by_index(id);
    const auto previous = symbol->get_instance();
    auto item = std::make_shared<daedalus::IItem>();
    prepare(item,symbol);
    symbol->set_instance(previous);
    preparedItems.emplace(id,PreparedItem{*item,symbol});
    };
  for(const auto& [id,fields] : itemDefinitions.items) {
    (void)fields;
    bind(id,[&](const auto& item, auto* symbol) { vm.allocate_instance(item,symbol); });
    }
  if(const auto id = itemDefinitions.brokenEye) {
    bind(*id,[&](const auto& item, auto* symbol) {
      ItemDefinitionRuntime::initialize(vm,item,symbol,itemDefinitions);
      brokenEyePrepared = *item;
      });
    brokenEyeSetting = findSymbol("TEXT_INNOSEYE_SETTING");
    brokenEyeGem = findSymbol("TEXT_INNOSEYE_GEM");
    }

  findSymbol("B_GIVEINVITEMS.AMOUNT")->bind_global_int(inventoryTransferVariables.amount);
  findSymbol("B_GIVEINVITEMS.ITEMINSTANCE")->bind_global_int(inventoryTransferVariables.item);
  findSymbol("B_GIVEINVITEMS.ITEMNAME")->bind_global_string(inventoryTransferVariables.itemName);
  findSymbol("B_GIVEINVITEMS.CONCATTEXT")->bind_global_string(inventoryTransferVariables.notice);
  inventoryGiver = findSymbol("B_GIVEINVITEMS.GIVER");
  inventoryTaker = findSymbol("B_GIVEINVITEMS.TAKER");
  directorVisual = findSymbol("B_SETNPCVISUAL");
  const auto string = [this](const char* name) -> std::string_view { return findSymbol(name)->get_string(); };
  const auto integer = [this](const char* name) { return findSymbol(name)->get_int(); };
  inventoryTransferText = {
    findSymbol("ITMI_GOLD")->index(),
    string("PRINT_GOLDGEGEBEN"),string("PRINT_GOLDERHALTEN"),
    string("PRINT_ADDON_GEGEBEN"),string("PRINT_ADDON_ERHALTEN"),
    string("PRINT_ITEMSGEGEBEN"),string("PRINT_ITEMSERHALTEN"),
    integer("YPOS_GOLDGIVEN"),integer("YPOS_GOLDTAKEN"),
    integer("YPOS_ITEMGIVEN"),integer("YPOS_ITEMTAKEN")
    };
  inventoryTransferFont = string("FONT_SCREENSMALL");
  }

bool GameScript::bindPreparedItem(const std::shared_ptr<daedalus::IItem>& item, uint32_t instance) {
  const auto found = preparedItems.find(instance);
  if(found==preparedItems.end())
    return false;
  const auto owner = item->user_ptr;
  static_cast<daedalus::DaedalusInstance&>(*item) = found->second.identity;
  item->user_ptr = owner;
  found->second.alias->set_instance(item);
  return true;
  }

void GameScript::initializeInstanceItem(const std::shared_ptr<daedalus::IItem>& item, size_t instance) {
  LoadProbe::Vobs::Scope probe("script_item");
  LoadProbe::Startup::Scope itemProbe(LoadProbe::Startup::Script,false,"placed-items");
  if(instance<=UINT32_MAX && bindPreparedItem(item,uint32_t(instance))) {
    if(itemDefinitions.brokenEye==instance) {
      static_cast<ItemRecord&>(*item) = brokenEyePrepared;
      item->text[0] = brokenEyeSetting->get_string();
      item->text[1] = brokenEyeGem->get_string();
      } else {
      static_cast<ItemRecord&>(*item) = itemDefinitions.items.at(uint32_t(instance));
      }
    return;
    }
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
