#include "placementdefinitions.h"

#include "definitionjson.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
using Json = rapidjson::Value;
using DefinitionJson::require;
using DefinitionJson::at;
using DefinitionJson::string;
using DefinitionJson::bytes;
PlacementDefinitions::Value value(const Json& json) {
  using Value = PlacementDefinitions::Value;
  Value result;
  const auto type=string(at(json,"type"));
  if(type=="self" || type=="null") {
    require(json.MemberCount()==1,"unexpected value field");
    result.kind=type=="self" ? Value::Kind::Self : Value::Kind::Null;
    return result;
    }
  const auto& data=at(json,"value");
  require(json.MemberCount()==2,"unexpected value field");
  if(type=="int") {
    require(data.IsInt(),"expected int32 value");
    result.kind=Value::Kind::Int;
    result.integer=data.GetInt();
    }
  else if(type=="float") {
    require(data.IsNumber(),"expected float value");
    const auto number=data.GetDouble();
    require(std::isfinite(number) && std::abs(number)<=std::numeric_limits<float>::max(),
            "float value out of range");
    result.kind=Value::Kind::Float;
    result.real=float(number);
    }
  else if(type=="string") {
    result.kind=Value::Kind::String;
    result.text=bytes(data);
    }
  else if(type=="symbol") {
    result.kind=Value::Kind::Symbol;
    result.text=string(data);
    require(!result.text.empty(),"empty symbol value");
    }
  else throw std::runtime_error("unsupported value type "+type);
  return result;
  }
PlacementDefinitions::Step step(const Json& json) {
  require(json.IsObject(),"step must be an object");
  if(json.HasMember("global")) {
    PlacementDefinitions::GlobalWrite write;
    write.symbol=string(at(json,"global"));
    require(!write.symbol.empty(),"empty global symbol");
    const auto& index=at(json,"index");
    require(index.IsUint() && index.GetUint()<=std::numeric_limits<uint16_t>::max(),"expected uint16 global index");
    write.index=uint16_t(index.GetUint());
    write.value=value(at(json,"value"));
    using Kind=PlacementDefinitions::Value::Kind;
    require(write.value.kind==Kind::Int || write.value.kind==Kind::Float || write.value.kind==Kind::String,
            "global write requires an int, float or string value");
    require(json.MemberCount()==3,"unexpected global write step member");
    return write;
    }
  PlacementDefinitions::Call call;
  call.function=string(at(json,"function"));
  require(!call.function.empty(),"empty function symbol");
  const auto& deferred=at(json,"deferred");
  require(deferred.IsBool(),"deferred must be a boolean");
  call.deferred=deferred.GetBool();
  const auto& arguments=at(json,"arguments");
  require(arguments.IsArray(),"arguments must be an array");
  for(const auto& argument:arguments.GetArray()) {
    try { call.args.push_back(value(argument)); }
    catch(const std::exception& e) { throw std::runtime_error("argument "+std::to_string(call.args.size()+1)+": "+e.what()); }
    }
  require(json.MemberCount()==3,"unexpected call step member");
  return call;
  }

std::vector<PlacementDefinitions::LiteralInsert> inserts(const Json& rows,size_t size) {
  require(rows.IsArray() && rows.Size()==size,"unexpected placement count");
  std::vector<PlacementDefinitions::LiteralInsert> result;
  result.reserve(rows.Size());
  for(const auto& row:rows.GetArray()) {
    require(row.IsArray() && row.Size()==3 && row[0].IsUint() && row[1].IsUint() && row[2].IsUint(),
            "expected instance, point and external symbol indices");
    result.push_back({row[0].GetUint(),row[1].GetUint(),row[2].GetUint()});
    }
  return result;
  }

