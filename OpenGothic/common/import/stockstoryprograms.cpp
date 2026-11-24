#include "baseimport.h"

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "definitionjson.h"
#include "itemdefinitions.h"
#include "npcsnapshot.h"
#include "programdata.h"
#include "game/definitions/stockstoryprograms.h"
#include "game/definitions/symbolcatalog.h"
#include "storygraph/bytecode.h"
#include "storygraph/exprjson.h"
#include "worlddata.h"

#include <algorithm>
#include <optional>
#include <set>

namespace {
using namespace DefinitionJson;
using namespace daedalus;
using namespace StockStoryPrograms;

ScriptSymbolCatalog::Kind symbolKind(const DaedalusScript& script, const DaedalusSymbol& symbol) {
  using Kind = ScriptSymbolCatalog::Kind;
  using Type = DaedalusDataType;
  if(symbol.is_member())
    return Kind::Other;
  if(symbol.type()==Type::INT)
    return symbol.is_const() ? Kind::Constant : Kind::Variable;
  if(symbol.type()==Type::FUNCTION)
    return Kind::Function;
  if(symbol.type()==Type::INSTANCE) {
    const auto* parent = &symbol;
    for(int i = 0; i<8 && parent!=nullptr && parent->type()!=Type::CLASS; ++i)
      parent = script.find_symbol_by_index(parent->parent());
    if(parent!=nullptr && parent->type()==Type::CLASS) {
      if(parent->name()=="C_NPC")  return Kind::Npc;
      if(parent->name()=="C_ITEM") return Kind::Item;
      if(parent->name()=="C_INFO") return Kind::Info;
      }
    }
  return Kind::Other;
  }

Json putNumbers(const std::vector<uint32_t>& values, Allocator& alloc) {
  Json result(rapidjson::kArrayType);
  for(auto value : values)
    result.PushBack(Json(value),alloc);
  return result;
  }

template<class T, class Encode>
Json writeTable(const T& values, Allocator& alloc, Encode&& encodeValue) {
  Json result(rapidjson::kObjectType);
  for(const auto& [name,value] : values)
    result.AddMember(text(encode(name),alloc),encodeValue(value),alloc);
  return result;
  }

void writeCatalog(const std::filesystem::path& base, const Catalog& catalog) {
  rapidjson::Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  put(doc,"format",text(format,alloc),alloc);
  put(doc,"version",Json(version),alloc);
  put(doc,"program_sha256",text(stockProgram,alloc),alloc);
  put(doc,"symbols",writeTable(catalog.symbols,alloc,[](auto value) { return Json(value); }),alloc);
  put(doc,"integers",writeTable(catalog.integers,alloc,[](auto value) { return Json(value); }),alloc);
  put(doc,"strings",writeTable(catalog.strings,alloc,[&](const auto& value) { return text(encode(value),alloc); }),alloc);
  Json programs(rapidjson::kArrayType);
  for(const auto& [id,entry] : catalog.programs) {
    Json row(rapidjson::kObjectType);
    put(row,"id",Json(id),alloc);
    put(row,"name",text(encode(entry.name),alloc),alloc);
    put(row,"return_type",Json(entry.returnType),alloc);
    put(row,"parameters",putNumbers(entry.parameters,alloc),alloc);
    put(row,"calls",putNumbers(entry.calls,alloc),alloc);
    rapidjson::Document steps;
    const auto serialized = storygraph::toJson(entry.program);
    steps.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag>(serialized.data(),serialized.size());
    require(!steps.HasParseError() && steps.IsArray(),"story programs: cannot serialize "+entry.name);
    Json copy;
    copy.CopyFrom(steps,alloc);
    put(row,"steps",std::move(copy),alloc);
    programs.PushBack(std::move(row),alloc);
    }
  put(doc,"programs",std::move(programs),alloc);
  const auto path = section(base);
  std::filesystem::create_directories(path.parent_path());
  DefinitionJson::write(path,doc);
  }

const DaedalusSymbol& addSymbol(Catalog& catalog, const DaedalusScript& script, std::string_view name) {
  const auto* symbol = script.find_symbol_by_name(name);
  require(symbol!=nullptr,"story programs: missing stock symbol "+std::string(name));
  catalog.symbols.emplace(symbol->name(),symbol->index());
  return *symbol;
  }

void captureTransferConstants(Catalog& catalog, const DaedalusScript& script) {
  for(const auto* name : {"ITMI_GOLD", "ITMI_ORNAMENT_ADDON_VATRAS", "SELF", "OTHER", "ITEM", "HERO",
                         "B_GIVEINVITEMS", "B_GIVEINVITEMS.GIVER", "B_GIVEINVITEMS.TAKER",
                         "B_GIVEINVITEMS.ITEMINSTANCE", "B_GIVEINVITEMS.AMOUNT",
                         "B_GIVEINVITEMS.ITEMNAME", "B_GIVEINVITEMS.CONCATTEXT"})
    addSymbol(catalog,script,name);

  for(const auto* name : {"PRINT_GOLDGEGEBEN", "PRINT_GOLDERHALTEN", "PRINT_ADDON_GEGEBEN",
                         "PRINT_ADDON_ERHALTEN", "PRINT_ITEMSGEGEBEN", "PRINT_ITEMSERHALTEN", "FONT_SCREENSMALL"}) {
    const auto& symbol = addSymbol(catalog,script,name);
    require(symbol.type()==DaedalusDataType::STRING && symbol.is_const() && symbol.count()==1,
            "story programs: expected a stock string constant "+symbol.name());
    catalog.strings.emplace(symbol.name(),symbol.get_string());
    }
  for(const auto* name : {"YPOS_GOLDGIVEN","YPOS_GOLDTAKEN","YPOS_ITEMGIVEN","YPOS_ITEMTAKEN"}) {
    const auto& symbol = addSymbol(catalog,script,name);
    require(symbol.type()==DaedalusDataType::INT && symbol.is_const() && symbol.count()==1,
            "story programs: expected a stock integer constant "+symbol.name());
    catalog.integers.emplace(symbol.name(),symbol.get_int());
    }
  }

void captureAliases(Catalog& catalog, const DaedalusScript& script, const std::vector<storygraph::Step>& steps) {
  const auto check = [&](const storygraph::Expr& expression) {
    using Kind = storygraph::Expr::Kind;
    switch(expression.kind) {
      case Kind::Var:
      case Kind::Inst:
      case Kind::Sym:
      case Kind::Call:
      case Kind::Fn:
        addSymbol(catalog,script,expression.name);
        break;
      default:
        break;
      }
    };
  for(const auto& step : steps) {
    for(const auto* expression : {&step.call,&step.target,&step.value,&step.cond})
      storygraph::ex::walk(*expression,check);
    captureAliases(catalog,script,step.then);
    captureAliases(catalog,script,step.otherwise);
    }
  }

bool sameExpr(const storygraph::ExprP& a, const storygraph::ExprP& b) {
  if(!a || !b)
    return a==b;
  return a->kind==b->kind && a->value==b->value && a->index==b->index && a->name==b->name && a->text==b->text &&
         std::equal(a->args.begin(),a->args.end(),b->args.begin(),b->args.end(),sameExpr);
  }

bool sameSteps(const std::vector<storygraph::Step>& a, const std::vector<storygraph::Step>& b) {
  return std::equal(a.begin(),a.end(),b.begin(),b.end(),[](const auto& a, const auto& b) {
    return a.kind==b.kind && a.hero==b.hero && a.line==b.line && a.text==b.text && a.id==b.id &&
           sameExpr(a.call,b.call) && sameExpr(a.target,b.target) && sameExpr(a.value,b.value) &&
           sameExpr(a.cond,b.cond) && sameSteps(a.then,b.then) && sameSteps(a.otherwise,b.otherwise);
    });
  }

bool portable(const storygraph::Program& program) {
  if(!program.error.empty())
    return false;
  const auto json = storygraph::toJson(program);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag>(json.data(),json.size());
  if(doc.HasParseError())
    return false;
  const auto read = storygraph::readProgram(doc,program.returns);
  return read.error.empty() && sameSteps(program.steps,read.steps);
  }

bool signatureType(DaedalusDataType type, bool allowVoid) {
  return (allowVoid && type==DaedalusDataType::VOID) || type==DaedalusDataType::FLOAT ||
         type==DaedalusDataType::INT || type==DaedalusDataType::STRING || type==DaedalusDataType::FUNCTION ||
         type==DaedalusDataType::INSTANCE;
  }

std::optional<Entry> captureEntry(Catalog& catalog, const DaedalusScript& script, const storygraph::Bytecode& code,
                                  const DaedalusSymbol& symbol, const storygraph::FnSummary& summary) {
  const auto returnType = symbol.has_return() ? symbol.rtype() : DaedalusDataType::VOID;
  if(symbol.has_local_variables_enabled() || !signatureType(returnType,true))
    return std::nullopt;
  Entry entry;
  entry.id = symbol.index();
  entry.name = symbol.name();
  entry.returnType = uint32_t(returnType);
  for(uint32_t i=0; i<symbol.count(); ++i) {
    const auto* parameter = script.find_symbol_by_index(symbol.index()+1+i);
    if(parameter==nullptr || !signatureType(parameter->type(),false))
      return std::nullopt;
    entry.parameters.push_back(uint32_t(parameter->type()));
    }
  entry.program = storygraph::liftProgram(script,code,entry.id);
  if(!portable(entry.program))
    return std::nullopt;
  addSymbol(catalog,script,symbol.name());
  for(const auto& callee : summary.calls)
    entry.calls.push_back(addSymbol(catalog,script,callee).index());
  captureAliases(catalog,script,entry.program.steps);
  return entry;
  }

void capturePrograms(Catalog& catalog, DaedalusVm& vm, const storygraph::Bytecode& code,
                     const std::filesystem::path& base) {
  std::vector<uint32_t> pending;
  const auto addRoot = [&](std::string_view name) {
    if(!name.empty())
      if(const auto* symbol = vm.find_symbol_by_name(name))
        pending.push_back(symbol->index());
    };
  // Find the same dialog roots as the storygraph tool.
  vm.enumerate_instances_by_class_name("C_INFO",[&](auto& symbol) {
    const auto info = vm.init_instance<IInfo>(&symbol);
    for(auto fn : {info->condition,info->information})
      if(fn>0)
        pending.push_back(uint32_t(fn));
    });

  // Item playback uses the first on_state slot of each prepared constructor.
  const auto items = ItemDefinitions::read(base,stockProgram);
  for(const auto& [name,item] : items.items)
    addRoot(item.symbols.at("on_state").front());

  // World triggers and MOBSI callbacks are named by the imported VOB data.
  const auto visit = [&](const auto& visit, const asset::VirtualObject& vob) -> void {
    if(const auto* trigger = dynamic_cast<const asset::VTriggerScript*>(&vob))
      addRoot(trigger->function);
    if(const auto* mob = dynamic_cast<const asset::VInteractiveObject*>(&vob)) {
      addRoot(mob->condition_function);
      if(!mob->on_state_change_function.empty())
        for(int32_t state=0; state<=mob->state_count; ++state)
          addRoot(mob->on_state_change_function+"_S"+std::to_string(state));
      }
    for(const auto& child : vob.children)
      if(child!=nullptr)
        visit(visit,*child);
    };
  for(const auto& dir : std::filesystem::directory_iterator(base/"worlds")) {
    if(!dir.is_directory())
      continue;
    const auto world = WorldData::read(dir.path());
    for(const auto& vob : world.vobs)
      if(vob!=nullptr)
        visit(visit,*vob);
    }

  std::set<uint32_t> visited;
  for(size_t i=0; i<pending.size(); ++i) {
    const auto id = pending[i];
    if(!visited.insert(id).second)
      continue;
    const auto* symbol = vm.find_symbol_by_index(id);
    if(symbol==nullptr || symbol->type()!=DaedalusDataType::FUNCTION || !storygraph::Bytecode::hasCode(*symbol))
      continue;
    const auto summary = code.summarize(id);
    pending.insert(pending.end(),summary.choices.begin(),summary.choices.end());
    for(const auto& name : summary.calls)
      if(const auto* callee = vm.find_symbol_by_name(name))
        pending.push_back(callee->index());
    if(auto entry = captureEntry(catalog,vm,code,*symbol,summary))
      catalog.programs.emplace(id,std::move(*entry));
    }
  }
}

