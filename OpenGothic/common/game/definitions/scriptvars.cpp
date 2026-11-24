#include "scriptvars.h"

#include "daedalus/script.h"
#include "savearchive.h"

#include <stdexcept>
#include <utility>

namespace {

enum class Tag : uint8_t {
  Empty      = 0,
  Int        = 1,
  Float      = 2,
  String     = 3,
  Reference  = 4,
  };

template<class T>
std::vector<T> readValues(SaveArchive& in) {
  uint32_t count = 0;
  in.read(count);
  if(count>uint16_t(-1))
    throw std::runtime_error("Invalid runtime state array size in save");
  std::vector<T> values;
  for(uint32_t i=0; i<count; ++i) {
    T value = {};
    in.read(value);
    values.push_back(std::move(value));
    }
  return values;
  }

} // namespace

ScriptVars::Row ScriptVars::readRow(SaveArchive& in) {
  uint8_t tag = 0;
  Row row{};
  in.read(row.id,tag);
  switch(Tag(tag)) {
    case Tag::Float:   row.value = readValues<float>(in); break;
    case Tag::Int:     row.value = readValues<int32_t>(in); break;
    case Tag::String:  row.value = readValues<std::string>(in); break;
    case Tag::Reference: {
      uint8_t type = 0;
      in.read(type);
      Reference ref;
      ref.kind = ReferenceKind(type);
      if(ref.kind>ReferenceKind::InventoryItem)
        throw std::runtime_error("Invalid runtime state reference in save");
      if(ref.kind!=ReferenceKind::None)
        in.read(ref.object);
      if(ref.kind==ReferenceKind::InventoryItem)
        in.read(ref.item);
      row.value = ref;
      break;
      }
    case Tag::Empty: break;
    default: throw std::runtime_error("Invalid runtime state field in save");
    }
  return row;
  }

void ScriptVars::writeRow(SaveArchive& out, const Row& row) {
  out.write(row.id);
  if(const auto* values = std::get_if<std::vector<int32_t>>(&row.value)) {
    out.write(uint8_t(Tag::Int),*values);
    }
  else if(const auto* values = std::get_if<std::vector<float>>(&row.value)) {
    out.write(uint8_t(Tag::Float),*values);
    }
  else if(const auto* values = std::get_if<std::vector<std::string>>(&row.value)) {
    out.write(uint8_t(Tag::String),*values);
    }
  else if(const auto* ref = std::get_if<Reference>(&row.value)) {
    out.write(uint8_t(Tag::Reference),uint8_t(ref->kind));
    if(ref->kind!=ReferenceKind::None)
      out.write(ref->object);
    if(ref->kind==ReferenceKind::InventoryItem)
      out.write(ref->item);
    }
  else {
    out.write(uint8_t(Tag::Empty));
    }
  }

void ScriptVars::State::bind(daedalus::DaedalusSymbol& symbol) {
  using Type = daedalus::DaedalusDataType;
  if(symbol.is_member())
    return;
  const auto id = symbol.index();
  if(symbol.type()==Type::INSTANCE) {
    const auto it = values.try_emplace(id,Object{}).first;
    symbol.bind_global_instance(std::get<Object>(it->second));
    return;
    }
  if(symbol.is_const() || symbol.count()==0)
    return;
  const auto bindArray = [&]<class T>(std::span<T> bound, std::unique_ptr<T[]>(daedalus::DaedalusSymbol::*take)()) {
    const auto it = values.try_emplace(id,std::in_place_type<Array<T>>).first;
    auto& array = std::get<Array<T>>(it->second);
    if(bound.empty()) {
      // Keep addresses of existing values, including strings already viewed by the session.
      array.storage = (symbol.*take)();
      bound = {array.storage.get(),symbol.count()};
      }
    array.values = bound;
    };
  using Symbol = daedalus::DaedalusSymbol;
  switch(symbol.type()) {
    case Type::INT:    bindArray(symbol.bound_global_int(),&Symbol::take_global_int);       break;
    case Type::FLOAT:  bindArray(std::span<float>{},&Symbol::take_global_float);            break;
    case Type::STRING: bindArray(symbol.bound_global_string(),&Symbol::take_global_string); break;
    default: break;
    }
  }

ScriptVars::Slot* ScriptVars::State::find(uint32_t id) {
  const auto it = values.find(id);
  return it==values.end() ? nullptr : &it->second;
  }

const ScriptVars::Slot* ScriptVars::State::find(uint32_t id) const {
  const auto it = values.find(id);
  return it==values.end() ? nullptr : &it->second;
  }

ScriptVars::SymbolHeader ScriptVars::readSymbolHeader(SaveArchive& in) {
  SymbolHeader header;
  in.read(header.format,header.count);
  return header;
  }

ScriptVars::SymbolDefinition ScriptVars::readSymbolDefinition(SaveArchive& in) {
  uint8_t kind = 0;
  SymbolDefinition record;
  in.read(kind,record.name,record.like,record.index);
  record.kind = SymbolKind(kind);
  return record;
  }

void ScriptVars::writeSymbolDefinitions(SaveArchive& out, std::span<const SymbolDefinition> symbols) {
  out.write(SymbolFormat,uint32_t(symbols.size()));
  for(const auto& symbol : symbols)
    out.write(uint8_t(symbol.kind),symbol.name,symbol.like,symbol.index);
  }
