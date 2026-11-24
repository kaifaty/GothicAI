#include "gamescript.h"

#include "storygraph/eval.h"
#include "world/objects/npc.h"

std::optional<GameScript::SymbolKind> GameScript::symbolKind(std::string_view name) const {
  if(symbolCatalog)
    if(const auto id = symbolCatalog->find(name))
      return symbolCatalog->at(*id)->kind;
  if(const auto* entry = own.find(name))
    return entry->kind==OwnSymbols::Kind::Npc ? SymbolKind::Npc : SymbolKind::Variable;

  using Type = daedalus::DaedalusDataType;
  auto* symbol = vm.find_symbol_by_name(name);
  if(symbol==nullptr)
    return std::nullopt;
  if(symbol->is_member())
    return SymbolKind::Other;
  switch(symbol->type()) {
    case Type::INSTANCE:
      for(int i=0; i<8 && symbol!=nullptr && symbol->type()!=Type::CLASS; ++i)
        symbol = vm.find_symbol_by_index(symbol->parent());
      if(symbol!=nullptr && symbol->type()==Type::CLASS) {
        if(symbol->name()=="C_NPC")  return SymbolKind::Npc;
        if(symbol->name()=="C_ITEM") return SymbolKind::Item;
        if(symbol->name()=="C_INFO") return SymbolKind::Info;
        }
      break;
    case Type::INT:      return symbol->is_const() ? SymbolKind::Constant : SymbolKind::Variable;
    case Type::FUNCTION: return SymbolKind::Function;
    default: break;
    }
  return SymbolKind::Other;
  }

std::optional<int32_t> GameScript::globalIntSymbol(std::string_view name) const {
  if(const auto value = own.intValue(name))
    return value;
  const auto* symbol = vm.find_symbol_by_name(name);
  if(symbol!=nullptr && symbol->type()==daedalus::DaedalusDataType::INT && !symbol->is_member())
    return symbol->get_int(0);
  return std::nullopt;
  }

std::optional<int32_t> GameScript::intConstant(std::string_view name) const {
  const auto* symbol = name.empty() ? nullptr : vm.find_symbol_by_name(name);
  if(symbol!=nullptr && symbol->is_const() && symbol->type()==daedalus::DaedalusDataType::INT)
    return symbol->get_int(0);
  return std::nullopt;
  }

std::optional<uint32_t> GameScript::findItemInstance(std::string_view name) const {
  using Type = daedalus::DaedalusDataType;
  const auto* symbol = vm.find_symbol_by_name(name);
  if(symbol==nullptr || symbol->type()!=Type::INSTANCE)
    return std::nullopt;
  auto* parent = vm.find_symbol_by_index(symbol->parent());
  while(parent!=nullptr && parent->type()!=Type::CLASS)
    parent = vm.find_symbol_by_index(parent->parent());
  if(parent!=nullptr && parent==vm.find_symbol_by_name("C_ITEM"))
    return symbol->index();
  return std::nullopt;
  }

std::optional<uint32_t> GameScript::addDirectorNpcSymbol(std::string_view name, std::string_view like, std::string& why) {
  if(auto* symbol = own.addNpc(vm,name,like,why))
    return symbol->index();
  return std::nullopt;
  }

std::string_view GameScript::stringSymbol(std::string_view name, std::string_view fallback) const {
  const auto* symbol = vm.find_symbol_by_name(name);
  if(symbol!=nullptr && symbol->type()==daedalus::DaedalusDataType::STRING)
    return symbol->get_string();
  return fallback;
  }

bool GameScript::isPartyMember(const Npc& npc) {
  const auto index = intSymbol("AIV_PARTYMEMBER",-1);
  return index>=0 && size_t(index)<NpcRecord::aivar_count && npc.handlePtr()->aivar[size_t(index)]!=0;
  }

bool GameScript::setGlobalIntSymbol(std::string_view name, int32_t value) {
  if(own.setInt(name,value))
    return true;
  auto* symbol = vm.find_symbol_by_name(name);
  if(symbol==nullptr || symbol->type()!=daedalus::DaedalusDataType::INT || symbol->is_member() || symbol->is_const())
    return false;
  symbol->set_int(value);
  return true;
  }

void GameScript::setPartyMember(Npc& npc, bool active) {
  const auto index = intSymbol("AIV_PARTYMEMBER",0);
  if(index>=0 && size_t(index)<NpcRecord::aivar_count)
    npc.handlePtr()->aivar[size_t(index)] = active ? 1 : 0;
  }

std::optional<bool> GameScript::queryNpcKnowsInfo(const Npc& npc, std::string_view name) {
  const auto* info = vm.find_symbol_by_name(name);
  const auto* knows = vm.find_symbol_by_name("NPC_KNOWSINFO");
  if(info==nullptr || knows==nullptr)
    return std::nullopt;
  if(nativeRoutineProgram)
    return doesNpcKnowInfo(*npc.handlePtr(),info->index());

  std::string error;
  const auto result = storygraph::callFunction(vm,*knows,
                       {storygraph::Value::instance(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr())),
                        storygraph::Value::integer(int32_t(info->index()))},error);
  if(result && result->type==storygraph::Value::Int)
    return result->i!=0;
  return std::nullopt;
  }
