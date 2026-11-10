#include "resources.h"
#include "loadprobe.h"
#include "frameprofile.h"
#include "contentcensus.h"
#include "graphics/material.h"
#include "utils/fileext.h"
#include "utils/string_frm.h"
#include "utils/userhome.h"
#include "world/chunkresidency.h"

#include <Tempest/Color>
#include <Tempest/Device>
#include <Tempest/File>
#include <Tempest/Log>
#include <Tempest/Pixmap>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace Tempest;

namespace {
thread_local bool loadingTextureTail = false;
}

Tempest::Texture2d* Resources::implLoadTexture(std::string_view cname, bool forceMips, bool pinned) {
  if(isHeadless() || cname.empty())
    return nullptr;

  std::string name(cname);
  if(auto it=texCache.find(name); it!=texCache.end()) {
    if(auto entry=textureRecords.find(it->second.get()); entry!=textureRecords.end() && pinned && !entry->second.pinned) {
      auto& record=entry->second;
      unqueueTexture(record);
      record.pinned=true;
      if(record.tail) {
        LoadProbe::Vobs::Scope probe("texture_load");
        LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Texture);
        auto full=decodeBaseTexture(record.source,record.mipmaps);
        auto replacement=dev->texture(full.pixmap,full.useMipmap);
        if(record.resident)
          retireTexture(std::move(*record.texture));
        *record.texture=std::move(replacement);
        record.resident=true;
        record.tail=false;
        record.bytes=LoadProbe::textureBytes(*record.texture);
        notifyTextureChanged(record);
        ++texturesPromoted;
        }
      queueTexture(record);
      }
    return it->second.get();
    }

  LoadProbe::Vobs::Scope probe("texture_load");
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Texture);
  const auto probeT = LoadProbe::now();
  TextureData data;
  Texture2d tex;
  try {
    data=implDecodeTexture(cname,forceMips,!pinned && textureStreaming && loadingTextureTail ? textureSkipMips : 0);
    if(!data.pixmap.isEmpty())
      tex=device().texture(data.pixmap,data.useMipmap);
    }
  catch(const std::exception& error) {
    Log::e("textures: unable to upload \"",cname,"\": ",error.what());
    }
  LoadProbe::countTexture(tex,probeT);
  if(!tex.isEmpty()) {
    auto t = std::make_unique<Texture2d>(std::move(tex));
    auto* ret=t.get();
    trackTexture(ret,data,TextureKind::Color,pinned);
    texCache[std::move(name)] = std::move(t);
    return ret;
    }
  texCache[std::move(name)] = nullptr;
  return nullptr;
  }

void Resources::indexHdTextures() {
  const char* dir = std::getenv("OPENGOTHIC_TEXTURES");
  if(dir==nullptr || dir[0]=='\0')
    return;
  std::error_code ec;
  for(auto& e:std::filesystem::directory_iterator(dir,ec)) {
    auto ext = e.path().extension().string();
    for(auto& c:ext)
      c = char(std::toupper(uint8_t(c)));
    if(ext!=".DDS" && ext!=".PNG")
      continue;
    auto key = e.path().stem().string();
    for(auto& c:key)
      c = char(std::toupper(uint8_t(c)));
    // DXT with mipmaps (DDS) over RGBA (PNG): a fraction of the memory
    auto& path = hdTextures[key];
    if(path.empty() || ext==".DDS")
      path = e.path().string();
    }
  if(ec)
    Log::e("textures: unable to read \"",dir,"\"");
  Log::i("textures: ",hdTextures.size()," of our own from \"",dir,"\"");
  }

