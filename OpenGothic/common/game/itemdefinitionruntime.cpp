#include "itemdefinitionruntime.h"
#include "itemrecordfields.h"
#include "itemloadprobe.h"
#include "programdata.h"

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
                                                               const daedalus::DaedalusScript& program) {
  const auto* enabled=std::getenv("OPENGOTHIC_ITEM_RECORDS");
  if(enabled==nullptr || std::strcmp(enabled,"0")!=0) {
    const auto sha256=ProgramData::encodeProgram(program.export_program()).sha256;
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
