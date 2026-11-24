#include "deathtrophydefinitions.h"

#include "definitionjson.h"
#include "gameplaydefinitions.h"

namespace {
using namespace DefinitionJson;
using namespace DeathTrophyDefinitions;
constexpr std::string_view format = "gothic-death-trophies";
constexpr int version = 1;
constexpr std::array matchNames{"instance","actor_species","self_species"};

std::filesystem::path section(const std::filesystem::path& base) {
  return base/"definitions"/GameplayDefinitions::stockProgram/"death-trophies.json";
  }

Trophies readTrophies(const Json& rows, bool allowChance, size_t& grantCount) {
  require(rows.IsArray() && !rows.Empty(),"expected death trophy rules");
  Trophies trophies;
  for(const auto& row : rows.GetArray()) {
    object(row,{"match","symbol","chance","grants"});
    const auto match = string(row["match"]);
    const auto kind = std::find(matchNames.begin(),matchNames.end(),match);
    require(kind!=matchNames.end() && row["symbol"].IsUint(),"invalid death trophy match");
    Trophy trophy;
    trophy.match = Match(kind-matchNames.begin());
    trophy.symbol = row["symbol"].GetUint();
    const auto& chance = row["chance"];
    if(!chance.IsNull()) {
      require(allowChance && chance.IsArray() && chance.Size()==2 && chance[1].IsBool(),
              "invalid death trophy chance");
      trophy.chance = Chance{integer(chance[0]),chance[1].GetBool()};
      }
    const auto& grants = row["grants"];
    require(grants.IsArray() && !grants.Empty(),"expected death trophy grants");
    for(const auto& grant : grants.GetArray()) {
      require(grant.IsArray() && grant.Size()==2,"invalid death trophy grant");
      const auto item = integer(grant[0]);
      const auto amount = integer(grant[1]);
      require(item>=0 && amount>0,"invalid death trophy item or amount");
      trophy.grants.push_back({item,amount});
      }
    grantCount += trophy.grants.size();
    trophies.push_back(std::move(trophy));
    }
  return trophies;
  }

Json encodeTrophies(const Trophies& trophies, Allocator& allocator) {
  Json rows(rapidjson::kArrayType);
  for(const auto& trophy : trophies) {
    Json row(rapidjson::kObjectType);
    put(row,"match",text(matchNames[size_t(trophy.match)],allocator),allocator);
    put(row,"symbol",Json(trophy.symbol),allocator);
    Json chance;
    if(trophy.chance) {
      chance.SetArray();
      chance.PushBack(Json(trophy.chance->threshold),allocator);
      chance.PushBack(Json(trophy.chance->exact),allocator);
      }
    put(row,"chance",std::move(chance),allocator);
    Json grants(rapidjson::kArrayType);
    for(const auto& grant : trophy.grants) {
      Json item(rapidjson::kArrayType);
      item.PushBack(Json(grant.item),allocator);
      item.PushBack(Json(grant.amount),allocator);
      grants.PushBack(item,allocator);
      }
    put(row,"grants",std::move(grants),allocator);
    rows.PushBack(row,allocator);
    }
  return rows;
  }
}

std::optional<Catalog> DeathTrophyDefinitions::load(const std::filesystem::path& base) {
  const auto path = section(base);
  if(!std::filesystem::exists(path))
    return std::nullopt;

  try {
    const auto document = DefinitionJson::read(path);
    object(document,{"format","version","program_sha256","taught","unconditional","randomized"});
    require(string(document["format"])==format && integer(document["version"])==version &&
            string(document["program_sha256"])==GameplayDefinitions::stockProgram,
            "invalid death trophy format or stock program");

    Catalog catalog;
    size_t grantCount = 0;
    const auto& taught = document["taught"];
    require(taught.IsArray() && taught.Size()==catalog.taught.size(),"invalid death trophy talent groups");
    for(rapidjson::SizeType i=0; i<taught.Size(); ++i) {
      const auto& row = taught[i];
      object(row,{"index","trophies"});
      const auto index = integer(row["index"]);
      require(index>=0 && index<=UINT16_MAX,"invalid death trophy talent index");
      catalog.taught[i] = {uint16_t(index),readTrophies(row["trophies"],false,grantCount)};
      }
    catalog.unconditional = readTrophies(document["unconditional"],false,grantCount);

    const auto& randomized = document["randomized"];
    require(randomized.IsArray() && randomized.Size()==catalog.randomized.size(),"invalid death trophy RNG groups");
    for(rapidjson::SizeType i=0; i<randomized.Size(); ++i) {
      const auto& row = randomized[i];
      object(row,{"bound","result","choices"});
      const auto bound = integer(row["bound"]);
      const auto& choices = row["choices"];
      require(bound>0 && row["result"].IsUint() && choices.IsArray() && choices.Size()==(i==2 ? 5u : 1u),
              "invalid death trophy RNG draw");
      auto& draw = catalog.randomized[i];
      draw.bound = bound;
      draw.result = row["result"].GetUint();
      for(const auto& group : choices.GetArray())
        draw.choices.push_back(readTrophies(group,true,grantCount));
      }
    require(grantCount==123,"incomplete stock death trophy grants");
    return catalog;
    } catch(const std::runtime_error& error) {
    throw std::runtime_error(path.string()+": "+error.what());
    }
  }

void DeathTrophyDefinitions::write(const std::filesystem::path& base, const Catalog& catalog) {
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  put(document,"format",text(format,allocator),allocator);
  put(document,"version",Json(version),allocator);
  put(document,"program_sha256",text(GameplayDefinitions::stockProgram,allocator),allocator);
  Json taught(rapidjson::kArrayType);
  for(const auto& talent : catalog.taught) {
    Json row(rapidjson::kObjectType);
    put(row,"index",Json(talent.index),allocator);
    put(row,"trophies",encodeTrophies(talent.trophies,allocator),allocator);
    taught.PushBack(row,allocator);
    }
  put(document,"taught",std::move(taught),allocator);
  put(document,"unconditional",encodeTrophies(catalog.unconditional,allocator),allocator);
  Json randomized(rapidjson::kArrayType);
  for(const auto& draw : catalog.randomized) {
    Json row(rapidjson::kObjectType);
    put(row,"bound",Json(draw.bound),allocator);
    put(row,"result",Json(draw.result),allocator);
    Json choices(rapidjson::kArrayType);
    for(const auto& group : draw.choices)
      choices.PushBack(encodeTrophies(group,allocator),allocator);
    put(row,"choices",std::move(choices),allocator);
    randomized.PushBack(row,allocator);
    }
  put(document,"randomized",std::move(randomized),allocator);
  const auto path = section(base);
  std::filesystem::create_directories(path.parent_path());
  DefinitionJson::write(path,document);
  }
