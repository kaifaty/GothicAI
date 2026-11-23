#include "npcdefinitions.h"

#include "definitionjson.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
using Json = rapidjson::Value;
using DefinitionJson::require;
using DefinitionJson::at;
using DefinitionJson::string;
using DefinitionJson::bytes;
NpcDefinitions::Value value(const Json& json) {
  using Value = NpcDefinitions::Value;
  Value result;
  const auto type=string(at(json,"type"));
  if(type=="self" || type=="null") {
    require(json.MemberCount()==1,"unexpected value field");
    result.kind=type=="self" ? Value::Kind::Self : Value::Kind::Null;
    return result;
    }
  const auto& data=at(json,"value");
  require(json.MemberCount()==2,"unexpected value field");
  if(type=="int") {
    require(data.IsInt(),"expected int32 value");
    result.kind=Value::Kind::Int;
    result.integer=data.GetInt();
    }
  else if(type=="float") {
    require(data.IsNumber(),"expected float value");
    const auto number=data.GetDouble();
    require(std::isfinite(number) && std::abs(number)<=std::numeric_limits<float>::max(),
            "float value out of range");
    result.kind=Value::Kind::Float;
    result.real=float(number);
    }
  else if(type=="string") {
    result.kind=Value::Kind::String;
    result.text=bytes(data);
    }
  else if(type=="symbol") {
    result.kind=Value::Kind::Symbol;
    result.text=string(data);
    require(!result.text.empty(),"empty symbol value");
    }
  else throw std::runtime_error("unsupported value type "+type);
  return result;
  }
NpcDefinitions::Step step(const Json& json) {
  require(json.IsObject(),"step must be an object");
  if(json.HasMember("field")) {
    NpcDefinitions::Field field;
    field.binding=string(at(json,"field"));
    require(field.binding.starts_with("C_NPC.") && field.binding.size()>6,"invalid NPC field binding");
    const auto& index=at(json,"index");
    require(index.IsUint() && index.GetUint()<=std::numeric_limits<uint16_t>::max(),"expected uint16 field index");
    field.index=uint16_t(index.GetUint());
    field.value=value(at(json,"value"));
    require(json.MemberCount()==3,"unexpected field step member");
    return field;
    }
  NpcDefinitions::Call call;
  call.function=string(at(json,"function"));
  require(!call.function.empty(),"empty function symbol");
  const auto& deferred=at(json,"deferred");
  require(deferred.IsBool(),"deferred must be a boolean");
  call.deferred=deferred.GetBool();
  const auto& arguments=at(json,"arguments");
  require(arguments.IsArray(),"arguments must be an array");
  for(const auto& argument:arguments.GetArray()) {
    try { call.args.push_back(value(argument)); }
    catch(const std::exception& e) { throw std::runtime_error("argument "+std::to_string(call.args.size()+1)+": "+e.what()); }
    }
  require(json.MemberCount()==3,"unexpected call step member");
  return call;
  }
}

NpcDefinitions::Library NpcDefinitions::read(const std::filesystem::path& base,
                                            std::string_view programSha256) {
  const auto file=base/"definitions"/programSha256/"npcs.json";
  try {
    std::ifstream input(file,std::ios::binary);
    require(bool(input),"cannot open file");
    const std::string text{std::istreambuf_iterator<char>(input),{}};
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    require(!doc.HasParseError(),"invalid JSON");
    require(string(at(doc,"format"))=="gothic-npc-definitions","unsupported format");
    const auto& version=at(doc,"version");
    require(version.IsInt() && version.GetInt()==1,"unsupported version");
    require(!string(at(doc,"program_name")).empty(),"empty program name");
    require(string(at(doc,"program_sha256"))==programSha256,"program SHA differs");
    require(string(at(doc,"string_encoding"))=="unicode-byte-mapping","unsupported string encoding");
    const auto& records=at(doc,"records");
    require(records.IsArray(),"records must be an array");
    require(doc.MemberCount()==6,"unexpected envelope field");
    Library result;
    for(const auto& record:records.GetArray()) {
      const auto name=string(at(record,"symbol"));
      try {
        require(!name.empty() && result.npcs.count(name)==0 && result.uncaptured.count(name)==0,
                "duplicate or empty symbol");
        const auto status=string(at(record,"status"));
        const auto& steps=at(record,"steps");
        require(steps.IsArray(),"steps must be an array");
        if(status=="uncaptured") {
          require(steps.Empty(),"uncaptured NPC has steps");
          result.uncaptured.emplace(name,string(at(record,"reason"))+": "+string(at(record,"detail")));
          require(record.MemberCount()==5,"unexpected uncaptured NPC field");
          continue;
          }
        require(status=="captured","unsupported capture status");
        require(record.MemberCount()==3,"unexpected captured NPC field");
        Definition definition;
        for(const auto& json:steps.GetArray()) {
          try { definition.steps.push_back(step(json)); }
          catch(const std::exception& e) { throw std::runtime_error("step "+std::to_string(definition.steps.size()+1)+": "+e.what()); }
          }
        result.npcs.emplace(name,std::move(definition));
        }
      catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
      }
    return result;
    }
  catch(const std::exception& e) { throw std::runtime_error("NPC definitions: "+file.string()+": "+e.what()); }
  }
