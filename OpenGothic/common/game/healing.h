#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Healing {
  constexpr uint64_t FoodDuration = 10000;
  constexpr size_t NoPotion = size_t(-1);

  enum class Category { Other, Potion, Food };
  enum class EffectMoment { Immediate, Destroy };

  EffectMoment effectMoment(Category category, bool potionEnabled, bool foodEnabled,
                            bool hasAnimation);

  struct Potion final {
    size_t item = NoPotion;
    int    amount = 0;
    bool   available = false;
    };

  size_t choosePotion(std::span<const Potion> candidates, int missing);

  struct Dose final {
    int hp = 0;
    int mana = 0;
    };

  struct Pool final {
    // Integer HP still owed, including the fraction earned but not released yet.
    uint64_t hp = 0;
    uint64_t mana = 0;
    uint64_t remaining = 0;
    double   hpFraction = 0;
    double   manaFraction = 0;
    };

  void add(Pool& pool, int hp, int mana);
  Dose advance(Pool& pool, uint64_t dt, int hpRoom, int manaRoom);
  void clear(Pool& pool);

  template<class Writer>
  void save(Writer& out, const Pool& pool) {
    out.write(pool.hp,pool.mana,pool.remaining);
    out.writeBytes(&pool.hpFraction,sizeof(pool.hpFraction));
    out.writeBytes(&pool.manaFraction,sizeof(pool.manaFraction));
    }

  template<class Reader>
  void load(Reader& in, Pool& pool) {
    in.read(pool.hp,pool.mana,pool.remaining);
    in.readBytes(&pool.hpFraction,sizeof(pool.hpFraction));
    in.readBytes(&pool.manaFraction,sizeof(pool.manaFraction));
    }
  }
