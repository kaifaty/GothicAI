#include "routinedefinitions.h"

#include <rapidjson/document.h>

#include <fstream>
#include <stdexcept>

namespace {
using Json = rapidjson::Value;
void require(bool ok, const std::string& why) {
  if(!ok) throw std::runtime_error(why);
  }
const Json& at(const Json& value, const char* key) {
  require(value.IsObject() && value.HasMember(key),std::string("missing field ")+key);
  return value[key];
  }
std::string string(const Json& value) {
  require(value.IsString(),"expected a string");
  return {value.GetString(),value.GetStringLength()};
  }
std::string bytes(const Json& value) {
  const auto text=string(value);
  std::string out;
  for(size_t i=0;i<text.size();++i) {
    const auto c=uint8_t(text[i]);
    if(c<128) out+=char(c);
    else {
      require((c==0xc2 || c==0xc3) && i+1<text.size() && (uint8_t(text[i+1])&0xc0)==0x80,
              "invalid script byte string");
      out+=char(((c&3)<<6)|(uint8_t(text[++i])&63));
      }
    }
  return out;
  }
void time(const Json& value, int& hour, int& minute) {
  require(value.IsArray() && value.Size()==2 && value[0].IsInt() && value[1].IsInt(),
          "expected [hour, minute] int32 values");
  hour=value[0].GetInt();
  minute=value[1].GetInt();
  }
}

RoutineDefinitions::Library RoutineDefinitions::read(const std::filesystem::path& base,
                                                     std::string_view programSha256) {
  const auto file=base/"definitions"/programSha256/"routines.json";
  try {
    std::ifstream input(file,std::ios::binary);
    require(bool(input),"cannot open file");
    const std::string text{std::istreambuf_iterator<char>(input),{}};
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    require(!doc.HasParseError(),"invalid JSON");
    require(string(at(doc,"format"))=="gothic-routine-definitions","unsupported format");
    const auto& version=at(doc,"version");
    require(version.IsInt() && version.GetInt()==1,"unsupported version");
    require(!string(at(doc,"program_name")).empty(),"empty program name");
    require(string(at(doc,"program_sha256"))==programSha256,"program SHA differs");
    require(string(at(doc,"string_encoding"))=="unicode-byte-mapping","unsupported string encoding");
    const auto& records=at(doc,"records");
    require(records.IsArray(),"records must be an array");
    Library result;
    for(const auto& record:records.GetArray()) {
      const auto name=string(at(record,"symbol"));
      try {
        require(!name.empty() && result.routines.count(name)==0 && result.uncaptured.count(name)==0,
                "duplicate or empty symbol");
        const auto status=string(at(record,"status"));
        const auto& rows=at(record,"rows");
        require(rows.IsArray(),"rows must be an array");
        if(status=="uncaptured") {
          require(rows.Empty(),"uncaptured routine has rows");
          result.uncaptured.emplace(name,string(at(record,"reason"))+": "+string(at(record,"detail")));
          continue;
          }
        require(status=="captured","unsupported capture status");
        std::vector<Row> routine;
        for(const auto& value:rows.GetArray()) {
          try {
            Row row;
            time(at(value,"start"),row.startHour,row.startMinute);
            time(at(value,"end"),row.endHour,row.endMinute);
            row.state=string(at(value,"state"));
            require(!row.state.empty(),"empty state symbol");
            row.waypoint=bytes(at(value,"waypoint"));
            require(value.MemberCount()==4,"unexpected row field");
            routine.push_back(std::move(row));
            }
          catch(const std::exception& e) { throw std::runtime_error("row "+std::to_string(routine.size()+1)+": "+e.what()); }
          }
        result.routines.emplace(name,std::move(routine));
        }
      catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
      }
    return result;
    }
  catch(const std::exception& e) { throw std::runtime_error("routine definitions: "+file.string()+": "+e.what()); }
  }
