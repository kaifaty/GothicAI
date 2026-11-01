#include "meleedodge.h"

#include <cctype>

namespace {
  std::string_view prefix(MeleeDodge::Stance stance) {
    switch(stance) {
      case MeleeDodge::Stance::Fist:    return "FIST";
      case MeleeDodge::Stance::OneHand: return "1H";
      case MeleeDodge::Stance::TwoHand: return "2H";
      }
    return "1H";
    }

  std::string_view move(MeleeDodge::Kind kind) {
    switch(kind) {
      case MeleeDodge::Kind::Dash:      return "DASH";
      case MeleeDodge::Kind::Roll:      return "ROLL";
      case MeleeDodge::Kind::HeavyRoll: return "ROLLHEAVY";
      }
    return "DASH";
    }

  char letter(MeleeDodge::Direction direction) {
    switch(direction) {
      case MeleeDodge::Direction::Back:    return 'B';
      case MeleeDodge::Direction::Left:    return 'L';
      case MeleeDodge::Direction::Right:   return 'R';
      case MeleeDodge::Direction::Forward: return 'F';
      }
    return 'B';
    }

  bool take(std::string_view& text, std::string_view head) {
    if(text.substr(0,head.size())!=head)
      return false;
    text.remove_prefix(head.size());
    return true;
    }
  }

MeleeDodge::Direction MeleeDodge::direction(float forwardAxis, float strafeAxis, float turnAxis) {
  if(forwardAxis>0.f)
    return Direction::Forward;
  if(forwardAxis<0.f)
    return Direction::Back;
  const float side = strafeAxis!=0.f ? strafeAxis : turnAxis;
  if(side>0.f)
    return Direction::Right;
  if(side<0.f)
    return Direction::Left;
  return Direction::Back;
  }

MeleeDodge::Kind MeleeDodge::kind(std::optional<MeleeImpact::ArmorClass> armor) {
  if(!armor)
    return Kind::Dash;
  switch(*armor) {
    case MeleeImpact::ArmorClass::Light:  return Kind::Dash;
    case MeleeImpact::ArmorClass::Medium: return Kind::Roll;
    case MeleeImpact::ArmorClass::Heavy:  return Kind::HeavyRoll;
    }
  return Kind::Dash;
  }

std::string MeleeDodge::animationName(Kind kind, Stance stance, Direction direction) {
  std::string name = "T_";
  name += prefix(stance);
  name += move(kind);
  name += letter(direction);
  return name;
  }

MeleeDodge::Plan MeleeDodge::plan(Kind kind, Stance stance, Direction direction) {
  Plan p;
  p.candidates.push_back({kind,animationName(kind,stance,direction)});
  if(kind!=Kind::Dash)
    p.candidates.push_back({Kind::Dash,animationName(Kind::Dash,stance,direction)});
  p.jumpBack = direction==Direction::Back;
  return p;
  }

std::optional<MeleeDodge::Move> MeleeDodge::parseAnimation(std::string_view name) {
  if(name.size()>32)
    return std::nullopt;
  std::string upper(name);
  for(auto& c:upper)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  std::string_view rest = upper;
  if(!take(rest,"T_"))
    return std::nullopt;
  Move m;
  if(take(rest,"FIST"))
    m.stance = Stance::Fist;
  else if(take(rest,"1H"))
    m.stance = Stance::OneHand;
  else if(take(rest,"2H"))
    m.stance = Stance::TwoHand;
  else
    return std::nullopt;
  if(take(rest,"DASH"))
    m.kind = Kind::Dash;
  else if(take(rest,"ROLLHEAVY"))
    m.kind = Kind::HeavyRoll;
  else if(take(rest,"ROLL"))
    m.kind = Kind::Roll;
  else
    return std::nullopt;
  if(rest.size()!=1)
    return std::nullopt;
  switch(rest[0]) {
    case 'B': m.direction = Direction::Back;    break;
    case 'L': m.direction = Direction::Left;    break;
    case 'R': m.direction = Direction::Right;   break;
    case 'F': m.direction = Direction::Forward; break;
    default:  return std::nullopt;
    }
  return m;
  }

