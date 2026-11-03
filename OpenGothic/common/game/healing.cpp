#include "healing.h"

#include <algorithm>

namespace {
  uint64_t advanceChannel(uint64_t& pending, double& fraction, uint64_t dt, uint64_t remaining) {
    fraction += (double(pending)-fraction)*double(dt)/double(remaining);
    const uint64_t whole = std::min(pending,uint64_t(fraction));
    pending -= whole;
    fraction -= double(whole);
    return whole;
    }

  int capped(uint64_t amount, int room) {
    return int(std::min(amount,uint64_t(std::max(room,0))));
    }
  }

Healing::EffectMoment Healing::effectMoment(Category category, bool potionEnabled, bool foodEnabled,
                                            bool hasAnimation) {
  const bool deferred = (category==Category::Potion && potionEnabled) ||
                        (category==Category::Food && foodEnabled);
  if(!deferred || !hasAnimation)
    return EffectMoment::Immediate;
  return EffectMoment::Destroy;
  }

size_t Healing::choosePotion(std::span<const Potion> candidates, int missing) {
  if(missing<=0)
    return NoPotion;
  const Potion* best = nullptr;
  const Potion* weakest = nullptr;
  for(const auto& potion:candidates) {
    if(!potion.available || potion.amount<=0)
      continue;
    if(weakest==nullptr || potion.amount<weakest->amount)
      weakest = &potion;
    if(potion.amount<=missing && (best==nullptr || potion.amount>best->amount))
      best = &potion;
    }
  if(best!=nullptr)
    return best->item;
  return weakest==nullptr ? NoPotion : weakest->item;
  }

void Healing::add(Pool& pool, int hp, int mana) {
  if(hp<=0 && mana<=0)
    return;
  pool.hp += uint64_t(std::max(hp,0));
  pool.mana += uint64_t(std::max(mana,0));
  pool.remaining = FoodDuration;
  }

Healing::Dose Healing::advance(Pool& pool, uint64_t dt, int hpRoom, int manaRoom) {
  if(pool.remaining==0 || dt==0)
    return {};
  if(dt>=pool.remaining) {
    const Dose dose{capped(pool.hp,hpRoom),capped(pool.mana,manaRoom)};
    clear(pool);
    return dose;
    }
  const uint64_t hp = advanceChannel(pool.hp,pool.hpFraction,dt,pool.remaining);
  const uint64_t mana = advanceChannel(pool.mana,pool.manaFraction,dt,pool.remaining);
  pool.remaining -= dt;
  return {capped(hp,hpRoom),capped(mana,manaRoom)};
  }

void Healing::clear(Pool& pool) {
  pool = {};
  }
