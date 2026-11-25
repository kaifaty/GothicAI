#include "fightdefinitiondata.h"

#include "definitionjson.h"

#include <iterator>

namespace {
namespace fs = std::filesystem;
using namespace DefinitionJson;
constexpr std::string_view format = "gothic-fight-definitions";
constexpr int version = 1;
}

fs::path FightDefinitions::section(const fs::path& base, std::string_view program) {
  return base/"definitions"/program/"fight.json";
  }

std::vector<FightTactic> FightDefinitions::read(const fs::path& base, std::string_view program) {
  const auto path = section(base,program);
  const auto doc = DefinitionJson::read(path);
  object(doc,{"format","version","program_sha256","tactics"});
  require(string(doc["format"])==format && integer(doc["version"])==version &&
          string(doc["program_sha256"])==program,"fight definitions: invalid format or program in "+path.string());
  const auto& tactics = doc["tactics"];
  require(tactics.IsArray(),"fight definitions: invalid tactic table");
  std::vector<FightTactic> result(tactics.Size());
  for(rapidjson::SizeType i=0; i<tactics.Size(); ++i) {
    const auto& tactic = tactics[i];
    require(tactic.IsArray() && tactic.Size()==std::size(slots),"fight definitions: invalid tactic row");
    for(rapidjson::SizeType slot=0; slot<std::size(slots); ++slot) {
      const auto& moves = tactic[slot];
      require(moves.IsArray() && moves.Size()==Def::FightAi::move_count,"fight definitions: invalid move row");
      auto& dst = result[i].*slots[slot].second;
      for(rapidjson::SizeType m=0; m<moves.Size(); ++m) {
        require(moves[m].IsUint(),"fight definitions: invalid move value");
        dst.move[m] = Def::FightMove(moves[m].GetUint());
        }
      }
    }
  return result;
  }

void FightDefinitions::write(const fs::path& base, std::string_view program, const std::vector<FightTactic>& tactics) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text(format,alloc),alloc);
  put(doc,"version",Json(version),alloc);
  put(doc,"program_sha256",text(program,alloc),alloc);
  Json rows(rapidjson::kArrayType);
  for(const auto& tactic : tactics) {
    Json row(rapidjson::kArrayType);
    for(const auto& slot : slots) {
      Json moves(rapidjson::kArrayType);
      for(auto move : (tactic.*slot.second).move)
        moves.PushBack(Json(uint32_t(move)),alloc);
      row.PushBack(moves,alloc);
      }
    rows.PushBack(row,alloc);
    }
  put(doc,"tactics",std::move(rows),alloc);
  const auto path = section(base,program);
  fs::create_directories(path.parent_path());
  DefinitionJson::write(path,doc);
  }
