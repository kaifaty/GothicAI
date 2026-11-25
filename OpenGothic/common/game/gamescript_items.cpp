#include "gamescript.h"

#include "basestore.h"
#include "game/definitions/gameplaydefinitions.h"
#include "game/definitions/nativebasecatalog.h"
#include "game/definitions/basescripts.h"
#include "gothic.h"
#include "loadprobe.h"
#include "resources.h"
#include "settings.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"

void GameScript::loadNativeDefinitions(const NativeBaseCatalog::Selection& catalog) {
  const auto& base = Resources::baseDir();
  const auto sha = GameplayDefinitions::stockProgram;
  itemDefinitions = ItemDefinitionRuntime::loadNative(base,catalog,sha);
  routineDefinitions = RoutineDefinitionRuntime::load(base,catalog,sha);
  npcDefinitions = NpcDefinitionRuntime::load(base,catalog,sha);
  npcDefinitions.helpers = NpcHelperRuntime::load(base,catalog,sha);
  activityDefinitions = ActivityDefinitionRuntime::load(base,catalog,sha);
  const auto* callback = NativeBaseCatalog::findIdentity(catalog.npcCallbacks,"B_ASSESSQUIETSOUND");
  if(callback==nullptr)
    throw BaseStore::Error("missing native quiet sound callback");
  quietSound = {ScriptFn(callback->id),intConstant("PERC_DIST_HEIGHT").value(),intConstant("PERC_DIST_INDOOR_HEIGHT").value()};
  nativeRoutineProgram = true;
  }

int32_t GameScript::itemRuntimeInt(std::string_view name) const {
  const auto value=globalIntSymbol(name);
  if(!value)
    throw std::runtime_error("missing item runtime integer "+std::string(name));
  return *value;
  }

void GameScript::setItemRuntimeInt(std::string_view name, int32_t value) {
  if(!setGlobalIntSymbol(name,value))
    throw std::runtime_error("missing mutable item runtime integer "+std::string(name));
  }

std::string_view GameScript::itemRuntimeText(std::string_view name) const {
  if(const auto* field=std::get_if<ScriptVars::Array<std::string>>(runtimeState.find(name)))
    return field->values.front();
  if(const auto* constant=NativeBaseCatalog::findConstant(*baseNativeCatalog(),name))
    if(const auto* values=std::get_if<std::vector<std::string>>(&constant->values))
      return values->front();
  throw std::runtime_error("missing item runtime text "+std::string(name));
  }

void GameScript::setItemRuntimeText(std::string_view name, std::string_view value) {
  if(auto* field=std::get_if<ScriptVars::Array<std::string>>(runtimeState.find(name))) {
    field->values.front()=value;
    return;
    }
  throw std::runtime_error("missing mutable item runtime text "+std::string(name));
  }

auto GameScript::createItemForQuery(size_t instance) -> std::shared_ptr<ItemRecord> {
  auto item=std::make_shared<daedalus::IItem>();
  if(instance>UINT32_MAX)
    throw std::runtime_error("item archetype is out of range");
  const auto id=uint32_t(instance);
  if(id==itemDefinitions.brokenEye) {
    static_cast<ItemRecord&>(*item)=brokenEyePrepared;
    item->text[0]=itemRuntimeText("TEXT_INNOSEYE_SETTING");
    item->text[1]=itemRuntimeText("TEXT_INNOSEYE_GEM");
    } else {
    static_cast<ItemRecord&>(*item)=itemDefinitions.items.at(id);
    }
  item->setRecordIdentity(id);
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
  const auto& calls=baseNativeCatalog()->npcConstructor.calls;
  const auto visual=calls.find("B_SETNPCVISUAL");
  return visual!=calls.end() && npcDefinitions.helpers.helpers.contains(visual->second.id);
  }

void GameScript::setupDirectorNpcEquipment(Npc& npc, const DirectorNpcLook* look,
                                          std::span<const std::pair<uint32_t,int32_t>> items) {
  struct RestoreSelf {
    ScriptVars::Object& slot;
    ScriptVars::Object previous;
    ~RestoreSelf() { slot=std::move(previous); }
    };
  const auto actor=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  auto& slot=storyActorSlot("SELF");
  const RestoreSelf restore{slot,slot};
  slot=actor;

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
    const auto id=baseNativeCatalog()->npcConstructor.calls.at("B_SETNPCVISUAL").id;
    invokeNpcHelper(actor,id,arguments);
    }
  for(const auto& [item,count] : items)
    createinvitems(actor,int32_t(item),count);
  }

