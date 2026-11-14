#include "game/meleedodge.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
  void require(bool condition, const char* message) {
    if(condition)
      return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }

  void requireNear(float actual, float expected, const char* message) {
    if(std::isfinite(actual) && std::abs(actual-expected)<.0001f)
      return;
    std::fprintf(stderr,"FAIL: %s: actual %.9g, expected %.9g\n",message,double(actual),double(expected));
    std::exit(EXIT_FAILURE);
    }

  void requireName(const std::string& actual, const char* expected, const char* message) {
    if(actual==expected)
      return;
    std::fprintf(stderr,"FAIL: %s: actual %s, expected %s\n",message,actual.c_str(),expected);
    std::exit(EXIT_FAILURE);
    }
  }

int main() {
  using namespace MeleeDodge;
  using MeleeImpact::ArmorClass;

  // Direction: the movement keys at the press, relative to the facing; forward/back wins over a side; none is back.
  require(direction( 0.f, 0.f)==Direction::Back,   "no direction dodges back");
  require(direction( 1.f, 0.f)==Direction::Forward,"forward");
  require(direction(-1.f, 0.f)==Direction::Back,   "back");
  require(direction( 0.f,-1.f)==Direction::Left,   "left");
  require(direction( 0.f, 1.f)==Direction::Right,  "right");
  require(direction( 1.f,-1.f)==Direction::Forward,"forward-left takes forward");
  require(direction( 1.f, 1.f)==Direction::Forward,"forward-right takes forward");
  require(direction(-1.f,-1.f)==Direction::Back,   "back-left takes back");
  require(direction(-1.f, 1.f)==Direction::Back,   "back-right takes back");
  // The keys that turn the hero (the arrows of the preset) give the side when no strafe key is held
  require(direction( 0.f, 0.f,-1.f)==Direction::Left, "turn left alone dodges left");
  require(direction( 0.f, 0.f, 1.f)==Direction::Right,"turn right alone dodges right");
  require(direction( 0.f, 1.f,-1.f)==Direction::Right,"a strafe key wins over a turn key");
  require(direction( 0.f,-1.f, 1.f)==Direction::Left, "strafe left wins over turn right");
  require(direction( 1.f, 0.f,-1.f)==Direction::Forward,"forward wins over a turn key");
  require(direction(-1.f, 0.f, 1.f)==Direction::Back, "back wins over a turn key");
  require(direction( 0.f, 0.f, 0.f)==Direction::Back, "no key at all dodges back");

  // Kind: by the armour's class, none is light.
  require(kind(std::nullopt)==Kind::Dash,          "no armour dashes");
  require(kind(ArmorClass::Light)==Kind::Dash,     "light armour dashes");
  require(kind(ArmorClass::Medium)==Kind::Roll,    "medium armour rolls");
  require(kind(ArmorClass::Heavy)==Kind::HeavyRoll,"heavy armour rolls slowly");
  require(kind(MeleeImpact::armorClass(39))==Kind::Dash,     "edge protection 39 is light");
  require(kind(MeleeImpact::armorClass(40))==Kind::Roll,     "edge protection 40 is medium");
  require(kind(MeleeImpact::armorClass(79))==Kind::Roll,     "edge protection 79 is medium");
  require(kind(MeleeImpact::armorClass(80))==Kind::HeavyRoll,"edge protection 80 is heavy");

  // Animation names, by the weapon prefix of T_%sJUMPB.
  requireName(animationName(Kind::Dash,Stance::OneHand,Direction::Back),"T_1HDASHB","1H dash back");
  requireName(animationName(Kind::Roll,Stance::TwoHand,Direction::Left),"T_2HROLLL","2H roll left");
  requireName(animationName(Kind::HeavyRoll,Stance::Fist,Direction::Forward),"T_FISTROLLHEAVYF","fist heavy roll forward");
  requireName(animationName(Kind::Dash,Stance::Fist,Direction::Right),"T_FISTDASHR","fist dash right");

  // Fallback chain: the armour's move, the dash the same way, then the old jump back only backwards.
  {
    const auto p = plan(Kind::Roll,Stance::OneHand,Direction::Left);
    require(p.candidates.size()==2 && !p.jumpBack,"roll left: two names, no jump back");
    requireName(p.candidates[0].name,"T_1HROLLL","roll left first");
    require(p.candidates[0].kind==Kind::Roll,"roll left first is a roll");
    requireName(p.candidates[1].name,"T_1HDASHL","roll left falls back to the dash left");
    require(p.candidates[1].kind==Kind::Dash,"the fallback is a dash");
  }
  {
    const auto p = plan(Kind::HeavyRoll,Stance::TwoHand,Direction::Back);
    require(p.candidates.size()==2 && p.jumpBack,"heavy roll back: two names, then the jump back");
    requireName(p.candidates[0].name,"T_2HROLLHEAVYB","heavy roll back first");
    requireName(p.candidates[1].name,"T_2HDASHB","heavy roll back falls back to the dash back");
  }
  {
    const auto p = plan(Kind::Dash,Stance::OneHand,Direction::Right);
    require(p.candidates.size()==1 && !p.jumpBack,"dash right: one name, no other dodge");
    requireName(p.candidates[0].name,"T_1HDASHR","dash right");
  }
  {
    const auto p = plan(Kind::Dash,Stance::Fist,Direction::Back);
    require(p.candidates.size()==1 && p.jumpBack,"fist dash back: one name, then the jump back");
    requireName(p.candidates[0].name,"T_FISTDASHB","fist dash back");
  }
  {
    const auto p = plan(Kind::Roll,Stance::OneHand,Direction::Forward);
    require(p.candidates.size()==2 && !p.jumpBack,"roll forward has no jump back");
  }
  {
    const auto p = plan(kind(ArmorClass::Heavy),Stance::OneHand,direction(1.f,0.f));
    require(p.candidates.size()==2 && !p.jumpBack,"heavy armour forward uses the slow roll then the dash");
    require(p.candidates[0].kind==Kind::HeavyRoll,"heavy armour forward chooses the slow roll");
    requireName(p.candidates[0].name,"T_1HROLLHEAVYF","heavy armour forward uses T_1HROLLHEAVYF");
    require(p.candidates[1].kind==Kind::Dash,"heavy armour forward falls back to the dash");
    }

  // The playing move is told by its name; every name the rules make reads back.
  for(auto k:{Kind::Dash,Kind::Roll,Kind::HeavyRoll})
    for(auto s:{Stance::Fist,Stance::OneHand,Stance::TwoHand})
      for(auto d:{Direction::Back,Direction::Left,Direction::Right,Direction::Forward}) {
        const auto m = parseAnimation(animationName(k,s,d));
        require(m && m->kind==k && m->stance==s && m->direction==d,"every dodge name reads back");
        }
  require(parseAnimation("t_1hdashb").has_value(),"names compare without case");
  for(const char* other:{"T_1HJUMPB","T_JUMPB","T_1HPARADEJUMPB","T_FISTPARADEJUMPB","T_1HDASHBX","T_1HDASH",
                         "S_1HDASHB","T_BOWDASHB","T_1HROLLHEAVY","T_DASHB","T_1HSFINISH",""})
    require(!parseAnimation(other).has_value(),"other animations are not dodges");

  // Out of a parade: a block parade is cut at once; a jump back (some carry a defence window) and a dodge are not.
  for(const char* block:{"T_1HPARADE_0","T_1HPARADE_0_A2","T_1HPARADE_0_A3","T_2HPARADE_0","T_FISTPARADE_0","T_1HPARADE_O"})
    require(cutsParade(block,true),"a block parade with its defence window is cut");
  require(!cutsParade("T_1HPARADE_0",false),"a layer without a defence window is no parade");
  for(const char* jump:{"T_1HPARADEJUMPB","T_2HPARADEJUMPB","T_FISTPARADEJUMPB","T_1HJUMPB","T_JUMPB","t_1hParadeJumpB"}) {
    require(isJumpBackAnimation(jump),"Gothic's jump backs are told by name");
    require(!cutsParade(jump,true),"a jump back is not cut, even with a defence window");
    }
  require(!isJumpBackAnimation("T_1HPARADE_0") && !isJumpBackAnimation("S_JUMP") && !isJumpBackAnimation("JUMPB"),
          "a parade or a jump is no jump back");
  require(!cutsParade("T_1HDASHB",true) && !cutsParade("T_2HROLLL",true),"a dodge is not cut by a dodge");

  // Invulnerability, from the start of the move, both ends included.
  require(!invulnerable(Kind::Dash,39), "dash: 39 ms is open");
  require( invulnerable(Kind::Dash,40), "dash: 40 ms is covered");
  require( invulnerable(Kind::Dash,360),"dash: 360 ms is covered");
  require(!invulnerable(Kind::Dash,361),"dash: 361 ms is open");
  require(!invulnerable(Kind::Dash,0),  "dash: the first frame is open");
  require(!invulnerable(Kind::Roll,39) && invulnerable(Kind::Roll,40) && invulnerable(Kind::Roll,470) &&
          !invulnerable(Kind::Roll,471),"roll: 40-470 ms");
  require(!invulnerable(Kind::HeavyRoll,39) && invulnerable(Kind::HeavyRoll,40) &&
          invulnerable(Kind::HeavyRoll,390) && !invulnerable(Kind::HeavyRoll,391),"heavy roll: 40-390 ms");
  require(invulnerability(Kind::Dash).beginMs==40 && invulnerability(Kind::Dash).endMs==360,"dash window");

  // Costs, and the rules of the jump back for paying them.
  requireNear(cost(Kind::Dash),15.f,"dash costs 15");
  requireNear(cost(Kind::Roll),20.f,"roll costs 20");
  requireNear(cost(Kind::HeavyRoll),30.f,"heavy roll costs 30");
  requireNear(charged(Kind::HeavyRoll,true),30.f,"with melee_stamina the cost is paid");
  requireNear(charged(Kind::HeavyRoll,false),0.f,"without melee_stamina nothing is paid");

  MeleeStamina::State state;
  MeleeStamina::reset(state,100.f,1000);
  MeleeStamina::drain(state,86.f,1000);
  requireNear(state.current,14.f,"14 stamina left");
  require(!staminaAllows(state,Kind::Dash,true,true),"native AI: 14 is short of a dash");
  require( staminaAllows(state,Kind::Dash,false,true),"player: a positive remainder starts a dash");
  require( staminaAllows(state,Kind::HeavyRoll,false,true),"player: a positive remainder starts a heavy roll");
  MeleeStamina::reset(state,100.f,1000);
  MeleeStamina::drain(state,85.f,1000);
  require( staminaAllows(state,Kind::Dash,true,true),"native AI: 15 pays a dash");
  require(!staminaAllows(state,Kind::Roll,true,true),"native AI: 15 is short of a roll");
  MeleeStamina::drain(state,charged(Kind::Roll,true),1000);
  require(state.current==0.f && state.exhausted,"a dash dearer than the rest empties the bar");
  require(!staminaAllows(state,Kind::Dash,false,true),"empty bar: the player is refused");
  require(!staminaAllows(state,Kind::Dash,true,true),"empty bar: the native AI is refused");
  MeleeStamina::recover(state,100.f,1000+800+700,false);
  requireNear(state.current,24.5f,"recovery to 24.5");
  require(!staminaAllows(state,Kind::Dash,false,true),"below a quarter the player is still refused");
  MeleeStamina::recover(state,100.f,1000+800+715,false);
  require(staminaAllows(state,Kind::Dash,false,true),"a quarter back allows the dodge again");
  MeleeStamina::drain(state,100.f,3000);
  require(staminaAllows(state,Kind::HeavyRoll,true,false) && staminaAllows(state,Kind::HeavyRoll,false,false),
          "without melee_stamina an empty bar does not refuse");

  // When a dodge may start.
  Situation fight;
  fight.meleeWeapon = true;
  require(refusal(fight)==Refusal::None,"in the fighting stance");
  for(auto a:{Activity::Walk,Activity::Run,Activity::Parade}) {
    auto s = fight;
    s.activity = a;
    require(refusal(s)==Refusal::None,"walking, running and from a parade");
    }
  {
    auto s = fight; s.enabled = false;
    require(refusal(s)==Refusal::Disabled,"melee_dodge=0 refuses");
    s = fight; s.meleeWeapon = false;
    require(refusal(s)==Refusal::NoMeleeWeapon,"no melee weapon: no dodge (the key jumps)");
    s = fight; s.attacking = true;
    require(refusal(s)==Refusal::Attacking,"not in a blow");
    s = fight; s.stunned = true;
    require(refusal(s)==Refusal::Stunned,"not stunned");
    s = fight; s.knockedDown = true;
    require(refusal(s)==Refusal::KnockedDown,"not knocked down");
    s = fight; s.inAir = true;
    require(refusal(s)==Refusal::InAir,"not in the air");
    s = fight; s.inWater = true;
    require(refusal(s)==Refusal::InWater,"not in water");
    s = fight; s.dodging = true;
    require(refusal(s)==Refusal::Dodging,"not while dodging");
    s = fight; s.activity = Activity::Other;
    require(refusal(s)==Refusal::Busy,"not from another state");
  }

  // The guard against a melee blow (Npc::meleeGuard asks it for the damage and both views of the melee shadow).
  {
    struct Case final {
      const char*    name;
      GuardSituation situation;
      Guard          expected;
      };
    const Case cases[] = {
      {"open",                                 {},                                                 {}},
      {"a jump back evades",                   {.jumpBack=true},                                   {.jumpedBack=true}},
      {"a parade blocks",                      {.parade=true},                                     {.blocked=true}},
      {"a parade's jump back pose does both",  {.jumpBack=true,.parade=true},                      {.jumpedBack=true,.blocked=true}},
      {"no block against bare claws",          {.jumpBack=true,.parade=true,.unblockable=true},    {.jumpedBack=true}},
      {"stunned: no parade",                   {.stunned=true,.parade=true},                       {}},
      {"stunned: no jump back",                {.stunned=true,.jumpBack=true},                     {}},
      {"knocked down: no parade",              {.knockedDown=true,.parade=true},                   {}},
      {"knocked down: no jump back",           {.knockedDown=true,.jumpBack=true},                 {}},
      {"a dodge's window passes",              {.dodging=true,.dodgeWindow=true,.jumpBack=true,.parade=true},
                                                                                                   {.dodged=true}},
      {"out of its window a dodge is open",    {.dodging=true,.jumpBack=true,.parade=true},        {}},
      {"stun does not close a dodge's window", {.dodging=true,.dodgeWindow=true,.stunned=true},    {.dodged=true}},
      };
    for(const auto& c:cases) {
      const auto g = guard(c.situation);
      if(g.dodged!=c.expected.dodged || g.jumpedBack!=c.expected.jumpedBack || g.blocked!=c.expected.blocked) {
        std::fprintf(stderr,"FAIL: guard, %s: dodged %d jumped back %d blocked %d\n",c.name,
                     int(g.dodged),int(g.jumpedBack),int(g.blocked));
        std::exit(EXIT_FAILURE);
        }
      }
  }
  std::puts("melee dodge contract passed");
  }