void Resources::indexMaterialMaps() {
  std::filesystem::path root;
  if(const char* dir = std::getenv("OPENGOTHIC_CONTENT")) {
    if(dir[0]=='\0')
      return;
    root = dir;
    } else if(const auto home=userHome(); !home.empty()) {
    root = home/".local/share/gothic-ai/content";
    } else {
    return;
    }
  root /= "materials";

  std::ifstream file(root/"materials.json", std::ios::binary);
  if(!file)
    return;
  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  rapidjson::Document table;
  table.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
  if(table.HasParseError() || !table.IsObject()) {
    Log::e("materials: invalid materials.json in \"",root.string(),"\"");
    return;
    }

  for(auto& entry:table.GetObject()) {
    if(!entry.value.IsObject()) {
      Log::e("materials: invalid entry \"",entry.name.GetString(),"\"");
      continue;
      }
    MaterialMapFiles maps;
    auto path = [&](const char* field, std::string& out, std::string* replacement = nullptr) {
      const auto it = entry.value.FindMember(field);
      if(it==entry.value.MemberEnd())
        return true;
      if(!it->value.IsString())
        return false;
      const std::string name(it->value.GetString(), it->value.GetStringLength());
      const auto relative = std::filesystem::path(name);
      if(name.empty() || name.find('\0')!=std::string::npos || relative.is_absolute() || !FileExt::hasExt(name,"DDS"))
        return false;
      for(const auto& component:relative)
        if(component=="..")
          return false;
      out = (root/relative).lexically_normal().string();
      if(replacement!=nullptr)
        *replacement = (std::filesystem::path("materials")/relative).lexically_normal().generic_string();
      return true;
      };
    bool valid = path("albedo", maps.albedo, &maps.albedoReplacement) && path("normal", maps.normal) && path("orm", maps.orm);
    if(const auto mode = entry.value.FindMember("antitile"); mode!=entry.value.MemberEnd()) {
      if(!mode->value.IsString()) {
        valid = false;
        } else {
        const std::string_view value(mode->value.GetString(), mode->value.GetStringLength());
        if(value=="offset")
          maps.antitile = Material::Antitile::Offset;
        else if(value=="rotate")
          maps.antitile = Material::Antitile::Rotate;
        else if(value!="off")
          valid = false;
        }
      }
    if(const auto cell = entry.value.FindMember("antitile_cell"); cell!=entry.value.MemberEnd()) {
      if(!cell->value.IsNumber()) {
        valid = false;
        } else {
        const auto value = cell->value.GetDouble();
        if(!std::isfinite(value) || value<=0 || value>std::numeric_limits<float>::max() ||
           float(value)<=0 || !std::isfinite(1.f/float(value)))
          valid = false;
        else
          maps.antitileCell = float(value);
        }
      }
    // Water in a pot clouded with ash and fat: how much of the material's own lit colour covers the clear water.
    if(const auto turbidity = entry.value.FindMember("turbidity"); turbidity!=entry.value.MemberEnd()) {
      if(!turbidity->value.IsNumber() || !std::isfinite(turbidity->value.GetDouble()) ||
         turbidity->value.GetDouble()<0 || turbidity->value.GetDouble()>1)
        valid = false;
      else
        maps.turbidity = float(turbidity->value.GetDouble());
      }
    // A stew: the turbidity is reached over this many cm of liquid (Beer-Lambert), clearer where it is thin.
    if(const auto depth = entry.value.FindMember("turbidity_depth"); depth!=entry.value.MemberEnd()) {
      if(!depth->value.IsNumber() || !std::isfinite(depth->value.GetDouble()) ||
         depth->value.GetDouble()<0 || depth->value.GetDouble()>1e6)
        valid = false;
      else
        maps.turbidityDepth = float(depth->value.GetDouble());
      }
    // Embers and glowing wood: the material gives off its own albedo colour times this, where the ORM's alpha
    // (DXT5) marks it; it slowly breathes (main.frag, mainGBuffer) and is added by the ambient pass.
    if(const auto emissive = entry.value.FindMember("emissive"); emissive!=entry.value.MemberEnd()) {
      if(!emissive->value.IsNumber() || !std::isfinite(emissive->value.GetDouble()) ||
         emissive->value.GetDouble()<0 || emissive->value.GetDouble()>1)
        valid = false;
      else
        maps.emissive = float(emissive->value.GetDouble());
      }
    // Hair cards (reports/content/2026-11-npc-hair): strands run along v, the root at v=0; `orm` is the hair map
    // (R shade, G the strand's number, B its depth in the clump), the surface is lit by the hair BRDF.
    if(const auto hair = entry.value.FindMember("hair"); hair!=entry.value.MemberEnd()) {
      if(!hair->value.IsBool())
        valid = false;
      else
        maps.hair = hair->value.GetBool();
      }
    if(const auto scale = entry.value.FindMember("scale"); scale!=entry.value.MemberEnd()) {
      if(!scale->value.IsNumber()) {
        valid = false;
        } else {
        const auto value = scale->value.GetDouble();
        if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())
          valid = false;
        else
          maps.scale = float(value);
        }
      }
    if(!valid) {
      Log::e("materials: invalid entry \"",entry.name.GetString(),"\"");
      continue;
      }
    std::string key(entry.name.GetString(), entry.name.GetStringLength());
    for(auto& c:key)
      c = char(std::toupper(uint8_t(c)));
    materialMaps[std::move(key)] = std::move(maps);
    }
  materialMapsPresent = !materialMaps.empty();
  Log::i("materials: ",materialMaps.size()," entries from \"",root.string(),"\"");
  }

const Texture2d* Resources::implLoadMaterialTexture(const std::string& path, TextureKind kind) {
  LoadProbe::Requests::add(2,path);
  LoadProbe::Vobs::Scope probe("texture_request");
  if(path.empty())
    return nullptr;
  const auto key = (kind==TextureKind::Normal ? "normal:" : kind==TextureKind::Orm ? "orm:" : "color:")+path;
  if(auto it = materialTextureCache.find(key); it!=materialTextureCache.end())
    return it->second.get();
  std::unique_ptr<Texture2d> texture;
  LoadProbe::Vobs::Scope loadProbe("texture_load");
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Texture);
  const auto probeT = LoadProbe::now();
  try {
    TextureData data=decodeBaseTexture(path,false,textureStreaming && loadingTextureTail ? textureSkipMips : 0);
    if(data.pixmap.format()!=TextureFormat::DXT5 && (kind==TextureKind::Normal || data.pixmap.format()!=TextureFormat::DXT1)) {
      Log::e("materials: unsupported DDS format in \"",path,"\"");
      } else {
      data.useMipmap=data.pixmap.mipCount()>1;
      data.source=path;
      texture=std::make_unique<Texture2d>(device().texture(data.pixmap,data.useMipmap));
      trackTexture(texture.get(),data,kind,false);
      textureRecords.at(texture.get()).sourceKind=TextureSource::Material;
      }
    }
  catch(...) {
    Log::e("materials: unable to load \"",path,"\"");
    }
  const auto ret = texture.get();
  if(ret!=nullptr)
    LoadProbe::countTexture(*ret,probeT);
  materialTextureCache.emplace(key,std::move(texture));
  return ret;
  }