template<size_t N>
std::array<std::vector<PlacementDefinitions::LiteralInsert>,N> insertGroups(const Json& source,
                                                                           const std::array<size_t,N>& sizes) {
  require(source.IsArray() && source.Size()==N,"unexpected chapter placement group count");
  std::array<std::vector<PlacementDefinitions::LiteralInsert>,N> groups;
  for(rapidjson::SizeType i=0;i<source.Size();++i)
    groups[i]=inserts(source[i],sizes[i]);
  return groups;
}

uint32_t symbolId(const Json& value) {
  require(value.IsUint(),"expected a symbol index");
  return value.GetUint();
  }

std::vector<uint32_t> actorIds(const Json& actors,size_t count) {
  require(actors.IsArray() && actors.Size()==count,"unexpected first-entry actor count");
  std::vector<uint32_t> result;
  result.reserve(count);
  for(const auto& actor:actors.GetArray())
    result.push_back(symbolId(actor));
  return result;
  }

PlacementDefinitions::RoomAssignment roomAssignment(const Json& row) {
  require(row.IsArray() && row.Size()==2,"expected a room and its guild");
  return {symbolId(row[0]),row[1].IsNull() ? std::nullopt : std::optional(symbolId(row[1]))};
  }

PlacementDefinitions::RoomInitializer roomInitializer(const Json& row,const PlacementDefinitions::RoomInitializerShape& shape) {
  using namespace PlacementDefinitions;
  DefinitionJson::object(row,{"mobs","rooms"});
  const auto& mobs=at(row,"mobs");
  const auto& rooms=at(row,"rooms");
  require(mobs.IsArray() && mobs.Size()==shape.mobs && rooms.IsArray() && rooms.Size()==shape.rooms,
          "unexpected sizes for room initializer "+std::string(shape.name));
  RoomInitializer result;
  for(const auto& mob:mobs.GetArray()) {
    require(mob.IsArray() && mob.Size()==4,"invalid room mob routine");
    result.mobs.push_back({DefinitionJson::integer(mob[0]),DefinitionJson::integer(mob[1]),symbolId(mob[2]),DefinitionJson::integer(mob[3])});
    }
  size_t choices=0;
  for(const auto& room:rooms.GetArray()) {
    require(room.IsArray(),"expected a room assignment or choice");
    if(room.Size()==4) {
      result.rooms.emplace_back(RoomChoice{symbolId(room[0]),symbolId(room[1]),roomAssignment(room[2]),roomAssignment(room[3])});
      ++choices;
      }
    else
      result.rooms.emplace_back(roomAssignment(room));
    }
  require(choices==shape.choices,"unexpected choice count in room initializer "+std::string(shape.name));
  return result;
  }
}

