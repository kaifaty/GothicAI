// Ground motion of the nav-sim stand agents (docs/nav-sim.md, task S2): the
// decision's keys pick a translation from the root-motion table, the turn is
// code (npckeys.cpp), and Jolt CharacterVirtual moves the NPC capsule over the
// static dump, tick by tick in the engine's order. Jolt stays inside motion.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace navsim {

class Pool;

struct MotionParams final {
  float radiusCm = 0, heightCm = 0, stepCm = 0, slideAngleDeg = 0, bodyCm = 0, turnDegS = 0, massKg = 0;
  };

// The legacy fall guard of DynamicWorld::NpcItem::hasFloorBelow: any solid floor
// below (x, y, z) within depth (landRay, landscape and objects, no water).
using FloorProbe = std::function<bool(float x, float y, float z, float depth)>;

struct MotionTicks final {
  float* position = nullptr; // [N*3*3] feet cm
  float* forward = nullptr;  // [N*3*3] body forward, XZ
  float* limits = nullptr;   // [N*3*3] step cm, max slope deg, body cm
  uint32_t* ms = nullptr;    // [N*3] game ms, low 32 bits
  uint8_t* flags = nullptr;  // [N*3]
  float* yaw = nullptr;      // [N*3] body yaw degrees
  };

class Motion final {
  public:
    static constexpr int Ticks = 3;
    enum Flag : uint8_t {
      Supported = 1, Blocked = 2, Falling = 4, Steep = 8, UnknownKeys = 16, Stepped = 32,
      };

    // vertices: [triangles*9] centimetres; categories 1 landscape, 2 static object,
    // 3 water (not solid: left out), 4 kinematic object (blocks automatic stairs).
    Motion(const float* vertices, const uint8_t* categories, size_t triangles, size_t agents,
           size_t shards, const MotionParams& params, FloorProbe floor, Pool& pool);
    ~Motion();

    size_t shards() const;
    void setTable(int level, size_t count, const int64_t* keys, const float* forwardRight);
    void setState(size_t first, size_t count, const float* position, const float* yawDeg,
                  const uint64_t* gameMs, const int8_t* history, const int32_t* held);
    void setYaw(size_t first, size_t count, const float* yawDeg);
    void setKeys(size_t first, size_t count, const int8_t* keys);
    // One decision of agents [first, first+count): ticks[i] in 0..3 (null: 3),
    // outputs indexed from first and right-aligned in the three tick slots.
    void step(Pool& pool, size_t first, size_t count, const uint8_t* ticks, const MotionTicks& out);
    void read(size_t first, size_t count, float* position, float* yawDeg, uint64_t* gameMs,
              uint8_t* flags, float* tableCm, int8_t* level) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
  };

}