Texture2d Resources::implLoadTextureUncached(std::string_view name, bool forceMips) {
  LoadProbe::Vobs::Scope probe("texture_load");
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Texture);
  auto decoded=implDecodeTexture(name,forceMips);
  try {
    return decoded.pixmap.isEmpty() ? Texture2d() : device().texture(decoded.pixmap,decoded.useMipmap);
    }
  catch(const std::exception& error) {
    Log::e("textures: unable to upload \"",name,"\": ",error.what());
    return {};
    }
  }

Resources::TextureData Resources::implDecodeTexture(std::string_view name, bool forceMips, uint32_t skipMips) {
  if(isHeadless() || name.empty())
    return {};

  if(!hdTextures.empty() && FileExt::hasExt(name,"TGA")) {
    std::string key(name.substr(0,name.size()-4));
    for(auto& c:key)
      c = char(std::toupper(uint8_t(c)));
    if(auto it = hdTextures.find(key); it!=hdTextures.end()) {
      try {
        TextureData decoded=decodeBaseTexture(it->second,true,skipMips);
        if(census)
          census->replaced(name,it->second);
        return decoded;
        }
      catch(...) {
        Log::e("textures: unable to load \"",it->second,"\"");
        }
      }
    }

  if(baseTextures.enabled && FileExt::hasExt(name,"TGA")) {
    std::string key(name.substr(0,name.size()-4));
    for(auto& c:key)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    if(const auto it = baseTextures.files.find(key); it!=baseTextures.files.end()) {
      try {
        auto decoded=decodeBaseTexture(baseTextures.dir/it->second,forceMips,skipMips);
        if(!decoded.pixmap.isEmpty()) {
          if(census)
            census->replaced(name,"base/textures/"+it->second);
          return decoded;
          }
        Log::e("base: unable to load texture \"",it->second,"\"");
        }
      catch(const std::exception& error) {
        Log::e("base: unable to load texture \"",it->second,"\": ",error.what());
        }
      }
    }

  Log::e("base: unable to load texture \"",name,"\"");
  if(census) census->missing(name,"texture unavailable");
  return {};
  }

Resources::TextureData Resources::decodeBaseTexture(const std::filesystem::path& path, bool forceMips, uint32_t skipMips) {
  if(skipMips!=0 && FileExt::hasExt(path.string(),"DDS")) {
    Tempest::RFile input(path.string());
    auto tail=Pixmap::loadDdsMipTail(input,skipMips);
    if(!tail.isEmpty()) {
      const auto mipmaps=forceMips || tail.mipCount()>1;
      return {std::move(tail),mipmaps,path,true};
      }
    }
  if(FileExt::hasExt(path.string(),"PNG"))
    return {Tempest::Pixmap(path.string()),true,path}; // device.texture(pm) generates mips for non-DXT originals.
  TextureData decoded{Tempest::Pixmap(path.string()),false,path};
  decoded.useMipmap = forceMips || decoded.pixmap.mipCount()>1;
  return decoded;
  }

bool Resources::TextureRecord::wanted() const {
  return pinned || std::any_of(demand.begin(),demand.end(),[](uint32_t count) { return count!=0; });
  }

std::tuple<uint32_t,uint64_t,uintptr_t> Resources::TextureRecord::demotionPriority() const {
  const auto distance=distances.empty() ? UINT32_MAX : distances.begin()->first;
  return {UINT32_MAX-distance,lastNeededFull,reinterpret_cast<uintptr_t>(texture)};
  }

void Resources::trackTexture(Texture2d* texture, const TextureData& data, TextureKind kind, bool pinned) {
  TextureRecord record;
  record.texture=texture;
  record.source=data.source;
  if(data.source.parent_path()==baseTextures.dir)
    record.sourceKind=TextureSource::Base;
  else if(kind!=TextureKind::Color || data.source.parent_path().filename()=="materials")
    record.sourceKind=TextureSource::Material;
  else if(std::any_of(hdTextures.begin(),hdTextures.end(),[&](const auto& entry) { return entry.second==data.source.string(); }))
    record.sourceKind=TextureSource::Hd;

  record.format=texture->format();
  record.kind=kind;
  record.mipmaps=data.useMipmap;
  record.pinned=pinned;
  record.tail=data.tail;
  record.hasTail=data.tail || (FileExt::hasExt(data.source.string(),"DDS") && data.pixmap.mipCount()>1 &&
                               data.pixmap.w()>=128 && data.pixmap.h()>=128);
  record.bytes=LoadProbe::textureBytes(*texture);
  auto entry=textureRecords.emplace(texture,std::move(record)).first;
  queueTexture(entry->second);
  }

Resources::TextureLoadScope::TextureLoadScope(bool tail):previous(loadingTextureTail) {
  loadingTextureTail=tail;
  }

Resources::TextureLoadScope::~TextureLoadScope() {
  loadingTextureTail=previous;
  }

void Resources::updateNearTailTime() {
  const auto now=std::chrono::steady_clock::now();
  nearTailSeconds+=std::chrono::duration<double>(now-nearTailUpdated).count()*double(nearTailTextures.size());
  nearTailUpdated=now;
  }

bool Resources::wantsFull(const TextureRecord& record) const {
  return !textureStreaming || textureSkipMips==0 || !record.hasTail || record.pinned ||
         record.distances.count(TextureAlwaysFull)!=0 ||
         (!record.distances.empty() && record.distances.begin()->first<=2);
  }

