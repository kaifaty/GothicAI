#pragma once

#include <Tempest/Vec>
#include <cstdint>

namespace MeleeImpact {
  enum class Weapon : uint8_t { Fist, OneHand, TwoHand };

  // World contact velocity is in cm/s; the returned impulse is in N s.
  Tempest::Vec3 impulse(Weapon weapon, const Tempest::Vec3& velocityCmS, float surfaceAngleDeg);
  // Knockdown threshold multiplier from the attacker's weapon hit chance (percent) and strength
  // against the defender's best weapon hit chance: 1.248 for two 60% masters of strength 60.
  float skillThresholdScale(int32_t attackerHitChance, int32_t attackerStrength, int32_t defenderHitChance);

  struct Posture final {
    float value = 0;
    uint64_t lastHitMs = 0;
    uint64_t immuneUntilMs = 0;

    void tick(uint64_t nowMs, uint64_t dtMs);
    bool hit(float impulseNs, float bodyMassKg, uint64_t nowMs, float thresholdScale = 1.f);
    void recover(uint64_t nowMs);
    void reset();
    };
  }
