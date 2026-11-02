#pragma once

#include "game/constants.h"
#include "graphics/mesh/animation.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

class Npc;
class Pose;

namespace NpcLogicAudit {
struct Bucket final {
  uint64_t count=0, early=0, late=0, interrupted=0, unpredictable=0;
  uint64_t events=0, eventMismatch=0;
  std::map<int64_t,uint64_t> endErrorMs;
  std::map<int64_t,uint64_t> eventErrorMs;
  };
struct Stats final {
  uint64_t ticks=0, bodyMismatch=0, weaponMismatch=0, early=0, late=0;
  uint64_t events=0, eventMismatch=0, wallNs=0;
  std::map<Action,Bucket> byAction;
  std::map<std::string,Bucket> byAnimation;
  std::map<std::string,uint64_t> deferred;
  std::string probeFields() const;
  std::string json() const;
  };
bool enabled();
const Stats& stats();
void reset();
std::string actionName(Action action);
}

// Shadow of the body, never used by the executor yet. Positions and rotations
// remain on Npc. Its clock needs sequence metadata and layer admission, not bones
// or Pose's elapsed animation time. Nothing in this structure is serialized.
struct NpcLogicalState final {
  struct Event final {
    Animation::LogicEvent source;
    uint64_t expected=0;
    };
  struct Layer final {
    const Animation::Sequence* sequence=nullptr;
    uint64_t start=0, origin=0, expectedEnd=0;
    float speed=1;
    BodyState state=BS_NONE;
    uint16_t combo=0;
    Action action=AI_None;
    bool finished=false;
    std::vector<Event> events;
    };
  struct ActionClock final {
    Action kind=AI_None;
    const Animation::Sequence* sequence=nullptr;
    uint64_t start=0, expectedEnd=0;
    bool predictable=false;
    };

  BodyState body=BS_NONE;
  WeaponState weapon=WeaponState::NoWeapon;
  std::optional<BodyState> interactiveState;
  std::vector<Layer> layers;
  ActionClock action;

  // Native deadlines observe completion; during load a saved wait also recovers
  // the admission time that Pose no longer retains.
  uint64_t nativeWaitEnd=0;
  bool actionWait=false;

  bool hasState(BodyState state) const;
  bool hasStateFlag(BodyState flag) const;
  BodyState bodyStateMasked() const;
  float animProgress(std::string_view name, uint64_t now) const;

  void sync(Npc& npc, bool rebuild=false, Action admitted=AI_None, const Animation::Sequence* requested=nullptr);
  void auditTick(Npc& npc);
  void recordActionEnd(uint64_t now);
  void sequenceEnd(const Animation::Sequence* sequence, uint64_t origin, uint64_t now);
  void eventFired(const Animation::Sequence* sequence, uint64_t origin,
                  const Animation::LogicEvent& event, uint64_t now);

  struct Tick final {
    Npc& npc;
    bool sample;
    explicit Tick(Npc& npc, bool sample=true);
    ~Tick();
    };
  struct Admission final {
    Npc& npc;
    Action kind;
    const Animation::Sequence* sequence=nullptr;
    Admission(Npc& npc, Action kind):npc(npc),kind(kind) {}
    ~Admission();
    };
  };
