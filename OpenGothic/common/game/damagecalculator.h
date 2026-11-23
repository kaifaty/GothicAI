#pragma once

#include "game/damagetypes.h"

#include "game/constants.h"

class Npc;
class Bullet;
class GameScript;

class DamageCalculator {
  public:
    enum {
      MinDamage = 5, //TODO: NPC_MINIMAL_DAMAGE?
      };

    using Val = DamageValue;
    using Damage = ::Damage;

    static Val     damageValue(Npc& src, Npc& other, const Bullet* b, bool isSpell, const DamageCalculator::Damage& splDmg, const CollideMask bMsk);
    static float   meleeMultiplier(float speedCmS, float typicalCmS, float bladeFraction, float surfaceAngleDeg, size_t targetIndex);
    static Val     damageFall(Npc& src, float speed);
    // G2's dice for an arrow, % (Gothic2.exe, oCAIArrow::ReportCollisionToAI): a hit chance of hitChance %,
    // dist cm between the shooter and the target
    static int32_t vanillaChance(const GameScript& script, int32_t hitChance, float dist);
    static auto    rangeDamageValue(Npc& src) -> Damage;
    static int32_t damageTypeMask(Npc& npc);

  private:
    static bool    checkDamageMask(Npc& src, Npc& other, const Bullet* b);

    static Val     rangeDamage(Npc& src, Npc& other, const Bullet& b, const CollideMask bMsk);
    static Val     rangeDamage(Npc& src, Npc& other, Damage dmg, const CollideMask bMsk);
    static Val     swordDamage(Npc& src, Npc& other);
  };

