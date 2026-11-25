#include "musicdefinitiondata.h"

#include "definitionjson.h"

#include <bit>
#include <cmath>
#include <set>
#include <type_traits>

namespace {
namespace fs = std::filesystem;
using namespace DefinitionJson;
constexpr std::string_view format = "gothic-music-themes";
constexpr int version = 1;

void readField(const Json& value, std::string& field) {
  field = bytes(value);
  }

void readField(const Json& value, int32_t& field) {
  field = integer(value);
  }

void readField(const Json& value, float& field) {
  require(value.IsUint(),"music themes: expected float bits");
  field = std::bit_cast<float>(value.GetUint());
  require(std::isfinite(field),"music themes: non-finite float");
  }

void readField(const Json& value, Def::MusicTransitionEffect& field) {
  require(value.IsUint() && value.GetUint()<=uint32_t(Def::MusicTransitionEffect::END_AND_INTO),
          "music themes: invalid transition effect");
  field = Def::MusicTransitionEffect(value.GetUint());
  }

void readField(const Json& value, Def::MusicTransitionType& field) {
  require(value.IsUint() && value.GetUint()<=uint32_t(Def::MusicTransitionType::MEASURE),
          "music themes: invalid transition type");
  field = Def::MusicTransitionType(value.GetUint());
  }

Json writeField(std::string_view field, Allocator& alloc) {
  return text(encode(field),alloc);
  }

Json writeField(int32_t field, Allocator&) {
  return Json(field);
  }

Json writeField(float field, Allocator&) {
  require(std::isfinite(field),"music themes: non-finite float");
  return Json(std::bit_cast<uint32_t>(field));
  }

template<class T>
  requires std::is_enum_v<T>
Json writeField(T field, Allocator&) {
  return Json(uint32_t(field));
  }
}

fs::path MusicDefinitionData::section(const fs::path& base, std::string_view program) {
  return base/"definitions"/program/"music-themes.json";
  }

MusicDefinitionData::Themes MusicDefinitionData::read(const fs::path& base, std::string_view program) {
  const auto path = section(base,program);
  const auto doc = DefinitionJson::read(path);
  object(doc,{"format","version","program_sha256","themes"});
  require(string(doc["format"])==format && integer(doc["version"])==version &&
          string(doc["program_sha256"])==program,"music themes: invalid format or program in "+path.string());

  const auto& rows = doc["themes"];
  require(rows.IsArray() && rows.Size()==stockThemeCount,"music themes: invalid theme table");
  Themes result;
  result.reserve(rows.Size());
  std::set<std::string> names;
  for(const auto& row : rows.GetArray()) {
#define MUSIC_KEY(type, field, extent, symbol) #field,
    object(row,{"id","name", DEF_MUSIC_THEME_FIELDS(MUSIC_KEY)});
#undef MUSIC_KEY
    auto name = bytes(row["name"]);
    require(!name.empty() && name.find_first_of("abcdefghijklmnopqrstuvwxyz")==std::string::npos &&
            names.insert(name).second,"music themes: invalid theme name "+name);
    require(row["id"].IsUint(),"music themes: invalid symbol ID for "+name);
    const auto id = row["id"].GetUint();
    require(result.empty() || result.back().id<id,"music themes: unordered symbol ID for "+name);
    Def::MusicTheme value{};
#define MUSIC_READ(type, field, extent, symbol) readField(row[#field],value.field);
    DEF_MUSIC_THEME_FIELDS(MUSIC_READ)
#undef MUSIC_READ
    result.push_back({id,std::move(name),std::move(value)});
    }
  return result;
  }

void MusicDefinitionData::write(const fs::path& base, std::string_view program, const Themes& themes) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text(format,alloc),alloc);
  put(doc,"version",Json(version),alloc);
  put(doc,"program_sha256",text(program,alloc),alloc);
  Json rows(rapidjson::kArrayType);
  for(const auto& [id,name,value] : themes) {
    Json row(rapidjson::kObjectType);
    put(row,"id",Json(id),alloc);
    put(row,"name",text(encode(name),alloc),alloc);
#define MUSIC_WRITE(type, field, extent, symbol) put(row,#field,writeField(value.field,alloc),alloc);
    DEF_MUSIC_THEME_FIELDS(MUSIC_WRITE)
#undef MUSIC_WRITE
    rows.PushBack(std::move(row),alloc);
    }
  put(doc,"themes",std::move(rows),alloc);
  const auto path = section(base,program);
  fs::create_directories(path.parent_path());
  DefinitionJson::write(path,doc);
  }
