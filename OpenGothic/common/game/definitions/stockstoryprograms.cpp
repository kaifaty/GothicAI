#include "stockstoryprograms.h"

#include "definitionjson.h"
#include "storygraph/exprjson.h"

#include <set>
#include <utility>

namespace {
namespace fs = std::filesystem;
using namespace DefinitionJson;
using namespace StockStoryPrograms;

uint32_t number(const Json& value) {
  require(value.IsUint(),"story programs: expected uint32");
  return value.GetUint();
  }

std::vector<uint32_t> numbers(const Json& values) {
  require(values.IsArray(),"story programs: expected a number list");
  std::vector<uint32_t> result;
  result.reserve(values.Size());
  for(const auto& value : values.GetArray())
    result.push_back(number(value));
  return result;
  }

template<class T, class Decode>
void readTable(T& target, const Json& values, Decode&& decode) {
  require(values.IsObject(),"story programs: expected a symbol table");
  for(auto it=values.MemberBegin(); it!=values.MemberEnd(); ++it) {
    auto name = bytes(it->name);
    require(!name.empty() && target.emplace(name,decode(it->value)).second,
            "story programs: invalid or duplicate symbol "+name);
    }
  }

void checkType(uint32_t type, bool allowVoid) {
  require((allowVoid || type!=0) && (type<=3 || type==5 || type==7),"story programs: invalid signature type");
  }

}

fs::path StockStoryPrograms::section(const fs::path& base) {
  return base/"definitions"/stockProgram/"story-programs.json";
  }

Catalog StockStoryPrograms::read(const fs::path& base) {
  const auto path = section(base);
  const auto doc = DefinitionJson::read(path);
  object(doc,{"format","version","program_sha256","symbols","integers","strings","programs"});
  require(string(doc["format"])==format && integer(doc["version"])==version &&
          string(doc["program_sha256"])==stockProgram,"story programs: invalid format or source in "+path.string());

  Catalog result;
  readTable(result.symbols,doc["symbols"],number);
  readTable(result.integers,doc["integers"],integer);
  readTable(result.strings,doc["strings"],bytes);
  std::set<uint32_t> ids;
  for(const auto& [name,id] : result.symbols)
    require(ids.insert(id).second,"story programs: duplicate symbol identity for "+name);
  for(const auto& [name,value] : result.integers)
    require(result.symbols.contains(name),"story programs: integer has no symbol "+name);
  for(const auto& [name,value] : result.strings)
    require(result.symbols.contains(name) && !result.integers.contains(name),"story programs: invalid string symbol "+name);

  const auto& programs = doc["programs"];
  require(programs.IsArray() && !programs.Empty(),"story programs: missing programs");
  for(const auto& row : programs.GetArray()) {
    object(row,{"id","name","return_type","parameters","calls","steps"});
    Entry entry;
    entry.id = number(row["id"]);
    entry.name = bytes(row["name"]);
    entry.returnType = number(row["return_type"]);
    checkType(entry.returnType,true);
    entry.parameters = numbers(row["parameters"]);
    for(auto type : entry.parameters)
      checkType(type,false);
    entry.calls = numbers(row["calls"]);
    for(auto id : entry.calls)
      require(ids.contains(id),"story programs: missing called symbol for "+entry.name);

    const auto symbol = result.symbols.find(entry.name);
    require(symbol!=result.symbols.end() && symbol->second==entry.id,"story programs: invalid function identity for "+entry.name);
    entry.program = storygraph::readProgram(row["steps"],entry.returnType!=0);
    require(entry.program.error.empty(),"story programs: malformed function "+entry.name+": "+entry.program.error);
    require(result.programs.emplace(entry.id,std::move(entry)).second,"story programs: duplicate function");
    }
  return result;
  }