void BaseImport::convertStockStoryPrograms(const std::filesystem::path& base) {
  auto source = NpcSnapshot::load(base);
  if(source.sha256!=stockProgram)
    return;
  const auto metadata = ProgramData::symbolMetadataPath(base,source.sha256);
  std::filesystem::create_directories(metadata.parent_path());
  ProgramData::writeSymbolMetadata(metadata,source.sha256,source.data);

  auto script = DaedalusScript::from_program(std::move(source.data));
  std::vector<ScriptSymbolCatalog::Entry> symbols;
  symbols.reserve(script.symbols().size());
  for(const auto& symbol : script.symbols())
    symbols.push_back({symbol.name(),symbolKind(script,symbol)});
  ScriptSymbolCatalog::write(base,source.sha256,symbols);

  register_all_script_classes(script);
  DaedalusVm vm(std::move(script),DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  vm.register_default_external([](const DaedalusSymbol&) {});
  const storygraph::Bytecode code(vm);
  Catalog catalog;
  captureTransferConstants(catalog,vm);
  capturePrograms(catalog,vm,code,base);
  const auto* laresSymbol = vm.find_symbol_by_name("DIA_ADDON_LARES_PATCH_INFO");
  require(laresSymbol!=nullptr && catalog.programs.contains(laresSymbol->index()),"story programs: Lares's function was not captured");
  const auto& lares = catalog.programs.at(laresSymbol->index());
  require(lares.parameters.empty() && lares.returnType==uint32_t(DaedalusDataType::VOID) &&
          lares.calls==std::vector<uint32_t>{catalog.symbols.at("B_GIVEINVITEMS")},
          "story programs: Lares's function or dependencies changed");
  writeCatalog(base,catalog);

  const auto actual = StockStoryPrograms::read(base);
  require(actual.programs.size()==catalog.programs.size() && actual.symbols==catalog.symbols &&
          actual.strings==catalog.strings && actual.integers==catalog.integers,
          "story programs: catalog differs after reading");
  for(const auto& [id,expected] : catalog.programs) {
    const auto& entry = actual.programs.at(id);
    require(entry.name==expected.name && entry.returnType==expected.returnType &&
            entry.parameters==expected.parameters && entry.calls==expected.calls &&
            sameSteps(entry.program.steps,expected.program.steps),
            "story programs: function differs after reading "+expected.name);
    }
  }
