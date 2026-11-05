#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>

#include "facerig.h"

// The eyes of an own head with "eyes" in its face profile (reports/content/2026-11-hero-head/eyes.md): where they
// look and how they get there.
//
// The fixation point is the target of Gaze. Talking, it goes over the other one's face: its eyes and mouth by
// turns, now and then away from it, more often while it speaks itself; thinking, it goes aside; with nothing to
// look at, small jumps around forward now and then.
// Each eye aims at the point from its own centre in the frame of the head bone as drawn. So when the head turns
// the eyes turn back in their sockets and the gaze stays (the vestibulo-ocular reflex), and a near point makes them
// converge. A change of the point over Saccade degrees is a jump: it takes 21 ms + 2.2 ms per degree, its speed a
// bell, no overshoot. A point moving up to Pursuit deg/s is followed smoothly, a faster one is caught up by jumps.
// Within the limits of the profile: beyond them the eye waits at its limit for Gaze to turn the head.
// Only decides: FaceRig turns the balls and moves the lids.
class Eyes final {
  public:
    enum class Kind : uint8_t {
      None,
      Forward,  // nothing to look at: around forward
      Eye,      // the other one's eyes, one or the other
      Mouth,
      Away,     // aside of the other one's face
      Aside,    // thinking (TalkGestures::eyesAside)
      Target,   // what Gaze looks at, not one it talks to
      };

    struct Input final {
      uint64_t           now      = 0;
      Tempest::Matrix4x4 head;               // the head bone as drawn, world
      bool               target   = false;   // Gaze looks at something...
      Tempest::Vec3      at;                 // ...here (world): the eyes of the other one when talking
      bool               talk     = false;   // it is the one it talks to: the eyes go over its face
      bool               speaking = false;   // its own line goes on...
      uint64_t           left     = 0;       // ...this long still, ms (0: not known)
      bool               aside    = false;   // it thinks of an answer, the eyes go aside...
      Tempest::Vec3      asideAt;            // ...here (world)
      bool               roam     = true;    // its own jumps over a face and around; off: they keep to `at` (tests)
      };

    struct Probe final {
      bool     valid    = false;
      Kind     kind     = Kind::None;
      float    yaw[2]   = {};    // degrees in the socket as shown, the left eye and the right one: + to the left
      float    pitch[2] = {};    // + up
      float    error    = -1;    // degrees from the fixation point, the worse eye; -1: none
      float    over     = 0;     // degrees beyond the limits of the profile, the worse eye (0)
      bool     moving   = false; // in a saccade
      uint32_t saccades = 0;
      uint32_t big      = 0;     // of BigSaccade degrees and more
      float    rise     = 0;     // cm the fixation point is above the middle of the eyes (world)
      float    down     = 0;     // degrees it is below the level of the eyes (world)
      float    head     = 0;     // degrees the head's forward is above the level (world)
      };

    static constexpr float Saccade     = 1.f;    // degrees: a farther change of the point is a jump
    static constexpr float BigSaccade  = 20.f;   // degrees: such a jump counts as a shift of the gaze (EyeBlink)
    static constexpr float Pursuit     = 30.f;   // deg/s the eyes follow a moving point at most
    static constexpr float Refractory  = 150.f;  // ms after a jump before one that catches a moving point up

    // ms a jump of that many degrees takes
    static float duration(float deg) { return 21.f + 2.2f*deg; }
    // the part of a jump done at t (0..1 of its time): minimum jerk, a bell of speed, no overshoot
    static float bell(float t);

    void     setup(const FaceProfile::Eyes* eyes, uint32_t seed);
    bool     isActive() const { return eyes!=nullptr; }
    void     update(const Input& in);
    // as shown now: 0 the left eye, 1 the right one
    FaceRig::EyeTurn turn(int side) const { return shown[side&1]; }
    uint32_t bigSaccades() const { return big; }
    void     probe(Probe& p) const;

    static Probe       subjectProbe;  // the npc the test bench watches
    static const void* probeNpc;

  private:
    enum class Mode : uint8_t { Free, Look, Listen, Speak, Think };

    struct Angles final {
      float yaw = 0, pitch = 0;  // degrees
      };

    void     pick(Mode mode, bool fresh, bool lineStart, const Input& in);
    auto     point(const Input& in) const -> Tempest::Vec3;
    Angles   want(int side, const Tempest::Vec3& world, bool limited) const;
    Angles   limited(int side, Angles a) const;
    float    over(int side, const Angles& a) const;
    float    rand01();
    float    range(float a, float b) { return a + (b-a)*rand01(); }

    const FaceProfile::Eyes* eyes = nullptr;
    uint32_t                 rnd  = 1;

    // the fixation point
    Mode               mode      = Mode::Free;
    bool               started   = false;
    bool               speaking  = false;
    Kind               kind      = Kind::None;
    float              side      = 1;    // of the other one's eyes
    Angles             offset;           // from the base point of Away, Aside, Target, Forward
    Tempest::Vec3      fixed;            // Forward: the point chosen in the world
    uint64_t           nextPick  = 0;
    bool               picked    = false;  // a new point or the point jumped: no waiting out the refractory time

    // the movement: the eye is at the point `anchor` looks at plus `err` (degrees in the socket)
    Tempest::Matrix4x4 toHead;           // world to the head bone
    Tempest::Vec3      last;             // the fixation point of the last update
    bool               hasLast   = false;
    uint64_t           lastNow   = 0;
    Angles             err[2];
    bool               moving    = false;
    Tempest::Vec3      jumpTo;           // where the jump goes: the point as it was when it started
    uint64_t           jumpFrom  = 0;
    float              jumpMs    = 0;
    Angles             jumpErr[2];
    uint64_t           landed    = 0;
    FaceRig::EyeTurn   shown[2];
    float              error     = -1;
    float              beyond    = 0;
    float              rise      = 0, down = 0, headUp = 0;  // the probe: the point from the eyes, the head (world)
    uint32_t           saccades  = 0;
    uint32_t           big       = 0;
  };
