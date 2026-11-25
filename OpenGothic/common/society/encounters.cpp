#include "encounters.h"

#include <stdexcept>
#include <utility>

namespace Society::Combat {

void Encounters::insert(uint32_t id, Encounter encounter) {
  const auto due = nextEventAt(encounter);
  if(encounter.now()!=now_ || (due && *due<now_) || encounters_.contains(id))
    throw std::invalid_argument("cannot schedule encounter at this clock or identity");
  for(const auto& actor : encounter.actors())
    if(owners_.contains(actor.id))
      throw std::invalid_argument("combatant already belongs to an encounter");

  const auto& entry = encounters_.emplace(id,std::move(encounter)).first->second;
  for(const auto& actor : entry.actors())
    owners_.emplace(actor.id,id);
  if(due)
    pending_.emplace(*due,id);
  }

const Encounter& Encounters::at(uint32_t id) const {
  return encounters_.at(id);
  }

std::optional<uint32_t> Encounters::owner(uint32_t actor) const {
  const auto it = owners_.find(actor);
  if(it==owners_.end())
    return {};
  return it->second;
  }

std::optional<Millis> Encounters::nextAt() const {
  if(pending_.empty())
    return {};
  return pending_.begin()->first;
  }

std::optional<std::pair<uint32_t,const Event*>> Encounters::runNext(Millis horizon) {
  if(horizon<now_)
    throw std::invalid_argument("encounter clock moved backwards");
  if(pending_.empty() || pending_.begin()->first>horizon) {
    now_ = horizon;
    return {};
    }
  const auto [when,id] = *pending_.begin();
  auto& encounter = encounters_.at(id);
  const auto& event = step(encounter);
  pending_.erase(pending_.begin());
  now_ = when;
  if(const auto next = nextEventAt(encounter))
    pending_.emplace(*next,id);
  return std::pair{id,&event};
  }

Encounter Encounters::take(uint32_t id) {
  const auto it = encounters_.find(id);
  if(it==encounters_.end())
    throw std::out_of_range("unknown encounter identity");
  auto encounter = std::move(it->second);
  if(const auto due = nextEventAt(encounter))
    pending_.erase({*due,id});
  for(const auto& actor : encounter.actors())
    owners_.erase(actor.id);
  encounters_.erase(it);
  return encounter;
  }

}
