#include "itemdefinitionruntime.h"
#include "game/definitions/nativebasecatalog.h"
#include "itemrecordfields.h"
#include "itemloadprobe.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>

namespace {
template<class T, class Lookup>
void resolveField(T& value, const std::vector<std::string>& names, size_t& index, const Lookup& lookup) {
  if constexpr(std::is_array_v<T>) {
    for(auto& element:value) resolveField(element,names,index,lookup);
    }
  else if constexpr(!std::is_same_v<T,std::string>) {
    const auto& name=names.at(index++);
    if(name.empty()) return;
    const auto id=lookup(name);
    if(!id || *id>INT32_MAX) throw std::runtime_error("unknown or invalid identity "+name);
    value=T(*id);
    }
  }
template<class FindItem, class FindReference>
ItemDefinitionRuntime::Definitions resolveRecords(const ItemDefinitions::Library& library, const FindItem& findItem,
                                                   const FindReference& findReference) {
  ItemDefinitionRuntime::Definitions definitions;
  for(const auto& [name,definition]:library.items) {
    try {
      const auto id=findItem(name);
      if(!id) throw std::runtime_error("unknown item identity");
      ItemRecord record=definition.record;
      itemRecordFields(record,[&](const char* field, auto& value, bool function) {
        if(!function) return;
        try {
          size_t index=0;
          const auto& names=definition.symbols.at(field);
          resolveField(value,names,index,[&](std::string_view name) { return findReference(field,name); });
          if(index!=names.size()) throw std::runtime_error("symbol array extent differs");
          }
        catch(const std::exception& e) { throw std::runtime_error(std::string(field)+": "+e.what()); }
        });
      if(record.on_state[0]>0)
        definitions.itemUsePrograms.insert(uint32_t(record.on_state[0]));
      if(!definitions.items.emplace(*id,std::move(record)).second)
        throw std::runtime_error("duplicate item identity");
      }
    catch(const std::exception& e) { throw std::runtime_error("item definitions: "+name+": "+e.what()); }
    }
  return definitions;
  }
}

ItemDefinitionRuntime::Definitions ItemDefinitionRuntime::resolve(const ItemDefinitions::Library& library,
                                                                  const daedalus::DaedalusScript& script) {
  const auto lookup=[&](std::string_view name) -> std::optional<uint32_t> {
    const auto* symbol=script.find_symbol_by_name(name);
    return symbol!=nullptr ? std::optional(symbol->index()) : std::nullopt;
    };
  auto definitions=resolveRecords(library,lookup,[&](std::string_view,std::string_view name) { return lookup(name); });
  for(const auto& [name,document]:library.documents) {
    const auto* root=script.find_symbol_by_name(name);
    const auto* handle=script.find_symbol_by_name(document.handle);
    using Type=daedalus::DaedalusDataType;
    if(root==nullptr || !definitions.itemUsePrograms.contains(root->index()) || handle==nullptr ||
       handle->name()!=root->name()+".NDOCID" || handle->type()!=Type::INT || handle->count()!=1 ||
       handle->is_const() || handle->is_member())
      throw std::runtime_error("invalid item document "+name);
    Document resolved{handle->index(),document.map,{}};
    for(const auto& call:document.calls) {
      const auto* external=script.find_symbol_by_name(call.function);
      if(external==nullptr || !external->is_external() || external->count()!=call.arguments.size()+1 || external->has_return())
        throw std::runtime_error("invalid document action "+call.function);
      resolved.calls.push_back({call.function,call.arguments});
      }
    definitions.documents.emplace(root->index(),std::move(resolved));
    }
  return definitions;
  }

