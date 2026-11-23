#pragma once

// The ground of the empty island (reports/engine/2026-11-13-empty-island/island-content.md): a height function of
// the whole 384 x 384 m field and the one mesh cut from it. Metres here, sea level at 0, x and z as the engine's X
// and Z; the mesh is in the engine's centimetres.

#include "asset/worldmesh.h"

#include <cstdint>
#include <vector>

namespace island {

constexpr float FieldHalf  = 192.f; // metres: 6 x 6 chunks of 64 m around the origin
constexpr int   ChunkSide  = 6;
constexpr float GridStep   = 1.f;   // metres between the ground's grid nodes

// Materials of the ground, in the order of the mesh's material table.
enum Ground : uint32_t { SandDry, SandWet, Grass, Rock, StreamBed, SeaWater, StreamWater, GroundCount };

// The material's name; its texture is this name with ".TGA".
const char* groundName(Ground ground);

// Stream water over its bed, metres, where the channel is full depth.
constexpr float StreamWaterDepth = 0.3f;

struct StreamPoint {
  float x = 0, z = 0;
  float bed = 0;   // height of the channel's flat bed
  float bank = 0;  // how far the bank top rises over the bed: full on land, fades to nothing in the sea
  };

class Terrain final {
  public:
    Terrain();

    // Distance to the shoreline, positive inland (first order: exact near the line).
    float shore(float x, float z) const;
    // The ground before the stream is cut into it.
    float base(float x, float z) const;
    // The ground.
    float height(float x, float z) const;

    // The stream's centre line, a point every metre from the spring to its end in the sea. Beds only descend.
    const std::vector<StreamPoint>& stream() const { return centre; }

    // The ground and the water as one mesh, material borders cut along level lines.
    asset::WorldMesh mesh() const;

  private:
    struct Foot {
      float dist = 1e9f; // to the centre line
      float bed  = 0;
      float bank = 0;
      };
    Foot  foot(float x, float z) const;
    float slope(float x, float z) const;
    float field(unsigned id, float x, float z) const;

    std::vector<StreamPoint> centre;
    float streamMin[2] = {}, streamMax[2] = {};
  };

// One terrain per process: the generator and its test read the same numbers.
const Terrain& terrain();

}