void Resources::unqueueTexture(TextureRecord& record) {
  updateNearTailTime();
  nearTailTextures.erase(&record);
  textureIdle.erase({record.lastDemand,reinterpret_cast<uintptr_t>(record.texture)});
  textureMissing.erase(&record);
  texturePromotions.erase(&record);
  textureDemotions.erase({record.demoteAt,reinterpret_cast<uintptr_t>(record.texture)});
  textureDemotionLru.erase(record.demotionPriority());
  }

void Resources::queueTexture(TextureRecord& record) {
  if(record.resident && record.tail && !record.distances.empty() && record.distances.begin()->first<=1)
    nearTailTextures.insert(&record);
  if(record.wanted()) {
    if(!record.failed) {
      if(!record.resident)
        textureMissing.insert(&record);
      else if(record.tail && wantsFull(record))
        texturePromotions.insert(&record);
      }
    if(!wantsFull(record) && (record.distances.empty() || record.distances.begin()->first>3)) {
      if(record.demoteAt==std::chrono::steady_clock::time_point{})
        record.demoteAt=std::chrono::steady_clock::now()+std::chrono::seconds(5);
      if(record.resident && !record.tail && !record.failed) {
        if(record.demoteAt<=std::chrono::steady_clock::now())
          textureDemotionLru.emplace(record.demotionPriority(),&record);
        else
          textureDemotions.emplace(std::make_pair(record.demoteAt,reinterpret_cast<uintptr_t>(record.texture)),&record);
        }
      } else {
      record.demoteAt={};
      }
    } else if(record.resident) {
    record.demoteAt={};
    textureIdle.emplace(std::make_pair(record.lastDemand,reinterpret_cast<uintptr_t>(record.texture)),&record);
    }
  }

void Resources::configureTextures(std::string_view budget, std::string_view skipMips, std::string_view target) {
  if(inst==nullptr)
    return;
  const auto* enabled=std::getenv("OPENGOTHIC_TEXTURE_STREAMING");
  const auto* budgetEnv=std::getenv("OPENGOTHIC_TEXTURE_BUDGET_MB");
  const auto* targetEnv=std::getenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  const auto* mipsEnv=std::getenv("OPENGOTHIC_TEXTURE_MIPS");
  const auto* skipEnv=std::getenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS");
  const auto integer=[](std::string_view value, int64_t fallback, const char* field, int64_t maximum) -> int64_t {
    if(value.empty())
      return fallback;
    std::string text(value);
    char* end=nullptr;
    const auto result=std::strtoll(text.c_str(),&end,10);
    if(end==text.c_str() || *end!='\0' || result<0 || result>maximum)
      throw std::runtime_error(std::string(field)+" must be a nonnegative integer");
    return result;
    };
  const auto megabytes=integer(budgetEnv!=nullptr ? std::string_view(budgetEnv) : budget,256,"texture_budget_mb",
                              std::numeric_limits<int64_t>::max()/1048576);
  const auto skip=integer(skipEnv!=nullptr ? std::string_view(skipEnv) : skipMips,2,"texture_far_skip_mips",UINT32_MAX);
  const auto targetMegabytes=integer(targetEnv!=nullptr ? std::string_view(targetEnv) : target,192,"texture_target_mb",
                                    std::numeric_limits<int64_t>::max()/1048576);
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  inst->textureBudget=uint64_t(megabytes)*1048576;
  inst->textureTarget=uint64_t(targetMegabytes)*1048576;
  inst->textureStreaming=enabled==nullptr || std::strcmp(enabled,"0")!=0;
  inst->textureSkipMips=inst->textureStreaming && (mipsEnv==nullptr || std::strcmp(mipsEnv,"0")!=0) ? uint32_t(skip) : 0;
  inst->textureOverBudget=false;
  for(auto& entry:inst->textureRecords) {
    inst->unqueueTexture(entry.second);
    inst->queueTexture(entry.second);
    }
  }

