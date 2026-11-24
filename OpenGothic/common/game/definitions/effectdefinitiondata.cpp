#include "effectdefinitiondata.h"

#include "definitionjson.h"

#include <bit>
#include <cmath>
#include <set>
#include <type_traits>

namespace {
namespace fs = std::filesystem;
using namespace DefinitionJson;
using EffectDefinitionData::Entry;
constexpr std::string_view particleFormat = "gothic-particle-effects";
constexpr std::string_view visualFormat = "gothic-visual-effects";
constexpr int version = 1;

void readField(const Json& value, std::string& field) {
  field = bytes(value);
  }

void readField(const Json& value, int32_t& field) {
  field = integer(value);
  }

void readField(const Json& value, float& field) {
  require(value.IsUint(),"effect definitions: expected float bits");
  field = std::bit_cast<float>(value.GetUint());
  require(std::isfinite(field),"effect definitions: non-finite float");
  }

template<size_t N>
void readField(const Json& value, std::string (&field)[N]) {
  require(value.IsArray() && value.Size()==N,"effect definitions: invalid USERSTRING array");
  for(size_t i=0; i<N; ++i)
    field[i] = bytes(value[rapidjson::SizeType(i)]);
  }

Json writeField(std::string_view field, Allocator& alloc) {
  return text(encode(field),alloc);
  }

Json writeField(int32_t field, Allocator&) {
  return Json(field);
  }

Json writeField(float field, Allocator&) {
  require(std::isfinite(field),"effect definitions: non-finite float");
  return Json(std::bit_cast<uint32_t>(field));
  }

template<size_t N>
Json writeField(const std::string (&field)[N], Allocator& alloc) {
  Json array(rapidjson::kArrayType);
  for(const auto& value : field)
    array.PushBack(writeField(value,alloc),alloc);
  return array;
  }

template<class T>
void checkEntry(const Json& row) {
#define EFFECT_KEY(type, field, extent, symbol) #field,
  if constexpr (std::is_same_v<T,Def::ParticleEffect>)
    object(row,{"id","name", DEF_PARTICLE_EFFECT_FIELDS(EFFECT_KEY)});
  else if constexpr (std::is_same_v<T,Def::EffectBase>)
    object(row,{"id","name", DEF_EFFECT_BASE_FIELDS(EFFECT_KEY)});
  else
    object(row,{"id","name", DEF_PARTICLE_EFFECT_EMIT_KEY_FIELDS(EFFECT_KEY)});
#undef EFFECT_KEY
  }

template<class T, class Visit>
void visitFields(T& value, Visit&& visit) {
  using Record = std::remove_const_t<T>;
#define EFFECT_FIELD(type, field, extent, symbol) visit(#field,value.field);
  if constexpr (std::is_same_v<Record,Def::ParticleEffect>) {
    DEF_PARTICLE_EFFECT_FIELDS(EFFECT_FIELD)
    }
  else if constexpr (std::is_same_v<Record,Def::EffectBase>) {
    DEF_EFFECT_BASE_FIELDS(EFFECT_FIELD)
    }
  else {
    DEF_PARTICLE_EFFECT_EMIT_KEY_FIELDS(EFFECT_FIELD)
    }
#undef EFFECT_FIELD
  }

struct SeenEntries {
  std::set<uint32_t> ids;
  std::set<std::string> names;
  };

template<class T>
std::vector<Entry<T>> readEntries(const Json& rows, size_t expectedCount, SeenEntries& seen) {
  require(rows.IsArray() && rows.Size()==expectedCount,"effect definitions: invalid table");
  std::vector<Entry<T>> result;
  result.reserve(rows.Size());
  for(const auto& row : rows.GetArray()) {
    checkEntry<T>(row);
    auto name = bytes(row["name"]);
    require(!name.empty() && name.find_first_of("abcdefghijklmnopqrstuvwxyz")==std::string::npos &&
            seen.names.insert(name).second,"effect definitions: invalid name "+name);
    require(row["id"].IsUint(),"effect definitions: invalid symbol ID for "+name);
    const auto id = row["id"].GetUint();
    require((result.empty() || result.back().id<id) && seen.ids.insert(id).second,
            "effect definitions: duplicate or unordered symbol ID for "+name);
    T value{};
    visitFields(value,[&](const char* field, auto& out) { readField(row[field],out); });
    result.push_back({id,std::move(name),std::move(value)});
    }
  return result;
  }

template<class T>
Json writeEntries(const std::vector<Entry<T>>& entries, Allocator& alloc) {
  Json rows(rapidjson::kArrayType);
  for(const auto& [id,name,value] : entries) {
    Json row(rapidjson::kObjectType);
    put(row,"id",Json(id),alloc);
    put(row,"name",text(encode(name),alloc),alloc);
    visitFields(value,[&](const char* field, const auto& in) { put(row,field,writeField(in,alloc),alloc); });
    rows.PushBack(std::move(row),alloc);
    }
  return rows;
  }

void checkHeader(const Json& doc, std::string_view format, std::string_view program, const fs::path& path) {
  require(string(doc["format"])==format && integer(doc["version"])==version &&
          string(doc["program_sha256"])==program,"effect definitions: invalid format or program in "+path.string());
  }

rapidjson::Document makeCatalog(std::string_view format, std::string_view program) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text(format,alloc),alloc);
  put(doc,"version",Json(version),alloc);
  put(doc,"program_sha256",text(program,alloc),alloc);
  return doc;
  }

void writeCatalog(const fs::path& path, const Json& doc) {
  fs::create_directories(path.parent_path());
  DefinitionJson::write(path,doc);
  }
}

fs::path EffectDefinitionData::particleSection(const fs::path& base, std::string_view program) {
  return base/"definitions"/program/"particle-effects.json";
  }

fs::path EffectDefinitionData::visualSection(const fs::path& base, std::string_view program) {
  return base/"definitions"/program/"visual-effects.json";
  }

EffectDefinitionData::Particles EffectDefinitionData::readParticles(const fs::path& base, std::string_view program) {
  const auto path = particleSection(base,program);
  const auto doc = DefinitionJson::read(path);
  object(doc,{"format","version","program_sha256","effects"});
  checkHeader(doc,particleFormat,program,path);
  SeenEntries seen;
  return readEntries<Def::ParticleEffect>(doc["effects"],stockParticleCount,seen);
  }

EffectDefinitionData::Visuals EffectDefinitionData::readVisuals(const fs::path& base, std::string_view program) {
  const auto path = visualSection(base,program);
  const auto doc = DefinitionJson::read(path);
  object(doc,{"format","version","program_sha256","effects","keys"});
  checkHeader(doc,visualFormat,program,path);
  SeenEntries seen;
  return {readEntries<Def::EffectBase>(doc["effects"],stockVisualCount,seen),
          readEntries<Def::ParticleEffectEmitKey>(doc["keys"],stockKeyCount,seen)};
  }

void EffectDefinitionData::writeParticles(const fs::path& base, std::string_view program, const Particles& data) {
  auto doc = makeCatalog(particleFormat,program);
  auto& alloc = doc.GetAllocator();
  put(doc,"effects",writeEntries(data,alloc),alloc);
  writeCatalog(particleSection(base,program),doc);
  }

void EffectDefinitionData::writeVisuals(const fs::path& base, std::string_view program, const Visuals& data) {
  auto doc = makeCatalog(visualFormat,program);
  auto& alloc = doc.GetAllocator();
  put(doc,"effects",writeEntries(data.effects,alloc),alloc);
  put(doc,"keys",writeEntries(data.keys,alloc),alloc);
  writeCatalog(visualSection(base,program),doc);
  }
