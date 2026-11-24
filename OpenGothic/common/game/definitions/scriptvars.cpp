#include "scriptvars.h"

#include "savearchive.h"

#include <utility>

namespace {

enum class Tag : uint32_t {
  Empty      = 0,
  Float      = 1,
  Int        = 2,
  String     = 3,
  Reference  = 7,
  };

template<class T>
std::vector<T> readValues(SaveArchive& in, std::string& name) {
  uint32_t count = 0;
  in.read(name,count);
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
  uint32_t tag = 0;
  in.read(tag);
  Row row;
  switch(Tag(tag)) {
    case Tag::Float:   row.value = readValues<float>(in,row.name); break;
    case Tag::Int:     row.value = readValues<int32_t>(in,row.name); break;
    case Tag::String:  row.value = readValues<std::string>(in,row.name); break;
    case Tag::Reference: {
      uint8_t type = 0;
      in.read(type);
      Reference ref;
      ref.kind = ReferenceKind(type);
      if(ref.kind!=ReferenceKind::None)
        in.read(row.name,ref.object);
      if(ref.kind==ReferenceKind::InventoryItem)
        in.read(ref.item);
      row.value = ref;
      break;
      }
    default:
      break;
    }
  return row;
  }

void ScriptVars::writeRow(SaveArchive& out, const Row& row) {
  if(const auto* values = std::get_if<std::vector<int32_t>>(&row.value)) {
    out.write(uint32_t(Tag::Int),row.name,*values);
    }
  else if(const auto* values = std::get_if<std::vector<float>>(&row.value)) {
    out.write(uint32_t(Tag::Float),row.name,*values);
    }
  else if(const auto* values = std::get_if<std::vector<std::string>>(&row.value)) {
    out.write(uint32_t(Tag::String),row.name,*values);
    }
  else if(const auto* ref = std::get_if<Reference>(&row.value)) {
    out.write(uint32_t(Tag::Reference),uint8_t(ref->kind));
    if(ref->kind!=ReferenceKind::None)
      out.write(row.name,ref->object);
    if(ref->kind==ReferenceKind::InventoryItem)
      out.write(ref->item);
    }
  else {
    out.write(uint32_t(Tag::Empty));
    }
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
