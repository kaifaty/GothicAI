#pragma once

#include "navdemowriter.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

// Stand-only, sensor-only recurrent walking policy. State belongs to the caller,
// so one immutable set of weights can serve independent NPC episodes.
class NavPolicy final {
  public:
    static constexpr size_t nearSize = 9*32*32, farSize = 9*25*25;
    static constexpr size_t vectorSize = NavDemoWriter::rayCount*9+24, legacyVectorSize = vectorSize+8;
    static constexpr size_t headCount = 6;
    inline static constexpr std::array<const char*,headCount> headNames = {
      "forward","strafe","turn","walk","jump","climb"
      };

    struct Features final {
      std::array<float,nearSize> near = {};
      std::array<float,farSize> far = {};
      std::vector<float> vector;
      };

    struct State final {
      // Empty means the zero initial state; clear at the start of each episode.
      std::vector<float> hidden;
      };

    struct Result final {
      std::array<std::vector<float>,headCount> logits;
      NpcKeys::KeyState keys;
      };

    explicit NavPolicy(const std::string& weights);
    // Matches nav_bc_data.encode_features on the NAVDEMO representation, including
    // the writer's per-cell uint8 quantization. Position, yaw and teacher hints are
    // deliberately absent from this path. Training-cache float16 packing is not
    // part of encode_features and is not applied here.
    // The weight header selects eye/roads/body, optionally followed by the
    // previous keys for legacy policies. New policies do not read those keys.
    Features encode(const NavDemoWriter::Snapshot& observation) const;
    Result step(const Features& features, State& state) const;
    Result step(const NavDemoWriter::Snapshot& observation, State& state) const;
    size_t hiddenSize() const { return hidden; }
    size_t vectorInputSize() const { return vectorEncoder.inputs; }

  private:
    struct Linear final {
      size_t inputs = 0, outputs = 0;
      std::vector<float> weight, bias;
      };
    struct MapEncoder final {
      Linear first, second; // OIHW flattened to O,(I*3*3).
      };

    static std::vector<float> linear(const float* input, const Linear& layer, bool relu = false);
    static std::array<float,64> map(const float* input, size_t size, const MapEncoder& encoder);

    size_t hidden = 0;
    MapEncoder nearEncoder, farEncoder;
    Linear vectorEncoder, fusion, gruInput, gruHidden;
    std::array<Linear,headCount> heads;
  };
