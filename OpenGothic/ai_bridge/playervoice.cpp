#include "playervoice.h"

#include "base64.h"
#include "jsonread.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <Tempest/Log>

#include <algorithm>
#include <chrono>
#include <cstring>

void PlayerVoice::begin(uint64_t now) {
  finish();
  held = true;
  pressedAt = now;
  firstMs = 0;
  firstDeliveryUs = 0;
  ++utterance;
  lips.reset();
  samples = chunks = ducked = 0;
  duckUntil = 0;
  freq = 0;
  deliveryUs.clear();
  shownUs.clear();
  }

bool PlayerVoice::append(uint32_t rate, const int16_t* pcm, size_t count, uint64_t now, bool npcVoice) {
  if(!held || count==0 || rate==0)
    return false;
  const bool first = lips==nullptr;
  const uint64_t startMs = durationMs();  // where the chunk lies in the utterance
  if(first) {
    freq = rate;
    arrivedAt = now;
    firstMs = now>pressedAt ? now-pressedAt : 0;
    lips = std::make_shared<LipSync::Track>(rate);
    lips->setName("player-microphone:"+std::to_string(utterance));
    lips->setPlayed(0);
    lips->setLiveInput();
    }
  if(rate!=freq) {
    Tempest::Log::e("[ai] ptt_audio: sample rate changed within an utterance");
    return false;
    }
  // the clock goes by the chunk that came soonest after its time: a first chunk that came late (a cold start, a
  // burst after it) does not hold the whole utterance back, a later one that comes late holds the last step
  if(!first && now>=startMs)
    arrivedAt = std::min(arrivedAt,now-startMs);
  if(npcVoice)
    duckUntil = now+DuckHoldMs;
  const bool duck = now<duckUntil;
  lips->setLiveGain(duck ? -LipSync::liveDuckDb() : 0.f);
  lips->append(pcm,count);
  if(duck)
    ++ducked;
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
  // the voice of an npc near the hero is in the microphone too (the speakers): it does not open his mouth
  Npc* const pl = world.player();
  bool npcVoice = false;
  for(uint32_t i=0; i<world.npcCount() && !npcVoice; ++i) {
    const Npc* npc = world.npcById(i);
    npcVoice = npc!=nullptr && ducks(npc==pl,npc->qDistTo(pl->position()),npc->isVoiceSounding());
    }
  const auto     before  = chunks;
  const uint64_t startMs = durationMs();  // where the chunk lies in the utterance
  const bool     first   = append(uint32_t(rate),pcm.data(),pcm.size(),now,npcVoice);
  if(chunks==before)
    return;
  const int64_t received = JsonRead::integer(msg,"received_at_us",0);
  if(received>0) {
    const auto appended = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    deliveryUs.push_back(appended-received);
    if(first)
      firstDeliveryUs = appended-received;
    // ...and to the tick whose mouth shows its start: now, or when the clock behind the arrived sound gets there
    const uint64_t shownAt = std::max(now,arrivedAt+LipSync::liveBufferMs()+startMs);
    shownUs.push_back(appended-received+int64_t(shownAt-now)*1000);
    }
  if(first)
    world.player()->startSpeech(lips);
  }

void PlayerVoice::tick(uint64_t now) {
  if(!held || lips==nullptr)
    return;
  const uint64_t buffer  = LipSync::liveBufferMs();
  const auto     elapsed = now>arrivedAt+buffer ? now-arrivedAt-buffer : 0;
  lips->setPlayed(std::min(durationMs(),elapsed));
  }

void PlayerVoice::finish() {
  held = false;
  if(lips==nullptr || lips->isFinished())
    return;
  lips->finish();
  lips->setPlayed(lips->durationMs());  // the common speech layers see the end immediately
  std::sort(deliveryUs.begin(),deliveryUs.end());
  std::sort(shownUs.begin(),shownUs.end());
  const auto percentile = [this](size_t percent) -> int64_t {
    return deliveryUs.empty() ? 0 : deliveryUs[(deliveryUs.size()-1)*percent/100];
    };
  const auto shown = [this](size_t percent) -> int64_t {
    return shownUs.empty() ? 0 : shownUs[(shownUs.size()-1)*percent/100];
    };
  Tempest::Log::i("[ai] ptt_audio ",lips->name()," chunks=",chunks," duration_ms=",durationMs(),
                 " ducked=",ducked,
                 " live_track=1 silent=1 delivery_samples=",deliveryUs.size(),
                 " delivery_median_us=",percentile(50)," delivery_p95_us=",percentile(95),
                 " shown_median_us=",shown(50)," shown_p95_us=",shown(95),
                 " first_ms=",firstMs," first_delivery_us=",firstDeliveryUs);
  }