void GameScript::bindPreparedInventory() {
  bindRuntimeVariable("B_GIVEINVITEMS.AMOUNT",inventoryTransferVariables.amount);
  bindRuntimeVariable("B_GIVEINVITEMS.ITEMINSTANCE",inventoryTransferVariables.item);
  bindRuntimeVariable("B_GIVEINVITEMS.ITEMNAME",inventoryTransferVariables.itemName);
  bindRuntimeVariable("B_GIVEINVITEMS.CONCATTEXT",inventoryTransferVariables.notice);
  const auto* catalog=baseNativeCatalog();
  if(itemDefinitions.brokenEye) {
    ItemRecord eye{};
    eye.name=itemRuntimeText("ITEM_BROKEN_EYE_NAME");
    eye.main_flag=itemRuntimeInt("ITEM_KAT_MAGIC");
    eye.flags=ItemFlag::AMULET | ItemFlag::MISSION;
    eye.visual=itemRuntimeText("ITEM_BROKEN_EYE_VISUAL");
    eye.material=itemRuntimeInt("MAT_STONE");
    eye.description=eye.name;
    const auto* equip=NativeBaseCatalog::findIdentity(catalog->itemCallbacks,"EQUIP_ITMI_INNOSEYE_BROKEN_MIS");
    if(equip==nullptr)
      throw std::runtime_error("missing broken Eye of Innos equipment effect");
    eye.on_equip=int32_t(equip->id);
    eye.inv_zbias=itemRuntimeInt("INVCAM_ENTF_AMULETTE_STANDARD");
    brokenEyePrepared=std::move(eye);
    }
  inventoryTransferText = {
    findItemInstance("ITMI_GOLD").value(),
    itemRuntimeText("PRINT_GOLDGEGEBEN"),itemRuntimeText("PRINT_GOLDERHALTEN"),
    itemRuntimeText("PRINT_ADDON_GEGEBEN"),itemRuntimeText("PRINT_ADDON_ERHALTEN"),
    itemRuntimeText("PRINT_ITEMSGEGEBEN"),itemRuntimeText("PRINT_ITEMSERHALTEN"),
    itemRuntimeInt("YPOS_GOLDGIVEN"),itemRuntimeInt("YPOS_GOLDTAKEN"),
    itemRuntimeInt("YPOS_ITEMGIVEN"),itemRuntimeInt("YPOS_ITEMTAKEN")
    };
  inventoryTransferFont=itemRuntimeText("FONT_SCREENSMALL");

  }

bool GameScript::bindPreparedItem(const std::shared_ptr<daedalus::IItem>& item, uint32_t instance) {
  auto* alias=ownedInstance(instance);
  if(alias==nullptr || (!itemDefinitions.items.contains(instance) && itemDefinitions.brokenEye!=instance))
    throw std::runtime_error("missing prepared item archetype "+std::to_string(instance));
  item->setRecordIdentity(instance);
  *alias=item;
  return true;
  }

void GameScript::initializeInstanceItem(const std::shared_ptr<daedalus::IItem>& item, size_t instance) {
  LoadProbe::Vobs::Scope probe("script_item");
  LoadProbe::Startup::Scope itemProbe(LoadProbe::Startup::Script,false,"placed-items");
  if(instance>UINT32_MAX)
    throw std::runtime_error("item archetype is out of range");
  const auto id=uint32_t(instance);
  bindPreparedItem(item,id);
  if(itemDefinitions.brokenEye==id) {
    static_cast<ItemRecord&>(*item)=brokenEyePrepared;
    item->text[0]=itemRuntimeText("TEXT_INNOSEYE_SETTING");
    item->text[1]=itemRuntimeText("TEXT_INNOSEYE_GEM");
    } else {
    static_cast<ItemRecord&>(*item)=itemDefinitions.items.at(id);
    }
  }

bool GameScript::invokeStockItemProgram(uint32_t function) {
  if(!itemDefinitions.itemUsePrograms.contains(function))
    return false;

  if(const auto it=itemDefinitions.documents.find(function); it!=itemDefinitions.documents.end()) {
    using storygraph::NativeValue;
    const auto& document=it->second;
    auto* slot=runtimeState.find(document.handle);
    auto* handle=slot ? std::get_if<ScriptVars::Array<int32_t>>(slot) : nullptr;
    if(handle==nullptr || handle->values.size()!=1)
      throw std::runtime_error("missing runtime handle for an item document");

    const auto invoke=[&](std::string_view name,std::span<const NativeValue> arguments) {
      const auto action=nativeExternals.find(name);
      if(action==nativeExternals.end())
        throw std::runtime_error("missing engine action for an item document: "+std::string(name));
      return action->second.call(arguments,{},{});
      };
    const auto id=invoke(document.map ? "DOC_CREATEMAP" : "DOC_CREATE",{}).value.i;
    handle->values[0]=id;
    std::vector<NativeValue> arguments{NativeValue::integer(id)};
    for(const auto& call:document.calls) {
      arguments.resize(1);
      for(const auto& arg:call.arguments) {
        if(const auto* str=std::get_if<std::string>(&arg))
          arguments.push_back(NativeValue::str(*str));
        else
          arguments.push_back(NativeValue::integer(std::get<int32_t>(arg)));
        }
      invoke(call.function,arguments);
      }
    arguments.resize(1);
    invoke("DOC_SHOW",arguments);
    return true;
    }

  int32_t result=0;
  bool failed=false;
  const auto ran=invokeStockProgram(function,result,&failed);
  if(failed)
    throw std::runtime_error("prepared item effect failed "+std::to_string(function));
  return ran;
  }
