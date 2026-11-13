#pragma once

#include "graphics/mesh/lipsync.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <rapidjson/document.h>

class World;

// The microphone has no sound producer: only its analysis track reaches the hero's speech layers.
// Its clock stays LipSync::liveBufferMs() behind the arrived sound. While an npc's voice sounds near the hero (its sound
// from the speakers is in the microphone) and DuckHoldMs after it, the chunks are LipSync::liveDuckDb() quieter.
class PlayerVoice final {
  public:
    static constexpr uint64_t DuckHoldMs = 150;  // the room and the output's delay after an npc's voice ends
    static constexpr float    DuckCm     = 2000; // an npc this near the hero is heard (WorldSound::talkRange)

    void begin();
    void audio(World& world, const rapidjson::Value& msg, uint64_t now);
    // npcVoice: an npc's voice sounds near the hero now
    bool append(uint32_t rate, const int16_t* pcm, size_t count, uint64_t now, bool npcVoice = false);
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
    uint64_t                     ducked = 0;     // chunks made quieter for an npc's voice
    uint64_t                     duckUntil = 0;
    std::shared_ptr<LipSync::Track> lips;
    std::vector<int64_t>          deliveryUs;   // from the server's chunk to the bridge (the system clocks)
    std::vector<int64_t>          shownUs;      // ...to the tick that shows its start
  };
