// Step 4 of the texture levels (textureflow.h, docs/world-streaming-textures.md): where the shortfall goes. Every
// texture in the missing or promotion queue is short of its wanted level and counted in one state; the state changes
// as its load starts, moves on the worker, becomes ready and is published.
#include "resources.h"
#include "frameprofile.h"
#include "texturelevels.h"
#include "graphics/texturelookahead.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
double milliseconds(std::chrono::steady_clock::duration d) {
  return std::chrono::duration<double,std::milli>(d).count();
  }
}

uint8_t Resources::visibleLevel(const TextureRecord& record) const {
  // What the screen needs now: the present demand without the look-ahead; stage 1 drops only the reserve, deeper
  // stages have none already.
  return wantedLevel(record,std::max<uint32_t>(textureLadder.stage,1),true);
  }

void Resources::flowEnter(TextureRecord& record, uint8_t wanted) {
  // `nearTailUpdated` is now: the queues change only right after updateNearTailTime (unqueueTexture, trackTexture).
  const auto now=nearTailUpdated;
  const uint32_t resident=record.resident ? record.firstMip : std::max<uint32_t>(record.mipCount,1);
  // Queued for the look-ahead alone: not short of the level its present demand needs.
  record.flowAt=now;
  if(resident>presentLevel(record)) {
    record.flowState=TextureFlow::state(record.loading,record.loading && record.loadLevel>wanted,record.loadPhase);
    ++textureFlow.count[record.flowState];
    record.flowVisible=resident>visibleLevel(record);
    if(record.flowVisible)
      ++textureFlow.visible;
    } else {
    record.flowState=TextureFlow::Ahead;
    ++textureFlow.ahead;
    }
  // A publication that left it short does not end the shortfall: only the one after which it is not short.
  record.flowPublished=false;
  if(record.shortSince!=std::chrono::steady_clock::time_point{})
    return;
  record.shortSince=now;
  record.flowLoaded=false;
  ++textureFlow.entries;
  textureFlow.bursts.add(now);
  }

void Resources::flowLeave(TextureRecord& record) {
  if(record.flowState==TextureFlow::NotShort)
    return;
  if(record.flowState==TextureFlow::Ahead)
    --textureFlow.ahead;
  else
    --textureFlow.count[record.flowState];
  if(record.flowVisible)
    --textureFlow.visible;
  record.flowState=TextureFlow::NotShort;
  record.flowVisible=false;
  }

void Resources::flowSettle(TextureRecord& record) {
  if(record.flowState!=TextureFlow::NotShort || record.shortSince==std::chrono::steady_clock::time_point{})
    return;
  // Met by an asynchronous publication: its latency, and the look-ahead's horizon from it. Its demand fell, or a
  // synchronous load met it: none.
  if(record.flowPublished) {
    const double ms=milliseconds(nearTailUpdated-record.shortSince);
    textureFlow.latency.add(ms);
    textureHorizonWindow.add(ms);
    if(textureHorizonWindow.count()>=TextureLookAhead::WindowSamples) {
      textureHorizon=TextureLookAhead::horizon(textureHorizonWindow.quantile(0.9),textureHorizonWindow.count());
      textureHorizonWindow=TextureFlow::Histogram();
      }
    }
  record.shortSince={};
  record.flowPublished=false;
  record.flowLoaded=false;
  }

void Resources::flowRestate(TextureRecord& record) {
  if(record.flowState==TextureFlow::NotShort || record.flowState==TextureFlow::Ahead)
    return;
  const auto wanted=wantedLevel(record);
  const auto next=TextureFlow::state(record.loading,record.loading && record.loadLevel>wanted,record.loadPhase);
  if(next==record.flowState)
    return;
  updateNearTailTime();
  --textureFlow.count[record.flowState];
  ++textureFlow.count[next];
  record.flowState=next;
  record.flowAt=nearTailUpdated;
  }

void Resources::flowStarted(TextureRecord& record, TextureReload& reload, uint8_t level) {
  record.loadLevel=level;
  record.loadPhase=TextureFlow::Phase::Queued;
  reload.bytes=textureLoadBytes(record,level);
  if(record.flowState!=TextureFlow::NotShort && !record.flowLoaded) {
    record.flowLoaded=true;
    textureFlow.wait.add(milliseconds(std::chrono::steady_clock::now()-record.shortSince));
    }
  flowRestate(record);
  }

