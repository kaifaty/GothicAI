#include "sfxdefinitiondata.h"

#include "definitionjson.h"

#include <bit>
#include <cmath>
#include <set>

namespace {
namespace fs = std::filesystem;
using namespace DefinitionJson;
constexpr std::string_view format = "gothic-sfx-definitions";
constexpr int version = 1;

void readField(const Json& value, std::string& field) {
  field = bytes(value);
  }

void readField(const Json& value, int32_t& field) {
  field = integer(value);
  }

void readField(const Json& value, float& field) {
  require(value.IsUint(),"sfx definitions: expected float bits");
  field = std::bit_cast<float>(value.GetUint());
  require(std::isfinite(field),"sfx definitions: non-finite float");
  }

Json writeField(std::string_view field, Allocator& alloc) {
  return text(encode(field),alloc);
  }

Json writeField(int32_t field, Allocator&) {
  return Json(field);
  }

Json writeField(float field, Allocator&) {
  require(std::isfinite(field),"sfx definitions: non-finite float");
  return Json(std::bit_cast<uint32_t>(field));
  }
}

fs::path SfxDefinitionData::section(const fs::path& base, std::string_view program) {
  return base/"definitions"/program/"sfx.json";
  }

SfxDefinitionData::Presets SfxDefinitionData::read(const fs::path& base, std::string_view program) {
  const auto path = section(base,program);
  const auto doc = DefinitionJson::read(path);
  object(doc,{"format","version","program_sha256","presets"});
  require(string(doc["format"])==format && integer(doc["version"])==version &&
          string(doc["program_sha256"])==program,"sfx definitions: invalid format or program in "+path.string());

  const auto& rows = doc["presets"];
  require(rows.IsArray() && rows.Size()==stockPresetCount,"sfx definitions: invalid preset table");
  Presets result;
  result.reserve(rows.Size());
  std::set<std::string> names;
  for(const auto& row : rows.GetArray()) {
#define SFX_KEY(type, field, extent, symbol) #field,
    object(row,{"name", DEF_SOUND_EFFECT_FIELDS(SFX_KEY)});
#undef SFX_KEY
    auto name = bytes(row["name"]);
    require(!name.empty() && names.insert(name).second,"sfx definitions: invalid preset name "+name);
    Def::SoundEffect sfx{};
#define SFX_READ(type, field, extent, symbol) readField(row[#field],sfx.field);
    DEF_SOUND_EFFECT_FIELDS(SFX_READ)
#undef SFX_READ
    result.emplace_back(std::move(name),std::move(sfx));
    }
  return result;
  }

void SfxDefinitionData::write(const fs::path& base, std::string_view program, const Presets& presets) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text(format,alloc),alloc);
  put(doc,"version",Json(version),alloc);
  put(doc,"program_sha256",text(program,alloc),alloc);
  Json rows(rapidjson::kArrayType);
  for(const auto& [name,sfx] : presets) {
    Json row(rapidjson::kObjectType);
    put(row,"name",text(encode(name),alloc),alloc);
#define SFX_WRITE(type, field, extent, symbol) put(row,#field,writeField(sfx.field,alloc),alloc);
    DEF_SOUND_EFFECT_FIELDS(SFX_WRITE)
#undef SFX_WRITE
    rows.PushBack(std::move(row),alloc);
    }
  put(doc,"presets",std::move(rows),alloc);
  const auto path = section(base,program);
  fs::create_directories(path.parent_path());
  DefinitionJson::write(path,doc);
  }
