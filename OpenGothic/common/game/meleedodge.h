#pragma once

#include "game/meleeimpact.h"
#include "game/meleestamina.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The melee dodge (step 6 of the NRFTW port, [GAME] melee_dodge): its own action, the direction of the movement
// keys at the press, the kind of move by the armour, its stamina cost and a window without any incoming harm.
// Pure rules: no world, no base. Npc::meleeDodge plays them; the playing move is told by its animation's name.
namespace MeleeDodge {
  // Relative to the dodger's facing.
  enum class Direction : uint8_t { Back, Left, Right, Forward };
  // Light armour (or none) dashes, medium rolls, heavy rolls slowly.
  enum class Kind      : uint8_t { Dash, Roll, HeavyRoll };
  // The weapon prefix of the animation names, as T_%sJUMPB has it: FIST, 1H, 2H.
  enum class Stance    : uint8_t { Fist, OneHand, TwoHand };
  // What the dodger is doing at the press, by its body state.
  enum class Activity  : uint8_t { Stand, Walk, Run, Parade, Other };
  enum class Refusal   : uint8_t { None, Disabled, NoMeleeWeapon, Attacking, Stunned, KnockedDown, InAir, InWater,
                                   Dodging, Busy };

  // Milliseconds from the start of the move, both ends included.
  struct Window final {
    uint64_t beginMs = 0;
    uint64_t endMs   = 0;
    };

  struct Situation final {
    bool     enabled     = true;  // [GAME] melee_dodge
    bool     meleeWeapon = false; // a melee weapon or the fists drawn
    bool     attacking   = false; // in a blow, from its swing on
    bool     stunned     = false;
    bool     knockedDown = false;
    bool     inAir       = false; // jumping, falling, sliding
    bool     inWater     = false; // wading above the knee, swimming, diving
    bool     dodging     = false; // a dodge plays already
    Activity activity    = Activity::Stand;
    };

  // A dodge animation: T_<stance><kind><direction>, e.g. T_1HDASHB, T_2HROLLL, T_FISTROLLHEAVYF.
  struct Move final {
    Kind      kind      = Kind::Dash;
    Stance    stance    = Stance::OneHand;
    Direction direction = Direction::Back;
    };

  struct Candidate final {
    Kind        kind = Kind::Dash;
    std::string name;
    };

  // The animations to look for, in order: the move of the armour's kind, then the dash the same way. None of them
  // there: back plays the original jump back (jumpBack), other ways there is no dodge.
  struct Plan final {
    std::vector<Candidate> candidates;
    bool                   jumpBack = false;
    };

  constexpr float DashCost      = 15.f;
  constexpr float RollCost      = 20.f;
  constexpr float HeavyRollCost = 30.f;

  // forwardAxis > 0: forward held, < 0: back; strafeAxis > 0: right, < 0: left. Both axes: forward/back wins;
  // neither: back.
  Direction   direction(float forwardAxis, float strafeAxis);
  // No armour: light.
  Kind        kind(std::optional<MeleeImpact::ArmorClass> armor);
  std::string animationName(Kind kind, Stance stance, Direction direction);
  Plan        plan(Kind kind, Stance stance, Direction direction);
  std::optional<Move> parseAnimation(std::string_view name);

  Window      invulnerability(Kind kind);
  bool        invulnerable(Kind kind, uint64_t elapsedMs);

  float       cost(Kind kind);
  // The rules of the jump back: the full cost for the native AI (fullCost), a positive remainder for the player;
  // an empty bar refuses both until a quarter is back; without melee_stamina nothing is asked or paid.
  bool        staminaAllows(const MeleeStamina::State& state, Kind kind, bool fullCost, bool staminaEnabled);
  float       charged(Kind kind, bool staminaEnabled);

  Refusal     refusal(const Situation& situation);

  std::string_view name(Direction direction);
  std::string_view name(Kind kind);
  std::string_view name(Refusal refusal);
  std::string_view name(std::optional<MeleeImpact::ArmorClass> armor);
  }
