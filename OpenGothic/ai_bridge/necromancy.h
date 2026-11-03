#pragma once

#include <Tempest/Texture2d>
#include <Tempest/UiOverlay>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class Npc;
class World;

// The necromancer's answer, an ending off the main game (docs/xardas-wrath.md): an npc kills the hero
// with a spell, raises him where he fell as a skeleton, and the game ends on a picture. The server starts
// it with the story effect `raise_dead`; the console with `ai necro <npc> [spell]`.
//
// Stages, in world time (the test bench's fixed step films it as played):
// - cast: the npc turns to the hero and casts its spell at him until he falls; a hero the spell does not kill
//   is finished off, and one the spell never reaches dies by the spell's effect after a while;
// - fallen: the hero lies dead for a moment;
// - raise: the npc readies the summoning, the summoning's light rises from the body;
// - risen: the hero lives again as a skeleton, getting up from the ground (T_SPAWN of the skeleton);
// - card: the screen goes black, the picture comes up with a title; a key ends the game (main menu).
class Necromancy final {
  public:
    struct Card final {
      std::string image;  // a picture file (png, jpg), drawn whole above the words; empty: words only
      std::string title;  // in the game's codepage
      std::string text;
      };

    Necromancy();
    ~Necromancy();

    // the spell is a script symbol (SPL_SKULL), its rune is given to the npc
    bool start(World& world, Npc& caster, Card card, std::string spell, std::string& error);
    void tick(World& world);
    bool active() const { return stage!=Stage::Idle; }
    // it has got as far as that stage: cast, fallen, raise, risen, card (the test bench waits for them)
    bool reached(std::string_view name) const;
    // the hero has fallen: the talk is over, its subtitles would only cover the scene
    bool hidesChat() const { return stage>=Stage::Fallen; }
    // a load or a new world: what runs belongs to the old one
    void clear();
    std::string status() const;
    // the card's key: what ends the game after it (the final card); empty: the main menu at once
    std::function<void()> onEnd;

  private:
    enum class Stage : uint8_t { Idle, Cast, Fallen, Raise, Risen, Card };

    void enter(World& world, Stage s);
    bool cast(World& world, Npc& caster, Npc& hero);  // true: a cast began
    void raise(World& world, Npc& caster, Npc& hero);
    void rise(World& world, Npc& hero);

    Stage       stage     = Stage::Idle;
    uint64_t    since     = 0;  // world ms the stage began
    uint64_t    nextCast  = 0;
    uint64_t    castFrom  = 0;  // the first cast (after the npc's last words), 0: not yet
    Npc*        caster    = nullptr;
    int32_t     heroHp    = 0;  // when the cast began: a hit shows as less
    std::string spell;
    Card        card;

    class Screen;
    std::unique_ptr<Screen> screen;
  };
