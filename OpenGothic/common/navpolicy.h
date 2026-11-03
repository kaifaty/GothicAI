#pragma once

#include "navdemowriter.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Stand-only, sensor-only recurrent walking policy. State belongs to the caller,
// so one immutable set of weights can serve independent NPC episodes.
class NavPolicy final {
  public:
    static constexpr size_t nearSize = 9*32*32, farSize = 9*25*25;
    static constexpr size_t hdNearSize = 10*64*64, hdFarSize = 10*50*100;
    static constexpr size_t bc4NearSize = 11*64*64, bc4FarSize = 11*50*100;
    static constexpr size_t vectorSize = NavDemoWriter::rayCount*9+24, legacyVectorSize = vectorSize+8;
    // Format 5: HD inputs plus one pace value after body (walk=1, run=0).
    static constexpr size_t paceVectorSize = vectorSize+1;
    static constexpr size_t headCount = 6;
    inline static constexpr std::array<const char*,headCount> headNames = {
      "forward","strafe","turn","walk","jump","climb"
      };
    inline static constexpr std::array<const char*,5> bc3HeadNames = {
      "forward","strafe","turn","walk","maneuver"
      };
    inline static constexpr std::array<const char*,4> paceHeadNames = {
      "forward","strafe","turn","maneuver"
      };

    struct Features final {
      std::vector<float> near = std::vector<float>(nearSize);
      std::vector<float> far = std::vector<float>(farSize);
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
    // the writer's per-cell uint8 quantization. HD weights use only position Y
    // to make memory heights relative to the feet; absolute pose, yaw and teacher
    // hints are absent. Training-cache float16 packing is not applied here.
    // The weight header selects the NAVDEMO road schema (v1 next waypoint or
    // v2 carrot), then eye/roads/body, optionally followed by previous keys for
    // legacy v1 policies. New policies do not read those keys.
    // Pace-input weights (format 5) append walk; it is required for them and
    // ignored by every other format, whose walk/run comes from a head.
    Features encode(const NavDemoWriter::Snapshot& observation, std::optional<bool> walk = std::nullopt) const;
    // Format 5 returns keys.walk equal to its pace input.
    Result step(const Features& features, State& state) const;
    Result step(const NavDemoWriter::Snapshot& observation, State& state,
                std::optional<bool> walk = std::nullopt) const;
    size_t hiddenSize() const { return hidden; }
    size_t vectorInputSize() const { return vectorEncoder.inputs; }
    uint32_t navDemoVersion() const { return demoVersion; }
    uint32_t navigationMemoryVersion() const { return memoryVersion; }
    // Legacy weights omit the memory version and retain v3 decay. An explicit
    // experiment override uses OPENGOTHIC_NAV_JOURNEY_MEMORY's 0/1 values.
    void configureMemory(NavigationMemory& memory, const char* journeyOverride = nullptr) const;
    uint32_t weightVersion() const { return formatVersion; }
    bool paceInput() const { return formatVersion==5; }
    size_t mapChannels() const { return formatVersion==4 ? 11 : formatVersion>=3 ? 10 : 9; }
    size_t nearInputSize() const { return formatVersion==4 ? bc4NearSize : formatVersion>=3 ? hdNearSize : nearSize; }
    size_t farInputSize() const { return formatVersion==4 ? bc4FarSize : formatVersion>=3 ? hdFarSize : farSize; }
    size_t actionHeadCount() const {
      return paceInput() ? paceHeadNames.size() : formatVersion>=2 ? bc3HeadNames.size() : headNames.size();
      }
    const char* headName(size_t index) const {
      return paceInput() ? paceHeadNames.at(index) : formatVersion>=2 ? bc3HeadNames.at(index) : headNames.at(index);
      }
    // v2+ joint jump/climb head {none,jump,climb}; v1 keeps separate jump (4) and climb (5).
    size_t maneuverHead() const { return paceInput() ? 3 : 4; }
    size_t headSize(size_t index) const { return index<3 || (formatVersion>=2 && index==maneuverHead()) ? 3 : 2; }
    // Decode sampled or argmax head indices into keys. Format 5 takes walk
    // from its pace input; the others from head 3.
    NpcKeys::KeyState keys(const std::array<size_t,headCount>& action, std::optional<bool> walk = std::nullopt) const;
    double headTemperature(size_t index) const { return temperatures.at(index); }

  private:
    struct Linear final {
      size_t inputs = 0, outputs = 0;
      std::vector<float> weight, bias;
      };
    struct MapEncoder final {
      Linear first, second; // OIHW flattened to O,(I*3*3).
      };

    static std::vector<float> linear(const float* input, const Linear& layer, bool relu = false);
    static std::array<float,64> map(const float* input, size_t rows, size_t columns, const MapEncoder& encoder);

    size_t hidden = 0;
    uint32_t formatVersion = 1;
    uint32_t demoVersion = 1;
    uint32_t memoryVersion = 3;
    std::array<double,headCount> temperatures = {1,1,1,1,1,1};
    MapEncoder nearEncoder, farEncoder;
    Linear vectorEncoder, fusion, gruInput, gruHidden;
    std::array<Linear,headCount> heads;
  };