void Resources::finishTextureLoading() {
  if(inst==nullptr || inst->dev==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  std::array<TextureRecord*,2> records{};
  std::array<std::future<Texture2d>,2> uploads;
  const auto publish=[&](size_t slot) {
    if(!uploads[slot].valid())
      return;
    auto texture=uploads[slot].get();
    auto& record=*records[slot];
    inst->unqueueTexture(record);
    if(record.resident)
      inst->retireTexture(std::move(*record.texture));
    *record.texture=std::move(texture);
    record.resident=true;
    if(record.tail)
      ++inst->texturesPromoted;
    record.tail=false;
    record.bytes=LoadProbe::textureBytes(*record.texture);
    inst->notifyTextureChanged(record);
    inst->queueTexture(record);
    };
  size_t slot=0;
  for(auto& entry:inst->textureRecords) {
    auto& record=entry.second;
    if(!record.wanted() || !inst->wantsFull(record) || (record.resident && !record.tail))
      continue;
    publish(slot);
    records[slot]=&record;
    uploads[slot]=std::async(std::launch::async,[path=record.source,mipmaps=record.mipmaps,device=inst->dev] {
      auto data=decodeBaseTexture(path,mipmaps);
      return device->texture(data.pixmap,data.useMipmap);
      });
    slot=(slot+1)%uploads.size();
    }
  publish(slot);
  publish((slot+1)%uploads.size());
  }

Resources::TextureStats Resources::textureStats() {
  if(inst==nullptr)
    return {};
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  inst->updateNearTailTime();
  TextureStats stats;
  stats.promoted=inst->texturesPromoted;
  stats.demoted=inst->texturesDemoted;
  stats.promotionQueue=inst->texturePromotions.size();
  stats.nearTailCount=inst->nearTailTextures.size();
  stats.nearTailSeconds=inst->nearTailSeconds;
  for(const auto& queue:inst->recycled)
    stats.retiredBytes+=queue.textureBytes;
  stats.liveBytes=Texture2d::liveBytes();
  stats.count=Texture2d::liveCount();
  stats.peakBytes=Texture2d::peakBytes();
  stats.evicted=inst->texturesEvicted;
  stats.reloaded=inst->texturesReloaded;
  stats.categoryBytes[size_t(TextureCategory::Other)]=stats.liveBytes;
  stats.categoryCount[size_t(TextureCategory::Other)]=stats.count;
  for(const auto& entry:inst->textureRecords) {
    const auto& record=entry.second;
    if(!record.resident)
      continue;
    const auto bytes=record.bytes;
    (record.tail ? stats.tailBytes : stats.fullBytes)+=bytes;
    stats.sourceBytes[size_t(record.sourceKind)]+=bytes;
    if(!record.tail) {
      stats.sourceFullBytes[size_t(record.sourceKind)]+=bytes;
      if(record.hasTail && inst->textureStreaming && inst->textureSkipMips!=0 &&
         !record.pinned && record.distances.count(TextureAlwaysFull)==0 &&
         !record.distances.empty() && record.distances.begin()->first<=2)
        stats.fullRing2Bytes+=bytes;
      }
    if(record.category==TextureCategory::Other)
      continue;
    stats.categoryBytes[size_t(record.category)]+=bytes;
    stats.categoryCount[size_t(record.category)]+=1;
    stats.categoryBytes[size_t(TextureCategory::Other)]-=bytes;
    stats.categoryCount[size_t(TextureCategory::Other)]-=1;
    }
  uint64_t tracked=0, trackedFull=0;
  for(const auto bytes:stats.sourceBytes)
    tracked+=bytes;
  for(const auto bytes:stats.sourceFullBytes)
    trackedFull+=bytes;
  stats.fullBytes=stats.liveBytes-stats.tailBytes;
  stats.sourceBytes[size_t(TextureSource::Other)]+=stats.liveBytes-tracked;
  stats.sourceFullBytes[size_t(TextureSource::Other)]+=stats.fullBytes-trackedFull;
  return stats;
  }

uint64_t Resources::textureGeneration() {
  if(inst==nullptr)
    return 0;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  return inst->textureEpoch;
  }

std::vector<const Texture2d*> Resources::textureChanges(uint64_t since) {
  if(inst==nullptr)
    return {};
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  std::vector<const Texture2d*> changed;
  const auto first=inst->changedTextures.upper_bound({since,std::numeric_limits<uintptr_t>::max()});
  for(auto it=first;it!=inst->changedTextures.end();++it)
    changed.push_back(it->second);
  return changed;
  }

// Bench `deterministic on` (common/testbench.cpp, reports/engine/2026-11-03-streaming-optimisation/deterministic-shots.md).
// The Full/Tail demotion runs on a wall-clock delay and on memory pressure: with `benchTexturesSettle` texturesReady()
// also waits for the timed demotions, a reload in flight and, above the texture target, the demotion LRU (below it
// the LRU never moves); while `benchTexturesFrozen` updateTextures() changes nothing, so levels depend on the view.
bool benchTexturesSettle = false;
bool benchTexturesFrozen = false;

bool Resources::texturesReady() {
  if(inst==nullptr || inst->dev==nullptr)
    return true;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  if(benchTexturesSettle && (!inst->textureDemotions.empty() || inst->textureReload ||
                             (!inst->textureDemotionLru.empty() && Texture2d::liveBytes()>inst->textureTarget)))
    return false;
  return inst->textureMissing.empty() && inst->texturePromotions.empty();
  }

std::string Resources::texturesWaitState() {
  if(inst==nullptr || inst->dev==nullptr)
    return "none";
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  return "missing="+std::to_string(inst->textureMissing.size())+" promotions="+std::to_string(inst->texturePromotions.size())+
         " demotions="+std::to_string(inst->textureDemotions.size())+" demotion_lru="+std::to_string(inst->textureDemotionLru.size())+
         " reload="+std::to_string(int(bool(inst->textureReload)))+" settle="+std::to_string(int(benchTexturesSettle));
  }

TextureFormat Resources::textureFormat(const Texture2d* texture) {
  if(texture==nullptr)
    return TextureFormat::Undefined;
  if(inst==nullptr)
    return texture->format();
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  if(auto record=inst->textureRecords.find(texture); record!=inst->textureRecords.end())
    return record->second.format;
  return texture->format();
  }

void Resources::textureDemand(const Material& material, TextureCategory category, bool add, uint32_t distance) {
  LoadProbe::Vobs::Scope probe("texture_demand");
  if(inst==nullptr || inst->dev==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  std::vector<const Texture2d*> textures=material.frames;
  textures.insert(textures.end(),{material.tex,material.normal,material.orm});
  std::sort(textures.begin(),textures.end(),std::less<const Texture2d*>());
  textures.erase(std::unique(textures.begin(),textures.end()),textures.end());
  bool unmatched=false;
  for(const auto* texture:textures) {
    auto entry=inst->textureRecords.find(texture);
    if(entry==inst->textureRecords.end())
      continue;
    auto& record=entry->second;
    auto& count=record.demand[size_t(category)];
    if(!add && count==0) {
      if(!unmatched) {
        Log::e("textures: demand released without an owner");
        unmatched=true;
        }
      assert(count!=0 && "texture demand released without an owner");
      continue;
      }
    const bool neededFull=record.wanted() && inst->wantsFull(record);
    inst->unqueueTexture(record);
    if(add) {
      ++count;
      ++record.distances[distance];
      record.lastDemand=++inst->textureClock;
      } else {
      --count;
      const auto found=record.distances.find(distance);
      assert(found!=record.distances.end() && found->second!=0);
      if(found!=record.distances.end() && --found->second==0)
        record.distances.erase(found);
      record.lastDemand=++inst->textureClock;
      }
    if(neededFull || (record.wanted() && inst->wantsFull(record)))
      record.lastNeededFull=record.lastDemand;
    for(size_t i=0;i<record.demand.size();++i)
      if(record.demand[i]!=0) {
        record.category=TextureCategory(i);
        break;
        }
    inst->queueTexture(record);
    }
  }

void Resources::retireTexture(Texture2d&& texture) {
  if(texture.isEmpty())
    return;
  auto& queue=recycled[recycledId];
  queue.textureBytes+=LoadProbe::textureBytes(texture);
  texture.markRetired();
  queue.textures.emplace_back(std::move(texture));
  }

void Resources::notifyTextureChanged(TextureRecord& record) {
  const auto identity=reinterpret_cast<uintptr_t>(record.texture);
  changedTextures.erase({record.changedGeneration,identity});
  record.changedGeneration=textureEpoch+1;
  changedTextures.emplace(std::make_pair(record.changedGeneration,identity),record.texture);
  texturePayloadChanged=true;
  }

void Resources::publishTexture() {
  const auto reload=textureReload;
  if(!reload || reload->ready.isEmpty())
    return;
  auto& record=textureRecords.at(reload->identity);
  unqueueTexture(record);
  const bool full=wantsFull(record);
  const bool hold=!record.distances.empty() && record.distances.begin()->first<=3;
  const bool delayElapsed=record.demoteAt!=std::chrono::steady_clock::time_point{} &&
                          record.demoteAt<=std::chrono::steady_clock::now();
  // The unpublished replacement is already included in the live counter.
  const auto liveWithoutReplacement=Texture2d::liveBytes()-LoadProbe::textureBytes(reload->ready);
  const bool pressure=liveWithoutReplacement>textureTarget;
  if(record.wanted() && (!reload->tail || (!full && (!record.resident || (!hold && delayElapsed && pressure))))) {
    const bool wasResident=record.resident;
    const bool wasTail=record.tail;
    if(wasResident)
      retireTexture(std::move(*record.texture));
    *record.texture=std::move(reload->ready);
    record.bytes=LoadProbe::textureBytes(*record.texture);
    record.resident=true;
    record.tail=reload->tail;
    notifyTextureChanged(record);
    FrameProfile::textureEvent(FrameProfile::TextureActivity::Publish);
    if(!wasResident)
      ++texturesReloaded;
    else if(wasTail && !record.tail)
      ++texturesPromoted;
    else if(!wasTail && record.tail)
      ++texturesDemoted;
    }
  queueTexture(record);
  textureReload.reset();
  }

void Resources::updateTextures(ChunkResidency* residency) {
  FrameProfile::Scope timing(FrameProfile::Zone::TextureUpdate);
  if(inst==nullptr || inst->dev==nullptr || benchTexturesFrozen)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  auto& resources=*inst;
  resources.updateNearTailTime();
  auto liveBytes=Texture2d::liveBytes();
  const auto finish=[&] {
    if(resources.texturePayloadChanged) {
      ++resources.textureEpoch;
      resources.texturePayloadChanged=false;
      }
    if(liveBytes<=resources.textureBudget) {
      resources.textureOverBudget=false;
      } else if(resources.textureStreaming && resources.textureIdle.empty() && !resources.textureOverBudget) {
      Log::i("textures: budget exceeded with no unrequested resident payloads (",liveBytes," > ",resources.textureBudget," bytes)");
      resources.textureOverBudget=true;
      }
    };
  const auto deadline=ChunkResidency::Clock::now()+ChunkResidency::WorkBudget;

  if(resources.textureReload) {
    const auto reload=resources.textureReload;
    try {
      if(reload->pending.valid() && reload->pending.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto payload=reload->pending.get();
        if(reload->tail && !payload.tail)
          resources.textureRecords.at(reload->identity).hasTail=false;
        reload->tail=payload.tail;
        reload->ready=std::move(payload.texture);
        }
      if(!reload->ready.isEmpty() && reload->queued.expired() && reload->ready.uploadReady()) {
        FrameProfile::textureEvent(FrameProfile::TextureActivity::Ready);
        if(residency==nullptr) {
          resources.publishTexture();
          } else {
          auto ticket=std::make_shared<int>(0);
          reload->queued=ticket;
          residency->enqueue([reload,ticket](ChunkResidency::Clock::time_point deadline) {
            if(ChunkResidency::Clock::now()>=deadline)
              return false;
            if(Resources::inst!=nullptr) {
              std::lock_guard<std::recursive_mutex> guard(Resources::inst->sync);
              if(Resources::inst->textureReload==reload)
                Resources::inst->publishTexture();
              }
            return true;
            });
          }
        }
      }
    catch(const std::exception& error) {
      auto& record=resources.textureRecords.at(reload->identity);
      Log::e("textures: unable to reload \"",record.source.string(),"\": ",error.what());
      resources.unqueueTexture(record);
      record.failed=true;
      resources.queueTexture(record);
      resources.textureReload.reset();
      }
    }

  if(!resources.textureReload) {
    const auto now=ChunkResidency::Clock::now();
    while(!resources.textureDemotions.empty() && resources.textureDemotions.begin()->first.first<=now) {
      auto* record=resources.textureDemotions.begin()->second;
      resources.textureDemotionLru.emplace(record->demotionPriority(),record);
      resources.textureDemotions.erase(resources.textureDemotions.begin());
      }
    TextureRecord* selected=nullptr;
    const auto consider=[&](TextureRecord* candidate) {
      const auto priority=[](const TextureRecord& record) {
        return record.pinned || record.distances.count(TextureAlwaysFull)!=0 ? uint32_t(0) :
               record.distances.empty() ? UINT32_MAX : record.distances.begin()->first;
        };
      if(selected==nullptr || priority(*candidate)<priority(*selected))
        selected=candidate;
      };
    for(auto* record:resources.textureMissing)
      consider(record);
    for(auto* record:resources.texturePromotions)
      consider(record);
    bool tail=false;
    if(selected!=nullptr) {
      tail=!resources.wantsFull(*selected);
      } else if(Texture2d::liveBytes()>resources.textureTarget && !resources.textureDemotionLru.empty()) {
      selected=resources.textureDemotionLru.begin()->second;
      tail=true;
      }
    if(selected!=nullptr) {
      auto reload=std::make_shared<TextureReload>();
      reload->identity=selected->texture;
      reload->tail=tail;
      const auto path=selected->source;
      const auto mipmaps=selected->mipmaps;
      const auto skip=tail ? resources.textureSkipMips : 0;
      auto* device=resources.dev;
      reload->pending=std::async(std::launch::async,[path,mipmaps,skip,device] {
        const auto started=LoadProbe::now();
        FrameProfile::TextureWork reading(FrameProfile::TextureActivity::Read);
        auto data=Resources::decodeBaseTexture(path,mipmaps,skip);
        reading.finish();
        FrameProfile::TextureWork uploading(FrameProfile::TextureActivity::Upload);
        auto texture=device->textureAsync(data.pixmap,data.useMipmap);
        uploading.finish();
        if(texture.isEmpty())
          throw std::runtime_error("texture reload returned an empty image");
        LoadProbe::countTexture(texture,started);
        FrameProfile::textureEvent(FrameProfile::TextureActivity::Complete);
        return TextureReload::Payload{std::move(texture),data.tail};
        });
      resources.textureReload=std::move(reload);
      }
    }

  liveBytes=Texture2d::liveBytes();
  size_t evictions=0;
  while(resources.textureStreaming && liveBytes>resources.textureBudget && !resources.textureIdle.empty() &&
        evictions<16 && ChunkResidency::Clock::now()<deadline) {
    auto* record=resources.textureIdle.begin()->second;
    assert(record->resident && !record->wanted());
    auto& neutral=resources.texturePlaceholders[size_t(record->kind)];
    if(neutral.isEmpty()) {
      Pixmap placeholder(1,1,TextureFormat::RGBA8);
      const std::array<uint8_t,4> color=record->kind==TextureKind::Normal ? std::array<uint8_t,4>{128,128,255,128} :
                                        record->kind==TextureKind::Orm ? std::array<uint8_t,4>{255,255,0,255} :
                                                                       std::array<uint8_t,4>{255,255,255,255};
      std::memcpy(placeholder.data(),color.data(),color.size());
      neutral=resources.dev->texture(placeholder,false);
      }
    auto replacement=neutral.shared();
    resources.unqueueTexture(*record);
    resources.retireTexture(std::move(*record->texture));
    *record->texture=std::move(replacement);
    record->resident=false;
    resources.notifyTextureChanged(*record);
    ++resources.texturesEvicted;
    ++evictions;
    liveBytes=Texture2d::liveBytes();
    }
  finish();
  }

Texture2d Resources::loadTextureUncached(std::string_view name, bool forceMips) {
  LoadProbe::Vobs::Scope probe("texture_request");
  return inst->implLoadTextureUncached(name, forceMips);
  }

const Texture2d* Resources::loadTexture(std::string_view name, bool forceMips, bool pinned) {
  LoadProbe::Requests::add(2,name);
  LoadProbe::Vobs::Scope probe("texture_request");
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implLoadTexture(name,forceMips,pinned);
  }

const Texture2d* Resources::loadTexture(Tempest::Color color) {
  LoadProbe::Vobs::Scope probe("texture_request");
  if(isHeadless() || color==Color())
    return nullptr;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  auto& cache = inst->pixCache;
  auto it = cache.find(color);
  if(it!=cache.end())
    return it->second.get();

  LoadProbe::Vobs::Scope loadProbe("texture_load");
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Texture);
  uint8_t iv[4] = { uint8_t(255.f*color.r()), uint8_t(255.f*color.g()), uint8_t(255.f*color.b()), uint8_t(255.f*color.a()) };
  Pixmap p2(1,1,TextureFormat::RGBA8);
  std::memcpy(p2.data(),iv,4);

  auto t       = std::make_unique<Texture2d>(device().texture(p2));
  auto ret     = t.get();
  cache[color] = std::move(t);
  return ret;
  }

bool Resources::hasTextureVariant(std::string_view name) {
  return name.find("_V0")!=std::string_view::npos || name.find("_C0")!=std::string_view::npos;
  }

std::string Resources::textureVariantName(std::string_view name, int32_t iv, int32_t ic) {
  std::string ret(name);
  // Preserve the original long-name fallback and first-tag substitution.
  if(name.size()>=128)
    return ret;
  if(const auto at = ret.find("_V0"); at!=std::string::npos)
    ret.replace(at+1,2,"V"+std::to_string(iv));
  if(const auto at = ret.find("_C0"); at!=std::string::npos)
    ret.replace(at+1,2,"C"+std::to_string(ic));
  return ret;
  }

const Texture2d *Resources::loadTexture(std::string_view name, int32_t iv, int32_t ic) {
  return loadMaterialAlbedo(textureVariantName(name,iv,ic));
  }

bool Resources::hasTexture(std::string_view name) const {
  if(!FileExt::hasExt(name,"TGA")) return false;
  std::string key(name.substr(0,name.size()-4));
  for(auto& c:key) c=char(std::toupper(static_cast<unsigned char>(c)));
  return hdTextures.count(key)!=0 || baseTextures.files.count(key)!=0;
  }

std::vector<const Texture2d*> Resources::loadTextureAnim(std::string_view name, bool pinned) {
  std::vector<const Texture2d*> ret;
  if(name.find("_A0")==std::string::npos &&
     name.find("_a0")==std::string::npos)
    return ret;

  for(int id=0; ; ++id) {
    size_t at = 0;
    char   buf[128]={};
    for(size_t i=0;i<name.size();++i) {
      if(i+2<name.size() && name[i]=='_' && (name[i+1]=='A' || name[i+1]=='a') && name[i+2]=='0'){
        at += size_t(std::snprintf(buf+at,sizeof(buf)-at,"_A%d",id));
        i+=2;
        } else {
        buf[at] = name[i];
        if('a'<=buf[at] && buf[at]<='z')
          buf[at] = char(buf[at]+'A'-'a');
        at++;
        }
      if(at>sizeof(buf))
        return ret;
      }
   auto t = inst->hasTexture(buf) ? loadTexture(buf,false,pinned) : nullptr;
    if(t==nullptr) {
      string_frm buf2(buf,".TGA");
      t = inst->hasTexture(buf2) ? loadTexture(buf2,false,pinned) : nullptr;
      if(t==nullptr)
        return ret;
      }
    ret.push_back(t);
    }
  }

Texture2d Resources::loadTexturePm(const Pixmap &pm) {
  LoadProbe::Vobs::Scope probe("texture_request");
  if(isHeadless())
    return Texture2d();
  LoadProbe::Vobs::Scope loadProbe("texture_load");
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Texture);
  if(pm.isEmpty()) {
    Pixmap p2(1,1,TextureFormat::R8);
    std::memset(p2.data(),0,1);
    return device().texture(p2);
    }
  return device().texture(pm);
  }

