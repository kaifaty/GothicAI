#pragma once

#include "combat.h"

#include <map>
#include <set>

namespace Society::Combat {

// Schedules the next event of each encounter. An actor belongs to at most one
// encounter until the caller takes its state back, including after resolution.
class Encounters {
  public:
    explicit Encounters(Millis now) : now_(now) {}

    void insert(uint32_t id, Encounter encounter);
    const Encounter& at(uint32_t id) const;
    std::optional<uint32_t> owner(uint32_t actor) const;
    std::optional<Millis> nextAt() const;

    // Advances only through events due by the horizon. When all are served the
    // clock reaches the horizon, even if the queue is empty.
    std::optional<std::pair<uint32_t, const Event*>> runNext(Millis horizon);
    // Cancels future queue entries and transfers the encounter to the caller.
    Encounter take(uint32_t id);

  private:
    Millis now_;
    std::map<uint32_t,Encounter> encounters_;
    std::map<uint32_t,uint32_t> owners_;
    std::set<std::pair<Millis,uint32_t>> pending_;
};

}
