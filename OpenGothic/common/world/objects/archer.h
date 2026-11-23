#pragma once

#include <Tempest/Vec>

#include <cstdint>

#include "game/archery.h"

// Archer: a bow drawn by hand (the manual aim). The string is drawn while the button is held and the bow
// holds still, held at its draw limit until the arms tire, let go with the button. The aim sways meanwhile:
// less with skill, more when drawing beyond comfortable strength, tired, out of breath, after a turn or shot.
// The hero's input drives it (PlayerControl), npc archers later
class Archer final {
  public:
    enum Stage : uint8_t {
      Lowered,   // the bow is not raised
      Raising,   // being raised, an arrow nocked (the animation): no draw yet
      Nocked,    // ready, the string not drawn
      Drawing,
      Held,      // at the chosen draw limit, possibly below full draw
      Reloading, // shot: the next arrow
      };

    struct Input final {
      bool     raised = false; // the bow is up (the stance)
      bool     ready  = false; // the bow holds still in the aim: an arrow on the string
      bool     draw   = false; // the button is held
      bool     burst  = true;  // draw beyond the comfortable strength limit
      bool     breath = false; // the breath is held
      float    turn   = 0;     // radians the aim turned since the last tick
      bool     reload = false; // the reload animation still plays: the next arrow is not on the string yet
      bool     loaded = true;  // an arrow (a bolt) is put in: raised without one, it is put in first
      float    crouch = 0;     // crouched 0..1: steadier
      float    peek   = 0;     // leaning out round a corner, 0..1 either way: less steady
      float    walk   = 0;     // stepping with the bow drawn 0..1: the steps shake it
      uint64_t dt     = 0;
      };

    // State for the test bench
    struct Probe final {
      bool  valid    = false;
      Stage stage    = Lowered;
      float draw     = 0;
      float hold     = 0;  // seconds at the chosen draw limit
      float fatigue  = 0;
      float breath   = 0;  // seconds of breath left
      bool  holding  = false;
      bool  short_   = false; // out of breath: recovering
      float disturb  = 0;
      float amp      = 0;  // degrees: the sway's scale now
      float spread   = 0;  // degrees: how far off the arrow may go (the crosshair opens that much)
      float yaw      = 0;  // degrees: the sway
      float pitch    = 0;
      float strength = 1;
      float skill    = 0;
      };

    void          reset(uint32_t seed);
    // the sway starts over from another seed, the rest as it is (the test bench, between shots)
    void          reseed(uint32_t s) { seed = s; clock = 0; }
    // how shaken the aim is (a turn, a shot, the next arrow put on), settling
    float         disturbance() const { return disturb; }
    // Moves on by in.dt; true: let the arrow go now, draw() far drawn
    bool          tick(const Input& in, const Archery::Stats& st);
    // After a release: the arrow left (reload) or could not (the string let down)
    void          onShot(bool shot);
    // The last shot as it is seen: how hard the bow kicked (Archery::releaseKick, of the draw and the draw
    // weight as it was let go) and how fast its shake settles (Archery::releaseSettle)
    void          setRelease(float kick, float settle) { relKick = kick; relSettle = settle; }
    float         releaseKick()   const { return relKick; }
    float         releaseSettle() const { return relSettle; }

    Stage         stage() const { return stg; }
    float         draw()  const { return drawAmt; }
    bool          isRaised() const { return stg!=Lowered; }
    // Reloading, the bow went down for the next arrow already (it comes up and holds still after)
    bool          isNocking() const { return stg==Reloading && unready; }
    // Where the aim sways off to: yaw (about up), pitch (+ up), radians. The slow part (the wander, the
    // breathing) moves the chest; the trembling (tired, too weak for the bow) the arms
    Tempest::Vec2 sway()   const { return sw; }
    Tempest::Vec2 tremor() const { return trm; }
    // The arms straining past their strength, as it is seen: x to the side, y up (cm: both hands and the
    // bow together, the arrow keeps its line - the tremor above is the aim's error), z the drawing hand
    // losing ground forward along the arrow (cm, >= 0)
    Tempest::Vec3 shake()  const { return shk; }
    // How far off the arrow may go now, radians: the sway's reach, trembling, the release
    float         spread() const { return spr; }

    void          probe(Probe& p) const;
    static Probe  playerProbe;

  private:
    Stage         stg       = Lowered;
    float         drawAmt   = 0;
    float         hold      = 0;
    float         fatigue   = 0;
    float         breath    = -1;  // seconds left, -1: full (not known yet)
    float         recover   = 0;   // seconds out of breath yet
    bool          holding   = false;
    float         calm      = 1;   // the sway's scale by the breath, smoothed
    float         disturb   = 0;
    float         amp       = 0;
    float         spr       = 0;
    bool          repress   = false; // the arms let the string down: the button has to be pressed again
    bool          unready   = false; // reloading: the bow moved (the next arrow is being nocked)
    float         stageTime = 0;
    double        clock     = 0;
    uint32_t      seed      = 0;
    float         strength  = 1;
    float         skill     = 0;
    Tempest::Vec2 sw;
    Tempest::Vec2 trm;
    Tempest::Vec3 shk;
    float         relKick   = 0;
    float         relSettle = 1;

    float         wander(float t, uint32_t axis) const;
    float         tremor(float t, uint32_t axis) const;
    float         shiver(float t, uint32_t axis) const;
    void          setStage(Stage s);
  };
