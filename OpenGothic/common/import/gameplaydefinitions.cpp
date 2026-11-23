#include "baseimport.h"
#include "game/definitions/gameplaydefinitions.h"
#include "npcsnapshot.h"

#include "daedalus/classes.h"

namespace {
using namespace DefinitionJson;
using namespace daedalus;

template<class T>
void capture(DaedalusVm& vm, Json& rows, DaedalusSymbol& symbol, Allocator& allocator) {
  const auto instance = [&] {
    if constexpr(std::is_same_v<T,DaedalusOpaqueInstance>)
      return vm.init_opaque_instance(&symbol);
    else
      return vm.init_instance<T>(&symbol);
    }();
  Json row(rapidjson::kObjectType);
  for(auto* member : vm.find_class_members(GameplayDefinitions::instanceClass(vm,symbol))) {
    Json values(rapidjson::kArrayType);
    for(uint16_t index=0; index<member->count(); ++index) {
      Json value;
      switch(member->type()) {
        case DaedalusDataType::INT: value.SetInt(member->get_int(index,instance.get())); break;
        case DaedalusDataType::FLOAT: value.SetFloat(member->get_float(index,instance.get())); break;
        case DaedalusDataType::STRING:
          value = text(encode(member->get_string(index,instance.get())),allocator);
          break;
        default: throw std::runtime_error("gameplay definitions: unsupported member "+member->name());
        }
      values.PushBack(value,allocator);
      }
    put(row,member->name().c_str(),std::move(values),allocator);
    }
  put(rows,symbol.name().c_str(),std::move(row),allocator);
  }

template<class T>
void capture(DaedalusVm& vm, Json& rows, std::string_view name, Allocator& allocator) {
  auto* symbol = vm.find_symbol_by_name(name);
  require(symbol!=nullptr,"gameplay definitions: missing instance "+std::string(name));
  capture<T>(vm,rows,*symbol,allocator);
  }
}

void BaseImport::convertGameplayDefinitions(const std::filesystem::path& base) {
  auto program = NpcSnapshot::load(base);
  if(program.sha256!=GameplayDefinitions::stockProgram)
    return;

  auto script = DaedalusScript::from_program(std::move(program.data));
  ISpell::register_(script);
  IFocus::register_(script);
  IGuildValues::register_(script);
  script.register_as_opaque("C_SVM");
  DaedalusVm vm(std::move(script));
  vm.observe_externals([](const auto& external) {
    throw std::runtime_error("gameplay definitions: unexpected external "+external.name());
    });
  vm.observe_writes([](const auto& symbol) {
    require(symbol.is_member(),"gameplay definitions: unexpected global write "+symbol.name());
    });
  vm.observe_reads([](const auto& symbol) {
    require(symbol.is_member() || symbol.is_const() || symbol.is_generated(),
            "gameplay definitions: unexpected mutable read "+symbol.name());
    });

  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  Json rows(rapidjson::kObjectType);
  vm.enumerate_instances_by_class_name("C_SPELL",[&](auto& symbol) { capture<ISpell>(vm,rows,symbol,allocator); });
  for(auto name : GameplayDefinitions::focusNames)
    capture<IFocus>(vm,rows,name,allocator);
  capture<IGuildValues>(vm,rows,"GIL_VALUES",allocator);
  vm.enumerate_instances_by_class_name("C_SVM",[&](auto& symbol) { capture<DaedalusOpaqueInstance>(vm,rows,symbol,allocator); });

  put(document,"format",text(GameplayDefinitions::format,allocator),allocator);
  put(document,"version",Json(GameplayDefinitions::version),allocator);
  put(document,"program_sha256",text(program.sha256,allocator),allocator);
  put(document,"instances",std::move(rows),allocator);

  const auto path = GameplayDefinitions::section(base);
  std::filesystem::create_directories(path.parent_path());
  DefinitionJson::write(path,document);
  }
