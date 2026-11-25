#pragma once

#include "societypolicy.h"
#include "society/society.h"
#include "world/animationlod.h"

#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

class World;
class Npc;
class Serialize;
class WayPoint;

// Projects the society rollout onto a freshly initialized native world for comparison.
class SocietyProjection final {
  public:
    static std::unique_ptr<SocietyProjection> start(World& world, double clockRate);
    static std::unique_ptr<SocietyProjection> restore(World& world, double clockRate, Serialize& archive);
    void save(World& world, Serialize& archive) const;
    bool advance(World& world);
    void sample(World& world);
    bool isPilot(const Npc& npc) const;
    std::string_view routineState(const Npc& npc) const;
    bool isGroupPilot() const { return groupLimit!=0; }
    bool wideGroupEnabled() const { return wideGroupPilot; }
    bool animationEnabled() const { return animationPilot; }
    bool earlyTickEnabled() const { return earlyTickPilot; }
    static constexpr float FarDistance = 6000.f; // Outside the native perception rings.
    bool persistentEnabled() const { return wideGroupPilot && earlyTickPilot; }
    using ProfileClock = std::chrono::steady_clock;
    ProfileClock::time_point beginEligibility() const {
      return profileCsv.is_open() ? ProfileClock::now() : ProfileClock::time_point{};
      }
    void endEligibility(ProfileClock::time_point start) {
      if(start!=ProfileClock::time_point{})
        eligibilityNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now()-start).count());
      }
    bool holdRoutine(const Npc& npc, std::string_view blocker);
    bool holdsRoutineAfterTick(const Npc& npc) const;
    void recordDeferred() { ++groupDeferred; }
    void recordExecution(bool skipped);
    void recordEarlyTick(const Npc& npc);
    void sleep(World& world, Npc& npc);
    void wake(World& world, Npc& npc);
    void wakeAll(World& world);
    void runDeadlines(World& world, uint64_t dt);
    void updateInterest(World& world);
    const AnimationLod::View* interestView() const { return view ? &*view : nullptr; }
    void writeProfile();
    bool isRoutePilot(const Npc& npc) const { return routePilot && isPilot(npc); }
    bool holdingRoute() const { return routeHeld; }
    std::optional<bool> stepRoute(Npc& npc, const WayPoint* destination, uint64_t dt, float destDist,
                                  std::string_view blocker);

  private:
    explicit SocietyProjection(Society::World data, bool decisionsEnabled);
    static std::unique_ptr<SocietyProjection> open(World& world, double clockRate, bool fresh);
    void bind(World& world);
    void selectPilot(World& world);
    bool advanceSimulation(World& world);
    void sampleShadow(World& world);
    uint8_t policyMode() const;
    const WayPoint* routinePoint(const Npc& npc) const;
    void schedule(uint32_t actor, uint64_t now);
    void resumePose(World& world, Npc& npc);

    struct SleepingActor {
      Npc* npc = nullptr;
      uint64_t entered = 0;
      uint64_t poseClock = 0;
      uint64_t stepAt = 0;
      int64_t routineAt = 0;
      std::pair<int32_t,int32_t> cell;
      bool committed = false;
      };

    struct Cell {
      Tempest::Vec3 lo, hi;
      std::vector<uint32_t> actors;
      };

    struct GroupHold {
      uint32_t actor;
      const WayPoint* point;
      };

    const Society::World data;
    Society::Sim sim;
    Society::DecisionBatch decisions;
    std::vector<Society::Choice> choices;
    std::unique_ptr<SocietyPolicy> policy;
    std::unordered_map<uint32_t,uint32_t> actorByCreation;
    std::unordered_map<uint32_t,uint32_t> groupActors;
    std::vector<GroupHold> heldGroup, nextGroup;
    std::vector<const WayPoint*> heldPoints, nextPoints;
    std::vector<uint8_t> actorTransitions;
    std::vector<SleepingActor> sleeping;
    std::map<std::pair<int32_t,int32_t>, Cell> cells;
    std::vector<uint32_t> interestWakes;
    std::set<std::pair<uint64_t,uint32_t>> deadlines;
    std::set<std::pair<int64_t,uint32_t>> routineDeadlines;
    std::optional<AnimationLod::View> view;
    Tempest::Vec3 viewHero;
    size_t sleepingCount = 0;
    uint64_t poseLead = 0;
    unsigned groupLimit = 0;
    bool animationPilot = false;
    bool earlyTickPilot = false;
    bool wideGroupPilot = false;
    size_t uniqueHeld = 0, uniqueReleased = 0;
    size_t groupSkipped = 0, groupRepasses = 0, groupDeferred = 0;
    size_t groupChecked = 0, groupStageBlocked = 0, groupLockBlocked = 0;
    size_t groupNearBlocked = 0, groupCameraBlocked = 0;
    std::ofstream profileCsv;
    uint64_t advanceNs = 0, eligibilityNs = 0;
    uint64_t bridgeNs = 0, bridgeOutsideTickNs = 0, wakeNs = 0, sampleNs = 0;
    uint64_t profileTick = 0;
    size_t admittedCount = 0, releasedCount = 0, profileHeldCount = 0;
    size_t wakeCount = 0;
    uint32_t pilot = Society::NoNpc;
    uint32_t pilotCreation = 0;
    bool pilotHolding = false, guardPostPilot = false;
    std::string_view pilotResult = "routine_not_ticked";
    std::string_view previousPilotResult;
    uint64_t pilotSkipped = 0, pilotGestureRepasses = 0;
    bool routePilot = false, routeHeld = false;
    uint64_t routeSteps = 0;
    std::string_view routeResult;
    Society::Time nextSample = 0;
    size_t events = 0;
  };
