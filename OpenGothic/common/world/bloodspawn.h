#pragma once

#include <Tempest/Vec>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "world/bloodstains.h"

class World;
class Npc;

// Where blood lands (physics stage 8, step 4c): a splash on the ground or a wall behind a flesh hit,
// and a pool under a body that bled before it died. Research and targets:
// reports/physics/2026-11-02-blood/README.md. The stains go to World::addBloodStain.
namespace BloodSpawn {
  // What a guild bleeds, from the vanilla C_GILVALUES blood fields.
  struct GuildBlood {
    bool               decal   = false; // leaves a stain at all
    BloodStains::Blood blood   = BloodStains::Blood::Red;
    float              reachCm = 150;   // blood_max_distance
    float              amount  = 1;     // blood_amount relative to the default 10
    };
  GuildBlood guildBlood(const World& world, int32_t guild);

  // Splash size from the damage of the hit (cm, half size of the decal).
  inline float splashRadius(int32_t damage, float amount) {
    const float r = 12.f + 0.22f*float(damage);
    return std::max(8.f, std::min(r, 40.f)*std::max(0.3f, std::min(amount, 1.2f)));
    }

  // Pool size from the body's mass: blood volume follows the mass, and the film is as thick on any
  // floor, so the area does too. A person (~75 kg) covers 0.3-0.6 m2 on stone; soil drinks a quarter.
  inline float poolRadius(float massKg, BloodStains::Surface surface) {
    const float r = 47.f*std::sqrt(std::max(massKg,1.f)/75.f);
    const float k = surface==BloodStains::Surface::Earth ? 0.8f : 1.f;
    return std::max(12.f, std::min(r, 110.f))*k;
    }

  // A body bleeds into a pool only if it was cut or shot shortly before death.
  constexpr int64_t PoolAfterHitMs = 30*BloodStains::MinuteMs;
  constexpr uint64_t SettleMs      = 1500; // real ms after death: the body has come to rest

  void onHit (World& world, Npc& victim, const Tempest::Vec3& wound, const Tempest::Vec3& dir, int32_t damage);
  void onDead(World& world, Npc& body);
  }
