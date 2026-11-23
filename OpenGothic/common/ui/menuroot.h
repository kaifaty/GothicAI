#pragma once

#include <Tempest/Widget>

#include "game/definitions/menudefinitions.h"

#include <memory>

#include "utils/keycodec.h"

class GameMenu;
class Npc;
namespace daedalus { class DaedalusVm; }

class MenuRoot : public Tempest::Widget {
  public:
    MenuRoot(KeyCodec& keyCodec);
    ~MenuRoot() override;

    void setMainMenu();
    void setMenu(std::string_view menu, KeyCodec::Action key = KeyCodec::Escape);
    void setMenu(GameMenu* w);
    void pushMenu(GameMenu* w);
    void popMenu();
    void closeAll();
    bool isActive() const;
    void setPlayer(const Npc& pl);
    void processMusicTheme();

    void showVersion(bool s);
    bool hasVersionLine() const;

    void mouseWheelEvent(Tempest::MouseEvent& event) override;
    void keyRepeatEvent (Tempest::KeyEvent&   event) override;
    void keyDownEvent   (Tempest::KeyEvent&   event) override;

  protected:
    void mouseDownEvent (Tempest::MouseEvent& event) override;
    void mouseUpEvent   (Tempest::MouseEvent& event) override;

  private:
    friend class GameMenu;

    void initSettings();
    bool hasMenu(std::string_view name) const;
    std::shared_ptr<MenuDefinitions::Menu> menu(std::string_view name);
    std::shared_ptr<MenuDefinitions::MenuItem> menuItem(std::string_view name);
    std::optional<std::pair<int32_t,int32_t>> infoPosition() const;
    void execute(int32_t symbol);

    std::unique_ptr<daedalus::DaedalusVm>    vm;
    std::optional<MenuDefinitions::Catalog> records;
    int32_t                                menuLanguage = -1;

    GameMenu*                              current=nullptr;
    std::vector<std::unique_ptr<GameMenu>> menuStack;
    KeyCodec&                              keyCodec;

    Tempest::Event::KeyType                cheatCode[6] = {};
    bool                                   showVersionHint = false;
  };
