#pragma once

#include "graphics/mesh/hitzones.h"

class Npc;
class Item;

// Archery: how well one handles a bow (a crossbow) in the manual aim. The skill makes the aim steady,
// strength sets the draw's speed and limit, dexterity the reload's tempo; the weapon makes the arrow
// fast. The physics decides the hit, no dice: these numbers are what to balance (the test bench)
namespace Archery {
  struct Stats final {
    bool  crossbow  = false;
    bool  bowStrength = false; // strength-based bow draw; false preserves the legacy model
    float skill     = 0; // 0..1: the talent (the hit chance of the scripts)
    float strength  = 1; // strength over what the weapon needs: below 1 too weak for it
    float need      = 10; // the weapon's draw weight in strength
    float steadyDraw = 1; // draw reached without a burst
    float burstDraw = 1; // draw reached with a burst
    float drawSpeed = 0; // unloaded draw speed, fractions per second
    float sway      = 0; // radians, RMS of the sway at full draw: breathing, the arms
    float settle    = 0; // seconds: a turn, a shot shake the aim that long
    float breath    = 0; // seconds the breath can be held
    float drawTime  = 0; // seconds to steadyDraw without a burst, 0: a crossbow, cocked
    float holdLimit = 0; // seconds at burstDraw before the arms tremble
    float speed     = 0; // cm/ms: the arrow at full draw
    float cone      = 0; // radians: the release throws the arrow off by up to that
    float reload    = 1; // the reload plays that many times as fast (dexterity; a crossbow: strength)
    };

  // the animation of the reload: the next arrow, the next bolt
  static constexpr const char* BowReload  = "T_BOWRELOAD";
  static constexpr const char* CBowReload = "T_CBOWRELOAD";
  inline const char* reloadAnim(bool crossbow) { return crossbow ? CBowReload : BowReload; }

  static constexpr float MinDraw = 0.3f; // drawn less: the arrow is not shot, the string let down
  static constexpr float GiveUp  = 4.f;  // seconds trembling past the hold limit: the arms let it down
  static constexpr float Burst   = 1.4f; // strength reserve available for a burst
  static constexpr float DrawT1  = 1.25f; // seconds to full draw with strength equal to draw weight (hero-balance tempo, #814)
  static constexpr float DrawEpsilon = 0.05f; // minimum draw-speed fraction at the burst limit

  bool  isBowStrengthEnabled();
  void  setBowStrengthEnabled(bool e);

  // Pure draw mechanics: r is strength / draw weight, draw is the fraction 0..1, dt is seconds.
  float bowDrawWeight(float damage, float strengthRequirement = 0);
  float steadyDraw(float r);
  float burstDraw(float r);
  float baseDrawSpeed();
  float drawSpeed(float r, float draw, float v0);
  float drawTime(float r, float draw, float v0);
  float advanceDraw(float r, float draw, float dt, float v0);
  float rohmert(float phi); // minutes at a fraction of maximum effort
  float holdLimit(float r, float draw);

  Stats stats(Npc& npc, const Item* weapon);
  Stats stats(float strength, float dexterity, float skill, float damage, float strengthRequirement,
              bool crossbow, bool bowStrength);
  // the bow canted for a quick shot (k 0..1): drawn short and fast, no anchor at the face, the eye not over
  // the arrow - quick to come to the aim, loose far off; a crossbow is not canted
  Stats canted(const Stats& s, float k);
  // where to aim from `from` (cm) for an arrow that fast (cm/ms) to come down at `to`: on the line it
  // leaves along (the low arc), as far as `to` is; out of reach: 45 degrees up
  Tempest::Vec3 launch(const Tempest::Vec3& from, const Tempest::Vec3& to, float speed);
  // the arrow's speed and damage at a part of the full draw
  float speedAt(const Stats& s, float draw);
  float damageAt(float draw);
  // The release as it is seen (graphics/mesh/bowdraw): how hard the bow kicks, 0..1, by the energy it
  // stored (~ draw weight times the draw squared): f^2*D/KickWeight, not less than KickMin so a weak shot
  // shows too. The archer's strength (r) does not make the kick, it settles it: the bow's shake dies out
  // in 0.1 s times releaseSettle(r)
  static constexpr float KickWeight = 70.f;   // the draw weight of the heavy bows of G2 (150 damage)
  static constexpr float KickMin    = 0.15f;
  float releaseKick(float draw, float drawWeight);
  float releaseSettle(float r);
  float zoneDamage(HitZones::Zone z);
  }
