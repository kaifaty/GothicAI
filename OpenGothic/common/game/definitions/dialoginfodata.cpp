#include "dialoginfodata.h"
#include "gameplaydefinitions.h"

namespace {
using namespace DefinitionJson;
constexpr std::string_view format = "gothic-dialog-infos";
constexpr int version = 1;
constexpr std::string_view ambientFormat = "gothic-ambient-dialog-infos";

std::filesystem::path ambientSection(const std::filesystem::path& base) {
  return DialogInfoData::section(base).parent_path()/"ambient-dialog-infos.json";
  }

uint32_t id(const Json& value) {
  require(value.IsUint(),"dialog infos: expected a symbol ID");
  return value.GetUint();
  }
}

std::filesystem::path DialogInfoData::section(const std::filesystem::path& base) {
  return base/"definitions"/GameplayDefinitions::stockProgram/"dialog-infos.json";
  }

std::optional<DialogInfoData::Rows> DialogInfoData::load(const std::filesystem::path& base) {
  const auto path = section(base);
  if(!std::filesystem::exists(path))
    return std::nullopt;

  const auto document = read(path);
  object(document,{"format","version","program_sha256","instances"});
  require(string(document["format"])==format && integer(document["version"])==version &&
          string(document["program_sha256"])==GameplayDefinitions::stockProgram && document["instances"].IsArray(),
          "dialog infos: invalid format or program in "+path.string());

  Rows rows;
  for(const auto& row : document["instances"].GetArray()) {
    require(row.IsArray() && row.Size()==2 && row[1].IsArray(),"dialog infos: invalid instance row");
    auto [entry,unique] = rows.try_emplace(id(row[0]));
    require(unique,"dialog infos: duplicate instance");
    for(const auto& field : row[1].GetArray()) {
      require(field.IsArray() && field.Size()==2,"dialog infos: invalid member write");
      auto& value = entry->second.emplace_back().value;
      entry->second.back().member = id(field[0]);
      const auto& data = field[1];
      if(data.IsInt())
        value = integer(data);
      else if(data.IsString())
        value = bytes(data);
      else {
        require(data.IsArray() && data.Size()==3,"dialog infos: invalid lesson");
        value = Lesson{integer(data[0]),integer(data[1]),bytes(data[2])};
        }
      }
    }
  return rows;
  }

void DialogInfoData::write(const std::filesystem::path& base, const Rows& rows) {
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  Json instances(rapidjson::kArrayType);
  for(const auto& [symbol,fields] : rows) {
    Json writes(rapidjson::kArrayType);
    for(const auto& field : fields) {
      Json member(rapidjson::kArrayType);
      member.PushBack(Json(field.member),allocator);
      if(const auto* number = std::get_if<int32_t>(&field.value))
        member.PushBack(Json(*number),allocator);
      else if(const auto* title = std::get_if<std::string>(&field.value))
        member.PushBack(text(encode(*title),allocator),allocator);
      else {
        const auto& lesson = std::get<Lesson>(field.value);
        Json value(rapidjson::kArrayType);
        value.PushBack(Json(lesson.talent),allocator);
        value.PushBack(Json(lesson.skill),allocator);
        value.PushBack(text(encode(lesson.title),allocator),allocator);
        member.PushBack(value,allocator);
        }
      writes.PushBack(member,allocator);
      }
    Json row(rapidjson::kArrayType);
    row.PushBack(Json(symbol),allocator);
    row.PushBack(writes,allocator);
    instances.PushBack(row,allocator);
    }

  put(document,"format",text(format,allocator),allocator);
  put(document,"version",Json(version),allocator);
  put(document,"program_sha256",text(GameplayDefinitions::stockProgram,allocator),allocator);
  put(document,"instances",std::move(instances),allocator);
  const auto path = section(base);
  std::filesystem::create_directories(path.parent_path());
  DefinitionJson::write(path,document);
  }

std::optional<DialogInfoData::AmbientBindings> DialogInfoData::loadAmbient(const std::filesystem::path& base) {
  const auto path = ambientSection(base);
  if(!std::filesystem::exists(path))
    return std::nullopt;
  try {
    const auto document = read(path);
    object(document,{"format","version","program_sha256","bindings"});
    require(string(document["format"])==ambientFormat && integer(document["version"])==version &&
            string(document["program_sha256"])==GameplayDefinitions::stockProgram &&
            document["bindings"].IsObject(),"invalid ambient dialog format");
    AmbientBindings bindings;
    for(const auto& entry : document["bindings"].GetObject()) {
      const auto name = bytes(entry.name);
      object(entry.value,{"actor","topics"});
      require(name.starts_with("B_ASSIGNAMBIENTINFOS_") && entry.value["topics"].IsArray(),
              "invalid ambient dialog binding");
      AmbientBinding binding;
      binding.actor = id(entry.value["actor"]);
      for(const auto& topic : entry.value["topics"].GetArray())
        binding.topics.push_back(id(topic));
      require(bindings.emplace(name,std::move(binding)).second,"duplicate ambient dialog binding");
      }
    require(!bindings.empty(),"missing ambient dialog bindings");
    return bindings;
    }
  catch(const std::runtime_error& error) {
    throw std::runtime_error(path.string()+": "+error.what());
    }
  }

void DialogInfoData::writeAmbient(const std::filesystem::path& base, const AmbientBindings& bindings) {
  rapidjson::Document document(rapidjson::kObjectType);
  auto& alloc = document.GetAllocator();
  put(document,"format",text(ambientFormat,alloc),alloc);
  put(document,"version",Json(version),alloc);
  put(document,"program_sha256",text(GameplayDefinitions::stockProgram,alloc),alloc);
  Json records(rapidjson::kObjectType);
  for(const auto& [name,binding] : bindings) {
    Json row(rapidjson::kObjectType);
    put(row,"actor",Json(binding.actor),alloc);
    Json topics(rapidjson::kArrayType);
    for(auto topic : binding.topics)
      topics.PushBack(Json(topic),alloc);
    put(row,"topics",std::move(topics),alloc);
    records.AddMember(text(encode(name),alloc),std::move(row),alloc);
    }
  put(document,"bindings",std::move(records),alloc);
  const auto path = ambientSection(base);
  std::filesystem::create_directories(path.parent_path());
  DefinitionJson::write(path,document);
  }
