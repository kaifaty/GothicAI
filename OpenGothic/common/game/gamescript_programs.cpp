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
#include <cctype>

namespace {
storygraph::NativeFunction preparedSignature(const StockStoryPrograms::Entry& program) {
  using daedalus::DaedalusDataType;
  storygraph::NativeFunction fn;
  fn.parameters.reserve(program.parameters.size());
  for(const auto parameter : program.parameters)
    fn.parameters.push_back(storygraph::nativeValueType(DaedalusDataType(parameter)));
  if(program.returnType!=0)
    fn.result=storygraph::nativeValueType(DaedalusDataType(program.returnType));
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
  : script(script), vm(script.getVm()) {
  try {
    if(script.nativeRoutineProgram) {
      const auto base = Resources::baseDir();
      const auto section = StockStoryPrograms::section(base);
      if(std::filesystem::exists(section)) {
        prepared = StockStoryPrograms::read(base);
        if(auto* census = Resources::contentCensus())
          census->replaced("GOTHIC.DAT:story-programs","base/"+section.lexically_relative(base).generic_string());
        if(baseNativeCatalog()!=nullptr)
          return;
        bindCatalog(*prepared);
        }
      }
    if(baseNativeCatalog()!=nullptr)
      throw BaseStore::Error("missing prepared native StoryGraph programs");

    // Director references include definitions that no stock quest happens to name.
    using enum daedalus::DaedalusDataType;
    for(const auto& [name,kind] : std::array{
          std::pair{"C_NPC",SymbolKind::Npc}, std::pair{"C_ITEM",SymbolKind::Item},
          std::pair{"C_INFO",SymbolKind::Info}})
      vm.enumerate_instances_by_class_name(name,[&](auto& symbol) { bindSymbol(symbol,kind); });

    for(const auto& symbol : vm.symbols()) {
      const auto type = symbol.type();
      if(symbol.is_member())
        continue;
      const bool callable = type==FUNCTION && (symbol.is_external() || vm.has_active_override(symbol));
      const bool state = type==FUNCTION && symbol.is_const() && symbol.name().starts_with("ZS_");
      const bool value = (type==INT || type==FLOAT || type==STRING) && !symbol.is_generated() &&
                         symbol.name().find('.')==std::string::npos;
      if(!callable && !state && !value)
        continue;
      auto& slot = *vm.find_symbol_by_index(symbol.index());
      bindSymbol(slot, callable || state ? SymbolKind::Function :
                       symbol.is_const() ? SymbolKind::Constant : SymbolKind::Variable);
      if(callable && !script.nativeExternals.contains(slot.name()))
        calls.try_emplace(slot.name(),vm,slot);
      }

    constexpr std::array classes = {"C_INFO","C_FOCUS","C_MENU_ITEM","C_NPC","C_ITEM"};
    for(const auto* name : classes)
      if(const auto* cls = vm.find_symbol_by_name(name))
        for(auto* member : vm.find_class_members(*cls))
          bindSymbol(*member,SymbolKind::Other);
    for(auto* alias : {vm.global_self(), vm.global_other(), vm.global_victim(), vm.global_hero()})
      if(alias!=nullptr)
        bindSymbol(*alias,SymbolKind::Npc);
    if(auto* alias = vm.global_item())
      bindSymbol(*alias,SymbolKind::Item);
    }
  catch(const std::runtime_error& error) {
    throw BaseStore::Error(error.what());
    }
  }

void GameScript::StockPrograms::bindSymbol(daedalus::DaedalusSymbol& symbol, SymbolKind kind) {
  bindings.insert_or_assign(symbol.name(),Binding{&symbol,kind});
  ids.try_emplace(symbol.index(),&symbol);
  }

void GameScript::StockPrograms::bindCatalog(const StockStoryPrograms::Catalog& catalog) {
  using enum daedalus::DaedalusDataType;
  for(const auto& [name,id] : catalog.symbols) {
    auto* symbol = vm.find_symbol_by_index(id);
    if(symbol==nullptr || symbol->name()!=name)
      throw BaseStore::Error("prepared story identity differs from game records: "+name);
    auto kind = SymbolKind::Other;
    switch(symbol->type()) {
      case FUNCTION:
        kind = SymbolKind::Function;
        break;
      case FLOAT:
      case INT:
      case STRING:
        kind = symbol->is_const() ? SymbolKind::Constant : SymbolKind::Variable;
        break;
      default:
        break;
      }
    bindSymbol(*symbol,kind);
    if(symbol->type()==FUNCTION && (symbol->is_external() || symbol->is_const()) &&
       !catalog.programs.contains(id) && !script.nativeExternals.contains(name))
      calls.try_emplace(name,vm,*symbol);
    }

  for(const auto& [id,record] : catalog.programs) {
    const auto found = ids.find(id);
    if(found==ids.end())
      throw BaseStore::Error("prepared story program is missing its identity: "+record.name);
    const auto& symbol = *found->second;
    if(symbol.name()!=record.name || symbol.type()!=FUNCTION || symbol.is_external() ||
       symbol.has_local_variables_enabled() ||
       (symbol.has_return() ? uint32_t(symbol.rtype()) : 0u)!=record.returnType)
      throw BaseStore::Error("prepared story function differs from game records: "+record.name);
    const auto params = vm.find_parameters_for_function(&symbol);
    if(!std::equal(params.begin(),params.end(),record.parameters.begin(),record.parameters.end(),
                   [](const auto& actual, uint32_t expected) { return uint32_t(actual.type())==expected; }))
      throw BaseStore::Error("prepared story function signature differs: "+record.name);
    for(auto child : record.calls)
      if(!catalog.programs.contains(child))
        throw BaseStore::Error("prepared story function dependency is missing: "+record.name);
    }
  }

auto GameScript::StockPrograms::binding(std::string_view name) const -> Binding {
  if(const auto found = bindings.find(name); found!=bindings.end())
    return found->second;
  if(auto* own = script.own.binding(name))
    return {own,own->type()==daedalus::DaedalusDataType::INSTANCE ? SymbolKind::Npc : SymbolKind::Variable};
  if(std::any_of(name.begin(),name.end(),[](unsigned char c) { return std::islower(c)!=0; })) {
    std::string upper(name);
    for(auto& ch : upper)
      ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return binding(upper);
    }
  return {};
  }

const StockStoryPrograms::Catalog* GameScript::StockPrograms::preparedCatalog() {
  return prepared ? &*prepared : nullptr;
  }

const StockStoryPrograms::Entry* GameScript::StockPrograms::program(uint32_t id) const {
  if(!prepared)
    return nullptr;
  const auto it = prepared->programs.find(id);
  return it!=prepared->programs.end() ? &it->second : nullptr;
  }

const StockStoryPrograms::Entry* GameScript::StockPrograms::program(std::string_view name) const {
  if(!prepared)
    return nullptr;
  const auto it = prepared->symbols.find(name);
  return it!=prepared->symbols.end() ? program(it->second) : nullptr;
  }

std::optional<storygraph::NativeFunction> GameScript::StockPrograms::function(std::string_view name) const {
  if(baseNativeCatalog()!=nullptr) {
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

  const auto [slot,kind] = binding(name);
  if(slot==nullptr || kind!=SymbolKind::Function)
    return std::nullopt;
  if(const auto direct = script.nativeExternals.find(slot->name()); direct!=script.nativeExternals.end())
    return direct->second.signature;
  const auto* prepared = program(slot->index());
  if(prepared==nullptr) {
    const auto call = calls.find(slot->name());
    if(call==calls.end() || !call->second.active())
      return std::nullopt;
    return call->second.signature();
    }

  auto fn = preparedSignature(*prepared);

  // Run complete prepared operations directly. Some of their VM adapters still
  // serve other engine entry points until those callers migrate.
  static constexpr std::string_view preparedOperations[] = {
    "B_STARTOTHERROUTINE", "B_GREG_COMESTODEXTER",
    "B_BUILDLEARNSTRING", "B_GETLEARNCOSTATTRIBUTE", "B_GETLEARNCOSTTALENT",
    "B_RAISEATTRIBUTE", "B_RAISEREALATTRIBUTELEARNCOUNTER", "B_RAISEFIGHTTALENT",
    "B_RAISEREALFIGHTTALENTPERCENT", "B_ADDFIGHTSKILL", "B_LOGENTRY", "B_SAY"
    };
  if(!vm.has_bound_native(*slot) || std::ranges::find(preparedOperations,slot->name())!=std::end(preparedOperations))
    fn.program = &prepared->program;
  return fn;
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

const storygraph::Program* GameScript::preparedStockProgram(const daedalus::DaedalusSymbol& fn, bool requireDependencies) {
  const auto* entry = storyPrograms().program(fn.index());
  if(entry==nullptr || entry->name!=fn.name())
    return nullptr;
  if(requireDependencies && !checkStoryProgram(entry->program,entry->parameters.size()).empty())
    return nullptr;
  return &entry->program;
  }

std::string_view GameScript::storyProgramName(uint32_t id) {
  const auto& programs = storyPrograms();
  if(const auto* program = programs.program(id))
    return program->name;
  if(const auto* catalog = baseNativeCatalog()) {
    const auto state = std::ranges::find(catalog->states,id,&NativeBaseCatalog::State::id);
    if(state!=catalog->states.end())
      return state->name;
    if(const auto* routine = NativeBaseCatalog::findIdentity(catalog->routines,id))
      return routine->name;
    return {};
    }
  const auto found = programs.ids.find(id);
  if(found!=programs.ids.end() && found->second->type()==daedalus::DaedalusDataType::FUNCTION)
    return found->second->name();
  return {};
  }

std::string_view GameScript::storySymbolName(uint32_t id) {
  if(const auto* catalog = baseNativeCatalog()) {
    if(const auto [entry,kind] = nativeStoryIdentity(*catalog,id); entry!=nullptr)
      return entry->name;
    if(const auto* entry = own.find(id))
      return entry->name;
    return storyProgramName(id);
    }
  const auto& programs = storyPrograms();
  if(const auto found = programs.ids.find(id); found!=programs.ids.end())
    return found->second->name();
  if(const auto* entry = own.find(id))
    return entry->name;
  return {};
  }

daedalus::DaedalusSymbol* GameScript::storySlot(uint32_t id) const {
  if(!stockPrograms)
    return nullptr;
  const auto found = stockPrograms->ids.find(id);
  if(found!=stockPrograms->ids.end())
    return found->second;
  if(const auto* entry = own.find(id))
    return own.binding(entry->name);
  return nullptr;
  }

std::optional<int32_t> GameScript::storySymbolIndex(std::string_view name) {
  if(const auto* catalog = baseNativeCatalog()) {
    if(const auto id = runtimeState.id(name))
      return int32_t(*id);
    if(const auto* constant = NativeBaseCatalog::findConstant(*catalog,name))
      return int32_t(constant->id);
    if(const auto [entry,kind] = nativeStoryIdentity(*catalog,name); entry!=nullptr)
      return int32_t(entry->id);
    if(const auto* entry = own.find(name))
      return int32_t(entry->index);
    const auto& programs = storyPrograms().prepared;
    const auto found = programs->symbols.find(storygraph::nativeOperationName(name));
    if(found!=programs->symbols.end())
      return int32_t(found->second);
    if(const auto id = storyScriptFunctionId(name))
      return int32_t(*id);
    return std::nullopt;
    }
  if(const auto [slot,kind] = storyPrograms().binding(name); slot!=nullptr)
    return int32_t(slot->index());
  return std::nullopt;
  }

std::optional<GameScript::SymbolKind> GameScript::storySymbolKind(std::string_view name) {
  if(const auto* catalog = baseNativeCatalog()) {
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
  if(const auto [slot,kind] = storyPrograms().binding(name); slot!=nullptr)
    return kind;
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
  if(const auto* catalog = baseNativeCatalog()) {
    for(const auto& routine : catalog->routines)
      append(routine.name);
    } else {
    for(const auto& symbol : vm.symbols())
      append(symbol.name());
    }
  return result;
  }

daedalus::DaedalusSymbol* GameScript::storySlot(std::string_view name, SymbolKind* kind) const {
  if(!stockPrograms)
    return nullptr;
  const auto [symbol, symbolKind] = stockPrograms->binding(name);
  if(kind!=nullptr)
    *kind = symbolKind;
  return symbol;
  }

std::optional<uint32_t> GameScript::storyScriptFunctionId(std::string_view name) {
  if(const auto* catalog = baseNativeCatalog()) {
    const auto key = storygraph::nativeOperationName(name);
    if(const auto* program = storyPrograms().program(key))
      return program->id;
    const auto state = std::ranges::find(catalog->states,key,&NativeBaseCatalog::State::name);
    if(state!=catalog->states.end())
      return state->id;
    if(const auto* routine = NativeBaseCatalog::findIdentity(catalog->routines,name))
      return routine->id;
    return std::nullopt;
    }
  const auto [slot,kind] = storyPrograms().binding(name);
  if(kind==SymbolKind::Function && slot->is_const() && !slot->is_external())
    return slot->index();
  return std::nullopt;
  }

bool GameScript::storySlotWritable(std::string_view name) {
  if(baseNativeCatalog()!=nullptr)
    return runtimeState.find(name)!=nullptr || BennetOre::progressField(*this,name)!=nullptr;
  using enum daedalus::DaedalusDataType;
  const auto [slot,kind] = storyPrograms().binding(name);
  if(slot==nullptr || slot->is_const() || slot->is_external() || slot->is_member())
    return false;
  const auto type = slot->type();
  return type==INT || type==FLOAT || type==STRING || type==FUNCTION || type==INSTANCE;
  }

Npc* GameScript::directorNpc(std::string_view name) {
  if(baseNativeCatalog()!=nullptr) {
    if(const auto* alias = storyOwnedInstance(name))
      return findNpc(dynamic_cast<daedalus::INpc*>(alias->get()));
    return nullptr;
    }
  const auto [slot,kind] = storyPrograms().binding(name);
  if(slot==nullptr || slot->type()!=daedalus::DaedalusDataType::INSTANCE)
    return nullptr;
  return findNpc(dynamic_cast<daedalus::INpc*>(slot->get_instance().get()));
  }

std::optional<storygraph::NativeFunction> GameScript::storyFunction(std::string_view name) {
  return storyPrograms().function(name);
  }

bool GameScript::invokeStockProgram(uint32_t id, int32_t& result, bool* failed) {
  if(!nativeRoutineProgram)
    return false;
  return storyPrograms().run(id,result,failed,false);
  }

bool GameScript::invokeStockProgram(const daedalus::DaedalusSymbol& fn, int32_t& result, bool* failed, bool preparedOnly) {
  if(!nativeRoutineProgram)
    return false;
  return storyPrograms().run(fn.index(),result,failed,preparedOnly);
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
