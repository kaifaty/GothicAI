#include "itemdefinitionruntime.h"
#include "itemrecordfields.h"
#include "itemloadprobe.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>

namespace {
template<class T> void resolveField(T& value, const std::vector<std::string>& names, size_t& index,
                                   const daedalus::DaedalusScript& script) {
  if constexpr(std::is_array_v<T>) {
    for(auto& element:value) resolveField(element,names,index,script);
    }
  else if constexpr(!std::is_same_v<T,std::string>) {
    const auto& name=names.at(index++);
    if(name.empty()) return;
    const auto* symbol=script.find_symbol_by_name(name);
    if(symbol==nullptr) throw std::runtime_error("unknown symbol "+name);
    value=T(symbol->index());
    }
  }
}

ItemDefinitionRuntime::Definitions ItemDefinitionRuntime::resolve(const ItemDefinitions::Library& library,
                                                                  const daedalus::DaedalusScript& script) {
  Definitions definitions;
  for(const auto& [name,definition]:library.items) {
    try {
      const auto* symbol=script.find_symbol_by_name(name);
      if(symbol==nullptr) throw std::runtime_error("unknown item symbol");
      ItemRecord record=definition.record;
      itemRecordFields(record,[&](const char* field, auto& value, bool function) {
        if(!function) return;
        try {
          size_t index=0;
          const auto& names=definition.symbols.at(field);
          resolveField(value,names,index,script);
          if(index!=names.size()) throw std::runtime_error("symbol array extent differs");
          }
        catch(const std::exception& e) { throw std::runtime_error(std::string(field)+": "+e.what()); }
        });
      definitions.items.emplace(symbol->index(),std::move(record));
      }
    catch(const std::exception& e) { throw std::runtime_error("item definitions: "+name+": "+e.what()); }
    }
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
    }
  LoadProbe::Items::Constructor probe;
  vm.init_instance(item,symbol);
  return false;
  }

void ItemDefinitionRuntime::registerStockRestoreEffects(Definitions& definitions, const daedalus::DaedalusScript& program) {
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
    const auto* function = program.find_symbol_by_name(row.callback);
    const auto* value = program.find_symbol_by_name(row.amount);
    if(function==nullptr || value==nullptr)
      throw std::runtime_error("stock item restore: missing symbol for "+std::string(row.callback));
    RestoreEffect effect{{row.attribute,value->index()},{}};
    if(!row.secondAmount.empty()) {
      const auto* second = program.find_symbol_by_name(row.secondAmount);
      if(second==nullptr)
        throw std::runtime_error("stock item restore: missing second dose for "+std::string(row.callback));
      effect.second = RestoreChange{row.secondAttribute,second->index()};
      }
    if(!definitions.restoreEffects.emplace(function->index(),effect).second)
      throw std::runtime_error("stock item restore: duplicate callback "+std::string(row.callback));
    }

  const auto* meatbug=program.find_symbol_by_name("USE_MEATBUGFLESH");
  if(meatbug==nullptr ||
     !definitions.restoreEffects.emplace(meatbug->index(),RestoreEffect{{ATR_HITPOINTS,int32_t(10)}, {}}).second)
    throw std::runtime_error("stock item restore: invalid USE_MEATBUGFLESH callback");

  // Full elixirs assign the corresponding maximum directly to the current attribute.
  static constexpr struct StockFullRestore {
    std::string_view callback;
    FullRestoreEffect effect;
    } full[] = {
    {"USEITPO_HEALTH_04", {ATR_HITPOINTS, ATR_HITPOINTSMAX}},
    {"USEITPO_MANA_04",   {ATR_MANA,      ATR_MANAMAX}},
    };
  for(const auto& [callback,effect]:full) {
    const auto* function=program.find_symbol_by_name(callback);
    if(function==nullptr || !definitions.fullRestoreEffects.emplace(function->index(),effect).second)
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
    const auto* function=program.find_symbol_by_name(callback);
    PermanentAttributeEffect effect{{attribute,int32_t(0)},refill};
    if(const auto* name=std::get_if<std::string_view>(&dose)) {
      const auto* symbol=program.find_symbol_by_name(*name);
      if(symbol==nullptr)
        throw std::runtime_error("stock permanent item attribute: missing dose "+std::string(callback));
      effect.increase.amount=symbol->index();
      } else {
      effect.increase.amount=std::get<int32_t>(dose);
      }
    if(function==nullptr || !definitions.permanentAttributeEffects.emplace(function->index(),effect).second)
      throw std::runtime_error("stock permanent item attribute: invalid callback "+std::string(callback));
    }
  }

void ItemDefinitionRuntime::registerStockArmorSetEffects(Definitions& definitions, const daedalus::DaedalusScript& program) {
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
    const auto* armorFlag = program.find_symbol_by_name(armor);
    const auto* beltFlag = program.find_symbol_by_name(belt);
    for(bool equip : {true,false}) {
      const auto callback = std::string(equip ? "EQUIP_" : "UNEQUIP_").append(item);
      const auto* function = program.find_symbol_by_name(callback);
      if(function==nullptr || armorFlag==nullptr || beltFlag==nullptr)
        throw std::runtime_error("stock armor set: missing symbol for "+callback);
      definitions.armorSetEffects.emplace(function->index(),ArmorSetEffect{armorFlag->index(),beltFlag->index(),equip});
      }
    }
  }

void ItemDefinitionRuntime::registerStockFightSkillEffects(Definitions& definitions, const daedalus::DaedalusScript& program) {
  const auto bind = [&](std::string_view suffix, Talent talent, int32_t bonus, bool playerOnly) {
    for(bool equip : {true,false}) {
      const auto name = std::string(equip ? "EQUIP_" : "UNEQUIP_").append(suffix);
      const auto* function = program.find_symbol_by_name(name);
      if(function==nullptr)
        throw std::runtime_error("stock weapon skill: missing symbol "+name);
      definitions.fightSkillEffects.emplace(function->index(),FightSkillEffect{talent,equip ? bonus : -bonus,playerOnly});
      }
    };
  const auto bindWeapons = [&](std::string_view hand, Talent talent, std::initializer_list<int> bonuses) {
    for(const auto bonus : bonuses) {
      const auto suffix = (bonus<10 ? "0" : "")+std::to_string(bonus);
      const auto* constant = program.find_symbol_by_name("WAFFENBONUS_"+suffix);
      if(constant==nullptr)
        throw std::runtime_error("stock weapon skill: missing bonus "+suffix);
      bind(std::string(hand).append(suffix),talent,constant->get_int(),true);
      }
    };
  // The remaining generic tiers have no stock item bindings.
  bindWeapons("1H_",TALENT_1H,{2,3,5,6,7,8,10});
  bindWeapons("2H_",TALENT_2H,{2,3,4,5,10});
  bind("ALRIKSSWORD",TALENT_1H,10,false);
  bind("MORGANSRING",TALENT_1H,10,false);
  }
