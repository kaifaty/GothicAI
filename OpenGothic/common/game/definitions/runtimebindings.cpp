#include "runtimebindings.h"

#include "daedalus/script.h"
#include "definitionjson.h"
#include "programdata.h"
#include "runtimebindingroots.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <rapidjson/writer.h>

namespace {

using namespace DefinitionJson;
namespace fs = std::filesystem;

using Type = daedalus::DaedalusDataType;
namespace Flags = daedalus::DaedalusSymbolFlag;

constexpr std::string_view Format = "gothic-runtime-bindings";
constexpr uint32_t Version = 1;
constexpr uint32_t MaxSlotCount = 1u << 20;

bool digest(std::string_view text) {
  return text.size()==64 && std::ranges::all_of(text,[](char ch) {
    return (ch>='0' && ch<='9') || (ch>='a' && ch<='f');
    });
  }

uint32_t number(const Json& value) {
  if(!value.IsUint())
    throw std::runtime_error("expected a binding number");
  return value.GetUint();
  }

void readValues(asset::ProgramSymbol& slot, const Json& values) {
  require(values.IsArray(),"binding values must be an array");
  const auto type = Type(slot.type);
  const bool scalar = type==Type::INT || type==Type::FLOAT || type==Type::STRING;
  const auto count = scalar && (slot.flags&Flags::MEMBER)==0 ? slot.count : 0u;
  require(values.Size()==count,"invalid value count for binding "+slot.name);
  for(const auto& value : values.GetArray()) {
    switch(type) {
      case Type::INT:    slot.int_values.push_back(integer(value)); break;
      case Type::FLOAT:  slot.float_bits.push_back(number(value)); break;
      case Type::STRING: slot.string_values.push_back(bytes(value)); break;
      default:           break;
      }
    }
  }

void verifyLinks(const asset::ProgramRecord& result) {
  for(size_t id=0; id<result.symbols.size(); ++id) {
    const auto& slot = result.symbols[id];
    if(slot.name.empty())
      continue;
    if(slot.parent>=0)
      require(!result.symbols[size_t(slot.parent)].name.empty(),"unbound parent of "+slot.name);
    if(Type(slot.type)!=Type::FUNCTION || (slot.flags&Flags::CONST)==0)
      continue;
    require(slot.count<result.symbols.size()-id,"function parameters exceed the binding extent");
    for(uint32_t offset=1; offset<=slot.count; ++offset)
      require(!result.symbols[id+offset].name.empty(),"unbound parameter of "+slot.name);
    }
  }

}

fs::path RuntimeBindings::path(const fs::path& base, std::string_view program) {
  require(digest(program),"game bindings: invalid program identity");
  return base/"definitions"/program/"game-bindings.json";
  }

std::string_view RuntimeBindings::recipe() {
  return RuntimeBindingRootsSha256;
  }

asset::ProgramRecord RuntimeBindings::read(const fs::path& base, std::string_view program) {
  const auto file = path(base,program);
  try {
    const auto doc = DefinitionJson::read(file);
    object(doc,{"format","version","program_sha256","recipe_sha256","program_version","slots","bindings"});
    require(string(doc["format"])==Format && number(doc["version"])==Version,"unsupported game bindings");
    require(string(doc["program_sha256"])==program,"game bindings differ from selected program");
    require(string(doc["recipe_sha256"])==recipe(),"game binding requirements changed; prepare the base again");
    const auto programVersion = number(doc["program_version"]);
    const auto slots = number(doc["slots"]);
    const auto& rows = doc["bindings"];
    require(programVersion<=std::numeric_limits<uint8_t>::max() && rows.IsArray() && !rows.Empty() &&
            slots>=rows.Size() && slots<=MaxSlotCount,"invalid game binding layout");
    asset::ProgramRecord result;
    result.version = uint8_t(programVersion);
    // The original ID range keeps world records, saved states and the first director ID stable.
    result.symbols.resize(slots);
    uint32_t nextId = 0;
    for(const auto& row : rows.GetArray()) {
      require(row.IsArray() && row.Size()==9,"invalid game binding row");
      const auto id = number(row[0]);
      require(id>=nextId && id<slots,"unordered or invalid game binding ID");
      nextId = id+1;
      auto& slot = result.symbols[id];
      slot.name = bytes(row[1]);
      slot.type = number(row[2]);
      slot.count = number(row[3]);
      slot.flags = number(row[4]);
      slot.parent = integer(row[5]);
      slot.return_type = number(row[6]);
      require(row[7].IsBool(),"invalid generated binding flag");
      slot.generated = row[7].GetBool();
      require(!slot.name.empty() && slot.type<=uint32_t(Type::INSTANCE) && slot.count<=4095 &&
              slot.parent>=-1 && slot.parent<int32_t(slots) && slot.return_type<=uint32_t(Type::INSTANCE),
              "invalid game binding: "+slot.name);
      // Native overrides still use the callable's address field as their key.
      // Give them stable, unique keys without importing offsets in the original bytecode.
      slot.address = int32_t(id);
      readValues(slot,row[8]);
      }
    verifyLinks(result);
    return result;
    }
  catch(const std::exception& error) {
    throw std::runtime_error("game bindings: "+file.string()+": "+error.what());
    }
  }

