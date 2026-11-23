#include "itemdefinitionruntime.h"
#include "itemrecordfields.h"
#include "itemloadprobe.h"

#include <cstdlib>
#include <cstring>
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
  // Each callback changes one of SELF's current attributes and returns.
  struct StockRestore {
    std::string_view callback;
    Attribute attribute;
    std::string_view amount;
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
    };
  for(const auto& [callback,attribute,amount] : stock) {
    const auto* function = program.find_symbol_by_name(callback);
    const auto* value = program.find_symbol_by_name(amount);
    if(function==nullptr || value==nullptr)
      throw std::runtime_error("stock item restore: missing symbol for "+std::string(callback));
    if(!definitions.restoreEffects.emplace(function->index(),RestoreEffect{attribute,value->index()}).second)
      throw std::runtime_error("stock item restore: duplicate callback "+std::string(callback));
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
