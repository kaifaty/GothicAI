#pragma once

#include <Tempest/Vec>

#include <cstddef>
#include <cstdint>
#include <vector>

class Npc;
class DynamicWorld;

// Opt-in navigation sensor. It neither drives NPCs nor publishes bridge observations.
// Coordinates and ranges are centimetres; yaw/pitch are degrees relative to the posed head.
class NavigationEye final {
  public:
    enum class HitKind : uint8_t { None, World, StaticVob, MovableVob, Npc, Water };
    // WalkingGazeDown rotates the sensor's effective head frame, without changing
    // animation or locomotion. At 1.8 m eye height its gaze meets floor at 3.5 m.
    enum class FloorSampling : uint8_t { Baseline, WalkingGazeDown, ForwardRows };

    struct Config final {
      size_t   layout = 500; // 250 / 500 / 1000 select 238 / 500 / 1036 rays
      float    centerRangeCm = 10000.f;
      float    peripheralRangeCm = 3000.f;
      float    rangeMultiplier = 1.f; // reserved for darkness/fog; no weather policy here
      float    nearRangeCm = 1000.f;
      uint64_t nearPeriodMs = 100;
      uint64_t farPeriodMs = 500;
      FloorSampling floorSampling = FloorSampling::WalkingGazeDown;
      };

    // The applied head frame plus the actor's own feet position. This overload
    // also lets offline sensor measurements replay the same observed poses.
    struct Pose final {
      Tempest::Vec3 origin = {}, forward = {1,0,0}, right = {0,0,1}, up = {0,1,0};
      Tempest::Vec3 position = {};
      };

    struct Ray final {
      float yawDeg = 0, pitchDeg = 0;
      float maxRangeCm = 0, distanceCm = 0, normalY = 0;
      HitKind kind = HitKind::None;
      Tempest::Vec3 origin = {}, direction = {}, point = {};
      uint64_t sampledAtMs = 0, nearSampledAtMs = 0;
      bool hit = false;
      // A fresh near miss is incomplete until its own far segment is cast.
      bool complete = false;
      };

    struct Update final {
      double costUs = 0;
      size_t casts = 0, nearCasts = 0, farCasts = 0;
      bool updated = false, farUpdated = false;
      };

    NavigationEye();
    explicit NavigationEye(const Config& config);
    Update update(const Npc& npc, uint64_t gameMs);
    Update update(const Pose& pose, const DynamicWorld& physics, const Npc* except, uint64_t gameMs);
    void reset();
    const Config& config() const { return cfg; }
    const std::vector<Ray>& samples() const { return rays; }
    // Stable 2*N slots: current observations followed by cached far rays. Cached
    // rays keep their original origin, direction and timestamp. Invalid cached
    // slots have distance zero. Physical casts are counted by rayCount().
    const std::vector<Ray>& memorySamples() const { return memoryRays; }
    size_t rayCount() const { return rays.size(); }
    float gazePitchDeg() const { return gazePitch; }
    static const char* hitKindName(HitKind kind);

  private:
    struct CachedFar final {
      Ray ray;
      bool valid = false;
      };

    Config cfg;
    std::vector<Ray> rays, memoryRays;
    std::vector<Tempest::Vec3> localDirections;
    std::vector<CachedFar> far;
    uint64_t lastNear = 0, lastFar = 0;
    Tempest::Vec3 previousPosition = {};
    float gazePitch = 0;
    bool sampled = false;
  };