void RuntimeBindings::validate(const fs::path& base, std::string_view stockProgram) {
  const auto index = ProgramData::readIndex(base/"programs");
  const auto* game = index.server();
  require(game!=nullptr && game->recordSha256==stockProgram,"native game requires the stock program identity");
  const auto file = path(base,stockProgram);
  const auto relative = file.lexically_relative(base).generic_string();
  const auto manifest = DefinitionJson::read(base/"base-files.json");
  const auto& entries = at(manifest,"files");
  require(entries.IsArray(),"invalid base file manifest");
  const auto listed = [&](const std::string& name) {
    return std::ranges::any_of(entries.GetArray(),[&](const auto& entry) {
      return entry.IsObject() && entry.HasMember("path") && entry["path"]==name.c_str();
      });
    };
  require(listed("programs/programs.json") && listed(relative),
          "base manifest has no stock game bindings; prepare the base again with content-convert resolve");
  (void)RuntimeBindings::read(base,stockProgram);
  }

void RuntimeBindings::write(const fs::path& base, std::string_view program,
                            const asset::ProgramRecord& source, std::span<const uint32_t> ids) {
  const auto file = path(base,program);
  require(source.symbols.size()<=MaxSlotCount && !ids.empty(),"invalid game binding extent");
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text(Format,alloc),alloc);
  put(doc,"version",Json(Version),alloc);
  put(doc,"program_sha256",text(program,alloc),alloc);
  put(doc,"recipe_sha256",text(recipe(),alloc),alloc);
  put(doc,"program_version",Json(uint32_t(source.version)),alloc);
  put(doc,"slots",Json(uint32_t(source.symbols.size())),alloc);
  Json rows(rapidjson::kArrayType);
  rows.Reserve(rapidjson::SizeType(ids.size()),alloc);
  for(auto id : ids) {
    const auto& slot = source.symbols.at(id);
    Json values(rapidjson::kArrayType);
    for(const auto value : slot.int_values)
      values.PushBack(value,alloc);
    for(const auto value : slot.float_bits)
      values.PushBack(value,alloc);
    for(const auto& value : slot.string_values)
      values.PushBack(text(encode(value),alloc),alloc);
    Json row(rapidjson::kArrayType);
    row.PushBack(id,alloc).PushBack(text(encode(slot.name),alloc),alloc).PushBack(slot.type,alloc)
       .PushBack(slot.count,alloc).PushBack(slot.flags,alloc).PushBack(slot.parent,alloc)
       .PushBack(slot.return_type,alloc).PushBack(slot.generated,alloc).PushBack(values,alloc);
    rows.PushBack(row,alloc);
    }
  put(doc,"bindings",std::move(rows),alloc);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  require(doc.Accept(writer),"cannot encode "+file.string());
  std::ofstream out(file,std::ios::binary);
  out.write(buffer.GetString(),std::streamsize(buffer.GetSize()));
  out.put('\n');
  out.close();
  require(bool(out),"cannot write "+file.string());
  (void)RuntimeBindings::read(base,program);
  }