PlacementDefinitions::Library PlacementDefinitions::read(const std::filesystem::path& base,
                                            std::string_view programSha256) {
  const auto file=base/"definitions"/programSha256/"placement.json";
  try {
    std::ifstream input(file,std::ios::binary);
    require(bool(input),"cannot open file");
    const std::string text{std::istreambuf_iterator<char>(input),{}};
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    require(!doc.HasParseError(),"invalid JSON");
    require(string(at(doc,"format"))=="gothic-placement-definitions","unsupported format");
    const auto version=DefinitionJson::integer(at(doc,"version"));
    require(version>=1 && version<=6,"unsupported version");
    require(!string(at(doc,"program_name")).empty(),"empty program name");
    require(string(at(doc,"program_sha256"))==programSha256,"program SHA differs");
    require(string(at(doc,"string_encoding"))=="unicode-byte-mapping","unsupported string encoding");
    const auto& records=at(doc,"records");
    require(records.IsArray(),"records must be an array");
    constexpr std::array fields{6u,7u,9u,11u,13u,14u};
    require(doc.MemberCount()==fields[size_t(version-1)],"unexpected envelope field");
    Library result;
    if(version>=2) {
      const auto& chapters=at(doc,"khorinis_late_chapters");
      DefinitionJson::object(chapters,{"chapter_4","chapter_5"});
      result.khorinisLateChapters=KhorinisLateChapters{
        insertGroups(at(chapters,"chapter_4"),KhorinisLateChapters::chapter4Sizes),
        insertGroups(at(chapters,"chapter_5"),KhorinisLateChapters::chapter5Sizes)};
      }
    if(version>=3) {
      result.addonChapter4=inserts(at(doc,"addon_chapter_4"),Library::addonChapter4Count);
      const auto& functions=at(doc,"startup_functions");
      require(functions.IsArray() && !functions.Empty(),"expected startup function indices");
      auto& ids=result.startupFunctions.emplace();
      ids.reserve(functions.Size());
      for(const auto& function:functions.GetArray()) {
        require(function.IsUint() && (ids.empty() || ids.back()<function.GetUint()),"expected sorted unique startup function indices");
        ids.push_back(function.GetUint());
        }
      }
    if(version>=4) {
      const auto& chapters=at(doc,"old_world_chapters");
      DefinitionJson::object(chapters,{"chapter_3","chapter_4"});
      result.oldWorldChapters=OldWorldChapters{
        insertGroups(at(chapters,"chapter_3"),OldWorldChapters::chapter3Sizes),
        insertGroups(at(chapters,"chapter_4"),OldWorldChapters::chapter4Sizes)};
      const auto& island=at(doc,"dragon_island");
      require(island.IsArray() && !island.Empty(),"expected Dragon Island placements");
      result.dragonIsland=inserts(island,island.Size());
      }
    if(version>=5) {
      const auto& rooms=at(doc,"room_initializers");
      require(rooms.IsObject() && rooms.MemberCount()==std::size(roomInitializerShapes),"unexpected room initializer count");
      for(const auto& shape:roomInitializerShapes)
        result.roomInitializers.emplace(shape.name,roomInitializer(at(rooms,shape.name),shape));
      const auto& first=at(doc,"first_entries");
      DefinitionJson::object(first,{"old_world","addon_world"});
      result.firstEntries=FirstEntries{actorIds(at(first,"old_world"),FirstEntries::oldWorldCount),
                                       actorIds(at(first,"addon_world"),FirstEntries::addonWorldCount)};
      }
    if(version>=6) {
      const auto& chapters=at(doc,"khorinis_early_chapters");
      DefinitionJson::object(chapters,{"chapter_2","chapter_3"});
      result.khorinisEarlyChapters=KhorinisEarlyChapters{
        inserts(at(chapters,"chapter_2"),KhorinisEarlyChapters::chapter2Count),
        insertGroups(at(chapters,"chapter_3"),KhorinisEarlyChapters::chapter3Sizes)};
      }
    for(const auto& record:records.GetArray()) {
      const auto name=string(at(record,"symbol"));
      try {
        require(name.starts_with("STARTUP_") && result.placements.count(name)==0 && result.uncaptured.count(name)==0,
                "duplicate or empty symbol");
        const auto status=string(at(record,"status"));
        const auto& steps=at(record,"steps");
        require(steps.IsArray(),"steps must be an array");
        if(status=="uncaptured") {
          require(steps.Empty(),"uncaptured placement has steps");
          result.uncaptured.emplace(name,string(at(record,"reason"))+": "+string(at(record,"detail")));
          require(record.MemberCount()==5,"unexpected uncaptured placement field");
          continue;
          }
        require(status=="captured","unsupported capture status");
        require(record.MemberCount()==3,"unexpected captured placement field");
        Definition definition;
        for(const auto& json:steps.GetArray()) {
          try { definition.steps.push_back(step(json)); }
          catch(const std::exception& e) { throw std::runtime_error("step "+std::to_string(definition.steps.size()+1)+": "+e.what()); }
          }
        result.placements.emplace(name,std::move(definition));
        }
      catch(const std::exception& e) { throw std::runtime_error(name+": "+e.what()); }
      }
    return result;
    }
  catch(const std::exception& e) { throw std::runtime_error("placement definitions: "+file.string()+": "+e.what()); }
  }
