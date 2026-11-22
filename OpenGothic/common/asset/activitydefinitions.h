#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Routine activities as data (reports/engine/2026-11-14-npc-data/README.md, step D4). The import
// recognises a template in the three script phases of a routine state; the game runs the same
// template walk below instead of the script. Both sides share this file, so the call stream the
// import compares with the VM is the stream the game issues.
namespace ActivityDefinitions {
enum class Phase : uint8_t { Init, Loop, End };
enum class Approach : uint8_t { NearestWaypoint, Distance };

// "An activity at a world object": approach the routine point, use the named object whenever it
// is free and the body is not already busy with it, leave the object at the end. The perception
// set stays a script helper executed by the VM at its place (reactions are step D5).
struct MobActivity {
  std::string perceptions;          // deferred helper without arguments, first in Init
  int32_t     walkMode = 0;         // AI_SetWalkMode argument
  Approach    approach = Approach::NearestWaypoint;
  int32_t     approachDistance = 0; // Distance: walk to the point when farther than this
  std::string scheme;               // object scheme: availability, use in Loop and leave in End
  int32_t     busyBodyState = 0;    // no new use while C_BodyStateContains(self, this)
  int32_t     useState = 0;         // AI_UseMob state in Loop
  int32_t     leaveState = 0;       // AI_UseMob state in End
  int32_t     loopResult = 0;       // Loop result
  bool operator==(const MobActivity&) const = default;
  };

// Script activity at a free point: approach, pose transitions and timed gestures.
struct PointActivity {
  struct Gesture {
    std::string animation; // empty: no gesture for this outcome
    bool resetTimer = false;
    bool operator==(const Gesture&) const = default;
    };
  std::string perceptions;
  int32_t walkMode = 1;
  int32_t approachDistance = 500;
  std::string point;
  std::string occupied = "align_waypoint";
  std::string enterAnimation, leaveAnimation;
  int32_t timerSeconds = 5;
  int32_t rollRange = 10;
  std::vector<Gesture> gestures; // indexed by the random outcome, [0, rollRange)
  int32_t initialStage = 1, walkingStage = 2, arrivedStage = 1, poseStage = 0; // aivar[19]
  int32_t loopResult = 0;
  bool operator==(const PointActivity&) const = default;
  };

struct Library {
  std::map<std::string,MobActivity> activities; // by the Init symbol of the state
  std::map<std::string,PointActivity> points;
  std::map<std::string,std::string> uncaptured; // state -> reason: detail
  };

// The engine as the template sees it: each method is one script external with SELF as its NPC.
class Host {
  public:
    virtual ~Host() = default;
    virtual void        deferred(std::string_view helper) = 0;              // script helper in the VM
    virtual const void* lookAtTarget() = 0;                                 // NPC_GETLOOKATTARGET(self)
    virtual bool        isValidNpc(const void* npc) = 0;                    // HLP_ISVALIDNPC(npc)
    virtual void        stopLookAt() = 0;                                   // AI_STOPLOOKAT(self)
    virtual void        removeWeapon() = 0;                                 // AI_REMOVEWEAPON(self)
    virtual void        setWalkMode(int32_t mode) = 0;                      // AI_SETWALKMODE(self,mode)
    virtual std::string waypoint() = 0;                                     // self.wp, a field read
    virtual std::string nearestWaypoint() = 0;                              // NPC_GETNEARESTWP(self)
    virtual bool        stringsEqual(std::string_view a, std::string_view b) = 0; // HLP_STRCMP(a,b)
    virtual int32_t     distanceToWaypoint(std::string_view waypoint) = 0;  // NPC_GETDISTTOWP(self,wp)
    virtual void        gotoWaypoint(std::string_view waypoint) = 0;        // AI_GOTOWP(self,wp)
    virtual bool        isMobAvailable(std::string_view scheme) = 0;        // WLD_ISMOBAVAILABLE(self,scheme)
    virtual int32_t     bodyState() = 0;                                    // NPC_GETBODYSTATE(self)
    virtual void        useMob(std::string_view scheme, int32_t state) = 0; // AI_USEMOB(self,scheme,state)
  };

// Writes every call of a host as text, in the form the import gives the script's own calls:
// NAME(args)->result, SELF as `self`, another NPC as `npc`. The deferred helper is `deferred NAME`.
class Recorder final : public Host {
  public:
    using Sink = std::function<void(std::string)>;
    Recorder(Host& host, Sink sink):host(host),sink(std::move(sink)) {}
    static std::string text(std::string_view value) { return "\""+std::string(value)+"\""; }
    void deferred(std::string_view helper) override {
      sink("deferred "+std::string(helper));
      host.deferred(helper);
      }
    const void* lookAtTarget() override {
      const auto* npc = host.lookAtTarget();
      sink(std::string("NPC_GETLOOKATTARGET(self)->")+(npc ? "npc" : "null"));
      return npc;
      }
    bool isValidNpc(const void* npc) override {
      const bool valid = host.isValidNpc(npc);
      sink(std::string("HLP_ISVALIDNPC(")+(npc ? "npc" : "null")+")->"+(valid ? "1" : "0"));
      return valid;
      }
    void stopLookAt() override { sink("AI_STOPLOOKAT(self)"); host.stopLookAt(); }
    void removeWeapon() override { sink("AI_REMOVEWEAPON(self)"); host.removeWeapon(); }
    void setWalkMode(int32_t mode) override {
      sink("AI_SETWALKMODE(self,"+std::to_string(mode)+")");
      host.setWalkMode(mode);
      }
    std::string waypoint() override { return host.waypoint(); }
    std::string nearestWaypoint() override {
      auto name = host.nearestWaypoint();
      sink("NPC_GETNEARESTWP(self)->"+text(name));
      return name;
      }
    bool stringsEqual(std::string_view a, std::string_view b) override {
      const bool equal = host.stringsEqual(a,b);
      sink("HLP_STRCMP("+text(a)+","+text(b)+")->"+(equal ? "1" : "0"));
      return equal;
      }
    int32_t distanceToWaypoint(std::string_view waypoint) override {
      const auto distance = host.distanceToWaypoint(waypoint);
      sink("NPC_GETDISTTOWP(self,"+text(waypoint)+")->"+std::to_string(distance));
      return distance;
      }
    void gotoWaypoint(std::string_view waypoint) override {
      sink("AI_GOTOWP(self,"+text(waypoint)+")");
      host.gotoWaypoint(waypoint);
      }
    bool isMobAvailable(std::string_view scheme) override {
      const bool available = host.isMobAvailable(scheme);
      sink("WLD_ISMOBAVAILABLE(self,"+text(scheme)+")->"+(available ? "1" : "0"));
      return available;
      }
    int32_t bodyState() override {
      const auto state = host.bodyState();
      sink("NPC_GETBODYSTATE(self)->"+std::to_string(state));
      return state;
      }
    void useMob(std::string_view scheme, int32_t state) override {
      // The external returns an int the script discards; the game always returns 0.
      sink("AI_USEMOB(self,"+text(scheme)+","+std::to_string(state)+")->0");
      host.useMob(scheme,state);
      }

