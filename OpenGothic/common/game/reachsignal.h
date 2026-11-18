#pragma once

#include <cstddef>
#include <cstdint>

// What a body running at an obstacle shows before and after the bump (reports/physics/2026-11-15-reach-wall):
// `warn` - the obstacle ahead is less than a quarter of a second away, the free hands go out to it;
// `press` - the body is held by it while it still asks to move, the legs stand instead of running in place.
// Read-only movement observations as BumpSignal's; the state is transient and never serialized.
namespace ReachSignal {
  constexpr float ProbeSpeed = 200.f;  // cm/s forward over the last 300 ms: slower bodies do not look ahead
  constexpr float WarnTime   = 0.25f;  // s to the contact: the hands start
  constexpr float KeepTime   = 0.35f;  // s: a warning given holds on up to here
  constexpr float WarnSpeed  = 200.f;  // cm/s of closing: the bump's own threshold of an answer
  constexpr float KeepSpeed  = 150.f;
  constexpr float PressFrom  = 0.55f;  // of the asked move not gone, any way: a wall met at 45 degrees leaves 0.7 of
  constexpr float PressFull  = 0.85f;  // it as a slide (no stand), one met at 10 degrees 0.17
  constexpr float PressTime  = 150.f;  // ms the stand comes and goes in
  constexpr float HandsFrom  = 0.3f;   // of the press asked: the hands stay on what holds the body
  constexpr uint64_t GraceTime = 150;  // ms after a warning the hands stay out whatever the press

  struct Input final {
    uint64_t dtMs     = 0;
    bool     enabled  = true;
    bool     eligible = false; // on the ground, a human, no fight move: BumpSignal's gates
    bool     moving   = false; // a move along the ground was asked
    bool     obstacle = false; // the move met something that is not the floor
    bool     contact  = false; // BumpSignal: the obstacle took a quarter of the move or more
    float    moved    = 1;     // of the asked move's length the body went along the ground, any way: a body that
                               // slides along the wall goes on
    float    avgSpeed = 0;     // cm/s forward over the last 300 ms
    // the look ahead along the move, cast when wantsProbe()
    bool     ahead    = false; // it met a surface that stands (not a floor, not a person)
    float    distance = 0;     // cm from the body's axis to the surface along the move
    float    cosine   = 0;     // of the angle between the move and the way into the surface
    float    radius   = 0;     // cm of the body's capsule
    };

  struct State final {
    float    press = 0;
    bool     warn  = false;
    uint64_t grace = 0;   // ms the hands stay out after the warning: the contact's own moves lose only a part
    };

  struct Result final {
    float press     = 0;     // 0..1, eased
    bool  warn      = false;
    bool  hands     = false; // the hands are wanted: warned, or held (the press as it is asked, not eased)
    bool  plant     = false; // warned, or held still: a palm keeps its point (a body that slides takes it along)
    float timeToHit = -1;    // s, -1: nothing ahead
    float closing   = 0;     // cm/s into the surface ahead
    };

  // The wall stop (GAME wall_stop): the hero held nearly head on by a wall that stands flat to both sides of the
  // body asks only the way into it - he stands instead of creeping along the wall with the key held. Further off
  // head on, at a door's jamb, a corner, a post or a niche he slides as the controller has it.
  constexpr float StopBegin  = 0.906f; // cosine of 25 degrees off head on: the stop begins
  constexpr float StopKeep   = 0.866f; // of 30: a stop begun holds on up to here
  constexpr float FlatReach  = 45.f;   // cm to both sides of the body the wall has to stand: a door a step away is
                                       // still slid into
  constexpr float FlatFacing = 0.94f;  // cosine of 20 degrees between the wall there and the one that holds
  constexpr float FlatStep   = 10.f;   // cm the wall may come nearer or go further between the looks: planks, beams
  constexpr float FlatRoom   = 25.f;   // cm past the capsule the wall may be at the furthest look

  struct WallLook final {
    bool  hit    = false; // a wall of the world or of a house, not a door, a thing or a body
    float facing = 0;     // cosine between its normal and the normal of what holds the body
    float gap    = 0;     // cm from the body's line to it
    };

  bool   flatWall   (const WallLook* looks, size_t count, float radius);
  // `into`: cosine between the asked move and the way into the wall
  bool   stops      (bool stopping, bool flat, float into);

  bool   wantsProbe (const Input& input);
  // cm from the body's axis: the capsule and the way of the warning time, with the room the hold-on needs
  float  probeLength(float avgSpeed, float radius);
  Result update     (State& state, const Input& input);
  }
