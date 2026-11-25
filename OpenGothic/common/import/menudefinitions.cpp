#include "baseimport.h"

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "definitionjson.h"
#include "game/definitions/menudefinitions.h"
#include "programdata.h"

#include <algorithm>
#include <stdexcept>

namespace {
using namespace MenuDefinitions;
using DefinitionJson::require;

Catalog captureMenu(const asset::ProgramRecord& program) {
  auto script = daedalus::DaedalusScript::from_program(program);
  daedalus::IMenu::register_(script);
  daedalus::IMenuItem::register_(script);
  daedalus::DaedalusVm vm(std::move(script),daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);

  Catalog catalog;
  vm.observe_writes([](const auto& symbol) {
    require(symbol.is_member(),"menu definition writes shared script state: "+symbol.name());
    });
  vm.observe_externals([](const auto& symbol) {
    throw std::runtime_error("menu definition calls an engine function: "+symbol.name());
    });
  const daedalus::DaedalusInstance* owner = nullptr;
  vm.observe_member_writes([&](const auto&, uint16_t, const auto* target) {
    require(target==owner,"menu definition mutates another instance");
    });
  vm.observe_member_reads([&](const auto&, uint16_t, const auto* target) {
    require(target==owner,"menu definition reads another instance");
    });
  vm.enumerate_instances_by_class_name("C_MENU",[&](auto& symbol) {
    const auto menu = std::make_shared<daedalus::IMenu>();
    owner = menu.get();
    vm.init_instance(menu,&symbol);
    catalog.menus.emplace(symbol.name(),*menu);
    });
  vm.enumerate_instances_by_class_name("C_MENU_ITEM",[&](auto& symbol) {
    const auto item = std::make_shared<daedalus::IMenuItem>();
    owner = item.get();
    vm.init_instance(item,&symbol);
    catalog.items.emplace(symbol.name(),*item);
    });

  const auto* x = vm.find_symbol_by_name("MENU_INFO_X");
  const auto* y = vm.find_symbol_by_name("MENU_INFO_Y");
  if(x!=nullptr && y!=nullptr)
    catalog.infoPosition = {x->get_int(),y->get_int()};

  // The engine calls only EXECUTE. Other event slots are retained as authored data.
  vm.observe_externals({});
  vm.observe_member_writes({});
  vm.observe_member_reads({});
  vm.observe_writes([](const auto& symbol) {
    throw std::runtime_error("menu callback writes script state: "+symbol.name());
    });
  std::vector<Action> actions;
  vm.register_external("playvideo",[&](std::string_view name) {
    actions.push_back({Action::Kind::Video,std::string(name)});
    return true;
    });
  vm.register_external("apply_options_video",[&] {
    actions.push_back({Action::Kind::ApplyVideoOptions,{}});
    });
  vm.register_default_external([&](std::string_view name) {
    require(name=="APPLY_OPTIONS_PERFORMANCE","unsupported menu callback effect: "+std::string(name));
    actions.push_back({Action::Kind::ApplyPerformanceOptions,{}});
    });
  for(const auto& [name,item] : catalog.items) {
    const auto symbol = item.on_event_action[uint8_t(MenuItemSelectEvent::EXECUTE)];
    if(symbol<=0 || catalog.actions.contains(symbol))
      continue;
    const auto* fn = vm.find_symbol_by_index(uint32_t(symbol));
    require(fn!=nullptr,"menu callback missing for "+name);
    vm.call_function(fn);
    catalog.actions.emplace(symbol,std::move(actions));
    actions.clear();
    }
  return catalog;
  }
}

void BaseImport::convertMenuDefinitions(const std::filesystem::path& base) {
  const auto index = ProgramData::readIndex(base/"programs");
  const auto it = std::ranges::find_if(index.selections,[&](const auto& choice) {
    return choice.request=="MENU.DAT" && choice.source &&
           index.sources[*choice.source].recordSha256==MenuDefinitions::stockProgram;
    });
  if(it==index.selections.end())
    return;
  const auto& source = index.sources[*it->source];
  std::string sha;
  const auto program = ProgramData::readProgram(index.path(source),&sha);
  require(sha==source.recordSha256,"menu definitions: program record differs from index");

  const auto expected = captureMenu(program);
  MenuDefinitions::write(base,sha,expected);
  require(MenuDefinitions::read(base,sha)==expected,"menu definitions differ after reading");
  }