void Resources::flowObserve(TextureReload& reload) {
  using TextureFlow::Phase;
  const auto found=textureRecords.find(reload.identity);
  if(found==textureRecords.end())
    return;
  auto& record=found->second;
  const auto& progress=*reload.progress;
  const auto worker=Phase(progress.phase.load(std::memory_order_acquire));
  auto phase=worker;
  // an upload not ready when its worker finished (Metal) is ready once its fence has passed, seen from here
  if(phase==Phase::Done && !reload.ready.isEmpty() && (record.loadPhase==Phase::Ready || reload.ready.uploadReady()))
    phase=Phase::Ready;
  if(phase==record.loadPhase && reload.level==record.loadLevel)
    return;
  const auto before=record.loadPhase;
  record.loadPhase=phase;
  record.loadLevel=reload.level;  // the file may have no tail at the level asked for: then a sharper one
  if(record.flowState==TextureFlow::NotShort || record.flowState==TextureFlow::Ahead)
    return;
  // Until now the interval since the last frame went to the state seen then. The worker moved on in between, at the
  // times it wrote: from each of them on, the time goes to the state it entered.
  updateNearTailTime();
  const bool stale=record.loading && record.loadLevel>wantedLevel(record);
  const auto shift=[&](Phase from, Phase to, TextureFlow::Clock::rep at) {
    const auto a=TextureFlow::state(record.loading,stale,from), b=TextureFlow::state(record.loading,stale,to);
    const auto since=std::max(TextureFlow::Clock::time_point(TextureFlow::Clock::duration(at)),record.flowAt);
    const double seconds=std::chrono::duration<double>(nearTailUpdated-since).count();
    if(a!=b && seconds>0) {
      textureFlow.seconds[a]-=seconds;
      textureFlow.seconds[b]+=seconds;
      }
    };
  if(record.flowState==TextureFlow::state(record.loading,stale,before)) {
    if(before<Phase::Uploading && worker>=Phase::Uploading)
      shift(before,Phase::Uploading,progress.uploading.load(std::memory_order_relaxed));
    if(before<Phase::Done && worker>=Phase::Done)
      shift(std::max(before,Phase::Uploading),worker,progress.done.load(std::memory_order_relaxed));
    }
  const auto next=TextureFlow::state(record.loading,stale,phase);
  --textureFlow.count[record.flowState];
  ++textureFlow.count[next];
  record.flowState=next;
  record.flowAt=nearTailUpdated;
  }

double Resources::textureLookAhead() {
  if(inst==nullptr || !inst->textureScreen || !inst->textureLookAheadOn)
    return 0;
  return inst->textureHorizon.load();
  }

void Resources::textureFlowStats(TextureStats& stats) {
  stats.flow=TextureFlow::Summary(textureFlow);
  stats.flow.horizon=textureLookAhead();
  stats.flow.bytesInFlight=0;
  for(const auto& reload:textureReloads)
    stats.flow.bytesInFlight+=reload->bytes;
  }

TextureFlow::Distributions Resources::textureFlowDistributions() {
  if(inst==nullptr)
    return {};
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  return {inst->textureFlow.latency,inst->textureFlow.wait,inst->textureFlow.bursts};
  }

uint64_t Resources::textureLoadBytes(const TextureRecord& record, uint8_t level) const {
  // Without a readable tail a load makes the payload it had: the device may have built its chain (a PNG, or a single
  // level read with mipmaps), which the file's own levels do not tell.
  if(record.lowest==0)
    return record.bytes;
  return TextureLevels::chainBytes(record.format,record.width,record.height,record.mipCount,level);
  }

bool Resources::admitsTextureLoad(const TextureRecord& record, uint8_t level) const {
  if(!textureScreen)
    return true;
  uint64_t inFlight=0;
  for(const auto& reload:textureReloads)
    inFlight+=reload->bytes;
  return TextureFlow::admits(textureReloads.size(),inFlight,textureLoadBytes(record,level),textureLimits);
  }

void Resources::publishReadyTextures(const std::vector<std::shared_ptr<TextureReload>>& ready) {
  if(ready.empty())
    return;
  // In the frame the main thread sees them ready, not a frame later through the residency's work queue; as many as
  // fit the budget, the oldest load first.
  TextureFlow::publishWithin(ready.size(),textureLimits,[&](size_t i) {
    const auto at=std::chrono::steady_clock::now();
    FrameProfile::textureEvent(FrameProfile::TextureActivity::Ready);
    publishTexture(ready[i]);
    const double cost=milliseconds(std::chrono::steady_clock::now()-at);
    ++textureFlow.publications;
    textureFlow.publishMs+=cost;
    textureFlow.publishMaxMs=std::max(textureFlow.publishMaxMs,cost);
    });
  }

std::function<void()> Resources::configureTextureFlow(std::string_view lookAhead) {
  // on by default; 0 in the setting or the environment (which wins) turns it off
  const char* env=std::getenv("OPENGOTHIC_TEXTURE_LOOKAHEAD");
  const std::string_view choice=env!=nullptr ? std::string_view(env) : lookAhead;
  if(!choice.empty() && choice!="0" && choice!="1")
    throw std::runtime_error("texture_lookahead must be 0 or 1");
  const bool on=choice!="0";
  return [this,on] {
    textureLookAheadOn=on;
    };
  }
