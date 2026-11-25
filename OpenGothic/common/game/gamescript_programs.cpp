#include "gamescript_programs.h"

#include <Tempest/Log>

#include "basestore.h"
#include "contentcensus.h"
#include "game/definitions/basescripts.h"
#include "game/definitions/nativebasecatalog.h"
#include "gothic.h"
#include "resources.h"
#include "world/objects/npc.h"

#include <algorithm>

namespace {
storygraph::NativeValue::Type valueType(uint32_t type) {
  // The prepared-program format keeps the converter's numeric type tags.
  using Type = storygraph::NativeValue;
  switch(type) {
    case 1: return Type::Float;
    case 2:
    case 5: return Type::Int;
    case 3: return Type::Str;
    case 7: return Type::Inst;
    default: throw BaseStore::Error("Unsupported prepared story operation type");
    }
  }

storygraph::NativeFunction preparedSignature(const StockStoryPrograms::Entry& program) {
  storygraph::NativeFunction fn;
  fn.parameters.reserve(program.parameters.size());
  for(const auto parameter : program.parameters)
    fn.parameters.push_back(valueType(parameter));
  if(program.returnType!=0)
    fn.result=valueType(program.returnType);
  return fn;
  }

template<class Key>
auto nativeStoryIdentity(const NativeBaseCatalog::Selection& catalog, Key key)
    -> std::pair<const NativeBaseCatalog::Identity*,GameScript::SymbolKind> {
  using Kind = GameScript::SymbolKind;
  const auto& names = catalog.identities;
  for(const auto& [rows,kind] : std::array{
      std::pair{std::span(names.npcs),Kind::Npc},std::pair{std::span(names.items),Kind::Item},
      std::pair{std::span(names.dialogs),Kind::Info}})
    if(const auto* entry = NativeBaseCatalog::findIdentity(rows,key))
      return {entry,kind};
  return {nullptr,Kind::Other};
  }
}

GameScript::StockPrograms::StockPrograms(GameScript& script)
  : script(script), prepared(StockStoryPrograms::read(Resources::baseDir())) {
  if(auto* census = Resources::contentCensus()) {
    const auto base = Resources::baseDir();
    const auto section = StockStoryPrograms::section(base);
    census->replaced("GOTHIC.DAT:story-programs","base/"+section.lexically_relative(base).generic_string());
    }
  }

const StockStoryPrograms::Entry* GameScript::StockPrograms::program(uint32_t id) const {
  const auto it = prepared.programs.find(id);
  return it!=prepared.programs.end() ? &it->second : nullptr;
  }

const StockStoryPrograms::Entry* GameScript::StockPrograms::program(std::string_view name) const {
  const auto it = prepared.symbols.find(name);
  return it!=prepared.symbols.end() ? program(it->second) : nullptr;
  }

std::optional<storygraph::NativeFunction> GameScript::StockPrograms::function(std::string_view name) const {
  const auto key = storygraph::nativeOperationName(name);
  if(const auto direct = script.nativeExternals.find(key); direct!=script.nativeExternals.end())
    return direct->second.signature;
  if(const auto* entry = program(key)) {
    auto fn = preparedSignature(*entry);
    fn.program = &entry->program;
    return fn;
    }
  return std::nullopt;
  }

auto GameScript::storyPrograms() -> StockPrograms& {
  if(stockPrograms==nullptr)
    stockPrograms = std::make_unique<StockPrograms>(*this);
  return *stockPrograms;
  }

const storygraph::Program* GameScript::preparedStockProgram(uint32_t id) {
  if(const auto* entry = storyPrograms().program(id))
    return &entry->program;
  return nullptr;
  }

std::string_view GameScript::storyProgramName(uint32_t id) {
  const auto& programs = storyPrograms();
  if(const auto* program = programs.program(id))
    return program->name;
  const auto* catalog = baseNativeCatalog();
  const auto state = std::ranges::find(catalog->states,id,&NativeBaseCatalog::State::id);
  if(state!=catalog->states.end())
    return state->name;
  if(const auto* routine = NativeBaseCatalog::findIdentity(catalog->routines,id))
    return routine->name;
  if(const auto* callback = NativeBaseCatalog::findIdentity(catalog->npcCallbacks,id))
    return callback->name;
  if(const auto* callback = NativeBaseCatalog::findIdentity(catalog->itemCallbacks,id))
    return callback->name;
  for(const auto& [name,callback] : catalog->npcConstructor.calls)
    if(callback.id==id)
      return name;
  return {};
  }

std::string_view GameScript::storySymbolName(uint32_t id) {
  const auto* catalog = baseNativeCatalog();
  if(const auto [entry,kind] = nativeStoryIdentity(*catalog,id); entry!=nullptr)
    return entry->name;
  if(const auto* entry = own.find(id))
    return entry->name;
  return storyProgramName(id);
  }

