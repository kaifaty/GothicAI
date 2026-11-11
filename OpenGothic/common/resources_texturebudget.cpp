// Step 3 of the texture levels (texturebudget.h, docs/world-streaming-textures.md): the memory target from the
// device's budget, and the stage that makes the wanted levels coarser when everything at its wanted level is still
// above it.
#include "resources.h"
#include "loadprobe.h"
#include "texturelevels.h"
#include "graphics/texturescreendemand.h"

#include <Tempest/Device>
#include <Tempest/Log>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace Tempest;

static_assert(TextureBudget::WholeChain==TextureScreenDemand::WholeChain,"the whole-chain exponent must match");

uint8_t Resources::resolutionLevel(const TextureRecord& record, uint32_t stage) const {
  const auto exponent=record.resolutions.rbegin()->first;
  const auto level=TextureLevels::level(record.width,record.height,textureViewScale,exponent,
                                        TextureBudget::reserve(stage,textureReserve),record.lowest);
  if(stage==0)
    return level;
  const auto ring=record.distances.empty() ? UINT32_MAX : record.distances.begin()->first;
  return TextureBudget::coarsen(level,record.lowest,stage,ring,exponent==TextureBudget::WholeChain);
  }

void Resources::setTextureBudget(const TextureBudget::Choice& budget, const TextureBudget::Choice& target,
                                 const TextureBudget::Choice& vram) {
  const bool wasRule=textureTargetChoice.automatic;
  textureTargetChoice=target;
  textureVram=vram;
  textureBudgetFollows=budget.automatic;
  if(!target.automatic) {
    textureTarget=target.bytes;
    textureTargetSource=target.source;
    } else if(!wasRule) {
    // until the next evaluation reads the device
    textureTarget=TextureBudget::Floor;
    textureTargetSource=TextureBudget::Source::Default;
    }
  textureBudget=budget.automatic ? textureTarget : budget.bytes;
  // Measuring (a target of 0), the ring rule, no levels: the ladder stays on stage 0. Called within requeueTextures.
  const bool measuring=!target.automatic && target.bytes==0;
  if(measuring || !textureScreen || !textureStreaming || textureSkipMips==0) {
    accountTextureStage(std::chrono::steady_clock::now());
    textureLadder=TextureBudget::Ladder();
    textureSums.clear();
    }
  // the device is read on the main thread, in the next updateTextures
  textureBudgetAt={};
  }

void Resources::updateTextureBudget(bool force) {
  const auto now=std::chrono::steady_clock::now();
  if(!force && now<textureBudgetAt)
    return;
  textureBudgetAt=now+TextureBudget::Period;
  accountTextureStage(now);
  textureDevice=dev!=nullptr ? dev->memoryBudget() : TextureBudget::Device();
  if(!textureVram.automatic) {
    // OPENGOTHIC_TEXTURE_VRAM_MB: the budget is pretended, the usage stays real
    textureDevice.budget=textureVram.bytes;
    textureDevice.known=true;
    }
  if(textureTargetChoice.automatic) {
    textureTarget=TextureBudget::follow(textureTarget,TextureBudget::target(textureDevice,Texture2d::liveBytes()));
    textureTargetSource=textureDevice.known ? TextureBudget::Source::Rule : TextureBudget::Source::Default;
    }
  if(textureBudgetFollows)
    textureBudget=textureTarget;
  logTextureBudget(now);
  chooseTextureStage(force,now);
  }