bool MeleeDodge::isJumpBackAnimation(std::string_view name) {
  constexpr std::string_view tail = "JUMPB";
  if(name.size()<tail.size()+2)
    return false;
  for(size_t i=0; i<tail.size(); ++i)
    if(std::toupper(static_cast<unsigned char>(name[name.size()-tail.size()+i]))!=tail[i])
      return false;
  return std::toupper(static_cast<unsigned char>(name[0]))=='T' && name[1]=='_';
  }

bool MeleeDodge::cutsParade(std::string_view name, bool defenceWindow) {
  return defenceWindow && !isJumpBackAnimation(name) && !parseAnimation(name).has_value();
  }

MeleeDodge::Window MeleeDodge::invulnerability(Kind kind) {
  // Starting numbers (reports/Свои анимации для боя.md, «Цель»): from the second frame on; the kinds differ by
  // their recovery, the end of a roll and the rise of a heavy one stay open. Balance is a later task.
  switch(kind) {
    case Kind::Dash:      return {40,360};
    case Kind::Roll:      return {40,470};
    case Kind::HeavyRoll: return {40,390};
    }
  return {40,360};
  }

bool MeleeDodge::invulnerable(Kind kind, uint64_t elapsedMs) {
  const auto w = invulnerability(kind);
  return w.beginMs<=elapsedMs && elapsedMs<=w.endMs;
  }

float MeleeDodge::cost(Kind kind) {
  switch(kind) {
    case Kind::Dash:      return DashCost;
    case Kind::Roll:      return RollCost;
    case Kind::HeavyRoll: return HeavyRollCost;
    }
  return DashCost;
  }

bool MeleeDodge::staminaAllows(const MeleeStamina::State& state, Kind kind, bool fullCost, bool staminaEnabled) {
  return fullCost ? MeleeStamina::canAfford(state,cost(kind),staminaEnabled) :
                    MeleeStamina::canAct(state,staminaEnabled);
  }

float MeleeDodge::charged(Kind kind, bool staminaEnabled) {
  return staminaEnabled ? cost(kind) : 0.f;
  }

MeleeDodge::Refusal MeleeDodge::refusal(const Situation& s) {
  if(!s.enabled)
    return Refusal::Disabled;
  if(!s.meleeWeapon)
    return Refusal::NoMeleeWeapon;
  if(s.knockedDown)
    return Refusal::KnockedDown;
  if(s.stunned)
    return Refusal::Stunned;
  if(s.inAir)
    return Refusal::InAir;
  if(s.inWater)
    return Refusal::InWater;
  if(s.attacking)
    return Refusal::Attacking;
  if(s.dodging)
    return Refusal::Dodging;
  if(s.activity==Activity::Other)
    return Refusal::Busy;
  return Refusal::None;
  }

std::string_view MeleeDodge::name(Direction direction) {
  switch(direction) {
    case Direction::Back:    return "back";
    case Direction::Left:    return "left";
    case Direction::Right:   return "right";
    case Direction::Forward: return "forward";
    }
  return "back";
  }

std::string_view MeleeDodge::name(Kind kind) {
  switch(kind) {
    case Kind::Dash:      return "dash";
    case Kind::Roll:      return "roll";
    case Kind::HeavyRoll: return "heavy_roll";
    }
  return "dash";
  }

std::string_view MeleeDodge::name(Refusal refusal) {
  switch(refusal) {
    case Refusal::None:          return "none";
    case Refusal::Disabled:      return "disabled";
    case Refusal::NoMeleeWeapon: return "no_melee_weapon";
    case Refusal::Attacking:     return "attacking";
    case Refusal::Stunned:       return "stunned";
    case Refusal::KnockedDown:   return "knocked_down";
    case Refusal::InAir:         return "in_air";
    case Refusal::InWater:       return "in_water";
    case Refusal::Dodging:       return "dodging";
    case Refusal::Busy:          return "busy";
    }
  return "none";
  }

std::string_view MeleeDodge::name(std::optional<MeleeImpact::ArmorClass> armor) {
  if(!armor)
    return "none";
  switch(*armor) {
    case MeleeImpact::ArmorClass::Light:  return "light";
    case MeleeImpact::ArmorClass::Medium: return "medium";
    case MeleeImpact::ArmorClass::Heavy:  return "heavy";
    }
  return "none";
  }
