#include "game/definitions/runtimebindings.h"

#include "asset/program.h"
#include "daedalus/script.h"
#include "definitionjson.h"
#include "game/definitions/questclosuredata.h"
#include "game/definitions/stockstoryprograms.h"
#include "npcdefinitions.h"
#include "npchelperdefinitions.h"
#include "placementdefinitions.h"
#include "runtimebindingroots.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

using namespace std::string_view_literals;

namespace {

using daedalus::DaedalusDataType;
namespace SymbolFlag = daedalus::DaedalusSymbolFlag;

bool isImmutableString(const asset::ProgramSymbol& symbol) {
  return symbol.type==uint32_t(DaedalusDataType::STRING) &&
         (symbol.flags & (SymbolFlag::CONST | SymbolFlag::MEMBER))==SymbolFlag::CONST;
  }

bool isAnonymous(std::string_view name) {
  return name.size()>1 && name.front()=='$' &&
         std::all_of(name.begin()+1,name.end(),[](char c) { return c>='0' && c<='9'; });
  }

class Roots final {
  public:
    explicit Roots(const asset::ProgramRecord& program) : selected(program.symbols.size(),true) {
      for(uint32_t id=0; id<program.symbols.size(); ++id) {
        const auto& symbol = program.symbols[id];
        if(!isImmutableString(symbol))
          continue;
        const auto name = std::string_view(symbol.name);
        if(isAnonymous(name)) {
          anonymous.emplace(name,id);
          selected[id] = false;
          }
        for(size_t index=0; index<symbol.string_values.size() && index<=UINT8_MAX; ++index)
          strings.try_emplace(symbol.string_values[index],id);
        }
      }

    void keep(uint32_t id) {
      if(id>=selected.size())
        throw std::runtime_error("prepared runtime binding is out of range: "+std::to_string(id));
      selected[id] = true;
      }

    void keepName(std::string_view name) {
      if(const auto found = anonymous.find(name); found!=anonymous.end())
        keep(found->second);
      }

    void keepString(std::string_view value) {
      // The runtime uses the first immutable match as a stack reference. Preserve the
      // original slot so construction leaves the saved temporary string untouched.
      if(const auto found = strings.find(value); found!=strings.end())
        keep(found->second);
      }

    std::vector<uint32_t> slots() const {
      std::vector<uint32_t> result;
      for(uint32_t id=0; id<selected.size(); ++id)
        if(selected[id])
          result.push_back(id);
      return result;
      }