Resources::MaterialMaps Resources::loadMaterialMaps(std::string_view name) {
  if(isHeadless() || name.empty())
    return {};
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  if(inst->materialMaps.empty())
    return {};
  std::string key(name);
  for(auto& c:key)
    c = char(std::toupper(uint8_t(c)));
  const auto it = inst->materialMaps.find(key);
  if(it==inst->materialMaps.end())
    return {};
  const auto& files = it->second;
  MaterialMaps ret;
  ret.albedo = inst->implLoadMaterialTexture(files.albedo,TextureKind::Color);
  ret.normal = inst->implLoadMaterialTexture(files.normal,TextureKind::Normal);
  ret.orm    = inst->implLoadMaterialTexture(files.orm,TextureKind::Orm);
  ret.scale  = files.scale;
  ret.antitileCell = files.antitileCell;
  ret.turbidity = files.turbidity;
  ret.turbidityDepth = files.turbidityDepth;
  ret.emissive = files.emissive;
  ret.antitile = files.antitile;
  ret.hair = files.hair;
  ret.present = true;
  ret.hasAlbedo = !files.albedo.empty();
  return ret;
  }

const Texture2d* Resources::loadMaterialAlbedo(std::string_view name) {
  const auto maps = loadMaterialMaps(name);
  if(!maps.hasAlbedo)
    return loadTexture(name,false,false);
  // A broken replacement must not silently open the original texture.
  if(maps.albedo==nullptr)
    return &fallbackTexture();
  materialAlbedoReplaced(name);
  return maps.albedo;
  }

void Resources::materialAlbedoReplaced(std::string_view name) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  if(!inst->census)
    return;
  std::string key(name);
  for(auto& c:key)
    c = char(std::toupper(uint8_t(c)));
  const auto it = inst->materialMaps.find(key);
  if(it!=inst->materialMaps.end() && !it->second.albedoReplacement.empty())
    inst->census->replaced(name,it->second.albedoReplacement);
  }

bool Resources::materialMapsEnabled() {
  return inst!=nullptr && inst->materialMapsPresent;
  }
