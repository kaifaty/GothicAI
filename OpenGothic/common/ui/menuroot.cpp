#include "menuroot.h"

#include <Tempest/Log>

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "game/definitions/baseprogram.h"
#include "game/definitions/basescripts.h"
#include "programdata.h"
#include "ui/gamemenu.h"
#include "gothic.h"
#include "settings.h"

#include <algorithm>

using namespace Tempest;

namespace {
std::string menuSymbol(std::string_view name) {
  std::string key(name);
  std::transform(key.begin(),key.end(),key.begin(),[](char ch) {
    return ch>='a' && ch<='z' ? char(ch-'a'+'A') : ch;
    });
  return key;
  }

void applyVideoOptions() {
  Log::d("TODO: apply_options_video");
  }
}

MenuRoot::MenuRoot(KeyCodec& keyCodec)
  :keyCodec(keyCodec) {
  setCursorShape(CursorShape::Hidden);

  Settings::inst().onSettingsChanged.bind(this,&MenuRoot::initSettings);
  initSettings();
  }

MenuRoot::~MenuRoot() {
  Settings::inst().onSettingsChanged.ubind(this,&MenuRoot::initSettings);
  }

void MenuRoot::initSettings() {
  const auto lang = Settings::getI("GAME", "language");
  if(lang==menuLanguage && (vm!=nullptr || records.has_value()))
    return;

  std::string sha;
  auto script = loadBaseProgram("MENU.DAT",lang,&sha);
  const auto base = baseProgramIndex().dir.parent_path();
  if(sha==MenuDefinitions::stockProgram && std::filesystem::exists(MenuDefinitions::section(base,sha))) {
    records = MenuDefinitions::read(base,sha);
    vm.reset();
    } else {
    vm = Gothic::inst().createPhoenixVm(std::move(script));
    vm->register_external("apply_options_video",applyVideoOptions);
    records.reset();
    }
  menuLanguage = lang;

  for(auto& i:menuStack)
    i->reloadItems();
  if(current!=nullptr)
    current->reloadItems();

  update();
  }

bool MenuRoot::hasMenu(std::string_view name) const {
  if(records)
    return records->menus.contains(menuSymbol(name));
  return vm->find_symbol_by_name(name)!=nullptr;
  }

std::shared_ptr<MenuDefinitions::Menu> MenuRoot::menu(std::string_view name) {
  if(records)
    return std::make_shared<MenuDefinitions::Menu>(records->menus.at(menuSymbol(name)));
  return vm->init_instance<daedalus::IMenu>(name);
  }

std::shared_ptr<MenuDefinitions::MenuItem> MenuRoot::menuItem(std::string_view name) {
  if(records) {
    const auto item = records->items.find(menuSymbol(name));
    if(item!=records->items.end())
      return std::make_shared<MenuDefinitions::MenuItem>(item->second);
    } else if(auto* symbol = vm->find_symbol_by_name(name)) {
    return vm->init_instance<daedalus::IMenuItem>(symbol);
    }
  Log::e("Cannot initialize menu item ",name,": Symbol not found.");
  return std::make_shared<MenuDefinitions::MenuItem>();
  }

std::optional<std::pair<int32_t,int32_t>> MenuRoot::infoPosition() const {
  if(records)
    return records->infoPosition;
  const auto* x = vm->find_symbol_by_name("MENU_INFO_X");
  const auto* y = vm->find_symbol_by_name("MENU_INFO_Y");
  if(x!=nullptr && y!=nullptr)
    return {{x->get_int(),y->get_int()}};
  return std::nullopt;
  }

void MenuRoot::execute(int32_t symbol) {
  if(symbol<=0)
    return;
  if(!records) {
    if(const auto* fn = vm->find_symbol_by_index(uint32_t(symbol)))
      vm->call_function(fn);
    return;
    }

  // The callbacks may publish an event that changes settings and reloads this catalog.
  const auto actions = records->actions.at(symbol);
  for(const auto& action : actions) {
    switch(action.kind) {
      case MenuDefinitions::Action::Kind::Video:
        Gothic::inst().onVideo(action.video,false);
        break;
      case MenuDefinitions::Action::Kind::ApplyVideoOptions:
        applyVideoOptions();
        break;
      case MenuDefinitions::Action::Kind::ApplyPerformanceOptions:
        Gothic::notImplementedRoutine("APPLY_OPTIONS_PERFORMANCE");
        break;
      }
    }
  }

void MenuRoot::setMainMenu() {
  setMenu(Gothic::inst().menuMain());
  showVersion(true);
  }

void MenuRoot::setMenu(std::string_view menuEv, KeyCodec::Action key) {
  if(!hasMenu(menuEv)) {
    Log::e("invalid menu-id: ",menuEv);
    return;
    }
  setMenu(new GameMenu(*this,keyCodec,menuEv,key));
  }

void MenuRoot::setMenu(GameMenu *w) {
  removeAllWidgets();
  if(w)
    addWidget(w);
  current=w;
  }

