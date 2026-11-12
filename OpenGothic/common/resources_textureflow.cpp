// Step 4 of the texture levels (textureflow.h, docs/world-streaming-textures.md): where the shortfall goes. Every
// texture in the missing or promotion queue is short of its wanted level and counted in one state; the state changes
// as its load starts, moves on the worker, becomes ready and is published.
#include "resources.h"
#include "frameprofile.h"
#include "texturelevels.h"

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

bool Resources::admitsTextureLoad(const TextureRecord& record, uint8_t level) const {
  if(!textureScreen)
    return true;
  uint64_t inFlight=0;
  for(const auto& reload:textureReloads)
    inFlight+=reload->bytes;
  const auto bytes=TextureLevels::chainBytes(record.format,record.width,record.height,record.mipCount,level);
  return TextureFlow::admits(textureReloads.size(),inFlight,bytes,textureLimits);
  }

void Resources::publishReadyTextures(const std::vector<std::shared_ptr<TextureReload>>& ready) {
  if(ready.empty())
    return;
  // In the frame the main thread sees them ready, not a frame later through the residency's work queue; as many as
  // fit the budget, the oldest load first.
  const auto started=std::chrono::steady_clock::now();
  size_t published=0;
  for(const auto& reload:ready) {
    const auto at=std::chrono::steady_clock::now();
    if(!TextureFlow::publishes(published,milliseconds(at-started),textureLimits))
      break;
    FrameProfile::textureEvent(FrameProfile::TextureActivity::Ready);
    publishTexture(reload);
    const double cost=milliseconds(std::chrono::steady_clock::now()-at);
    ++textureFlow.publications;
    textureFlow.publishMs+=cost;
    textureFlow.publishMaxMs=std::max(textureFlow.publishMaxMs,cost);
    ++published;
    }
  }

std::function<void()> Resources::configureTextureFlow(std::string_view lookAhead) {
  const auto integer=[](const char* name, int64_t fallback, int64_t minimum, int64_t maximum) -> int64_t {
    const char* value=std::getenv(name);
    if(value==nullptr || value[0]=='\0')
      return fallback;
    char* end=nullptr;
    const auto result=std::strtoll(value,&end,10);
    if(end==value || *end!='\0' || result<minimum || result>maximum)
      throw std::runtime_error(std::string(name)+" must be an integer from "+std::to_string(minimum)+" to "+
                               std::to_string(maximum));
    return result;
    };
  const TextureFlow::Limits defaults;
  TextureFlow::Limits limits;
  limits.loads=size_t(integer("OPENGOTHIC_TEXTURE_LOADS",int64_t(defaults.loads),1,16));
  limits.bytes=uint64_t(integer("OPENGOTHIC_TEXTURE_LOAD_MB",int64_t(defaults.bytes/1048576),1,65536))*1048576;
  // on by default; 0 in the setting or the environment (which wins) turns it off
  const char* env=std::getenv("OPENGOTHIC_TEXTURE_LOOKAHEAD");
  const std::string_view choice=env!=nullptr ? std::string_view(env) : lookAhead;
  if(!choice.empty() && choice!="0" && choice!="1")
    throw std::runtime_error("texture_lookahead must be 0 or 1");
  const bool on=choice!="0";
  return [this,limits,on] {
    textureLimits=limits;
    textureLookAheadOn=on;
    };
  }