  private:
    Host& host;
    Sink  sink;
  };

// C_BodyStateContains of Gothic 2: the same state number and the same free-hands and
// interruptable flags (BS_MAX | BS_FLAG_INTERRUPTABLE | BS_FLAG_FREEHANDS).
inline constexpr int32_t BodyStateMask = 0x1f | 0x8000 | 0x10000;
inline bool bodyStateContains(int32_t bodyState, int32_t wanted) {
  return (bodyState & BodyStateMask)==(wanted & BodyStateMask);
  }

// One phase of the template. Evaluation order follows the script: both operands of a condition
// are asked even when the first one already decides it, as Daedalus does not short-circuit.
inline int run(const MobActivity& activity, Phase phase, Host& host) {
  switch(phase) {
    case Phase::Init: {
      host.deferred(activity.perceptions);
      // B_ResetAll: stop looking at a valid target, put the weapon away.
      if(host.isValidNpc(host.lookAtTarget()))
        host.stopLookAt();
      host.removeWeapon();
      host.setWalkMode(activity.walkMode);
      bool away = false;
      if(activity.approach==Approach::NearestWaypoint) {
        const auto nearest = host.nearestWaypoint();
        away = !host.stringsEqual(nearest,host.waypoint());
        } else {
        away = host.distanceToWaypoint(host.waypoint())>activity.approachDistance;
        }
      if(away)
        host.gotoWaypoint(host.waypoint());
      return 0;
      }
    case Phase::Loop: {
      const bool available = host.isMobAvailable(activity.scheme);
      const bool busy      = bodyStateContains(host.bodyState(),activity.busyBodyState);
      if(available && !busy)
        host.useMob(activity.scheme,activity.useState);
      return activity.loopResult;
      }
    case Phase::End:
      host.useMob(activity.scheme,activity.leaveState);
      return 0;
    }
  return 0;
  }
}
