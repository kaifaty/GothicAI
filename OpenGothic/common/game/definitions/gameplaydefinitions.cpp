#include "gameplaydefinitions.h"

#include "utils/string_frm.h"

#include <type_traits>

using namespace DefinitionJson;

namespace {

template<class T>
T value(const Json& data, std::string_view name) {
  if constexpr(std::is_same_v<T,int32_t>) {
    return integer(data);
    }
  else if constexpr(std::is_same_v<T,float>) {
    require(data.IsFloat(),"gameplay definitions: expected a float for "+std::string(name));
    return data.GetFloat();
    }
  else {
    static_assert(std::is_same_v<T,std::string>);
    return bytes(data);
    }
  }

template<class Record>
Record readRecord(const Json& row, std::string_view name) {
  require(row.IsObject(),"gameplay definitions: invalid "+std::string(name));
  Record record {};
  size_t size = 0;
  Record::fields([&](const char* field, auto member) {
    const auto& values = at(row,field);
    auto& destination = record.*member;
    using T = std::remove_reference_t<decltype(destination)>;
    constexpr auto count = std::is_array_v<T> ? std::extent_v<T> : 1;
    require(values.IsArray() && values.Size()==count,
            "gameplay definitions: unexpected value count for "+std::string(field));
    if constexpr(std::is_array_v<T>) {
      for(rapidjson::SizeType i = 0; i<count; ++i)
        destination[i] = value<std::remove_extent_t<T>>(values[i],field);
      }
    else {
      destination = value<T>(values[0],field);
      }
    ++size;
    });
  require(row.MemberCount()==size,"gameplay definitions: unexpected member count for "+std::string(name));
  return record;
  }

}

std::shared_ptr<const GameplayDefinitions> GameplayDefinitions::load(const std::filesystem::path& base) {
  const auto path = section(base);
  if(!std::filesystem::exists(path))
    return {};
  auto rows = DefinitionJson::read(path);
  object(rows,{"format","version","program_sha256","instances"});
  require(string(rows["format"])==format && integer(rows["version"])==version &&
          string(rows["program_sha256"])==stockProgram && rows["instances"].IsObject(),
          "gameplay definitions: invalid format or program in "+path.string());
  return std::shared_ptr<const GameplayDefinitions>(new GameplayDefinitions(std::move(rows)));
  }

std::vector<std::pair<std::string,SpellRecord>> GameplayDefinitions::spells() const {
  std::vector<std::pair<std::string,SpellRecord>> result;
  for(auto it = rows["instances"].MemberBegin(); it != rows["instances"].MemberEnd(); ++it) {
    auto name = string(it->name);
    if(name.starts_with("SPELL_"))
      result.emplace_back(name,readRecord<SpellRecord>(it->value,name));
    }
  require(!result.empty(),"gameplay definitions: missing spells");
  return result;
  }

FocusRecord GameplayDefinitions::focus(std::string_view name) const {
  const std::string key(name);
  return readRecord<FocusRecord>(at(rows["instances"],key.c_str()),name);
  }

GuildRecord GameplayDefinitions::guild() const {
  return readRecord<GuildRecord>(at(rows["instances"],"GIL_VALUES"),"GIL_VALUES");
  }

std::string_view GameplayDefinitions::speech(int voice, std::string_view member) const {
  auto position = voices.find(voice);
  if(position==voices.end()) {
    string_frm name("SVM_",voice);
    const auto& instances = rows["instances"];
    const auto row = instances.FindMember(name.c_str());
    if(row==instances.MemberEnd())
      return "";
    require(row->value.IsObject(),"gameplay definitions: invalid SVM voice");
    Voice decoded;
    for(auto it = row->value.MemberBegin(); it != row->value.MemberEnd(); ++it) {
      require(it->value.IsArray() && it->value.Size()==1,"gameplay definitions: invalid SVM member");
      require(decoded.emplace(string(it->name),bytes(it->value[0])).second,"gameplay definitions: duplicate SVM member");
      }
    position = voices.emplace(voice,std::move(decoded)).first;
    }
  const auto item = position->second.find(member);
  return item==position->second.end() ? std::string_view("") : std::string_view(item->second);
  }
