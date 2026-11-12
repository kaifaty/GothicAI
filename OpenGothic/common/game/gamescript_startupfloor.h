#pragma once

#include <Tempest/Vec>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class GameScript;
class Npc;

// One floor fix per NPC on a fresh world start (docs/world-streaming.md).
//
// STARTUP_/INIT_ insert every NPC with Wld_InsertNpc, which used to run GameScript::fixNpcPosition at once;
// WorldObjects::resetPositionToTA then moves each NPC to its routine point and fixes it again. Inside that
// window the insert-time fix is left pending: the NPC keeps the position its insertion set.
//
// A pending fix is run ("materialised") before anything could observe a difference:
//  - a script external that may read or change it (classify(): Args — its NPC arguments, All — every pending NPC);
//  - exact mode only: another NPC's fix or move during the final placement that could touch its possible positions;
//  - the end of the window, for an NPC the placement did not move.
// The final placement clears it otherwise, since it runs its own fix from the routine point.
//
// A materialised fix reproduces the insert-time one exactly. It sees what that fix saw: older NPCs at their
// insert-time positions (older pending ones near it are materialised first) and no newer NPC (their bodies are
// hidden through DynamicWorld::setNpcReplaySeq). The radius is the farthest reach of a fix: distBias 150 + 800 cm,
// plus body radii.
//
// OPENGOTHIC_STARTUP_FLOOR_DEFER selects the mode:
//  - unset or `loose` (default): the final placement has no barriers, so an NPC's final fix may see a not-yet-placed
//    neighbour at its insert position instead of its insert-fixed one. Not exact: on NEWWORLD 21 of 1053 NPCs start
//    0.5-2 m apart (mostly monster packs); the day parity (tolerances-6c) passes; 1.1-1.3 s faster on a fresh start.
//  - `exact`: every move near a pending NPC materialises it first, so the result is byte-identical to `0`; the
//    placement then still runs ~1020 of 1054 fixes, so it is not faster. Kept to measure.
//  - `0`: the fix at insertion, as before.
class StartupFloor final {
  public:
    explicit StartupFloor(GameScript& owner);

    enum class Access : uint8_t {
      None, // reads/changes no position, body, floor, point or collision
      Args, // reads/changes only its NPC arguments (newer NPCs near them saw them in their fix)
      All,  // anything else, also unknown externals
      };
    static Access classify(std::string_view external);
    static bool   enabled();

    size_t registerExternal(std::string_view name, Access a);

    void begin();
    void end();
    bool active() const { return isActive; }

    // Wld_InsertNpc: instead of the insert-time fix.
    void inserted(Npc& npc);
    // A script external is about to run; args are its NPC arguments.
    void external(size_t id, Npc* const* args, size_t count);
    // Final placement: npc is about to leave its position for dest (its routine point or the dead point).
    void beforeResetMove(Npc& npc, const Tempest::Vec3& dest);
    // Final placement leaves npc where it is (a kept body).
    void beforeKeep(Npc& npc);
    void forget(Npc& npc);

  private:
    struct Entry {
      Npc*          npc    = nullptr;
      Tempest::Vec3 anchor = {};
      uint32_t      seq    = 0;
      };
    enum Reason : uint8_t {
      R_External,
      R_Neighbour,
      R_ResetMove,
      R_Keep,
      R_Dead,
      R_End,
      R_Count
      };
    struct ExternalStat {
      std::string name;
      Access      access         = Access::All;
      uint64_t    calls          = 0;
      uint64_t    callsPending   = 0; // while any fix was pending
      uint64_t    pendingArgs    = 0; // NPC arguments with a pending fix
      uint64_t    materialised   = 0;
      };

    void   materialise(Npc& npc, Reason r);
    void   materialiseNear(const Tempest::Vec3& at, float reach, uint32_t olderThan, Reason r);
    void   materialiseNewerNear(const Tempest::Vec3& at, float reach, uint32_t newerThan, Reason r);
    size_t find(const Npc& npc) const;
    float  radius() const;
    void   report() const;
    void   placed(const Npc& npc);

    GameScript&               owner;
    bool                      isActive = false;
    std::vector<Entry>        pending;   // insertion order
    std::vector<ExternalStat> externals;

    uint64_t                  inserts = 0, deferred = 0, cleared = 0;
    uint64_t                  byReason[R_Count] = {};
    // Probe census: externals naming each inserted NPC before its final placement.
    struct Window {
      uint32_t reads     = 0; // Args/All externals
      uint32_t dataCalls = 0; // None externals
      bool     placed    = false;
      };
    std::unordered_map<const Npc*,Window> window;
    uint64_t                  globalReads = 0;
  };
