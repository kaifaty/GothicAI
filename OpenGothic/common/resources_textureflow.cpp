// Step 4 of the texture levels (textureflow.h, docs/world-streaming-textures.md): where the shortfall goes. Every
// texture in the missing or promotion queue is short of its wanted level and counted in one state; the state changes
// as its load starts, moves on the worker, becomes ready and is published.
#include "resources.h"
#include "texturelevels.h"

#include <algorithm>
#include <chrono>

namespace {
double milliseconds(std::chrono::steady_clock::duration d) {
  return std::chrono::duration<double,std::milli>(d).count();
  }
}

uint8_t Resources::visibleLevel(const TextureRecord& record) const {
  // Stage 1 drops only the reserve; deeper stages have none already.
  return wantedLevel(record,std::max<uint32_t>(textureLadder.stage,1));
  }

void Resources::flowEnter(TextureRecord& record, uint8_t wanted) {
  // `nearTailUpdated` is now: the queues change only right after updateNearTailTime (unqueueTexture, trackTexture).
  const auto now=nearTailUpdated;
  record.flowState=TextureFlow::state(record.loading,record.loading && record.loadLevel>wanted,record.loadPhase);
  ++textureFlow.count[record.flowState];
  const uint32_t resident=record.resident ? record.firstMip : std::max<uint32_t>(record.mipCount,1);
  record.flowVisible=resident>visibleLevel(record);
  if(record.flowVisible)
    ++textureFlow.visible;
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
  --textureFlow.count[record.flowState];
  if(record.flowVisible)
    --textureFlow.visible;
  record.flowState=TextureFlow::NotShort;
  record.flowVisible=false;
  }

void Resources::flowSettle(TextureRecord& record) {
  if(record.flowState!=TextureFlow::NotShort || record.shortSince==std::chrono::steady_clock::time_point{})
    return;
  // Met by an asynchronous publication: its latency. Its demand fell, or a synchronous load met it: none.
  if(record.flowPublished)
    textureFlow.latency.add(milliseconds(nearTailUpdated-record.shortSince));
  record.shortSince={};
  record.flowPublished=false;
  record.flowLoaded=false;
  }

void Resources::flowRestate(TextureRecord& record) {
  if(record.flowState==TextureFlow::NotShort)
    return;
  const auto wanted=wantedLevel(record);
  const auto next=TextureFlow::state(record.loading,record.loading && record.loadLevel>wanted,record.loadPhase);
  if(next==record.flowState)
    return;
  updateNearTailTime();
  --textureFlow.count[record.flowState];
  ++textureFlow.count[next];
  record.flowState=next;
  }

void Resources::flowStarted(TextureRecord& record, TextureReload& reload, uint8_t level) {
  record.loadLevel=level;
  record.loadPhase=TextureFlow::Phase::Queued;
  reload.bytes=TextureLevels::chainBytes(record.format,record.width,record.height,record.mipCount,level);
  if(record.flowState!=TextureFlow::NotShort && !record.flowLoaded) {
    record.flowLoaded=true;
    textureFlow.wait.add(milliseconds(std::chrono::steady_clock::now()-record.shortSince));
    }
  flowRestate(record);
  }

void Resources::flowObserve(TextureReload& reload) {
  const auto found=textureRecords.find(reload.identity);
  if(found==textureRecords.end())
    return;
  auto& record=found->second;
  auto phase=TextureFlow::Phase(reload.phase->load(std::memory_order_acquire));
  if(!reload.ready.isEmpty())
    phase=record.loadPhase==TextureFlow::Phase::Ready || reload.ready.uploadReady() ? TextureFlow::Phase::Ready
                                                                                   : TextureFlow::Phase::Done;
  if(phase==record.loadPhase && reload.level==record.loadLevel)
    return;
  record.loadPhase=phase;
  record.loadLevel=reload.level;  // the file may have no tail at the level asked for: then a sharper one
  flowRestate(record);
  }

void Resources::textureFlowStats(TextureStats& stats) {
  stats.flow=textureFlow;
  stats.flow.bytesInFlight=0;
  for(const auto& reload:textureReloads)
    stats.flow.bytesInFlight+=reload->bytes;
  }