ItemDefinitionRuntime::Definitions ItemDefinitionRuntime::resolveNative(const ItemDefinitions::Library& library,
                                                                        const NativeBaseCatalog::Selection& catalog) {
  const auto index=[](const std::vector<NativeBaseCatalog::Identity>& rows) {
    std::unordered_map<std::string_view,uint32_t> result;
    result.reserve(rows.size());
    for(const auto& [id,name] : rows)
      result.emplace(name,id);
    return result;
    };
  const auto items=index(catalog.identities.items);
  const auto owners=index(catalog.identities.npcs);
  const auto callbacks=index(catalog.itemCallbacks);
  const auto lookup=[](const auto& map,std::string_view name) -> std::optional<uint32_t> {
    const auto it=map.find(name);
    return it!=map.end() ? std::optional(it->second) : std::nullopt;
    };
  auto definitions=resolveRecords(library,[&](std::string_view name) { return lookup(items,name); },
    [&](std::string_view field,std::string_view name) { return lookup(field=="owner" ? owners : callbacks,name); });
  for(const auto& [name,document] : library.documents) {
    const auto root=lookup(callbacks,name);
    const auto handle=std::ranges::find(catalog.initialState,document.handle,&NativeBaseCatalog::InitialRow::name);
    if(!root || !definitions.itemUsePrograms.contains(*root) || handle==catalog.initialState.end() ||
       document.handle!=name+".NDOCID")
      throw std::runtime_error("invalid item document "+name);
    const auto* values=std::get_if<std::vector<int32_t>>(&handle->value);
    if(values==nullptr || values->size()!=1)
      throw std::runtime_error("invalid item document handle "+document.handle);
    Document resolved{handle->id,document.map,{}};
    for(const auto& call : document.calls)
      resolved.calls.push_back({call.function,call.arguments});
    definitions.documents.emplace(*root,std::move(resolved));
    }
  for(const auto& [name,reason] : library.uncaptured) {
    if(name!="ITMI_INNOSEYE_BROKEN_MIS")
      throw std::runtime_error("item definitions: no native constructor for "+name+": "+reason);
    definitions.brokenEye=lookup(items,name);
    if(!definitions.brokenEye || !callbacks.contains("EQUIP_ITMI_INNOSEYE_BROKEN_MIS"))
      throw std::runtime_error("item definitions: missing identity for the broken Eye of Innos");
    }
  return definitions;
  }

ItemDefinitionRuntime::Definitions ItemDefinitionRuntime::loadNative(const std::filesystem::path& base,
                                                                     const NativeBaseCatalog::Selection& catalog,
                                                                     std::string_view sha256) {
  auto definitions=resolveNative(ItemDefinitions::read(base,sha256),catalog);
  registerStockEffects(definitions,catalog);
  return definitions;
  }

ItemDefinitionRuntime::Definitions ItemDefinitionRuntime::load(const std::filesystem::path& base,
                                                               const daedalus::DaedalusScript& program,
                                                               std::string_view sha256) {
  const auto* enabled=std::getenv("OPENGOTHIC_ITEM_RECORDS");
  if(enabled==nullptr || std::strcmp(enabled,"0")!=0) {
    return resolve(ItemDefinitions::read(base,sha256),program);
    }
  return {};
  }

bool ItemDefinitionRuntime::initialize(daedalus::DaedalusVm& vm, const std::shared_ptr<daedalus::IItem>& item,
                                       daedalus::DaedalusSymbol* symbol, const Definitions& definitions) {
  if(symbol!=nullptr) {
    const auto found=definitions.items.find(symbol->index());
    if(found!=definitions.items.end()) {
      // allocate_instance binds the symbol and type. Constructor calls restore SELF,
      // context, PC and stack; pure item constructors leave no other VM state behind.
      vm.allocate_instance(item,symbol);
      static_cast<ItemRecord&>(*item)=found->second;
      return true;
      }
    if(definitions.brokenEye==symbol->index()) {
      // This constructor samples the current language/story setting, including on each recreation.
      // Its two generated literals are bound by the verified stock program fingerprint.
      const auto get=[&](std::string_view name) { return vm.find_symbol_by_name(name); };
      vm.allocate_instance(item,symbol);
      item->name=get("$15687")->get_string();
      item->main_flag=get("ITEM_KAT_MAGIC")->get_int();
      item->flags=ItemFlag(get("ITEM_AMULET")->get_int() | get("ITEM_MISSION")->get_int());
      item->value=0;
      item->visual=get("$15688")->get_string();
      item->material=get("MAT_STONE")->get_int();
      item->description=item->name;
      item->on_equip=int32_t(get("EQUIP_ITMI_INNOSEYE_BROKEN_MIS")->index());
      item->text[0]=get("TEXT_INNOSEYE_SETTING")->get_string();
      item->text[1]=get("TEXT_INNOSEYE_GEM")->get_string();
      item->inv_zbias=get("INVCAM_ENTF_AMULETTE_STANDARD")->get_int();
      return true;
      }
    }
  LoadProbe::Items::Constructor probe;
  vm.init_instance(item,symbol);
  return false;
  }

