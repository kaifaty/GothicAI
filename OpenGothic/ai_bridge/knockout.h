#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

// Pure preflight for the far-fight outcome. No world or controller state changes on refusal.
namespace Knockout {
struct Actor {
  bool        exists = false;
  bool        human = false;
  bool        hero = false;
  bool        dead = false;
  bool        down = false;
  bool        immortal = false;
  bool        insideSimulation = false;
  bool        bodyControlled = false;
  bool        externalControl = false;
  int32_t     hp = 0;
  std::string weapon;
  };

struct Result {
  std::string error;
  int32_t     winnerHp = 0;
  std::string dropped;
  };

inline Result check(const Actor& self, const Actor& by, bool same, int64_t loss) {
  if(loss<0)
    return {"knockout: loss must be a nonnegative integer",0,{}};
  for(bool loser:{true,false}) {
    const auto& actor = loser ? self : by;
    const char* role = loser ? "self" : "by";
    const char* reason = !actor.exists ? "no npc" :
                         !actor.human ? "not human" :
                         actor.hero ? "hero" :
                         actor.dead || actor.hp<=0 ? "dead" :
                         actor.down ? "down" :
                         actor.immortal ? "immortal" :
                         actor.insideSimulation ? "inside simulation" :
                         actor.bodyControlled ? "body controlled" :
                         actor.externalControl ? "external control" : nullptr;
    if(reason!=nullptr)
      return {std::string("knockout: ")+reason+": "+role,0,{}};
    }
  if(same)
    return {"knockout: same npc",0,{}};
  return {{},int32_t(std::max<int64_t>(1,int64_t(by.hp)-loss)),self.weapon};
  }
}