std::optional<int32_t> GameScript::storySymbolIndex(std::string_view name) {
  const auto* catalog = baseNativeCatalog();
  if(const auto id = runtimeState.id(name))
    return int32_t(*id);
  if(const auto* constant = NativeBaseCatalog::findConstant(*catalog,name))
    return int32_t(constant->id);
  if(const auto [entry,kind] = nativeStoryIdentity(*catalog,name); entry!=nullptr)
    return int32_t(entry->id);
  if(const auto* entry = own.find(name))
    return int32_t(entry->index);
  const auto& programs = storyPrograms().prepared;
  const auto found = programs.symbols.find(storygraph::nativeOperationName(name));
  if(found!=programs.symbols.end())
    return int32_t(found->second);
  if(const auto id = storyScriptFunctionId(name))
    return int32_t(*id);
  return std::nullopt;
  }

std::optional<GameScript::SymbolKind> GameScript::storySymbolKind(std::string_view name) {
  const auto* catalog = baseNativeCatalog();
  if(BennetOre::progressField(*this,name)!=nullptr)
    return SymbolKind::Variable;
  if(const auto [entry,kind] = nativeStoryIdentity(*catalog,name); entry!=nullptr)
    return kind;
  if(const auto* entry = own.find(name))
    return entry->kind==OwnSymbols::Kind::Int ? SymbolKind::Variable : SymbolKind::Npc;
  if(const auto id = runtimeState.id(name)) {
    const auto row = std::ranges::lower_bound(catalog->initialState,*id,{},&NativeBaseCatalog::InitialRow::id);
    if(row!=catalog->initialState.end() && row->id==*id)
      if(const auto* alias = std::get_if<NativeBaseCatalog::Alias>(&row->value))
        return *alias==NativeBaseCatalog::Alias::Npc ? SymbolKind::Npc : SymbolKind::Item;
    return SymbolKind::Variable;
    }
  if(NativeBaseCatalog::findConstant(*catalog,name)!=nullptr)
    return SymbolKind::Constant;
  if(nativeExternals.contains(storygraph::nativeOperationName(name)) || storyScriptFunctionId(name))
    return SymbolKind::Function;
  return std::nullopt;
  }

std::vector<std::string_view> GameScript::storyRoutineVariants(int32_t npcId) const {
  const auto suffix = "_"+std::to_string(npcId);
  constexpr std::string_view prefix = "RTN_";
  std::vector<std::string_view> result;
  const auto append = [&](std::string_view name) {
    if(name.size()>prefix.size()+suffix.size() && name.starts_with(prefix) && name.ends_with(suffix))
      result.push_back(name.substr(prefix.size(),name.size()-prefix.size()-suffix.size()));
    };
  for(const auto& routine : baseNativeCatalog()->routines)
    append(routine.name);
  return result;
  }

std::optional<uint32_t> GameScript::storyScriptFunctionId(std::string_view name) {
  const auto* catalog = baseNativeCatalog();
  const auto key = storygraph::nativeOperationName(name);
  if(const auto* program = storyPrograms().program(key))
    return program->id;
  const auto state = std::ranges::find(catalog->states,key,&NativeBaseCatalog::State::name);
  if(state!=catalog->states.end())
    return state->id;
  if(const auto* routine = NativeBaseCatalog::findIdentity(catalog->routines,name))
    return routine->id;
  if(const auto* callback = NativeBaseCatalog::findIdentity(catalog->npcCallbacks,name))
    return callback->id;
  if(const auto* callback = NativeBaseCatalog::findIdentity(catalog->itemCallbacks,name))
    return callback->id;
  if(const auto it = catalog->npcConstructor.calls.find(key); it!=catalog->npcConstructor.calls.end())
    return it->second.id;
  return std::nullopt;
  }

bool GameScript::storySlotWritable(std::string_view name) {
  if(BennetOre::progressField(*this,name)!=nullptr)
    return true;
  return runtimeState.find(name)!=nullptr && nativeStoryIdentity(*baseNativeCatalog(),name).first==nullptr;
  }

Npc* GameScript::directorNpc(std::string_view name) {
  if(const auto* alias = storyOwnedInstance(name))
    return findNpc(dynamic_cast<NpcInstance*>(alias->get()));
  return nullptr;
  }

std::optional<storygraph::NativeFunction> GameScript::storyFunction(std::string_view name) {
  return storyPrograms().function(name);
  }

bool GameScript::invokeStockProgram(uint32_t id, int32_t& result, bool* failed) {
  return storyPrograms().run(id,result,failed);
  }

int32_t GameScript::dialogFunction(uint32_t id) {
  if(id==0)
    return 0;
  auto& gothic = Gothic::inst();
  auto& overlay = gothic.dialogScriptFn;
  int32_t result = 0;
  if(overlay && overlay(id,result))
    return result;
  bool failed = false;
  const bool ran = invokeStockProgram(id,result,&failed);
  if(gothic.dialogStockResultFn)
    gothic.dialogStockResultFn(ran,failed);
  if(ran)
    return result;
  Tempest::Log::e("[story] no native dialog program for ",id);
  return 0;
  }