namespace {

using namespace ItemDefinitionRuntime;
using RestoreAmount = decltype(RestoreChange::amount);

struct StockEffectBindings {
  std::function<uint32_t(std::string_view)> callback;
  std::function<uint32_t(std::string_view)> flag;
  std::function<int32_t(std::string_view)> constant;
  std::function<RestoreAmount(std::string_view)> dose;
  };

void registerRestoreEffects(Definitions& definitions, const StockEffectBindings& bindings) {
  // These callbacks make one or two constant changes to the current attributes of SELF.
  struct StockRestore {
    std::string_view callback;
    Attribute attribute;
    std::string_view amount;
    Attribute secondAttribute=ATR_HITPOINTS;
    std::string_view secondAmount={};
    };
  static constexpr StockRestore stock[] = {
    {"USE_SHELLFLESH",       ATR_HITPOINTS,  "HP_SHELLFLESH"},
    {"USERUM",               ATR_MANA,       "MANA_RUM"},
    {"USEGROG",              ATR_HITPOINTS,  "HP_GROG"},
    {"USE_CHEESE",           ATR_HITPOINTS,  "HP_CHEESE"},
    {"USE_BACON",            ATR_HITPOINTS,  "HP_BACON"},
    {"USE_BREAD",            ATR_HITPOINTS,  "HP_BREAD"},
    {"USE_FISH",             ATR_HITPOINTS,  "HP_FISH"},
    {"USE_RAWMEAT",          ATR_HITPOINTS,  "HP_RAWMEAT"},
    {"USE_MEAT",             ATR_HITPOINTS,  "HP_MEAT"},
    {"USE_STEW",             ATR_HITPOINTS,  "HP_STEW"},
    {"USE_FISHSOUP",         ATR_HITPOINTS,  "HP_FISHSOUP"},
    {"USE_SAUSAGE",          ATR_HITPOINTS,  "HP_SAUSAGE"},
    {"USE_HONEY",            ATR_HITPOINTS,  "HP_HONEY"},
    {"USE_WATER",            ATR_HITPOINTS,  "HP_WATER"},
    {"USE_BEET",             ATR_HITPOINTS,  "HP_BEET"},
    {"USE_MANA_HERB_01",     ATR_MANA,       "MANA_MANA_HERB_01"},
    {"USE_MANA_HERB_02",     ATR_MANA,       "MANA_MANA_HERB_02"},
    {"USE_MANA_HERB_03",     ATR_MANA,       "MANA_MANA_HERB_03"},
    {"USE_HEALTH_HERB_01",   ATR_HITPOINTS,  "HP_HEALTH_HERB_01"},
    {"USE_HEALTH_HERB_02",   ATR_HITPOINTS,  "HP_HEALTH_HERB_02"},
    {"USE_HEALTH_HERB_03",   ATR_HITPOINTS,  "HP_HEALTH_HERB_03"},
    {"USE_MUSHROOM_02",      ATR_HITPOINTS,  "HP_MUSHROOM_02"},
    {"USE_FORESTBERRY",      ATR_HITPOINTS,  "HP_FORESTBERRY"},
    {"USE_PLANEBERRY",       ATR_HITPOINTS,  "HP_PLANEBERRY"},
    {"USE_TEMP_HERB",        ATR_HITPOINTS,  "HP_TEMP_HERB"},
    {"USE_PERM_HERB",        ATR_HITPOINTS,  "HP_PERM_HERB"},
    {"USEITPO_MANA_01",      ATR_MANA,       "MANA_ESSENZ"},
    {"USEITPO_MANA_02",      ATR_MANA,       "MANA_EXTRAKT"},
    {"USEITPO_MANA_03",      ATR_MANA,       "MANA_ELIXIER"},
    {"USEITPO_HEALTH_01",    ATR_HITPOINTS,  "HP_ESSENZ"},
    {"USEITPO_HEALTH_02",    ATR_HITPOINTS,  "HP_EXTRAKT"},
    {"USEITPO_HEALTH_03",    ATR_HITPOINTS,  "HP_ELIXIER"},
    {"USE_SMELLYFISH",       ATR_HITPOINTS,  "HP_HERING"},
    {"USE_SCHAFSWURST",      ATR_HITPOINTS,  "HP_SAUSAGE"},
    {"USE_HEALHILDA",        ATR_HITPOINTS,  "HP_ESSENZ"},
    {"USE_HEALRANDOLPH",     ATR_HITPOINTS,  "HP_ESSENZ"},
    {"USE_BEER",             ATR_HITPOINTS,  "HP_BEER",      ATR_MANA,      "MANA_BEER"},
    {"USE_BOOZE",            ATR_HITPOINTS,  "HP_BOOZE",     ATR_MANA,      "MANA_BOOZE"},
    {"USE_WINE",             ATR_HITPOINTS,  "HP_WINE",      ATR_MANA,      "MANA_WINE"},
    {"USE_MILK",             ATR_HITPOINTS,  "HP_MILK",      ATR_MANA,      "MANA_MILK"},
    {"USE_BLUEPLANT",        ATR_MANA,       "MANA_BLUEPLANT", ATR_HITPOINTS, "HP_BLUEPLANT"},
    };
  for(const auto& row : stock) {
    const auto function = bindings.callback(row.callback);
    RestoreEffect effect{{row.attribute,bindings.dose(row.amount)},{}};
    if(!row.secondAmount.empty())
      effect.second=RestoreChange{row.secondAttribute,bindings.dose(row.secondAmount)};
    if(!definitions.restoreEffects.emplace(function,effect).second)
      throw std::runtime_error("stock item restore: duplicate callback "+std::string(row.callback));
    }

  if(!definitions.restoreEffects.emplace(bindings.callback("USE_MEATBUGFLESH"),
                                          RestoreEffect{{ATR_HITPOINTS,int32_t(10)}, {}}).second)
    throw std::runtime_error("stock item restore: duplicate USE_MEATBUGFLESH callback");

  // Full elixirs assign the corresponding maximum directly to the current attribute.
  static constexpr struct StockFullRestore {
    std::string_view callback;
    FullRestoreEffect effect;
    } full[] = {
    {"USEITPO_HEALTH_04", {ATR_HITPOINTS, ATR_HITPOINTSMAX}},
    {"USEITPO_MANA_04",   {ATR_MANA,      ATR_MANAMAX}},
    };
  for(const auto& [callback,effect]:full) {
    if(!definitions.fullRestoreEffects.emplace(bindings.callback(callback),effect).second)
      throw std::runtime_error("stock item full restore: invalid callback "+std::string(callback));
    }

  static constexpr struct StockPermanentAttribute {
    std::string_view callback;
    Attribute attribute;
    std::variant<std::string_view,int32_t> dose;
    std::optional<Attribute> refill = {};
    } permanent[] = {
    {"USEITPO_PERM_STR",     ATR_STRENGTH,     "STR_ELIXIER"},
    {"USEITPO_PERM_DEX",     ATR_DEXTERITY,    "DEX_ELIXIER"},
    {"USEITPO_PERM_HEALTH",  ATR_HITPOINTSMAX, "HPMAX_ELIXIER",   ATR_HITPOINTS},
    {"USEITPO_PERM_MANA",    ATR_MANAMAX,      "MANAMAX_ELIXIER", ATR_MANA},
    {"USEITPO_LITTLEMANA",   ATR_MANAMAX,      int32_t(3),        ATR_MANA},
    {"USE_STRENGTH_HERB_01", ATR_STRENGTH,     int32_t(1)},
    {"USE_DEX_HERB_01",      ATR_DEXTERITY,    int32_t(1)},
    };
  for(const auto& [callback,attribute,dose,refill]:permanent) {
    const auto* name=std::get_if<std::string_view>(&dose);
    const auto amount=name ? bindings.dose(*name) : RestoreAmount(std::get<int32_t>(dose));
    PermanentAttributeEffect effect{{attribute,amount},refill};
    if(!definitions.permanentAttributeEffects.emplace(bindings.callback(callback),effect).second)
      throw std::runtime_error("stock permanent item attribute: invalid callback "+std::string(callback));
    }
  }

void registerArmorSetEffects(Definitions& definitions, const StockEffectBindings& bindings) {
  struct StockArmorSet {
    std::string_view item;
    std::string_view armorFlag;
    std::string_view beltFlag;
    };
  static constexpr StockArmorSet stock[] = {
    {"ITAR_MIL_L",       "MILARMOR_EQUIPPED",     "MIL01_EQUIPPED"},
    {"ITAR_MIL_M",       "MILARMOR_EQUIPPED",     "MIL01_EQUIPPED"},
    {"ITAR_SLD_L",       "SLDARMOR_EQUIPPED",     "SLD01_EQUIPPED"},
    {"ITAR_SLD_M",       "SLDARMOR_EQUIPPED",     "SLD01_EQUIPPED"},
    {"ITAR_SLD_H",       "SLDARMOR_EQUIPPED",     "SLD01_EQUIPPED"},
    {"ITAR_DJG_CRAWLER", "MCARMOR_EQUIPPED",      "MC_EQUIPPED"},
    {"ITAR_NOV_L",       "NOVARMOR_EQUIPPED",     "NOV01_EQUIPPED"},
    {"ITAR_KDF_L",       "KDFARMOR_EQUIPPED",     "KDF01_EQUIPPED"},
    {"ITAR_KDF_H",       "KDFARMOR_EQUIPPED",     "KDF01_EQUIPPED"},
    {"ITAR_LEATHER_L",   "LEATHERARMOR_EQUIPPED", "LEATHER01_EQUIPPED"},
    };
  for(const auto& [item,armor,belt] : stock) {
    const auto armorFlag = bindings.flag(armor);
    const auto beltFlag = bindings.flag(belt);
    for(bool equip : {true,false}) {
      const auto callback = std::string(equip ? "EQUIP_" : "UNEQUIP_").append(item);
      definitions.armorSetEffects.emplace(bindings.callback(callback),ArmorSetEffect{armorFlag,beltFlag,equip});
      }
    }
  }

void registerFightSkillEffects(Definitions& definitions, const StockEffectBindings& bindings) {
  const auto bind = [&](std::string_view suffix, Talent talent, int32_t bonus, bool playerOnly) {
    for(bool equip : {true,false}) {
      const auto name = std::string(equip ? "EQUIP_" : "UNEQUIP_").append(suffix);
      definitions.fightSkillEffects.emplace(bindings.callback(name),FightSkillEffect{talent,equip ? bonus : -bonus,playerOnly});
      }
    };
  const auto bindWeapons = [&](std::string_view hand, Talent talent, std::initializer_list<int> bonuses) {
    for(const auto bonus : bonuses) {
      const auto suffix = (bonus<10 ? "0" : "")+std::to_string(bonus);
      bind(std::string(hand).append(suffix),talent,bindings.constant("WAFFENBONUS_"+suffix),true);
      }
    };
  // The remaining generic tiers have no stock item bindings.
  bindWeapons("1H_",TALENT_1H,{2,3,5,6,7,8,10});
  bindWeapons("2H_",TALENT_2H,{2,3,4,5,10});
  bind("ALRIKSSWORD",TALENT_1H,10,false);
  bind("MORGANSRING",TALENT_1H,10,false);
  }

void registerEquipmentStatEffects(Definitions& definitions, const StockEffectBindings& bindings) {
  constexpr EquipmentAttributeWrite hp{ATR_HITPOINTS,2}, maxHp{ATR_HITPOINTSMAX};
  constexpr EquipmentAttributeWrite mana{ATR_MANA,0}, maxMana{ATR_MANAMAX};
  constexpr EquipmentAttributeWrite strength{ATR_STRENGTH}, dexterity{ATR_DEXTERITY};
  struct Change {
    decltype(EquipmentStatChange::stat) stat;
    std::string_view amount;
    };
  static constexpr struct Pair {
    std::string_view callback;
    std::initializer_list<Change> changes;
    } stock[] = {
    {"ITAM_PROT_EDGE_01", {{PROT_EDGE, "AM_PROTEDGE"}, {PROT_BLUNT, "AM_PROTEDGE"}}},
    {"ITAM_PROT_FIRE_01", {{PROT_FIRE, "AM_PROTFIRE"}}},
    {"ITAM_PROT_MAGE_01", {{PROT_MAGIC, "AM_PROTMAGE"}}},
    {"ITAM_PROT_POINT_01", {{PROT_POINT, "AM_PROTPOINT"}}},
    {"VALUE_AM_PROTTOTAL",
     {{PROT_EDGE, "AM_TPROTEDGE"}, {PROT_BLUNT, "AM_TPROTEDGE"}, {PROT_POINT, "AM_TPROTPOINT"},
      {PROT_FIRE, "AM_TPROTFIRE"}, {PROT_MAGIC, "AM_TPROTMAGE"}}},
    {"ITBE_ADDON_PROT_EDGE", {{PROT_EDGE, "BELTBONUS_PROTEDGE"}, {PROT_BLUNT, "BELTBONUS_PROTEDGE"}}},
    {"ITBE_ADDON_PROT_EDGPOI",
     {{PROT_EDGE, "BELTBONUS_PROTEDGPOI"}, {PROT_BLUNT, "BELTBONUS_PROTEDGPOI"}, {PROT_POINT, "BELTBONUS_PROTEDGPOI"}}},
    {"ITBE_ADDON_PROT_FIRE", {{PROT_FIRE, "BELTBONUS_PROTFIRE"}}},
    {"ITBE_ADDON_PROT_MAGIC", {{PROT_MAGIC, "BELTBONUS_PROTMAGIC"}}},
    {"ITBE_ADDON_PROT_POINT", {{PROT_POINT, "BELTBONUS_PROTPOINT"}}},
    {"ITBE_ADDON_PROT_TOTAL",
     {{PROT_EDGE, "BELTBONUS_PROTTOTAL"}, {PROT_BLUNT, "BELTBONUS_PROTTOTAL"}, {PROT_POINT, "BELTBONUS_PROTTOTAL"},
      {PROT_MAGIC, "BELTBONUS_PROTTOTAL"}, {PROT_FIRE, "BELTBONUS_PROTTOTAL"}}},
    {"ITRI_PROT_EDGE_01", {{PROT_EDGE, "RI_PROTEDGE"}, {PROT_BLUNT, "RI_PROTEDGE"}}},
    {"ITRI_PROT_EDGE_02", {{PROT_EDGE, "RI_PROTEDGE02"}, {PROT_BLUNT, "RI_PROTEDGE02"}}},
    {"ITRI_PROT_FIRE_01", {{PROT_FIRE, "RI_PROTFIRE"}}},
    {"ITRI_PROT_FIRE_02", {{PROT_FIRE, "RI_PROTFIRE02"}}},
    {"ITRI_PROT_MAGE_01", {{PROT_MAGIC, "RI_PROTMAGE"}}},
    {"ITRI_PROT_MAGE_02", {{PROT_MAGIC, "RI_PROTMAGE02"}}},
    {"ITRI_PROT_POINT_01", {{PROT_POINT, "RI_PROTPOINT"}}},
    {"ITRI_PROT_POINT_01_MIS", {{PROT_POINT, "RI_PROTPOINT"}}},
    {"ITRI_PROT_POINT_02", {{PROT_POINT, "RI_PROTPOINT02"}}},
    {"ITRI_PROT_TOTAL_01",
     {{PROT_EDGE, "RI_TPROTEDGE"}, {PROT_BLUNT, "RI_TPROTEDGE"}, {PROT_POINT, "RI_TPROTPOINT"},
      {PROT_FIRE, "RI_TPROTFIRE"}, {PROT_MAGIC, "RI_TPROTMAGE"}}},
    {"ITRI_PROT_TOTAL_02",
     {{PROT_EDGE, "RI_TPROTEDGE02"}, {PROT_BLUNT, "RI_TPROTEDGE02"}, {PROT_POINT, "RI_TPROTPOINT02"},
      {PROT_FIRE, "RI_TPROTFIRE02"}, {PROT_MAGIC, "RI_TPROTMAGE02"}}},
    {"VALENTINOSRING", {{PROT_EDGE, "RI_PROTEDGE"}, {PROT_BLUNT, "RI_PROTEDGE"}}},
    {"ITAM_DEX_01", {{ATR_DEXTERITY, "AM_DEX"}}},
    {"ITAM_DEX_STRG_01", {{ATR_DEXTERITY, "AM_DEXSTRG_DEX"}, {ATR_STRENGTH, "AM_DEXSTRG_STRG"}}},
    {"ITAM_STRG_01", {{ATR_STRENGTH, "AM_STRG"}}},
    {"ITBE_ADDON_DEX_10", {{ATR_DEXTERITY, "BELTBONUS_DEX02"}}},
    {"ITBE_ADDON_DEX_5", {{ATR_DEXTERITY, "BELTBONUS_DEX01"}}},
    {"ITBE_ADDON_STR_10", {{ATR_STRENGTH, "BELTBONUS_STR02"}}},
    {"ITBE_ADDON_STR_5", {{ATR_STRENGTH, "BELTBONUS_STR01"}}},
    {"ITRI_DEX_01", {{ATR_DEXTERITY, "RI_DEX"}}},
    {"ITRI_DEX_02", {{ATR_DEXTERITY, "RI_DEX02"}}},
    {"ITRI_DEX_STRG_01", {{ATR_STRENGTH, "RI_DEXSTRG_STRG"}, {ATR_DEXTERITY, "RI_DEXSTRG_DEX"}}},
    {"ITRI_STR_01", {{ATR_STRENGTH, "RI_STRG"}}},
    {"ITRI_STR_02", {{ATR_STRENGTH, "RI_STRG02"}}},
    {"ITAM_ADDON_FRANCO", {{strength,"STR_FRANCO"}, {dexterity,"DEX_FRANCO"},
                           {maxHp,"HP_ITAM_ADDON_FRANCO"}, {hp,"HP_ITAM_ADDON_FRANCO"}}},
    {"ITAM_HP_01", {{maxHp,"AM_HP"}, {hp,"AM_HP"}}},
    {"ITAM_MANA_01", {{maxMana,"AM_MANA"}, {mana,"AM_MANA"}}},
    {"ITAM_MANA_ANGAR", {{maxMana,"AM_MANA"}, {mana,"AM_MANA"}}},
    {"ITRI_HP_01", {{maxHp,"RI_HP"}, {hp,"RI_HP"}}},
    {"ITRI_HP_02", {{maxHp,"RI_HP02"}, {hp,"RI_HP02"}}},
    {"ITRI_MANA_01", {{maxMana,"RI_MANA"}, {mana,"RI_MANA"}}},
    {"ITRI_MANA_02", {{maxMana,"RI_MANA02"}, {mana,"RI_MANA02"}}},
    {"ITRI_TENGRON", {{maxHp,"RI_HP"}, {hp,"RI_HP"}}},
    };
  const auto bind = [&](std::string_view callback,bool equip,std::initializer_list<Change> source) {
    std::vector<EquipmentStatChange> changes;
    changes.reserve(source.size());
    for(const auto& [stat,amount] : source)
      changes.push_back({stat,bindings.constant(amount)});
    const auto name = std::string(equip ? "EQUIP_" : "UNEQUIP_").append(callback);
    if(!definitions.equipmentStatEffects.emplace(bindings.callback(name),EquipmentStatEffect{std::move(changes),equip}).second)
      throw std::runtime_error("stock equipment stat: invalid callback "+name);
    };
  for(const auto& row : stock)
    for(bool equip : {true,false})
      bind(row.callback,equip,row.changes);
  bind("ITAM_HP_MANA_01",true, {{hp,"AM_HPMANA_HP"}, {maxHp,"AM_HPMANA_HP"},
                               {mana,"AM_HPMANA_MANA"}, {maxMana,"AM_HPMANA_MANA"}});
  bind("ITAM_HP_MANA_01",false,{{maxMana,"AM_HPMANA_MANA"}, {maxHp,"AM_HPMANA_HP"},
                               {hp,"AM_HPMANA_HP"}, {mana,"AM_HPMANA_MANA"}});
  bind("ITRI_HP_MANA_01",true, {{mana,"RI_HPMANA_MANA"}, {maxMana,"RI_HPMANA_MANA"},
                               {hp,"RI_HPMANA_HP"}, {maxHp,"RI_HPMANA_HP"}});
  // The stock ring subtracts its health bonus from current mana when removed.
  bind("ITRI_HP_MANA_01",false,{{maxMana,"RI_HPMANA_MANA"}, {maxHp,"RI_HPMANA_HP"},
                               {hp,"RI_HPMANA_HP"}, {mana,"RI_HPMANA_HP"}});
  }


void bindStockEffects(Definitions& definitions, const StockEffectBindings& bindings) {
  registerRestoreEffects(definitions,bindings);
  registerArmorSetEffects(definitions,bindings);
  registerFightSkillEffects(definitions,bindings);
  registerEquipmentStatEffects(definitions,bindings);
  }

}

