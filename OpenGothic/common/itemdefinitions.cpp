#include "itemdefinitions.h"
#include "itemrecordfields.h"

#include "definitionjson.h"

#include <fstream>
#include <stdexcept>
#include <type_traits>

namespace {
using Json = rapidjson::Value;
using DefinitionJson::require;
using DefinitionJson::at;
using DefinitionJson::string;
using DefinitionJson::bytes;

void readDocuments(ItemDefinitions::Library& library,const std::filesystem::path& file,std::string_view programSha256) {
  if(!std::filesystem::exists(file))
    return;
  const auto doc=DefinitionJson::read(file);
  require(string(at(doc,"format"))==ItemDefinitions::DocumentFormat,"unsupported document format");
  require(DefinitionJson::integer(at(doc,"version"))==ItemDefinitions::DocumentVersion,"unsupported document version");
  require(string(at(doc,"program_sha256"))==programSha256,"document program SHA differs");
  const auto& records=at(doc,"records");
  require(records.IsObject(),"document records must be an object");
  for(auto it=records.MemberBegin();it!=records.MemberEnd();++it) {
    auto name=string(it->name);
    const auto& record=it->value;
    ItemDefinitions::Document out;
    out.handle=string(at(record,"handle"));
    require(!out.handle.empty() && !name.empty(),"empty document function or handle");
    const auto& map=at(record,"map");
    require(map.IsBool(),"invalid document map mode");
    out.map=map.GetBool();
    const auto& calls=at(record,"calls");
    require(calls.IsArray(),"document calls must be an array");
    for(const auto& entry:calls.GetArray()) {
      ItemDefinitions::DocumentCall call;
      call.function=string(at(entry,"function"));
      const auto types=ItemDefinitions::documentArguments(call.function);
      const auto& args=at(entry,"arguments");
      require(!types.empty() && args.IsArray() && args.Size()==types.size(),"invalid document action "+call.function);
      for(rapidjson::SizeType i=0;i<args.Size();++i) {
        if(types[i]=='s') call.arguments.emplace_back(bytes(args[i]));
        else call.arguments.emplace_back(DefinitionJson::integer(args[i]));
        }
      out.calls.push_back(std::move(call));
      }
    require(library.documents.emplace(std::move(name),std::move(out)).second,"duplicate item document");
    }
  }

template<class T> void field(T& out, const Json& value, bool function, std::vector<std::string>& names) {
  if constexpr(std::is_array_v<T>) {
    require(value.IsArray() && value.Size()==std::extent_v<T>,"array extent differs");
    for(size_t i=0;i<std::extent_v<T>;++i) field(out[i],value[rapidjson::SizeType(i)],function,names);
    }
  else if constexpr(std::is_same_v<T,std::string>) out=bytes(value);
  else {
    if(function) {
      if(value.IsNull()) { out=T(0); names.emplace_back(); }
      else if(value.IsInt() && value.GetInt()==-1) { out=T(-1); names.emplace_back(); }
      else { names.push_back(string(value)); require(!names.back().empty(),"empty symbol name"); out=T(0); }
      }
    else {
      require(value.IsInt(),"expected int32");
      out=T(value.GetInt());
      }
    }
  }
}

std::string_view ItemDefinitions::documentArguments(std::string_view function) {
  constexpr std::pair<std::string_view,std::string_view> signatures[]={
    {"DOC_SETPAGES","i"},{"DOC_SETPAGE","isi"},{"DOC_SETFONT","is"},{"DOC_SETMARGINS","iiiiii"},
    {"DOC_PRINTLINE","is"},{"DOC_PRINTLINES","is"},{"DOC_SETLEVEL","s"},{"DOC_SETLEVELCOORDS","iiii"},
    };
  for(const auto& [name,types]:signatures)
    if(name==function)
      return types;
  return {};
  }

ItemDefinitions::Library ItemDefinitions::read(const std::filesystem::path& base, std::string_view programSha256) {
  const auto file=base/"definitions"/programSha256/"items.json";
  try {
    std::ifstream input(file,std::ios::binary);
    require(bool(input),"cannot open file");
    const std::string text{std::istreambuf_iterator<char>(input),{}};
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    require(!doc.HasParseError(),"invalid JSON");
    require(string(at(doc,"format"))=="gothic-item-definitions","unsupported format");
    const auto& version=at(doc,"version");
    require(version.IsInt() && version.GetInt()==1,"unsupported version");
    require(!string(at(doc,"program_name")).empty(),"empty program name");
    require(string(at(doc,"program_sha256"))==programSha256,"program SHA differs");
    require(string(at(doc,"string_encoding"))=="unicode-byte-mapping","unsupported string encoding");
    require(string(at(doc,"native_schema"))==ItemRecordSchema,"ItemRecord schema differs");
    const auto& records=at(doc,"records");
    require(records.IsArray(),"records must be an array");
    Library result;
    for(const auto& record:records.GetArray()) {
      const auto name=string(at(record,"symbol"));
      try {
        require(!name.empty() && result.items.count(name)==0 && result.uncaptured.count(name)==0,"duplicate or empty symbol");
        const auto status=string(at(record,"status"));
        if(status=="uncaptured") {
          require(!record.HasMember("fields"),"uncaptured item has fields");
          result.uncaptured.emplace(name,string(at(record,"reason"))+": "+string(at(record,"detail")));
          continue;
          }
        require(status=="captured","unsupported capture status");
        const auto& calls=at(record,"calls");
        require(calls.IsArray() && calls.Empty(),"item constructor calls are unsupported");
        const auto& fields=at(record,"fields");
        require(fields.IsObject(),"fields must be an object");
        Definition definition;
        size_t count=0;
        itemRecordFields(definition.record,[&](const char* key, auto& value, bool function) {
          try {
            std::vector<std::string> names;
            field(value,at(fields,key),function,names);
            if(function) definition.symbols.emplace(key,std::move(names));
            ++count;
            }
          catch(const std::exception& e) { throw std::runtime_error(std::string(key)+": "+e.what()); }
          });
        require(count==fields.MemberCount(),"unexpected ItemRecord field");
        result.items.emplace(name,std::move(definition));
        }
      catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
      }
    readDocuments(result,file.parent_path()/"item-documents.json",programSha256);
    return result;
    }
  catch(const std::exception& e) { throw std::runtime_error("item definitions: "+file.string()+": "+e.what()); }
  }
