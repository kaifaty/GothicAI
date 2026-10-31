#pragma once

#include "game/navigationeye.h"
#include "game/navigationmemory.h"
#include "game/npckeys.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Stand-only demonstration storage. No world queries, teacher decisions or bridge traffic.
// samples.bin.gz is gzip (level 1), followed after decompression by a 32-byte header:
// char[8] "NAVDEMO\0", uint32 version=1|2|3, recordBytes, rays=238, near=64, far=100,
// periodMs=100. Integers and IEEE float32 are little endian, without struct padding.
// Records match scripts/nav_demos.py::RECORD_DTYPES[version]. V2 road slots are
// carrot direction/distance, goal direction/distance, known remainder, beyond-known.
// V3 retains V2 roads and adds the familiar channel to each memory cell.
class NavDemoWriter final {
  public:
    static constexpr uint32_t version = 1, rayCount = 238, nearSize = 64, farSize = 100;
    static constexpr uint32_t headerBytes = 32;
    // Memory cell: 7 uint8 channels (confidence/free/wall/ledge/drop/water/movable),
    // V3 adds uint8 familiar after movable. All channels are round(255*p).
    // float32 height in world centimetres, uint8 heightValid. Unknown height is 0/0.
    // Each ray: f32 distance, u8 kind, f32 normalY, u32 ageMs, u32 nearAgeMs,
    // u8 complete, u8 hit. Keys: five f32 (forward/strafe/turn/headYaw/headPitch),
    // then three u8 (walk/jump/climb).
    static constexpr uint32_t recordBytes = 173838;
    static constexpr uint32_t recordBytesV3 = recordBytes+nearSize*nearSize+farSize*farSize;
    static constexpr uint32_t recordBytesForVersion(uint32_t formatVersion) {
      return formatVersion==3 ? recordBytesV3 : recordBytes;
      }

    struct Body final {
      // Current body frame, x=right, y=up, z=forward.
      std::array<float,3> velocityCmS = {};
      // inAir, falling, sliding, climbing, swimming; each is 0 or 1.
      std::array<uint8_t,5> flags = {};
      std::array<float,3> groundNormal = {};
      float groundSlopeDeg = 0;
      float headYawDeg = 0, headPitchDeg = 0;
      };

    struct Roads final {
      // Directions are unit vectors in the current body's frame: x=right,
      // y=up, z=forward. Zero means no next waypoint / coincident target.
      std::array<float,3> nextDirection = {};
      float nextDistanceCm = 0;
      std::array<float,3> goalDirection = {};
      float goalDistanceCm = 0, remainingVisualCm = 0;
      bool goalInKnown = false;
      // Version 2 replaces next/remainingVisual/goalInKnown on disk with these
      // familiar-road-only guidance fields, retaining the same packed size.
      std::array<float,3> carrotDirection = {};
      float carrotDistanceCm = 0, remainingKnownCm = 0;
      bool beyondKnown = false;
      };

    struct Snapshot final {
      uint64_t gameMs = 0;
      // World physical feet position (cm), body yaw (degrees).
      std::array<float,3> position = {};
      float bodyYawDeg = 0;
      Body body;
      Roads roads;
      NpcKeys::KeyState previous, keys;
      const std::vector<NavigationEye::Ray>* eye = nullptr;
      const NavigationMemory::View* memory = nullptr;
      // Debug-only teacher hint. Excluded by Python learner_input().
      std::array<float,3> teacherCorner = {};
      float teacherPathCm = 0;
      };

    explicit NavDemoWriter(const std::string& path, uint32_t formatVersion = version);
    ~NavDemoWriter();
    NavDemoWriter(const NavDemoWriter&) = delete;
    NavDemoWriter& operator=(const NavDemoWriter&) = delete;

    // Includes validation, encoding, compression and gzwrite, in microseconds.
    // Throws std::runtime_error for bad samples, short writes and close failures.
    double append(const Snapshot& sample);
    void finish();
    uint64_t records() const { return count; }

  private:
    void write(const std::vector<uint8_t>& data);
    void* stream = nullptr;
    std::vector<uint8_t> buffer;
    uint64_t count = 0, previousMs = 0;
    uint32_t fileVersion = version;
  };
