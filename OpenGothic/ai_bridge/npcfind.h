#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include "game/gamescript.h"
#include "world/objects/npc.h"
#include "world/world.h"

namespace AiNpc {

inline constexpr float GiveRange = 350;  // cm: an npc hands an item over from this close

inline std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

// An npc by script symbol ("PLAYER" for the hero). Symbols of monsters are shared by all of their kind
// ("WOLF"): with `near` the closest living one is taken, not just the first in the world.
inline Npc* findNpc(World& world, std::string_view name, const Npc* near = nullptr) {
  if(name=="PLAYER" || name=="HERO")
    return world.player();
  const auto id = world.script().storyNpcId(name);
  if(!id)
    return nullptr;
  if(near!=nullptr) {
    Npc*  best  = nullptr;
    float bestD = 0;
    world.detectNpc(near->position(),5000,[&](Npc& n) {
      if(n.instanceSymbol()!=*id || n.isDead() || &n==near)
        return;
      const float d = n.qDistTo(*near);
      if(best==nullptr || d<bestD) {
        best  = &n;
        bestD = d;
        }
      });
    if(best!=nullptr)
      return best;
    }
  return world.findNpcByInstance(*id);
  }

// a script state function (ZS_ATTACK, ZS_FLEE...), invalid if the scripts have none by that name
inline ScriptFn scriptState(World& world, std::string_view name) {
  const auto id = world.script().storyScriptFunctionId(name);
  return id ? ScriptFn(*id) : ScriptFn();
  }

// the name of a script state function, empty if none
inline std::string stateName(World& world, ScriptFn fn) {
  return fn.isValid() ? std::string(world.script().storyProgramName(uint32_t(fn.ptr))) : std::string();
  }

// `from` hands up to `count` of `item` to `to` once it has walked up (within twice GiveRange): an attached
// npc's give_item, a free npc's give to another (v15)
inline bool giveItem(Npc& from, Npc& to, size_t item, size_t count, std::string& err) {
  if(from.qDistTo(to)>GiveRange*GiveRange*4) {
    err = "could not reach the target";
    return false;
    }
  const size_t have = from.inventory().itemCount(item);
  if(have==0) {
    err = "npc no longer has the item";
    return false;
    }
  to.addItem(item,from,std::min(have,count));
  return true;
  }

// walking, running: anything but standing at its place or doing something there
inline bool isMoving(const Npc& npc) {
  const auto bs = npc.bodyStateMasked();
  return bs==BS_WALK || bs==BS_SNEAK || bs==BS_RUN || bs==BS_SPRINT;
  }

}
