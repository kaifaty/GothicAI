#pragma once

#include "graphics/mesh/lipsync.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <rapidjson/document.h>

class World;

// The microphone has no sound producer: only its analysis track reaches the hero's speech layers.
class PlayerVoice final {
  public:
    static constexpr uint64_t BufferMs = 20;

    void begin();
    void audio(World& world, const rapidjson::Value& msg, uint64_t now);
    bool append(uint32_t rate, const int16_t* pcm, size_t count, uint64_t now);
    void tick(uint64_t now);
    void finish();

    std::shared_ptr<LipSync::Track> track() const { return lips; }
    uint64_t durationMs() const { return freq==0 ? 0 : samples*1000/freq; }

  private:
    bool                         held = false;
    uint64_t                     utterance = 0;
    uint64_t                     arrivedAt = 0;
    uint32_t                     freq = 0;
    uint64_t                     samples = 0;
    uint64_t                     chunks = 0;
    std::shared_ptr<LipSync::Track> lips;
    std::vector<int64_t>          deliveryUs;
  };
