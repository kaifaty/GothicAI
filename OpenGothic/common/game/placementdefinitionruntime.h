#pragma once

#include "placementdefinitions.h"
#include "daedalus/vm.h"

#include <map>
#include <set>
#include <unordered_map>

namespace PlacementDefinitionRuntime {
struct Argument {
  PlacementDefinitions::Value value;
  daedalus::DaedalusDataType type = daedalus::DaedalusDataType::INT;
  uint32_t literal = 0;
  uint8_t literalIndex = 0;
  };
struct Call {
  uint32_t function = 0;
  std::vector<Argument> args;
  bool deferred = false;
  };
struct GlobalWrite {
  uint32_t symbol = 0;
  uint16_t index = 0;
  PlacementDefinitions::Value value;
  };
using Step = std::variant<Call,GlobalWrite>;
struct Definitions {
  std::unordered_map<uint32_t,std::vector<Step>> placements;
  std::optional<PlacementDefinitions::KhorinisLateChapters> khorinisLateChapters;
  std::vector<PlacementDefinitions::LiteralInsert> addonChapter4;
  std::optional<PlacementDefinitions::OldWorldChapters> oldWorldChapters;
  std::vector<PlacementDefinitions::LiteralInsert> dragonIsland;
  std::map<std::string,PlacementDefinitions::RoomInitializer> roomInitializers;
  std::optional<PlacementDefinitions::FirstEntries> firstEntries;
  std::optional<PlacementDefinitions::KhorinisEarlyChapters> khorinisEarlyChapters;
  PlacementDefinitions::TriggerInserts triggerInserts;
  };

inline std::vector<uint32_t> startupFunctions(const daedalus::DaedalusScript& script) {
  using Type = daedalus::DaedalusDataType;
  using Op = daedalus::DaedalusOpcode;
  std::map<uint32_t,uint32_t> addresses;
  std::vector<uint32_t> pending;
  for(const auto& symbol:script.symbols())
    if(!symbol.is_external() && (symbol.type()==Type::FUNCTION || symbol.type()==Type::PROTOTYPE ||
        (symbol.type()==Type::INSTANCE && symbol.is_const() && symbol.address()>0 && symbol.name().find('.')==std::string::npos)) &&
       symbol.address()<script.size()) {
      addresses.emplace(symbol.address(),symbol.index());
      if(symbol.name().starts_with("STARTUP_")) pending.push_back(symbol.address());
      }
  std::set<uint32_t> visited;
  std::set<uint32_t> result;
  while(!pending.empty()) {
    const auto address=pending.back(); pending.pop_back();
    if(!visited.insert(address).second) continue;
    const auto found=addresses.find(address);
    if(found==addresses.end()) throw std::runtime_error("placement probe: missing function at "+std::to_string(address));
    result.insert(found->second);
    const auto next=std::next(found);
    const auto end=next==addresses.end() ? script.size() : next->first;
    for(auto pc=address;pc<end;) {
      const auto op=script.instruction_at(pc);
      if(op.op==Op::BL) pending.push_back(op.address);
      pc+=op.size;
      }
    }
  return {result.begin(),result.end()};
}

bool enabled();
Definitions resolve(const PlacementDefinitions::Library& library, const daedalus::DaedalusScript& script);
Definitions load(const std::filesystem::path& base, const daedalus::DaedalusScript& program,
                 std::string_view sha256);
void observeCalls(daedalus::DaedalusVm& vm, const std::filesystem::path& base, std::string_view sha256);
// The startup game entry point and the contract share the record/VM choice.
bool execute(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol* function,
             const Definitions& definitions);
}