  private:
    std::vector<bool> selected;
    std::unordered_map<std::string_view,uint32_t> anonymous, strings;
  };

void retainStoryQueryRoots(Roots& roots, const rapidjson::Value& node) {
  if(node.IsObject()) {
    for(auto member=node.MemberBegin(); member!=node.MemberEnd(); ++member)
      retainStoryQueryRoots(roots,member->value);
    return;
    }
  if(!node.IsArray())
    return;
  if(node.Size()>=2 && node[0].IsString() && node[1].IsString()) {
    const std::string_view tag(node[0].GetString(),node[0].GetStringLength());
    const std::string_view name(node[1].GetString(),node[1].GetStringLength());
    if(tag=="var"sv || tag=="inst"sv || tag=="sym"sv || tag=="call"sv || tag=="fn"sv)
      roots.keepName(name.substr(0,name.find('[')));
    }
  for(const auto& child : node.GetArray())
    retainStoryQueryRoots(roots,child);
  }

void retainCallArguments(Roots& roots, const NpcDefinitions::Call& call) {
  for(const auto& arg : call.args) {
    if(arg.kind==NpcDefinitions::Value::Kind::String)
      roots.keepString(arg.text);
    else if(arg.kind==NpcDefinitions::Value::Kind::Symbol)
      roots.keepName(arg.text);
    }
  }

void retainPlacementRoots(Roots& roots, const PlacementDefinitions::Library& placement) {
  const auto points = [&](const auto& rows) {
    for(const auto& row : rows)
      roots.keep(row.point);
    };
  const auto groups = [&](const auto& rows) {
    for(const auto& group : rows)
      points(group);
    };
  if(placement.khorinisEarlyChapters) {
    points(placement.khorinisEarlyChapters->chapter2);
    groups(placement.khorinisEarlyChapters->chapter3);
    }
  if(placement.khorinisLateChapters) {
    groups(placement.khorinisLateChapters->chapter4);
    groups(placement.khorinisLateChapters->chapter5);
    }
  if(placement.oldWorldChapters) {
    groups(placement.oldWorldChapters->chapter3);
    groups(placement.oldWorldChapters->chapter4);
    }
  points(placement.addonChapter4);
  points(placement.dragonIsland);
  for(const auto& [name,rows] : placement.triggerInserts)
    groups(rows);

  for(const auto& [name,region] : placement.roomInitializers) {
    for(const auto& mob : region.mobs)
      roots.keep(mob.name);
    for(const auto& room : region.rooms) {
      if(const auto* fixed = std::get_if<PlacementDefinitions::RoomAssignment>(&room)) {
        roots.keep(fixed->label);
        } else {
        const auto& choice = std::get<PlacementDefinitions::RoomChoice>(room);
        roots.keep(choice.equal.label);
        roots.keep(choice.otherwise.label);
        }
      }
    }

  for(const auto& [name,definition] : placement.placements)
    for(const auto& step : definition.steps) {
      if(const auto* call = std::get_if<PlacementDefinitions::Call>(&step))
        retainCallArguments(roots,*call);
      else if(const auto& write = std::get<PlacementDefinitions::GlobalWrite>(step);
              write.value.kind==NpcDefinitions::Value::Kind::String)
        roots.keepString(write.value.text);
      }
  }

void retainNpcRoots(Roots& roots, const NpcDefinitions::Library& npcs,
                    const NpcHelperDefinitions::Library& helpers) {
  for(const auto& [name,definition] : npcs.npcs)
    for(const auto& step : definition.steps)
      if(const auto* call = std::get_if<NpcDefinitions::Call>(&step))
        retainCallArguments(roots,*call);

  const auto keepVisual = [&](const NpcHelperDefinitions::SetNpcVisual& visual) {
    roots.keepString(visual.model);
    roots.keepString(visual.maleBody);
    roots.keepString(visual.femaleBody);
    };
  for(const auto& [name,helper] : helpers.helpers) {
    if(const auto* visual = std::get_if<NpcHelperDefinitions::SetNpcVisual>(&helper)) {
      keepVisual(*visual);
      } else if(const auto* skeleton = std::get_if<NpcHelperDefinitions::SkeletonLord>(&helper)) {
      keepVisual(skeleton->visual);
      roots.keepString(skeleton->model);
      roots.keepString(skeleton->head);
      for(const auto& overlay : skeleton->overlays)
        roots.keepString(overlay);
      }
    }
  }

}

std::vector<uint32_t> RuntimeBindings::collect(const std::filesystem::path& base,
                                               const asset::ProgramRecord& source) {
  Roots roots(source);
  for(const auto name : RuntimeBindingLiteralNames)
    roots.keepName(name);
  for(const auto& [name,id] : StockStoryPrograms::read(base).symbols)
    roots.keep(id);
  retainStoryQueryRoots(roots,DefinitionJson::read(base/"storygraph/story.json"));

  const auto sha = StockStoryPrograms::stockProgram;
  retainPlacementRoots(roots,PlacementDefinitions::read(base,sha));
  retainNpcRoots(roots,NpcDefinitions::read(base,sha),NpcHelperDefinitions::read(base,sha));
  for(const auto& group : QuestClosureData::read(base)) {
    for(const auto& row : group.rows)
      roots.keep(row.topic);
    if(group.guildRule)
      roots.keep(group.guildRule->topic);
    }
  return roots.slots();
  }
