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
  uint64_t events=0, eventMismatch=0, waitCount=0, waitEarly=0, waitLate=0;
  std::map<int64_t,uint64_t> endErrorMs, waitErrorMs;
  std::map<int64_t,uint64_t> eventErrorMs;
  };
struct Stats final {
  uint64_t ticks=0, bodyMismatch=0, weaponMismatch=0, early=0, late=0;
  uint64_t events=0, eventMismatch=0, wallNs=0, bodySamples=0, weaponSamples=0;
  // Overrides share their source with Npc::bodyState and do not test layers.
  uint64_t bodyInteractiveSamples=0, bodyExternalSamples=0;
  uint64_t simplifiedBodySamples=0, simplifiedBodyMismatch=0;
  uint64_t waitEarly=0, waitLate=0;
  std::map<std::string,uint64_t> reanchored, bodyMismatchKinds, weaponMismatchKinds;
  std::map<Action,Bucket> byAction;
  std::map<std::string,Bucket> byAnimation;
  std::map<std::string,uint64_t> deferred;
  uint64_t reanchoredCount() const;
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
    std::vector<std::pair<uint64_t,WeaponState>> weaponEvents;
    };
  struct ActionClock final {
    Action kind=AI_None;
    const Animation::Sequence* sequence=nullptr;
    uint64_t start=0, origin=0, expectedEnd=0;
    bool predictable=false;
    };

  BodyState body=BS_NONE;
  WeaponState weapon=WeaponState::NoWeapon;
  std::optional<BodyState> interactiveState;
  std::vector<Layer> layers;
  ActionClock action;

  // Native waits are observations only; never an input to the predicted end.
  uint64_t nativeWaitEnd=0;
  bool actionWait=false;
  bool initialized=false;
  uint64_t worldStep=34;
  std::optional<BodyState> externalState;
  std::optional<BodyState> executorState;
  std::map<std::string,float> speeds;
  struct PendingAction { Action kind; const Animation::Sequence* sequence; uint64_t at, wait; };
  std::vector<PendingAction> admissions;

  bool hasState(BodyState state) const;
  bool hasStateFlag(BodyState flag) const;
  BodyState bodyStateMasked() const;
  float animProgress(std::string_view name, uint64_t now) const;

  void sync(Npc& npc, bool rebuild=false, Action admitted=AI_None, const Animation::Sequence* requested=nullptr);
  void advance(uint64_t now);
  void auditTick(Npc& npc, bool bodyInput=false, bool weaponInput=false);
  void admit(Npc& npc, Action kind, const Animation::Sequence* requested);
  void waitReleased(uint64_t now);
  void updateBody();
  void schedule(Layer& layer);
  void recordActionEnd(uint64_t now);
  void sequenceEnd(const Animation::Sequence* sequence, uint64_t origin, uint64_t now);
  void eventFired(const Animation::Sequence* sequence, uint64_t origin,
                  const Animation::LogicEvent& event, uint64_t now);

  struct Tick final {
    Npc& npc;
    explicit Tick(Npc& npc, uint64_t dt);

    };
  struct Admission final {
    Npc& npc;
    Action kind;
    const Animation::Sequence* sequence=nullptr;
    Admission(Npc& npc, Action kind):npc(npc),kind(kind) {}
    ~Admission();
    };
  };
