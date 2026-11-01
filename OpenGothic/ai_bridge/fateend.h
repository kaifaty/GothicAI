#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "game/chronicle.h"

class World;

// The playthrough's final card (docs/achievements.md, stage 4): at any end of the game - the hero's death,
// the necromancer's answer, the story's finale - the screen goes black and the chronicler's summary of this
// fate comes up: its name, up to three of its best deeds with their engravings, one line about it all. The
// server writes it (fate_end -> fate_card, protocol v56); without the server, or with the card off, the game
// ends as it did before. A key takes the player to the main menu.
//
// Stages, in world time (the test bench's fixed step films it as played):
// - dead: the hero lies dead a moment before the card is asked for (a death only);
// - asked: black, the chronicler is still writing;
// - card: the card comes up; after a moment a key ends the game.
class FateEnd final {
  public:
    // kind (death, wrath, finale), the killer's symbol and name in the game's codepage; the answer comes
    // once, on the main thread
    using Ask = std::function<void(std::string kind, std::string killer, std::string name,
                                   std::function<void(const FateCard&)> answer)>;

    FateEnd();
    ~FateEnd();

    void setAsk(Ask a) { ask = std::move(a); }
    // watches for the hero's death; `busy`: another ending has the hero (the necromancer's)
    void tick(World& world, bool busy);
    // the game ends by the necromancer's card or the story's finale: the card, then the main menu.
    // `black`: the screen is black already
    void finish(World& world, std::string_view kind, bool black);
    bool active() const { return stage!=Stage::Idle && stage!=Stage::Done; }
    // it has got as far as that stage: dead, asked, card; "off": the server gave no card
    bool reached(std::string_view name) const;
    // a load or a new world: what runs belongs to the old one
    void clear();
    std::string status() const;

  private:
    enum class Stage : uint8_t { Idle, Dead, Asked, Card, Done };

    void request(World& world, std::string kind, std::string killer, std::string name);
    void answer(uint64_t gen, const FateCard& card);
    void end();

    Stage       stage    = Stage::Idle;
    uint64_t    since    = 0;  // world ms the stage began
    uint64_t    gen      = 0;  // answers to an older request are dropped
    bool        exits    = false;  // the session ends after the card (wrath, finale), not on a death
    bool        off      = false;
    std::string kind;
    FateCard    card;
    Ask         ask;

    class Screen;
    std::unique_ptr<Screen> screen;
  };
