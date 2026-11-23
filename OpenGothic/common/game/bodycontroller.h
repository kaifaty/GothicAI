#pragma once

#include "fightalgo.h"
#include <rapidjson/fwd.h>
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class Npc;

// Simulation-thread authority. All adapters share this parser, lease state and executor.
class BodyController final {
  public:
    // Pure command acceptance gate, also exercised with fake states by selfTestJson().
    struct GateState final {
      uint64_t session=0,world=0,generation=0,goal=0,target=0,observation=0,observedGame=0,gameNow=0,wallAge=0;
      bool targetValid=false,paused=false,infrastructure=false,busy=false,realtime=false;
      FightAlgo::ResolverState resolver;
      };
    struct GateCommand final {
      uint64_t session=0,world=0,generation=0,goal=0,target=0,observation=0,issuedGame=0,ttl=0;
      FightAlgo::PolicyAction action=FightAlgo::PolicyAction::Continue;
      };
    static const char* validate(const GateState& state,const GateCommand& command);
    struct AcquireState final {
      bool targetValid=false,externalControl=false,player=false,talking=false,terminal=false,safeBoundary=false;
      bool nativeCombat=false,queueEmpty=true,queueCombatOnly=false,overlayEmpty=true,gotoEmpty=true,gotoToTarget=false;
      };
    static const char* validateAcquire(const AcquireState& state);
    // The live pending-request lifecycle, with an explicit wall clock for deterministic tests.
    struct BoundaryRequest final {
      uint64_t id=0,target=0,deadlineMs=0;
      bool pending=false;
      bool begin(uint64_t request,uint64_t enemy,uint64_t nowMs,uint64_t awaitMs);
      const char* advance(const AcquireState& state,uint64_t currentTarget,uint64_t nowMs);
      bool cancel();
      };
    static bool nativeCombatGoto(uint8_t hint,const Npc* target,const Npc* provenance,const Npc* gotoNpc,bool gotoWaypoint);
    struct Snapshot final {
      bool owned=false, releasePending=false, controlAllowed=false, canDecide=false;
      uint64_t controlGeneration=0, goalVersion=0, commandId=0, executionId=0;
      std::array<bool,12> mask{};
      std::string provider;
      };
    static void setActors(const std::vector<Npc*>& actors);
    static bool hasOwners();
    // Validate before editing the subscription. Leases and pending boundary requests survive.
    static const char* updateWatch(const std::vector<Npc*>& actors, uint64_t periodMs);
    static void fixturePrepare(Npc& actor, Npc& target, float distanceCm, float yawOffsetDeg);
    static void fixtureLock(Npc& actor, uint64_t durationMs);
    static std::string selfTestJson();
    // Only the ordinary realtime bridge validates against successfully sent observations.
    static std::string handleJson(const rapidjson::Value& request,bool sentObservations=false);
    // Extra boundary diagnostics are opt-in for CPU fixtures, outside the strict wire schema.
    static std::string statusJson(const Npc* actor=nullptr,bool boundaryDiagnostics=false);
    static std::string drainJson();
    static std::vector<std::string> drainPolicyJson();
    // The exact post-animation row, after its observation clock has been published.
    static void onObservation(Npc& actor,std::string_view row);
    static void setSink(std::function<void(std::string_view)> sink);
    static Snapshot snapshot(const Npc& actor);
    static bool owns(const Npc& actor);
    // Bench-only transition after an independently validated, real post-animation threat.
    // Keeps scripted leases or explicitly returns the idle defender to ordinary native AI.
    static void activateBenchReaction(Npc& primary,Npc& opponent,bool native,std::string_view mode);
    static void takeover(Npc& actor, std::string_view reason);
    static bool deferNative(Npc& actor, std::function<void()> mutation);
    static void legacyRejected(Npc& actor, std::string_view reason);
    // Cancel requests without changing epochs, so an adapter can deliver their final events.
    static void cancelPending(std::string_view reason);
    static void disconnect(std::string_view provider);
    static void forget(const Npc* actor);
    static void reset(std::string_view reason="reset");
    static void pause(bool paused);
    static bool isPaused();
    // Call from the IPC pump as well as simulation: wall watchdog still fires at dt=0.
    static void pumpWatchdogs();
    static bool infrastructureFailure();
    static std::string infrastructureReason();
    static void postStep();
    // Returns true while this authority consumed the NPC's body tick (including locks).
    static bool tick(Npc& actor, uint64_t dt);
    // Shared native executor callbacks. Consume is never treated as a segment start.
    static void consumed(Npc& actor);
    static void takeHit(Npc& actor);
    static FightAlgo::ResolverState resolverState(const Npc& actor);
    // Bench-only opt-in telemetry. It never participates in perception, masks or decisions.
    static bool visibilityDiagnosticsEnabled();
    static uint64_t visibilityDiagnosticsGeneration();
    static void visibilityDiagnosticRecorded(bool dropped=false) noexcept;
    static void visibilityDiagnosticFailed() noexcept;
    static std::string visibilityDiagnosticsJson();

  private:
    static AcquireState acquireState(const Npc& actor,uint64_t target);
    static void grantAcquire(Npc& actor,uint64_t target,uint64_t timeout,const std::string& provider,
                             const std::string& mode,uint64_t requestId=0,bool enginePolicy=false);
    static void pollAcquire(Npc& actor,bool grant);
    static bool stopOwnedMovement(Npc& actor);
  };
