#pragma once

#include "itemdefinitionruntime.h"
#include "itemrecordfields.h"

#include <stdexcept>
#include <type_traits>

namespace ItemDefinitionRuntime {
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
Definitions resolveRecords(const ItemDefinitions::Library& library, const FindItem& findItem,
                                                   const FindReference& findReference) {
  Definitions definitions;
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
