#include "gamescript_programs.h"

#include <Tempest/Log>

#include "basestore.h"
#include "contentcensus.h"
#include "game/definitions/basescripts.h"
#include "gothic.h"
#include "programdata.h"
#include "resources.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <cctype>

GameScript::StockPrograms::StockPrograms(GameScript& script)
  : script(script), vm(script.getVm()) {
  try {
    if(script.nativeRoutineProgram) {
      const auto base = baseProgramIndex().dir.parent_path();
      const auto section = StockStoryPrograms::section(base);
      if(std::filesystem::exists(section)) {
        prepared = StockStoryPrograms::read(base);
        bindCatalog(*prepared);
        if(auto* census = Resources::contentCensus())
          census->replaced("GOTHIC.DAT:story-programs","base/"+section.lexically_relative(base).generic_string());
        }
      }

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

    constexpr std::array classes = {"C_NPC","C_ITEM","C_INFO","C_FOCUS","C_MENU_ITEM"};
    for(size_t i=0; i<classes.size(); ++i) {
      const auto* cls = vm.find_symbol_by_name(classes[i]);
      if(cls==nullptr)
        continue;
      for(auto* member : vm.find_class_members(*cls)) {
        const std::string_view field = member->name();
        members[i].emplace(field.substr(field.find('.')+1),member);
        bindSymbol(*member,SymbolKind::Other);
        }
      }
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
    if(symbol->type()==FUNCTION && (symbol->is_external() || symbol->is_const()) && !script.nativeExternals.contains(name))
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
    calls.try_emplace(record.name,vm,*found->second);
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

std::optional<storygraph::NativeFunction> GameScript::StockPrograms::function(std::string_view name) const {
  const auto [slot,kind] = binding(name);
  if(slot==nullptr || kind!=SymbolKind::Function)
    return std::nullopt;
  if(const auto direct = script.nativeExternals.find(slot->name()); direct!=script.nativeExternals.end())
    return direct->second.signature;
  const auto call = calls.find(slot->name());
  if(call==calls.end())
    return std::nullopt;
  auto fn = call->second.signature();
  const auto* prepared = program(slot->index());
  // Run complete prepared operations directly. Some of their VM adapters still
  // serve other engine entry points until those callers migrate.
  static constexpr std::string_view preparedOperations[] = {
    "B_STARTOTHERROUTINE", "B_GREG_COMESTODEXTER",
    "B_BUILDLEARNSTRING", "B_GETLEARNCOSTATTRIBUTE", "B_GETLEARNCOSTTALENT",
    "B_RAISEATTRIBUTE", "B_RAISEREALATTRIBUTELEARNCOUNTER", "B_RAISEFIGHTTALENT",
    "B_RAISEREALFIGHTTALENTPERCENT", "B_ADDFIGHTSKILL", "B_LOGENTRY", "B_SAY"
    };
  if(prepared!=nullptr && std::ranges::find(preparedOperations,slot->name())!=std::end(preparedOperations)) {
    fn.program = &prepared->program;
    return fn;
    }
  if(call->second.active())
    return fn;
  if(prepared==nullptr)
    return std::nullopt;
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
  const auto found = programs.ids.find(id);
  if(found!=programs.ids.end() && found->second->type()==daedalus::DaedalusDataType::FUNCTION)
    return found->second->name();
  return {};
  }

std::string_view GameScript::storySymbolName(uint32_t id) {
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
  if(const auto [slot,kind] = storyPrograms().binding(name); slot!=nullptr)
    return int32_t(slot->index());
  return std::nullopt;
  }

std::optional<GameScript::SymbolKind> GameScript::storySymbolKind(std::string_view name) {
  if(const auto [slot,kind] = storyPrograms().binding(name); slot!=nullptr)
    return kind;
  return std::nullopt;
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
  const auto [slot,kind] = storyPrograms().binding(name);
  if(kind==SymbolKind::Function && slot->is_const() && !slot->is_external())
    return slot->index();
  return std::nullopt;
  }

bool GameScript::storySlotWritable(std::string_view name) {
  using enum daedalus::DaedalusDataType;
  const auto [slot,kind] = storyPrograms().binding(name);
  if(slot==nullptr || slot->is_const() || slot->is_external() || slot->is_member())
    return false;
  const auto type = slot->type();
  return type==INT || type==FLOAT || type==STRING || type==FUNCTION || type==INSTANCE;
  }

Npc* GameScript::directorNpc(std::string_view name) {
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
