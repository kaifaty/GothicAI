#include "tradeinventorydata.h"
#include "gameplaydefinitions.h"

namespace {
using namespace DefinitionJson;
using namespace TradeInventoryData;
constexpr std::string_view format = "gothic-trade-inventory";
constexpr int version = 1;

std::filesystem::path section(const std::filesystem::path& base) {
  return base/"definitions"/GameplayDefinitions::stockProgram/"trade-inventory.json";
  }

uint32_t id(const Json& value) {
  require(value.IsUint(),"trade inventory: expected a symbol ID");
  return value.GetUint();
  }

const Json& row(const Json& value, rapidjson::SizeType fields) {
  require(value.IsArray() && value.Size()==fields,"trade inventory: invalid record row");
  return value;
  }

template<class Read>
auto readList(const Json& value, Read read) {
  require(value.IsArray(),"trade inventory: expected a list");
  std::vector<decltype(read(value))> result;
  result.reserve(value.Size());
  for(const auto& entry : value.GetArray())
    result.push_back(read(entry));
  return result;
  }

template<class T, class Write>
Json writeList(const std::vector<T>& values, Allocator& allocator, Write write) {
  Json result(rapidjson::kArrayType);
  for(const auto& value : values)
    result.PushBack(write(value,allocator),allocator);
  return result;
  }

template<class T>
Json optionalValue(const std::optional<T>& value) {
  return value ? Json(*value) : Json();
  }

Grant readGrant(const Json& value) {
  row(value,4);
  Grant result;
  result.recipient = id(value[0]);
  result.item = integer(value[1]);
  result.amount = integer(value[2]);
  if(!value[3].IsNull())
    result.itemSymbol = id(value[3]);
  return result;
  }

Json writeGrant(const Grant& value, Allocator& allocator) {
  Json result(rapidjson::kArrayType);
  result.PushBack(Json(value.recipient),allocator);
  result.PushBack(Json(value.item),allocator);
  result.PushBack(Json(value.amount),allocator);
  result.PushBack(optionalValue(value.itemSymbol),allocator);
  return result;
  }

Batch readBatch(const Json& value) {
  row(value,5);
  Batch result;
  if(!value[0].IsNull())
    result.granted = id(value[0]);
  if(!value[1].IsNull())
    result.minimumChapter = integer(value[1]);
  require(value[2].IsBool(),"trade inventory: invalid old-world condition");
  result.requiresOldWorld = value[2].GetBool();
  result.grants = readList(value[3],readGrant);
  if(!value[4].IsNull()) {
    const auto& bonus = row(value[4],3);
    const auto condition = integer(bonus[0]);
    using Condition=Bonus::Condition;
    require(condition==int(Condition::HeroGuild) || condition==int(Condition::SecretSign),
            "trade inventory: invalid stock bonus");
    result.bonus = Bonus{Condition(condition),readList(bonus[1],readGrant),readList(bonus[2],readGrant)};
    }
  return result;
  }

Json writeBatch(const Batch& value, Allocator& allocator) {
  Json result(rapidjson::kArrayType);
  result.PushBack(optionalValue(value.granted),allocator);
  result.PushBack(optionalValue(value.minimumChapter),allocator);
  result.PushBack(Json(value.requiresOldWorld),allocator);
  result.PushBack(writeList(value.grants,allocator,writeGrant),allocator);
  Json bonus;
  if(value.bonus) {
    bonus.SetArray();
    bonus.PushBack(Json(int(value.bonus->condition)),allocator);
    bonus.PushBack(writeList(value.bonus->grants,allocator,writeGrant),allocator);
    bonus.PushBack(writeList(value.bonus->following,allocator,writeGrant),allocator);
    }
  result.PushBack(bonus,allocator);
  return result;
  }

Stock readStock(const Json& value) {
  row(value,3);
  return Stock{id(value[0]),id(value[1]),readList(value[2],readBatch)};
  }

Json writeStock(const Stock& value, Allocator& allocator) {
  Json result(rapidjson::kArrayType);
  result.PushBack(Json(value.function),allocator);
  result.PushBack(Json(value.actor),allocator);
  result.PushBack(writeList(value.batches,allocator,writeBatch),allocator);
  return result;
  }
}

std::optional<TradeInventoryData::Record> TradeInventoryData::load(const std::filesystem::path& base) {
  const auto path = section(base);
  if(!std::filesystem::exists(path))
    return std::nullopt;
  const auto document = read(path);
  object(document,{"format","version","program_sha256","hero","runes","junk","traders","restocks","defeated","stock"});
  require(string(document["format"])==format && integer(document["version"])==version &&
          string(document["program_sha256"])==GameplayDefinitions::stockProgram,"trade inventory: invalid format or program");

  const auto remove = [](const Json& value) {
    row(value,2);
    return std::pair{integer(value[0]),integer(value[1])};
    };
  Record record;
  record.hero = integer(document["hero"]);
  record.runes = readList(document["runes"],remove);
  record.junk = readList(document["junk"],remove);
  record.traders = readList(document["traders"],[](const Json& value) {
    row(value,2);
    return std::pair{integer(value[0]),id(value[1])};
    });
  record.restocks = readList(document["restocks"],[](const Json& value) {
    row(value,2);
    return std::pair{id(value[0]),id(value[1])};
    });
  record.defeated = readList(document["defeated"],id);
  record.stock = readList(document["stock"],readStock);
  require(record.runes.size()==54 && record.junk.size()==73 && record.traders.size()==50 &&
          record.restocks.size()==50 && record.defeated.size()==50 && record.stock.size()==50,
          "trade inventory: incomplete stock records");
  return record;
  }

void TradeInventoryData::write(const std::filesystem::path& base, const Record& record) {
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  const auto writePair = [](const auto& value, Allocator& alloc) {
    Json result(rapidjson::kArrayType);
    result.PushBack(Json(value.first),alloc);
    result.PushBack(Json(value.second),alloc);
    return result;
    };
  put(document,"format",text(format,allocator),allocator);
  put(document,"version",Json(version),allocator);
  put(document,"program_sha256",text(GameplayDefinitions::stockProgram,allocator),allocator);
  put(document,"hero",Json(record.hero),allocator);
  put(document,"runes",writeList(record.runes,allocator,writePair),allocator);
  put(document,"junk",writeList(record.junk,allocator,writePair),allocator);
  put(document,"traders",writeList(record.traders,allocator,writePair),allocator);
  put(document,"restocks",writeList(record.restocks,allocator,writePair),allocator);
  put(document,"defeated",writeList(record.defeated,allocator,[](uint32_t value, Allocator&) { return Json(value); }),allocator);
  put(document,"stock",writeList(record.stock,allocator,writeStock),allocator);
  const auto path = section(base);
  std::filesystem::create_directories(path.parent_path());
  DefinitionJson::write(path,document);
  }