void ItemDefinitionRuntime::registerStockEffects(Definitions& definitions, const daedalus::DaedalusScript& program) {
  const auto symbol=[&](std::string_view name) -> const daedalus::DaedalusSymbol& {
    if(const auto* value=program.find_symbol_by_name(name))
      return *value;
    throw std::runtime_error("missing stock item effect symbol "+std::string(name));
    };
  const auto id=[&](std::string_view name) { return symbol(name).index(); };
  const auto integer=[&](std::string_view name) {
    const auto& value=symbol(name);
    if(value.type()!=daedalus::DaedalusDataType::INT || !value.is_const())
      throw std::runtime_error("invalid stock item constant "+std::string(name));
    return value.get_int();
    };
  bindStockEffects(definitions,{id,id,integer,id});
  }

void ItemDefinitionRuntime::registerStockEffects(Definitions& definitions, const NativeBaseCatalog::Selection& catalog) {
  const auto callback=[&](std::string_view name) {
    if(const auto* value=NativeBaseCatalog::findIdentity(catalog.itemCallbacks,name))
      return value->id;
    throw std::runtime_error("missing native item effect callback "+std::string(name));
    };
  const auto flag=[&](std::string_view name) {
    const auto row=std::ranges::find(catalog.initialState,name,&NativeBaseCatalog::InitialRow::name);
    if(row!=catalog.initialState.end())
      if(const auto* values=std::get_if<std::vector<int32_t>>(&row->value); values!=nullptr && values->size()==1)
        return row->id;
    throw std::runtime_error("missing native item effect state "+std::string(name));
    };
  const auto constant=[&](std::string_view name) {
    if(const auto* value=NativeBaseCatalog::findConstant(catalog,name))
      if(const auto* values=std::get_if<std::vector<int32_t>>(&value->values); values!=nullptr && values->size()==1)
        return values->front();
    throw std::runtime_error("missing native item effect constant "+std::string(name));
    };
  bindStockEffects(definitions,{callback,flag,constant,constant});
  }
