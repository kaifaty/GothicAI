#pragma once

#include "society.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Society::Combat {

// All timestamps, delays and movement rates use one caller-provided clock.
// The engine adapter converts native combat durations to game milliseconds.
using Millis = int64_t;
inline constexpr uint32_t NoActor = 0xffffffffu;

enum class AttackKind : uint8_t { HumanMelee, BeastMelee, Ranged, Magic };
enum class ActorState : uint8_t { Active, KnockedOut, Dead, Withdrawn };
enum class Outcome : uint8_t { Active, Won, Draw, Timeout, EventLimit };

struct Point {
  int32_t x = 0, z = 0; // cm
  bool operator==(const Point&) const = default;
};

struct AttackProfile {
  AttackKind kind = AttackKind::HumanMelee;
  bool calibrated = false; // experimental profiles are allowed for offline runs only
  int32_t rangeCm = 0, areaRadiusCm = 0;
  Millis windup = 0, recovery = 0, flight = 0;
  uint16_t hitPermille = 0, criticalPermille = 0;
  // Numbers are supplied by the game adapter or the offline test corpus.
  std::array<int32_t,8> damage{};
  uint8_t damageMask = 0;
  uint16_t normalDamageDivisor = 1;
  int32_t minDamage = 0;
  int32_t resourceCost = 0; // ammunition for Ranged, mana for Magic
  Millis stunDuration = 0, tickPeriod = 0, effectDuration = 0;
  int32_t tickDamage = 0;
};

struct Combatant {
  uint32_t id = NoActor, side = 0, profile = 0, target = NoActor;
  Point position;
  int32_t hp = 0, ammunition = 0, mana = 0;
  int32_t strength = 0, moveCmPerSecond = 0;
  std::array<int32_t,8> protection{}; // negative means immune to that damage type
  Millis readyAt = 0; // zero means immediate at encounter start
  Millis stunnedUntil = 0;
  ActorState state = ActorState::Active;
  bool operator==(const Combatant&) const = default;
};

enum class EventKind : uint8_t { Ready, Arrive, Release, Impact, EffectTick, Withdraw, End };

struct Change {
  uint32_t id = NoActor;
  int32_t hpDelta = 0, ammunitionDelta = 0, manaDelta = 0;
  Point position;
  ActorState state = ActorState::Active;
  Millis stunnedUntil = 0;
  bool operator==(const Change&) const = default;
};

struct Event {
  Millis at = 0;
  EventKind kind = EventKind::Ready;
  uint32_t actor = NoActor, target = NoActor;
  Outcome outcome = Outcome::Active;
  std::optional<uint32_t> winner;
  std::vector<Change> changes;
  bool operator==(const Event&) const = default;
};

// Copying the encounter preserves its scheduled events and random stream.
// Construction validates the supplied profiles and bounded encounter state.
class Encounter {
  public:
    std::span<const Combatant> actors() const { return actors_; }
    Millis now() const { return now_; }
    Outcome outcome() const { return outcome_; }
    std::optional<uint32_t> winner() const { return winner_; }

  private:
    friend struct Access;
    struct Pending {
      Millis at = 0;
      uint32_t seq = 0, actor = NoActor, target = NoActor;
      EventKind kind = EventKind::Ready;
      Point point;
      Millis expiry = 0;
      bool operator<(const Pending& other) const;
    };

    Encounter() = default;
    Millis now_ = 0, deadline_ = 0;
    bool lethal_ = false;
    bool settling_ = false;
    Rng rng_;
    std::vector<Combatant> actors_;
    std::vector<AttackProfile> profiles_;
    std::vector<Pending> pending_;
    std::vector<uint32_t> waitingForRange_;
    uint32_t nextSeq_ = 0, processed_ = 0, committed_ = 0;
    Outcome outcome_ = Outcome::Active;
    std::optional<uint32_t> winner_;
    Event event_;
};

Encounter start(Millis now, Millis deadline, bool lethal, uint64_t seed,
                std::span<const Combatant> actors, std::span<const AttackProfile> profiles, bool gameplay = false);
std::optional<Millis> nextEventAt(const Encounter&);
// Pops a single event. The returned record is valid until the next step.
const Event& step(Encounter&);
// Stop motion and new attacks at handoff. Already released attacks still resolve on their due time.
void sealAttacks(Encounter&);
bool hasCommitted(const Encounter&);
// Places a withdrawal after already queued events at the given timestamp.
void withdraw(Encounter&, uint32_t actor, Millis at);
// The versioned bytes preserve future events and RNG state. Archive emitted events separately.
std::vector<uint8_t> save(const Encounter&);
Encounter restore(std::span<const uint8_t> snapshot, bool gameplay = false);

}
