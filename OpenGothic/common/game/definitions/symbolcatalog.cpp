#include "symbolcatalog.h"

#include "definitionjson.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace {
using namespace DefinitionJson;
using namespace ScriptSymbolCatalog;

constexpr std::string_view format = "gothic-script-symbols";
constexpr int version = 1;

std::filesystem::path section(const std::filesystem::path& base, std::string_view program) {
  return base/"definitions"/program/"symbols.json";
  }

}

Catalog::Catalog(std::vector<Entry> values) : entries(std::move(values)) {
  names.reserve(entries.size());
  for(uint32_t id = 0; id<entries.size(); ++id)
    names.insert_or_assign(entries[id].name,id);
  }

const Entry* Catalog::at(size_t id) const {
  return id<entries.size() ? &entries[id] : nullptr;
  }

std::optional<uint32_t> Catalog::find(std::string_view name) const {
  std::string upper(name);
  std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char ch) { return char(std::toupper(ch)); });
  const auto entry = names.find(upper);
  if(entry==names.end())
    return std::nullopt;
  return entry->second;
  }

std::unique_ptr<Catalog> ScriptSymbolCatalog::load(const std::filesystem::path& base, std::string_view program) {
  const auto path = section(base,program);
  if(!std::filesystem::exists(path))
    return {};
  const auto document = DefinitionJson::read(path);
  object(document,{"format","version","program_sha256","symbols"});
  require(string(document["format"])==format && integer(document["version"])==version &&
          string(document["program_sha256"])==program,"symbol catalog: invalid format or source in "+path.string());

  const auto& rows = document["symbols"];
  require(rows.IsArray() && !rows.Empty(),"symbol catalog: missing symbols");
  std::vector<Entry> entries;
  entries.reserve(rows.Size());
  for(const auto& row : rows.GetArray()) {
    require(row.IsArray() && row.Size()==2,"symbol catalog: invalid symbol row");
    const auto kind = integer(row[1]);
    require(kind>=int(Kind::Other) && kind<=int(Kind::Function),"symbol catalog: invalid symbol kind");
    entries.push_back({bytes(row[0]),Kind(kind)});
    }
  return std::make_unique<Catalog>(std::move(entries));
  }

void ScriptSymbolCatalog::write(const std::filesystem::path& base, std::string_view program, std::span<const Entry> entries) {
  rapidjson::Document document(rapidjson::kObjectType);
  auto& alloc = document.GetAllocator();
  put(document,"format",text(format,alloc),alloc);
  put(document,"version",Json(version),alloc);
  put(document,"program_sha256",text(program,alloc),alloc);
  Json rows(rapidjson::kArrayType);
  for(const auto& entry : entries) {
    Json row(rapidjson::kArrayType);
    row.PushBack(text(encode(entry.name),alloc),alloc);
    row.PushBack(Json(int(entry.kind)),alloc);
    rows.PushBack(std::move(row),alloc);
    }
  put(document,"symbols",std::move(rows),alloc);
  const auto path = section(base,program);
  std::filesystem::create_directories(path.parent_path());
  DefinitionJson::write(path,document);
  }