void MenuRoot::pushMenu(GameMenu *w) {
  if(widgetsCount()>0) {
    takeWidget(current);
    menuStack.emplace_back(current);
    }
  if(w) {
    addWidget(w);
    w->onTick();
    }
  current = w;
  }

void MenuRoot::popMenu() {
  if(menuStack.size()==0) {
    if(Gothic::inst().isInGame() || Gothic::inst().checkLoading()!=Gothic::LoadState::Idle) {
      current=nullptr;
      removeAllWidgets();
      owner()->setFocus(true);
      }
    return;
    }
  GameMenu* w = menuStack.back().release();

  removeAllWidgets();
  addWidget(w);
  current=w;
  current->onTick();

  menuStack.pop_back();
  }

void MenuRoot::closeAll() {
  menuStack.clear();
  if(Gothic::inst().isInGame() || Gothic::inst().checkLoading()!=Gothic::LoadState::Idle) {
    current=nullptr;
    removeAllWidgets();
    }
  }

bool MenuRoot::isActive() const {
  return current!=nullptr;
  }

void MenuRoot::setPlayer(const Npc &pl) {
  if(current!=nullptr)
    current->setPlayer(pl);
  }

void MenuRoot::processMusicTheme() {
  if(current!=nullptr)
    current->processMusicTheme();
  }

void MenuRoot::showVersion(bool s) {
  showVersionHint = s;
  update();
  }

bool MenuRoot::hasVersionLine() const {
  return showVersionHint;
  }

void MenuRoot::mouseDownEvent(MouseEvent& event) {
  if(current!=nullptr) {
    if(event.button==Event::ButtonRight) {
      popMenu();
      } else {
      current->onKeyboard(KeyCodec::ActionGeneric);
      }
    } else {
    event.ignore();
    }
  }

void MenuRoot::mouseUpEvent(MouseEvent&) {
  }

void MenuRoot::mouseWheelEvent(MouseEvent &event) {
  if(current!=nullptr) {
    if(event.delta>0)
      current->onKeyboard(KeyCodec::Forward); else
    if(event.delta<0)
      current->onKeyboard(KeyCodec::Back);
    } else {
    event.ignore();
    }
  }

void MenuRoot::keyRepeatEvent(Tempest::KeyEvent &e) {
  if(current==nullptr) {
    e.ignore();
    return;
    }

  if(e.key == Event::K_A || e.key == Event::K_Left)
    current->onKeyboard(KeyCodec::Left);
  else if(e.key == Event::K_D || e.key == Event::K_Right)
    current->onKeyboard(KeyCodec::Right);
  else if(e.key == Event::K_W || e.key == Event::K_Up)
    current->onKeyboard(KeyCodec::Forward);
  else if(e.key == Event::K_S || e.key == Event::K_Down)
    current->onKeyboard(KeyCodec::Back);
  }

void MenuRoot::keyDownEvent(KeyEvent &e) {
  const float scale = Gothic::interfaceScale(this);
  size_t sz = std::extent_v<decltype(cheatCode)>;
  for(size_t i=1; i<sz; ++i)
    cheatCode[i-1] = cheatCode[i];
  cheatCode[sz-1] = e.key;
  if(cheatCode[0]==Event::K_M &&
     cheatCode[1]==Event::K_A &&
     cheatCode[2]==Event::K_R &&
     cheatCode[3]==Event::K_V &&
     cheatCode[4]==Event::K_I &&
     cheatCode[5]==Event::K_N) {
    Gothic::inst().setMarvinEnabled(true);
    auto& fnt = Resources::font(scale);
    Gothic::inst().onPrintScreen("MARVIN-MODE",2,4, 1,fnt);
    }

  if(cheatCode[sz-2]==Event::K_4 &&
     cheatCode[sz-1]==Event::K_2) {
    Gothic::inst().setMarvinEnabled(false);
    auto& fnt = Resources::font(scale);
    Gothic::inst().onPrintScreen("WHAT WAS THE QUESTION?",2,4, 1,fnt);
    }

  if(current!=nullptr) {
    if(e.key==Event::K_W || e.key==Event::K_Up)
      current->onKeyboard(KeyCodec::Forward);
    else if(e.key==Event::K_S || e.key==Event::K_Down)
      current->onKeyboard(KeyCodec::Back);
    else if(e.key==Event::K_A || e.key==Event::K_Left)
      current->onKeyboard(KeyCodec::Left);
    else if(e.key==Event::K_D || e.key==Event::K_Right)
      current->onKeyboard(KeyCodec::Right);
    else if(e.key==Event::K_Return)
      current->onKeyboard(KeyCodec::ActionGeneric);
    else if(e.key==Event::K_Delete)
      current->onKeyboard(KeyCodec::K_Del);
    else if(e.key==Event::K_ESCAPE || keyCodec.tr(e)==current->keyClose())
      popMenu();
    }
  }