void Resources::chooseTextureStage(bool force, std::chrono::steady_clock::time_point now) {
  const bool measuring=!textureTargetChoice.automatic && textureTargetChoice.bytes==0;
  if(measuring || !textureScreen || !textureStreaming || textureSkipMips==0) {
    if(textureLadder.stage!=0)
      requeueTextures([&] { textureLadder=TextureBudget::Ladder(); });
    textureSums.clear();
    return;
    }
  const auto started=std::chrono::steady_clock::now();
  textureSums=textureStageSums();
  auto ladder=textureLadder;
  const auto stage=TextureBudget::step(ladder,textureSums,textureTarget,now,force);
  if(stage!=textureLadder.stage)
    requeueTextures([&] { textureLadder=ladder; });
  else
    textureLadder=ladder;
  textureStageDeepest=std::max(textureStageDeepest,stage);
  const char* dump=std::getenv("OPENGOTHIC_TEXTURE_DUMP");
  if(dump==nullptr || dump[0]=='\0')
    return;
  char line[240];
  std::snprintf(line,sizeof(line),"[texture-budget] stage=%u deepest=%zu target=%llu d0=%llu d=%llu live=%llu records=%zu pass_ms=%.3f",
                stage,textureSums.size()-1,(unsigned long long)textureTarget,(unsigned long long)textureSums.front(),
                (unsigned long long)textureSums[std::min<size_t>(stage,textureSums.size()-1)],
                (unsigned long long)Texture2d::liveBytes(),textureRecords.size(),
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
  Log::i(line);
  }

std::vector<uint64_t> Resources::textureStageSums() {
  TextureBudget::Sums sums;
  uint64_t tracked=0;
  // ready replacements not yet published are live but belong to no record's bytes
  for(const auto& reload:textureReloads)
    if(!reload->ready.isEmpty())
      tracked+=LoadProbe::textureBytes(reload->ready);
  const float    log2K=textureViewScale;
  const uint32_t reserve=textureReserve;
  for(auto& entry:textureRecords) {
    auto& r=entry.second;
    if(r.resident)
      tracked+=r.bytes;
    if(!r.wanted())
      continue;
    if(r.failed) {
      sums.fixed(r.resident ? r.bytes : 0);
      continue;
      }
    if(r.stageTop<0) {
      r.stageTop=0.5*std::log2(double(r.width)*double(r.height));
      for(uint32_t level=0;level<r.stageChain.size();++level)
        r.stageChain[level]=TextureLevels::chainBytes(r.format,r.width,r.height,r.mipCount,level);
      }
    const auto bytes=[&](uint8_t level) {
      return level<r.stageChain.size() ? r.stageChain[level] : TextureLevels::chainBytes(r.format,r.width,r.height,r.mipCount,level);
      };
    const bool measured=!r.resolutions.empty() && r.lowest!=0 && !r.pinned && r.distances.count(TextureAlwaysFull)==0;
    if(!measured) {
      // whole by rule, or judged by the ring rule: the same level at every stage
      const auto level=wantedLevel(r);
      sums.fixed(r.resident && r.firstMip==level ? r.bytes : bytes(level));
      continue;
      }
    const auto exponent=r.resolutions.rbegin()->first;
    const auto level0=TextureLevels::levelAt(r.stageTop,log2K,exponent,reserve,r.lowest);
    const auto level1=reserve==0 ? level0 : TextureLevels::levelAt(r.stageTop,log2K,exponent,0,r.lowest);
    const uint8_t cap=r.ringNeeds.empty() ? r.lowest : r.ringNeeds.begin()->first<=2 ? 0 : tailLevel(r);
    sums.measured(level0,level1,cap,r.lowest,r.distances.begin()->first,exponent==TextureBudget::WholeChain,bytes);
    }
  // everything live outside the records: UI, fonts, colours, placeholders
  const auto live=Texture2d::liveBytes();
  sums.fixed(live>tracked ? live-tracked : 0);
  return sums.finish();
  }

void Resources::accountTextureStage(std::chrono::steady_clock::time_point now) {
  if(textureStageClock!=std::chrono::steady_clock::time_point{})
    textureStageSeconds[std::min<size_t>(textureLadder.stage,textureStageSeconds.size()-1)]+=
        std::chrono::duration<double>(now-textureStageClock).count();
  textureStageClock=now;
  }

void Resources::logTextureBudget(std::chrono::steady_clock::time_point now) {
  if(!textureBudgetLogDue &&
     !(TextureBudget::logWorthy(textureTargetLogged,textureTarget) && now-textureTargetLoggedAt>=TextureBudget::LogInterval))
    return;
  const auto mib=[](uint64_t bytes) { return (unsigned long long)(bytes/TextureBudget::MiB); };
  const auto& d=textureDevice;
  char line[512];
  if(d.known)
    std::snprintf(line,sizeof(line),"textures: device \"%s\" budget %llu MiB%s, usage %llu MiB, %s, %s; target %llu MiB (%s), "
                  "unrequested up to %llu MiB",
                  dev!=nullptr ? dev->properties().name : "none",mib(d.budget),
                  textureVram.automatic ? "" : " (OPENGOTHIC_TEXTURE_VRAM_MB)",mib(d.usage),
                  d.shared ? "shared memory" : "own memory",d.measured ? "measured" : "fallback from the heap size",
                  mib(textureTarget),TextureBudget::name(textureTargetSource),mib(textureBudget));
  else
    std::snprintf(line,sizeof(line),"textures: device \"%s\" budget not readable; target %llu MiB (%s), unrequested up to %llu MiB",
                  dev!=nullptr ? dev->properties().name : "none",mib(textureTarget),TextureBudget::name(textureTargetSource),
                  mib(textureBudget));
  Log::i(line);
  textureBudgetLogDue=false;
  textureTargetLogged=textureTarget;
  textureTargetLoggedAt=now;
  }

void Resources::textureBudgetStats(TextureStats& stats) {
  accountTextureStage(std::chrono::steady_clock::now());
  stats.device=textureDevice;
  stats.targetBytes=textureTarget;
  stats.budgetBytes=textureBudget;
  stats.targetSource=textureTargetSource;
  stats.stage=textureLadder.stage;
  stats.stageDeepest=textureStageDeepest;
  stats.stageSeconds=textureStageSeconds;
  if(!textureSums.empty()) {
    stats.stageBytes0=textureSums.front();
    stats.stageBytes=textureSums[std::min<size_t>(textureLadder.stage,textureSums.size()-1)];
    }
  }
