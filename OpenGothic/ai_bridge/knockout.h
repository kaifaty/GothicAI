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

inline std::string participants(const Actor& self, const Actor& by, bool same, const char* effect) {
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
      return std::string(effect)+": "+reason+": "+role;
    }
  if(same)
    return std::string(effect)+": same npc";
  return {};
  }

inline int32_t remainingHp(int32_t hp, int64_t loss) {
  return int32_t(std::max<int64_t>(1,int64_t(hp)-loss));
  }

inline Result check(const Actor& self, const Actor& by, bool same, int64_t loss) {
  if(loss<0)
    return {"knockout: loss must be a nonnegative integer",0,{}};
  if(auto error = participants(self,by,same,"knockout"); !error.empty())
    return {error,0,{}};
  return {{},remainingHp(by.hp,loss),self.weapon};
  }

struct WoundResult {
  std::string error;
  int32_t     selfHp = 0;
  int32_t     byHp = 0;
  };

inline WoundResult wound(const Actor& self, const Actor& by, bool same, int64_t loss, int64_t byLoss) {
  if(loss<0)
    return {"wound: loss must be a nonnegative integer",0,0};
  if(byLoss<0)
    return {"wound: by_loss must be a nonnegative integer",0,0};
  if(auto error = participants(self,by,same,"wound"); !error.empty())
    return {error,0,0};
  return {{},remainingHp(self.hp,loss),remainingHp(by.hp,byLoss)};
  }
}
