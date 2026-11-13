#include "playervoice.h"

#include "base64.h"
#include "jsonread.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <Tempest/Log>

#include <algorithm>
#include <chrono>
#include <cstring>

void PlayerVoice::begin() {
  finish();
  held = true;
  ++utterance;
  lips.reset();
  samples = chunks = 0;
  freq = 0;
  deliveryUs.clear();
  }

bool PlayerVoice::append(uint32_t rate, const int16_t* pcm, size_t count, uint64_t now) {
  if(!held || count==0 || rate==0)
    return false;
  const bool first = lips==nullptr;
  if(first) {
    freq = rate;
    arrivedAt = now;
    lips = std::make_shared<LipSync::Track>(rate);
    lips->setName("player-microphone:"+std::to_string(utterance));
    lips->setPlayed(0);
    lips->setLiveInput();
    }
  if(rate!=freq) {
    Tempest::Log::e("[ai] ptt_audio: sample rate changed within an utterance");
    return false;
    }
  lips->append(pcm,count);
  samples += count;
  ++chunks;
  tick(now);
  return first;
  }

void PlayerVoice::audio(World& world, const rapidjson::Value& msg, uint64_t now) {
  if(!held || world.player()==nullptr)
    return;
  const auto rate = JsonRead::integer(msg,"rate",0);
  if(rate<=0 || rate>UINT32_MAX) {
    Tempest::Log::e("[ai] ptt_audio: invalid sample rate");
    return;
    }
  const auto bytes = fromBase64(JsonRead::str(msg,"data"));
  if(bytes.empty() || bytes.size()%2!=0) {
    Tempest::Log::e("[ai] ptt_audio: expected PCM16 mono samples");
    return;
    }
  std::vector<int16_t> pcm(bytes.size()/2);
  std::memcpy(pcm.data(),bytes.data(),bytes.size());
  const auto before = chunks;
  const bool first = append(uint32_t(rate),pcm.data(),pcm.size(),now);
  if(chunks==before)
    return;
  const int64_t received = JsonRead::integer(msg,"received_at_us",0);
  if(received>0) {
    const auto appended = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    deliveryUs.push_back(appended-received);
    }
  if(first)
    world.player()->startSpeech(lips);
  }

void PlayerVoice::tick(uint64_t now) {
  if(!held || lips==nullptr)
    return;
  const auto elapsed = now>arrivedAt+BufferMs ? now-arrivedAt-BufferMs : 0;
  lips->setPlayed(std::min(durationMs(),elapsed));
  }

void PlayerVoice::finish() {
  held = false;
  if(lips==nullptr || lips->isFinished())
    return;
  lips->finish();
  lips->setPlayed(lips->durationMs());  // the common speech layers see the end immediately
  std::sort(deliveryUs.begin(),deliveryUs.end());
  const auto percentile = [this](size_t percent) -> int64_t {
    return deliveryUs.empty() ? 0 : deliveryUs[(deliveryUs.size()-1)*percent/100];
    };
  Tempest::Log::i("[ai] ptt_audio ",lips->name()," chunks=",chunks," duration_ms=",durationMs(),
                 " live_track=1 silent=1 delivery_samples=",deliveryUs.size(),
                 " delivery_median_us=",percentile(50)," delivery_p95_us=",percentile(95));
  }
