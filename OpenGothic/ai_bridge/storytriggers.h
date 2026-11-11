#pragma once

#include <rapidjson/document.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "deathscan.h"
#include "expr.h"
#include "program.h"

class Npc;
class StoryRuntime;
class World;

namespace daedalus {
  class DaedalusVm;
  }

// The director's triggers (docs/director.md, stage 3): an event, a condition and a program, as a graph_patch
// operation ("trigger"). The bridge counts them: it has the engine's events and runs the programs
// (StoryRuntime), with or without the server.
//
// Events: `state` — the condition, computed four times a second (self: the trigger's npc or the hero, other:
// the hero); `death` — somebody died (self: the dead, other: the killer, if any); `dialog` — a dialog or a
// choice ran (self: the npc, other: the hero). A new event is a new source here and a new name of `on`.
//
// Every trigger has a story variable of the director's by its id: how many times it fired. The registry of
// own symbols makes it and the save keeps it, so a trigger that fires once (the default) fires while its
// condition holds and it has not fired yet, before a save as after a load. One that repeats fires when its
// condition comes true: what held when it was put or the world was loaded is no change. Conditions and
// programs read the count as a variable.
class StoryTriggers final {
  public:
    StoryTriggers(StoryRuntime& rt, std::function<void(std::string&&)> send);

    // a "trigger" operation of graph_patch; empty, or why it was not applied
    std::string apply(const rapidjson::Value& op);
    // patches anew: the server sends the branch's triggers again
    void        clear();
    // a load or a new world: the deaths of the old one are not news of the new one
    void        clearWorld();

    // the state triggers, four times a second, and the deaths
    void        tick(World& world);
    // a dialog or a choice ran (DialogGraph::chosen)
    void        onDialog(World& world, Npc& npc, const std::string& node);

    // debug `triggers`: [{"id", "on", "npc", "node", "repeat", "count", "holds" (state: the condition now, null:
    // not computed), "broken"?}]
    std::string list(World& world);
    std::string status() const;

  private:
    enum class On : uint8_t { State, Death, Dialog };
    struct Trigger final {
      std::string         id;       // its count's variable too
      On                  on = On::State;
      std::string         npc;      // state: self; death, dialog: whose (empty: anybody's)
      std::string         node;     // dialog: which dialog or choice (empty: any)
      std::string         world;    // empty: any
      storygraph::ExprP   when;     // nullptr: always
      storygraph::Program program;
      bool                repeat = false;
      std::optional<bool> was;      // repeat: the condition as last computed, none yet
      daedalus::DaedalusVm* checkedFor = nullptr;
      std::string         broken;   // why it can't run in checkedFor
      };

    // its names exist in this VM; logged once if not
    bool        ready(Trigger& t, daedalus::DaedalusVm& v);
    std::string check(const Trigger& t, daedalus::DaedalusVm& v) const;
    bool        inWorld(const Trigger& t, World& world) const;
    // a trigger that fires once and fired: it does not any more
    bool        spent(const Trigger& t);
    // with self and other bound: the condition (nullopt: not computed)
    std::optional<bool> holds(const Trigger& t);
    // with self and other bound: the count grows, the program runs, the server hears it
    void        fire(World& world, Trigger& t, Npc* self, Npc* other);
    void        onDeath(World& world, Npc& dead, Npc* killer);

    StoryRuntime&                      rt;
    std::function<void(std::string&&)> send;
    std::vector<Trigger>               triggers;  // in the order they were put
    DeathScan                          deaths;
    uint64_t                           nextState = 0, nextDeaths = 0;
    size_t                             fired = 0, failed = 0;
  };
