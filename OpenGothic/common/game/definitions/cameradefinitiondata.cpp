#include "cameradefinitiondata.h"

#include "definitionjson.h"

#include <bit>
#include <cmath>
#include <set>

namespace {
namespace fs = std::filesystem;
using namespace DefinitionJson;
constexpr std::string_view format = "gothic-camera-definitions";
constexpr int version = 1;

void readField(const Json& value, int32_t& field) {
  field = integer(value);
  }

void readField(const Json& value, float& field) {
  require(value.IsUint(),"camera definitions: expected float bits");
  field = std::bit_cast<float>(value.GetUint());
  require(std::isfinite(field),"camera definitions: non-finite float");
  }

Json writeField(int32_t field) {
  return Json(field);
  }

Json writeField(float field) {
  require(std::isfinite(field),"camera definitions: non-finite float");
  return Json(std::bit_cast<uint32_t>(field));
  }
}

fs::path CameraDefinitionData::section(const fs::path& base, std::string_view program) {
  return base/"definitions"/program/"camera.json";
  }

CameraDefinitionData::Presets CameraDefinitionData::read(const fs::path& base, std::string_view program) {
  const auto path = section(base,program);
  const auto doc = DefinitionJson::read(path);
  object(doc,{"format","version","program_sha256","presets"});
  require(string(doc["format"])==format && integer(doc["version"])==version &&
          string(doc["program_sha256"])==program,"camera definitions: invalid format or program in "+path.string());

  const auto& rows = doc["presets"];
  require(rows.IsArray() && rows.Size()==stockPresetCount,"camera definitions: invalid preset table");
  Presets result;
  result.reserve(rows.Size());
  std::set<std::string> names;
  for(const auto& row : rows.GetArray()) {
#define CAMERA_KEY(type, field, extent, symbol) #field,
    object(row,{"name", DEF_CAMERA_FIELDS(CAMERA_KEY)});
#undef CAMERA_KEY
    auto name = bytes(row["name"]);
    require(!name.empty() && names.insert(name).second,"camera definitions: invalid preset name "+name);
    Def::Camera camera{};
#define CAMERA_READ(type, field, extent, symbol) readField(row[#field],camera.field);
    DEF_CAMERA_FIELDS(CAMERA_READ)
#undef CAMERA_READ
    result.emplace_back(std::move(name),camera);
    }
  return result;
  }

void CameraDefinitionData::write(const fs::path& base, std::string_view program, const Presets& presets) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text(format,alloc),alloc);
  put(doc,"version",Json(version),alloc);
  put(doc,"program_sha256",text(program,alloc),alloc);
  Json rows(rapidjson::kArrayType);
  for(const auto& [name,camera] : presets) {
    Json row(rapidjson::kObjectType);
    put(row,"name",text(encode(name),alloc),alloc);
#define CAMERA_WRITE(type, field, extent, symbol) put(row,#field,writeField(camera.field),alloc);
    DEF_CAMERA_FIELDS(CAMERA_WRITE)
#undef CAMERA_WRITE
    rows.PushBack(std::move(row),alloc);
    }
  put(doc,"presets",std::move(rows),alloc);
  const auto path = section(base,program);
  fs::create_directories(path.parent_path());
  DefinitionJson::write(path,doc);
  }
