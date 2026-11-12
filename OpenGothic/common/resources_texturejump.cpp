// Synchronous texture loading: the levels before the first frame of a world, read at the coarsest level while it
// loads, and the textures a view jump left far short (docs/world-streaming-textures.md).
#include "resources.h"
#include "loadprobe.h"
#include "texturelevels.h"

#include <Tempest/Device>
#include <Tempest/Log>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <future>

using namespace Tempest;

void Resources::TextureStartup::begin() {
  if(active || inst==nullptr)
    return;
  active=true;
  ++inst->textureStartup;
  // A world loads: the budget line is written again, and the stage is chosen afresh before its first frame.
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  inst->textureBudgetLogDue=true;
  inst->textureBudgetAt={};
  }

void Resources::TextureStartup::end() {
  if(!active)
    return;
  active=false;
  if(inst!=nullptr)
    --inst->textureStartup;
  }

size_t Resources::loadTexturesNow(const std::vector<TextureRecord*>& records, std::chrono::steady_clock::time_point deadline,
                                  bool limited) {
  struct Upload {
    Texture2d texture;
    uint8_t   level = 0;
    };
  std::array<TextureRecord*,2> slots{};
  std::array<std::future<Upload>,2> uploads;
  size_t published=0;
  const auto publish=[&](size_t slot) {
    if(!uploads[slot].valid())
      return;
    auto& record=*slots[slot];
    Upload upload;
    try {
      upload=uploads[slot].get();
      }
    catch(const std::exception& error) {
      Log::e("textures: unable to load \"",record.source.string(),"\": ",error.what());
      unqueueTexture(record);
      record.failed=true;
      queueTexture(record);
      return;
      }
    unqueueTexture(record);
    if(record.resident)
      retireTexture(std::move(*record.texture));
    *record.texture=std::move(upload.texture);
    record.resident=true;
    if(upload.level<record.firstMip)
      ++texturesPromoted;
    record.firstMip=upload.level;
    record.bytes=LoadProbe::textureBytes(*record.texture);
    notifyTextureChanged(record);
    queueTexture(record);
    ++published;
    };
  size_t slot=0;
  const auto late=[&] { return limited && std::chrono::steady_clock::now()>=deadline; };
  for(auto* record:records) {
    if(late())
      break;
    // Waiting for the slot's previous load can pass the deadline: nothing starts after it.
    publish(slot);
    if(late())
      break;
    const auto level=wantedLevel(*record);
    slots[slot]=record;
    uploads[slot]=std::async(std::launch::async,[path=record->source,mipmaps=record->mipmaps,level,device=dev] {
      auto data=decodeBaseTexture(path,mipmaps,level);
      return Upload{device->texture(data.pixmap,data.useMipmap),data.firstMip};
      });
    slot=(slot+1)%uploads.size();
    }
  publish(slot);
  publish((slot+1)%uploads.size());
  return published;
  }

void Resources::finishTextureLoading() {
  if(inst==nullptr || inst->dev==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  // On the main thread before the first frame, with the owners evaluated: the device's budget and the stage first,
  // so the textures load straight to the stage's levels.
  inst->updateTextureBudget(true);
  std::vector<TextureRecord*> records;
  for(auto& entry:inst->textureRecords) {
    auto& record=entry.second;
    if(!record.wanted())
      continue;
    const auto level=inst->wantedLevel(record);
    // The ring rule completes only the whole chains; levels complete every shortfall.
    if((!inst->textureScreen && level!=0) || (record.resident && record.firstMip<=level))
      continue;
    records.push_back(&record);
    }
  inst->loadTexturesNow(records,{},false);
  }

void Resources::completeTextureJump() {
  if(inst==nullptr || inst->dev==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  auto& r=*inst;
  if(!r.textureScreen)
    return;
  const auto started=std::chrono::steady_clock::now();
  if(r.textureJump.open)
    r.closeTextureJump(false);
  ++r.textureJumps;
  r.textureJump=TextureJump();
  r.textureJump.open=true;
  r.textureJump.at=started;
  r.textureJump.shortAt=r.textureMissing.size()+r.texturePromotions.size();

  // The missing and promotion queues hold exactly the wanted textures short of their level.
  struct Candidate {
    TextureRecord* record = nullptr;
    int64_t        shortfall = 0;
    uint32_t       ring = 0;
    };
  std::vector<Candidate> candidates;
  const auto consider=[&](TextureRecord* record) {
    const uint32_t resident=record->resident ? record->firstMip : std::max<uint32_t>(record->mipCount,1);
    const int64_t  shortfall=int64_t(resident)-int64_t(r.wantedLevel(*record));
    if(shortfall<2 || record->failed)
      return;
    const uint32_t ring=record->pinned || record->distances.count(TextureAlwaysFull)!=0 ? 0u :
                        record->distances.empty() ? UINT32_MAX : record->distances.begin()->first;
    candidates.push_back({record,shortfall,ring});
    };
  for(auto* record:r.textureMissing)
    consider(record);
  for(auto* record:r.texturePromotions)
    consider(record);
  std::sort(candidates.begin(),candidates.end(),[](const Candidate& a, const Candidate& b) {
    if(a.shortfall!=b.shortfall || a.ring!=b.ring)
      return TextureLevels::loadsBefore(a.shortfall,a.ring,b.shortfall,b.ring);
    return a.record->texture<b.record->texture;
    });
  r.textureJump.candidates=candidates.size();
  if(r.textureJumpMs>0 && !candidates.empty()) {
    std::vector<TextureRecord*> records;
    records.reserve(candidates.size());
    for(auto& c:candidates)
      records.push_back(c.record);
    r.textureJump.completed=r.loadTexturesNow(records,started+std::chrono::milliseconds(r.textureJumpMs),true);
    }
  r.textureJump.syncMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  r.textureJumpSyncMsMax=std::max(r.textureJumpSyncMsMax,r.textureJump.syncMs);
  r.updateTextureJump();
  }

void Resources::updateTextureJump() {
  // the shortfall against the present demand: with the look-ahead the queues also hold textures loaded ahead
  if(textureJump.open && textureFlow.shortfall()==0)
    closeTextureJump(true);
  }

void Resources::closeTextureJump(bool reached) {
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-textureJump.at).count();
  if(reached)
    textureJumpZeroMsMax=std::max(textureJumpZeroMsMax,ms);
  else
    ++textureJumpsUnresolved;
  textureJump.open=false;
  const char* dump=std::getenv("OPENGOTHIC_TEXTURE_DUMP");
  if(dump==nullptr || dump[0]=='\0')
    return;
  char line[200];
  std::snprintf(line,sizeof(line),"[texture-jump] short=%zu far_short=%zu completed=%zu sync_ms=%.1f zero_ms=%.1f",
                textureJump.shortAt,textureJump.candidates,textureJump.completed,textureJump.syncMs,reached ? ms : -1.0);
  Log::i(line);
  }
