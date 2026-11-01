#include "knockout.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <tuple>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if(condition) return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }

auto snapshot(const Knockout::Actor& actor) {
  return std::make_tuple(actor.exists,actor.human,actor.hero,actor.dead,actor.down,actor.immortal,
                         actor.insideSimulation,actor.bodyControlled,actor.externalControl,
                         actor.hp,actor.weapon);
  }
}

int main() {
  using Knockout::Actor;
  const Actor loser{true,true,false,false,false,false,false,false,false,100,"ITMW_SWORD"};
  const Actor winner{true,true,false,false,false,false,false,false,false,80,{}};

  const auto normal = Knockout::check(loser,winner,false,30);
  require(normal.error.empty() && normal.winnerHp==50 && normal.dropped=="ITMW_SWORD",
          "a valid far fight plans the winner's loss and the loser's equipped weapon");
  const auto overkill = Knockout::check(loser,winner,false,200);
  require(overkill.error.empty() && overkill.winnerHp==1,
          "a loss greater than the winner's HP leaves one HP");
  const auto zero = Knockout::check(loser,winner,false,0);
  require(zero.error.empty() && zero.winnerHp==80,
          "zero loss preserves the winner's HP");
  const auto maximum = Knockout::check(loser,winner,false,std::numeric_limits<int64_t>::max());
  require(maximum.error.empty() && maximum.winnerHp==1,
          "the largest loss cannot overflow into positive HP");
  auto unarmed = loser;
  unarmed.weapon.clear();
  const auto noWeapon = Knockout::check(unarmed,winner,false,0);
  require(noWeapon.error.empty() && noWeapon.dropped.empty(),
          "an unarmed loser produces no dropped item");
  require(Knockout::check(loser,loser,true,0).error=="knockout: same npc",
          "one NPC cannot be both winner and loser");
  require(Knockout::check(loser,winner,false,-1).error==
          "knockout: loss must be a nonnegative integer","negative loss is refused");

  const struct {
    bool Actor::* field;
    bool          invalid;
    const char*   reason;
    } refusals[] = {
    {&Actor::exists,false,"no npc"},
    {&Actor::human,false,"not human"},
    {&Actor::hero,true,"hero"},
    {&Actor::dead,true,"dead"},
    {&Actor::down,true,"down"},
    {&Actor::immortal,true,"immortal"},
    {&Actor::insideSimulation,true,"inside simulation"},
    {&Actor::bodyControlled,true,"body controlled"},
    {&Actor::externalControl,true,"external control"},
    };
  for(bool self:{true,false}) {
    for(const auto& refusal:refusals) {
      auto a = loser;
      auto b = winner;
      (self ? a : b).*refusal.field = refusal.invalid;
      const auto beforeA = snapshot(a);
      const auto beforeB = snapshot(b);
      const auto result = Knockout::check(a,b,false,30);
      const auto expected = std::string("knockout: ")+refusal.reason+(self ? ": self" : ": by");
      require(result.error==expected && result.winnerHp==0 && result.dropped.empty(),
              "each invalid participant refuses without planning damage or a drop");
      require(snapshot(a)==beforeA && snapshot(b)==beforeB,
              "refusal leaves both participants unchanged");
      }
    auto a = loser;
    auto b = winner;
    (self ? a : b).hp = 0;
    require(Knockout::check(a,b,false,30).error==
            (self ? "knockout: dead: self" : "knockout: dead: by"),
            "zero HP refuses even before the script state reports death");
    }
  require(snapshot(loser)==snapshot(Actor{true,true,false,false,false,false,false,false,false,100,"ITMW_SWORD"}) &&
          snapshot(winner)==snapshot(Actor{true,true,false,false,false,false,false,false,false,80,{}}),
          "successful preflight also leaves HP and equipment unchanged");
  std::printf("Knockout contract: %d checks passed\n",checks);
  }
