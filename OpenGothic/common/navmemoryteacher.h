#pragma once

#include "navdemowriter.h"

#include <array>
#include <cstdint>
#include <vector>

// A keyboard teacher whose entire observation is a subset of NAVDEMO v2's
// learner_input. No world pose, object identity, world query or NavMesh enters
// this interface. Memory is quantized BEFORE planning, exactly as on disk.
class NavMemoryTeacher final {
  public:
    struct Cell final {
      // confidence, free, wall, ledge, drop, water, movable (NAVDEMO order).
      std::array<uint8_t,7> channels = {};
      uint8_t familiar = 0; // NAVDEMO v3's existing eighth learner channel.
      float groundHeightCm = 0;
      uint8_t heightValid = 0;
      };
    struct Input final {
      NavDemoWriter::Body body;
      NavDemoWriter::Roads roads;
      NpcKeys::KeyState previous;
      std::array<Cell,64*64> near = {};
      std::array<Cell,100*100> far = {};

      // Reads ONLY the layers of memory, never memory.pose or traveledCm.
      static Input fromLearner(const NavDemoWriter::Body& body,
                               const NavDemoWriter::Roads& roads,
                               const NpcKeys::KeyState& previous,
                               const NavigationMemory::View& memory);
      };
    struct Point final {
      // Current body frame, centimetres; positive right / forward.
      float rightCm = 0, forwardCm = 0;
      };

    // Production defaults are a 15-cm margin (75 cm at live water) and unknown cost four. Alternate
    // values are read-only probes, never parameters of decide(). Near-only plans to a
    // bounded local proxy and must not be read as reaching a distant goal.
    struct DiagnosticsOptions final {
      float bodyRadiusCm = 15, unknownCost = 4;
      bool nearOnly = false, knownFreeOnly = false;
      };
    struct Diagnostics final {
      bool planComplete = false, actualTargetReached = false;
      bool targetOutsideMap = false, targetClamped = false;
      bool targetClear = false, startClear = false, startFound = false;
      bool startEscapeRequired = false;
      Point startEscapeTarget;
      uint32_t overlapDangerCells = 0;
      Point target, localTarget, snappedTarget;
      float targetScale = 1;
      float snappedTargetDistanceCm = 0, endpointDistanceToActualTargetCm = 0;
      float planLengthCm = 0;
      // Exact A* graph cost (start node has cost zero, as in production).
      float planCost = 0;
      // Raw dangerous / known-free evidence over each native-resolution map.
      // Blocked fraction uses active graph nodes (far excludes near overlap).
      std::array<float,2> dangerFraction = {}, blockedFraction = {}, knownFreeFraction = {};
      uint32_t reachableNodes = 0;
      std::vector<Point> path;
      };
    static Diagnostics probe(const Input& input, const DiagnosticsOptions& options);

    struct PlanStatus final {
      bool startFound = false, escapeRequired = false;
      Point escapeTarget;
      uint32_t overlapDangerCells = 0;
      // No start was found and the body exit chose a way out (bodyExitCm > 0) or found none.
      bool bodyExit = false;
      float bodyExitCm = 0;
      };
    const PlanStatus& lastPlanStatus() const { return latestPlanStatus; }

    // Which rule of decide() produced the keys. Diagnosis only, never an input.
    enum class Branch : uint8_t {
      Airborne,       // in the air, falling, climbing: no keys
      AtGoal,         // within 75 cm of the goal
      StartNotFound,  // no start cell and (with the body exit off, or no exit) no keys
      Escape,         // a step out of the raw occupied start cell
      EscapeBlocked,  // occupied start, but neither axis step is clear
      BodyExit,       // no start cell: a step along the body-testified exit
      Standoff,       // a moving target's stand-off carrot is at the body: hold or retreat
      Recovery,       // stuck: a backward or side recovery step
      RecoveryBlocked,// stuck, and the recovery step is not clear
      NoAim,          // the plan gives no direction
      Behind,         // the route is behind: a backward or side step
      BehindBlocked,  // the route is behind and neither step is clear
      Strafe, Forward,
      Turn,           // turning toward the route, no step by design
      ForwardBlocked, // facing the route, but the next forward step is not clear
      SideBlocked,    // the route is to the side, and the side step is not clear
      Count
      };
    static const char* name(Branch branch);
    Branch lastBranch() const { return branch; }

    // The body exit (default on). Off restores the teacher without it: a stand
    // comparison switch, never changed during an episode.
    void setBodyExit(bool enabled) { bodyExitEnabled = enabled; }
    bool bodyExit() const { return bodyExitEnabled; }

    void reset();
    // Call exactly once per 100-ms decision, reset at every episode boundary.
    // Airborne observations never request jumps or climbing.
    // Familiar static evidence costs as unknown unless OPENGOTHIC_NAV_FAMILIAR_MAP=1.
    // The learner input and hard veto for live danger remain unchanged.
    NpcKeys::KeyState decide(const Input& input);
    // Latest full-resolution memory route for debug/rendering ONLY. Starts at
    // the body origin; its points have no world coordinates or object IDs.
    const std::vector<Point>& plan() const { return route; }
    float planLengthCm() const { return routeLength; }
    bool planComplete() const { return complete; }

  private:
    void updateTrack(const Input& input);

    std::vector<Point> route;
    PlanStatus latestPlanStatus;
    Branch branch = Branch::NoAim;
    bool bodyExitEnabled = true;
    // Where the feet have stood, newest last, in the current body frame:
    // dead-reckoned from the observed velocity and the previous turn key.
    std::vector<Point> track;
    // Where an escape step pushed without moving the body: 30 cm along the step,
    // carried like the track, forgotten after `ttl` decisions.
    struct Refusal final { Point point; uint32_t ttl = 0; };
    std::vector<Refusal> refused;
    uint32_t pushTicks = 0;
    // The exit cell of the last body exit, carried like the track; exit cells the
    // body reached without finding a start, forgotten after their `ttl`.
    Point exitCell;
    bool exitPending = false;
    std::vector<Refusal> failedExits;
    float routeLength = 0;
    uint32_t stuckTicks = 0, recoveryTicks = 0, recoveryAttempts = 0;
    float recoverySide = 1;
    bool complete = false;
    bool observedGoal = false;
    Point previousGoal;
    uint32_t movingTicks = 0, runningTargetTicks = 0;
  };
