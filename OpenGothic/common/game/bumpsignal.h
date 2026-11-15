#pragma once

#include <Tempest/Vec>

#include <array>
#include <cstddef>
#include <cstdint>

struct BumpHit final {
  enum class Kind : uint8_t { Wall, Waist, Person, Pushed };
  Kind          kind = Kind::Wall;
  Tempest::Vec3 normal;
  float         strength = 0;
  float         speed = 0; // cm/s
  };

// Read-only movement observations; the state is transient and never serialized.
namespace BumpSignal {
  struct Input final {
    uint64_t      nowMs = 0; // End of this controller move.
    uint64_t      dtMs = 0;
    Tempest::Vec3 requested, accepted, normal;
    bool obstacle = false;
    bool enabled = true;
    bool human = true;
    bool grounded = true;
    bool airborne = false, falling = false, sliding = false;
    bool swimming = false, diving = false, jumping = false;
    bool attacking = false, blocking = false, dodging = false, rolling = false;
    bool meleeStep = false, down = false, ragdoll = false, interactive = false;
    };

  struct State final {
    struct Sample final {
      uint64_t beginMs = 0, endMs = 0;
      float speed = 0;
      };
    // 64 moves cover the 300 ms window down to 4.7 ms a move; faster frames keep the newest ones.
    std::array<Sample,64> speeds;
    size_t first = 0, count = 0;
    uint64_t lastReactionMs = 0;
    bool reacted = false;
    bool contact = false;
    };

  struct Result final {
    bool contactStarted = false; // Log every start, including zero strength and cooldown.
    bool react = false;
    bool eligible = false;       // The body is one that may answer: on the ground, a human, no fight move.
    bool contact = false;        // The obstacle holds a quarter of the move or more.
    float avgSpeed = 0, speed = 0, angleDeg = 0, lost = 0, strength = 0;
    };

  Result update(State& state, const Input& input);
  }
