#pragma once

#include <Tempest/Vec>

#include <cstdint>

class Npc;

// ArcherAi: an npc's archer draws by hand as the hero does (game/archery, Npc::tickManualAim) instead of
// G2's dice: the fight AI decides to shoot (one MV_ATTACK is one arrow), this aims and lets go. It aims at
// the chest, over it by the drop, ahead of a runner, and off by as much as G2's dice would miss: the
// dice's chance at that distance (DamageCalculator::vanillaChance), as a spread round the chest, less
// what the bow's own sway throws off. Hits are then physics: cover, dodging, the zones
class ArcherAi final {
  public:
    // the dice turned into aim: a shot as sure as G2's (perArrow), or as many hits a second as G2's
    // (perSecond: the hand draws slower than the dice shoots, each arrow a bit surer)
    enum Mode : uint8_t {
      PerSecond,
      PerArrow,
      };

    // State for the test bench
    struct Probe final {
      bool          valid  = false;
      bool          active = false; // aiming by hand now
      bool          want   = false; // an arrow asked for
      float         sigma  = 0;     // degrees per axis: the error it shoots with
      Tempest::Vec2 err    = {};    // degrees: this arrow's (right, up)
      float         lead   = 0;     // cm ahead of the target
      float         pv     = 0;     // the dice's chance, its target chance
      float         ps     = 0;
      };

    // [GAME] npcManualAim in Gothic.ini, on by default (Marvin: toggle npcmanualaim), G2 only; off: the dice
    static bool    isEnabled();
    static void    setEnabled(bool e);
    // the test bench: the mode, an error forced (degrees per axis, <0: the dice's), the lead, the seeds
    static Mode    mode;
    static float   forcedSigma;
    static bool    leadOn;
    static uint32_t seedBase;
    static uint32_t epoch;          // the bench changed the mode or the error: the arrow drawn now too

    // every frame (Npc::tick, before the fight AI's waits): raises, reloads, draws and lets go; lowers
    // when the fight moves on
    void           tick(Npc& npc, uint64_t dt);
    // MV_ATTACK wants an arrow: true once it is over (it left, given up); asked every frame it is pending
    bool           attack(Npc& npc);
    // the arrow asked for is withdrawn (the test bench lets go): the string let down, the bow lowered
    void           cancel(Npc& npc);
    // this one aims by hand (else the dice)
    bool           isDriving(Npc& npc) const;
    void           probe(Probe& p) const;

    static Probe       subjectProbe; // the npc the test bench watches
    static const void* probeNpc;

  private:
    void           begin(Npc& npc);
    void           end(Npc& npc);
    void           nextError(Npc& npc, const Npc& tg);
    void           spread(Npc& npc, const Npc& tg);
    float          gauss();

    bool          active   = false;
    bool          want     = false; // the fight AI asked for an arrow...
    bool          done     = false; // ...and it left
    bool          diceOnly = false; // cannot steady or draw the bow: the dice for this weapon
    bool          diceWeak = false; // strength-model fallback; ignored when that model is disabled
    bool          seeded   = false;
    uint64_t      wantAt   = 0;     // ms the arrow was asked for...
    uint64_t      askedAt  = 0;     // ...and the last time (the fight AI stops asking: dropped)
    uint32_t      epochSeen = 0;
    const void*   tgLast   = nullptr; // the target its speed and error are for (compared only)
    uint64_t      raiseAt  = 0;     // ms the bow went up
    uint64_t      heldAt   = 0;     // ms it reached the chosen draw limit
    uint64_t      shotAt   = 0;     // ms of the last arrow...
    float         cycle    = 0;     // ...ms between its arrows, as it shoots them (smoothed), 0: not yet
    uint32_t      rng      = 1;
    float         sigma    = 0;     // radians per axis
    float         pv = 0, ps = 0, rho = 1;
    Tempest::Vec2 err      = {};    // this arrow's error, in sigmas (right, up)...
    Tempest::Vec2 errFrom  = {};    // ...eased in from the last one's
    float         easeT    = 1;
    Tempest::Vec3 tgPrev   = {};    // the target's speed, smoothed (the lead)
    Tempest::Vec3 tgVel    = {};
    bool          tgKnown  = false;
    float         lead     = 0;
    const void*   weapon   = nullptr; // diceOnly holds for this weapon
  };
