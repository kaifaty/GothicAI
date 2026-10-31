#pragma once

#include <rapidjson/document.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/constants.h"
#include "game/gamescript.h"

class Npc;
class World;
class WayPoint;
class Interactive;

// NPCs taken off script control (routines, perceptions) and driven by the external AI server.
// One action in flight per NPC; a new action preempts the current one.
class NpcController final {
  public:
    using Emit = std::function<void(std::string&&)>;

    explicit NpcController(Emit emit);

    bool attach(World& world, std::string_view symbol, std::string& err);
    bool detach(World& world, std::string_view symbol, std::string_view reason, std::string& err);
    void detachAll(World& world, std::string_view reason);
    // world is about to be destroyed: forget npc's without touching them
    void dropAll(std::string_view reason);

    void action(World& world, const rapidjson::Value& msg);
    // a snapshot of the npc now: attached ones get theirs on the next tick, free ones (v7) at once;
    // false if there is no such npc
    bool requestSnapshot(World& world, std::string_view symbol);
    void tick(World& world);
    void perception(World& world, Npc& self, Npc& other, Npc* victim, PercType perc);

    bool        isAttached(std::string_view symbol) const;
    // attached npc, if it is still alive in this world
    Npc*        find(World& world, std::string_view symbol);
    std::string status(World* world) const;

    std::function<void(const std::string& symbol, std::string_view reason)> onDetached;
    // follow mode ended on its own (target lost/too far/timeout); the npc stays attached
    std::function<void(const std::string& symbol, std::string_view reason)> onFollowEnded;

  private:
    struct Action final {
      std::string     id;
      std::string     type;
      uint64_t        start    = 0;
      uint64_t        deadline = 0;
      uint64_t        timeout  = 0;     // ms, counted once the npc is done talking
      const WayPoint* point    = nullptr;
      size_t          target   = 0;     // instance of target npc
      bool            toPlayer = false;
      float           radius   = 0;     // success radius for movement, cm
      bool            closeApproach = false; // explicit short scene approach, with a clear ray
      Tempest::Vec3    movedFrom;        // go_to_npc: last movement anchor for stuck detection
      Tempest::Vec3    plannedFrom;      // where the previous route was planned
      uint64_t        movedAt  = 0;
      uint64_t        nextProgress = 0;
      uint64_t        updatedAt = 0;     // last known tick/distance, safe even after the world is gone
      double          distance = -1;
      size_t          replans  = 0;
      bool            byWay    = false;
      bool            walkStarted = false;
      size_t          item     = 0;     // give_item, take_item: item instance and count (take: what it had)
      size_t          count    = 0;
      ScriptFn        state;            // fight/flee: the script state that runs it
      uint64_t        stateStart = 0;   // fight/flee: startup clock after queued speech
      const Npc*      targetNpc = nullptr; // fight/flee: exactly whom (symbols of monsters are shared)
      std::string     animation;
      uint64_t        holdMs = 0;
      uint64_t        heldAt = 0;
      bool            entered  = false; // the npc is in that state already
      // use_chest (v28): the chest, what to take out and put in (item, count), how far it got
      Interactive*    chest    = nullptr;
      std::vector<std::pair<size_t,size_t>> take, put;
      bool            opened   = false; // the items have moved, it closes the chest now
      bool            relock   = false; // its lock was opened for its owner: locked again after
      std::string     moved;            // what moved: "took=ITEM:N,…;put=ITEM:N,…"
      std::string     pace;             // go_to_waypoint (v38): run, walk, or hero (his, as a guide ahead)
      };

    struct Follow final {
      size_t          target   = 0;
      bool            toPlayer = false;
      uint64_t        until    = 0;
      uint64_t        nextStep = 0;
      Tempest::Vec3   movedFrom;          // where it last was a step away from: stuck if it stays there
      uint64_t        movedAt  = 0;
      bool            byWay    = false;   // going round by the waynet, a straight walk having got stuck
      };

    struct Controlled final {
      size_t                instance = 0;
      std::string           symbol;
      Npc*                  npc      = nullptr;
      std::optional<Action> act;
      std::optional<Follow> follow;
      uint64_t              nextSnapshot = 0;
      bool                  scripted     = false; // handed to a script state (fight/flee) for now
      };

    Controlled* find(std::string_view symbol);
    const Controlled* find(std::string_view symbol) const;

    bool startAction(World& world, Controlled& c, Action& a, const rapidjson::Value& args, std::string& err);
    void planNpcWalk(World& world, Controlled& c, Action& a, Npc& target, bool byWay);
    void updateAction(World& world, Controlled& c, uint64_t now);
    void updateStateAction(World& world, Controlled& c, uint64_t now);
    bool startScriptState(World& world, Controlled& c, Action& a, std::string_view state, const rapidjson::Value& args,
                          std::string& err);
    void regainControl(Controlled& c);
    void keepHeroPace(World& world, Npc& npc);
    void updateFollow(World& world, Controlled& c, uint64_t now);
    void stopFollow(Controlled& c, std::string_view reason);
    bool giveItem(World& world, Controlled& c, const Action& a, std::string& err);
    void finishAction(Controlled& c, std::string_view status, std::string_view detail, double distance = -1);
    void useChest(World& world, Controlled& c, Action& a);
    void release(Controlled& c, bool resumeRoutine);

    void emitDetached(std::string_view symbol, std::string_view reason);
    void emitActionResult(std::string_view id, std::string_view npc, std::string_view status,
                          std::string_view detail, double distance);
    void emitSnapshot(World& world, Controlled& c);
    void emitSnapshot(World& world, Npc& npc, std::string_view symbol, std::string_view action, bool following);

    Emit                                   emit;
    std::vector<Controlled>                npcs;
    std::unordered_map<uint64_t,uint64_t>  percThrottle;
  };
