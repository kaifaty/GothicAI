#pragma once

#include <Tempest/Vec>
#include <cstddef>
#include <cstdint>

namespace BloodStains {
  constexpr int64_t MinuteMs        = 60*1000;           // game milliseconds
  constexpr int64_t LifetimeOutMs   = 24*60*MinuteMs;    // outdoors
  constexpr int64_t LifetimeInMs    = 3*24*60*MinuteMs;  // indoors
  constexpr float   DrawRangeCm     = 4000.f;            // decals farther away are not drawn
  constexpr size_t  DrawLimit       = 128;               // drawn per frame: the nearest (user, 2026-11-02: the cost)

  enum class Kind : uint8_t {
    Splash = 0, // hit spatter on the ground or a wall
    Pool   = 1, // under a dead body, grows
    Smear  = 2, // behind a dragged body (step 4d)
    };

  // What the guild bleeds, from the vanilla C_GILVALUES blood fields.
  enum class Blood : uint8_t {
    Red    = 0, // people and most beasts (ZBLOODSPLAT2, BQM_Scavenger2)
    Yellow = 1, // meatbug, waran (BQM_Meatbug)
    Gore   = 2, // zombie (BQM_Zombie)
    Stone  = 3, // golems (BQM_Golem): chips, not blood
    };

  // Surface under the stain, from the triangle's material group.
  enum class Surface : uint8_t {
    Hard  = 0, // stone, wood, metal: blood pools and shines
    Earth = 1, // soaks in: smaller, darker, matte
    Snow  = 2,
    };

  struct Stain {
    uint32_t       id       = 0;      // never reused within a world
    Kind           kind     = Kind::Splash;
    Blood          blood    = Blood::Red;
    Surface        surface  = Surface::Hard;
    bool           indoor   = false;
    uint8_t        variant  = 0;      // texture variant in the atlas
    Tempest::Vec3  pos;               // on the surface
    Tempest::Vec3  normal   = {0,1,0};
    Tempest::Vec3  dir      = {1,0,0}; // along the surface: splash elongation, decal rotation
    float          radiusCm = 0;      // final size; a pool reaches it after growing
    int64_t        bornAt   = 0;      // game ms
    uint32_t       bodyId   = 0;      // a pool: id of the dead NPC's body trace, 0: none
    };

  inline int64_t lifetimeMs(const Stain& s) {
    return s.indoor ? LifetimeInMs : LifetimeOutMs;
    }

  }
