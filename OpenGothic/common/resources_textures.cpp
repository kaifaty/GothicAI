#include "resources.h"
#include "loadprobe.h"
#include "frameprofile.h"
#include "contentcensus.h"
#include "texturelevels.h"
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
      if(record.tail()) {
        LoadProbe::Vobs::Scope probe("texture_load");
        LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Texture);
        auto full=decodeBaseTexture(record.source,record.mipmaps);
        auto replacement=dev->texture(full.pixmap,full.useMipmap);
        if(record.resident)
          retireTexture(std::move(*record.texture));
        *record.texture=std::move(replacement);
        record.resident=true;
        record.firstMip=0;
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
    data=implDecodeTexture(cname,forceMips,textureLoadSkip(pinned));
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
    // Strands as ribbons (graphics/mesh/groommesh.cpp): `albedo` is the colour ramp of the hair, looked up by a
    // strand's tint; lit by the hair BRDF as cards are.
    if(const auto strand = entry.value.FindMember("strand"); strand!=entry.value.MemberEnd()) {
      if(!strand->value.IsBool())
        valid = false;
      else
        maps.strand = strand->value.GetBool();
      }
    // An eyeball (reports/content/2026-11-hero-head/eye-shader.md): the wet highlight of the tear film (F0 0.021) by
    // the roughness of `orm`, lit in the shade of the lids the face rig computes per vertex (FaceRig, lid_shade).
    if(const auto eye = entry.value.FindMember("eye"); eye!=entry.value.MemberEnd()) {
      if(!eye->value.IsBool())
        valid = false;
      else
        maps.eye = eye->value.GetBool();
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
    TextureData data=decodeBaseTexture(path,false,textureLoadSkip(false));
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
    // The whole chain's size from the header, then the tail reader starts again at the header.
    std::array<uint8_t,128> header{};
    const auto got=input.read(header.data(),header.size());
    if(input.unget(got)!=got)
      throw std::runtime_error("unable to rewind \""+path.string()+"\"");
    auto tail=Pixmap::loadDdsMipTail(input,skipMips);
    if(!tail.isEmpty()) {
      const auto word=[&](size_t at) {
        return uint32_t(header[at])|uint32_t(header[at+1])<<8|uint32_t(header[at+2])<<16|uint32_t(header[at+3])<<24;
        };
      TextureData ret;
      ret.useMipmap=forceMips || tail.mipCount()>1;
      ret.source=path;
      ret.height=got==header.size() ? word(12) : tail.h();
      ret.width=got==header.size() ? word(16) : tail.w();
      ret.mipCount=std::max(tail.mipCount(),got==header.size() ? std::max(1u,word(28)) : tail.mipCount());
      ret.firstMip=uint8_t(std::min<uint32_t>(ret.mipCount-tail.mipCount(),255));
      ret.pixmap=std::move(tail);
      return ret;
      }
    }
  TextureData decoded;
  decoded.source=path;
  if(FileExt::hasExt(path.string(),"PNG")) {
    decoded.pixmap=Tempest::Pixmap(path.string());
    decoded.useMipmap=true; // device.texture(pm) generates mips for non-DXT originals.
    } else {
    decoded.pixmap=Tempest::Pixmap(path.string());
    decoded.useMipmap=forceMips || decoded.pixmap.mipCount()>1;
    }
  decoded.width=decoded.pixmap.w();
  decoded.height=decoded.pixmap.h();
  decoded.mipCount=std::max(1u,decoded.pixmap.mipCount());
  return decoded;
  }

bool Resources::TextureRecord::wanted() const {
  return pinned || std::any_of(demand.begin(),demand.end(),[](uint32_t count) { return count!=0; });
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
  record.firstMip=data.firstMip;
  record.width=data.width;
  record.height=data.height;
  record.mipCount=data.mipCount;
  // A readable tail: a DDS with more than one level and both sides at least 128.
  if(FileExt::hasExt(data.source.string(),"DDS"))
    record.lowest=std::max(data.firstMip,TextureLevels::lowest(data.width,data.height,data.mipCount));
  record.bytes=LoadProbe::textureBytes(*texture);
  auto entry=textureRecords.emplace(texture,std::move(record)).first;
  queueTexture(entry->second);
  }

uint32_t Resources::textureLoadSkip(bool pinned) const {
  if(pinned || !textureStreaming || textureSkipMips==0)
    return 0;
  // World load with levels: the coarsest allowed level (the DDS reader stops there), whatever the scope.
  if(textureScreen && textureStartup.load()!=0)
    return TextureLevels::CoarsestRead;
  return loadingTextureTail ? textureSkipMips : 0;
  }

Resources::TextureLoadScope::TextureLoadScope(bool tail):previous(loadingTextureTail) {
  loadingTextureTail=tail;
  }

Resources::TextureLoadScope::~TextureLoadScope() {
  loadingTextureTail=previous;
  }

void Resources::updateNearTailTime() {
  const auto now=std::chrono::steady_clock::now();
  const auto elapsed=std::chrono::duration<double>(now-nearTailUpdated).count();
  nearTailSeconds+=elapsed*double(nearTailTextures.size());
  // Missing and promotion queues hold exactly the wanted textures resident coarser than wanted.
  shortfallSeconds+=elapsed*double(textureMissing.size()+texturePromotions.size());
  nearTailUpdated=now;
  }

uint8_t Resources::tailLevel(const TextureRecord& record) const {
  return uint8_t(std::min<uint32_t>(textureSkipMips,record.lowest));
  }

uint8_t Resources::wantedLevel(const TextureRecord& record) const {
  if(!textureStreaming || textureSkipMips==0 || record.lowest==0 || record.pinned ||
     record.distances.count(TextureAlwaysFull)!=0)
    return 0;
  const auto tail=tailLevel(record);
  // The ring rule: the whole chain within ring 2, the tail further away.
  if(!textureScreen || (record.ringNeeds.empty() && record.resolutions.empty()))
    return !record.distances.empty() && record.distances.begin()->first<=2 ? 0 : tail;
  uint8_t level=record.lowest;
  if(!record.ringNeeds.empty())
    level=std::min<uint8_t>(level,record.ringNeeds.begin()->first<=2 ? 0 : tail);
  if(!record.resolutions.empty())
    level=std::min(level,resolutionLevel(record,textureLadder.stage));
  return level;
  }

std::tuple<uint64_t,uint64_t,uintptr_t> Resources::textureDemotionKey(const TextureRecord& record) const {
  const auto identity=reinterpret_cast<uintptr_t>(record.texture);
  if(!textureScreen) {
    const auto distance=record.distances.empty() ? UINT32_MAX : record.distances.begin()->first;
    return {uint64_t(UINT32_MAX-distance),record.lastNeededFull,identity};
    }
  const auto keep=TextureLevels::chainBytes(record.format,record.width,record.height,record.mipCount,wantedLevel(record));
  const auto gain=record.bytes>keep ? record.bytes-keep : 0;
  return {UINT64_MAX-gain,record.lastNeededFull,identity};
  }

void Resources::unqueueTexture(TextureRecord& record) {
  updateNearTailTime();
  nearTailTextures.erase(&record);
  textureIdle.erase({record.lastDemand,reinterpret_cast<uintptr_t>(record.texture)});
  textureMissing.erase(&record);
  texturePromotions.erase(&record);
  textureDemotions.erase({record.demoteAt,reinterpret_cast<uintptr_t>(record.texture)});
  if(auto found=textureDemotionLru.find(record.demotionKey); found!=textureDemotionLru.end() && found->second==&record)
    textureDemotionLru.erase(found);
  }

void Resources::queueTexture(TextureRecord& record) {
  if(record.resident && record.tail() && !record.distances.empty() && record.distances.begin()->first<=1)
    nearTailTextures.insert(&record);
  if(record.wanted()) {
    const auto wanted=wantedLevel(record);
    if(!record.failed) {
      if(!record.resident)
        textureMissing.insert(&record);
      else if(record.firstMip>wanted)
        texturePromotions.insert(&record);
      }
    // Lowering waits five seconds of eligibility. An owner judged by the ring rule keeps it within ring 3.
    const auto& rings=textureScreen ? record.ringNeeds : record.distances;
    const bool hold=!rings.empty() && rings.begin()->first<=3;
    const bool eligible=textureScreen ? wanted>record.firstMip && !hold : wanted!=0 && !hold;
    if(eligible) {
      if(record.demoteAt==std::chrono::steady_clock::time_point{})
        record.demoteAt=std::chrono::steady_clock::now()+std::chrono::seconds(5);
      if(record.resident && record.firstMip<wanted && !record.failed) {
        if(record.demoteAt<=std::chrono::steady_clock::now()) {
          record.demotionKey=textureDemotionKey(record);
          textureDemotionLru.emplace(record.demotionKey,&record);
          } else {
          textureDemotions.emplace(std::make_pair(record.demoteAt,reinterpret_cast<uintptr_t>(record.texture)),&record);
          }
        }
      } else {
      record.demoteAt={};
      }
    } else if(record.resident) {
    record.demoteAt={};
    textureIdle.emplace(std::make_pair(record.lastDemand,reinterpret_cast<uintptr_t>(record.texture)),&record);
    }
  }

void Resources::requeueTextures(const std::function<void()>& change) {
  ++textureRequeues;
  for(auto& entry:textureRecords)
    unqueueTexture(entry.second);
  change();
  for(auto& entry:textureRecords)
    queueTexture(entry.second);
  }

void Resources::configureTextures(std::string_view budget, std::string_view skipMips, std::string_view target,
                                  std::string_view screenMips) {
  if(inst==nullptr)
    return;
  const auto* enabled=std::getenv("OPENGOTHIC_TEXTURE_STREAMING");
  const auto* budgetEnv=std::getenv("OPENGOTHIC_TEXTURE_BUDGET_MB");
  const auto* targetEnv=std::getenv("OPENGOTHIC_TEXTURE_TARGET_MB");
  const auto* mipsEnv=std::getenv("OPENGOTHIC_TEXTURE_MIPS");
  const auto* skipEnv=std::getenv("OPENGOTHIC_TEXTURE_FAR_SKIP_MIPS");
  const auto* screenEnv=std::getenv("OPENGOTHIC_TEXTURE_SCREEN_MIPS");
  const auto* reserveEnv=std::getenv("OPENGOTHIC_TEXTURE_MIP_RESERVE");
  const auto* jumpEnv=std::getenv("OPENGOTHIC_TEXTURE_JUMP_MS");
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
  // absent, empty or auto: the budget follows the target, the target follows the device (texturebudget.h)
  const auto budgetChoice=TextureBudget::choose(budgetEnv,budget,"texture_budget_mb");
  const auto skip=integer(skipEnv!=nullptr ? std::string_view(skipEnv) : skipMips,2,"texture_far_skip_mips",UINT32_MAX);
  const auto targetChoice=TextureBudget::choose(targetEnv,target,"texture_target_mb");
  const auto vram=TextureBudget::choose(std::getenv("OPENGOTHIC_TEXTURE_VRAM_MB"),{},"OPENGOTHIC_TEXTURE_VRAM_MB");
  const auto screen=integer(screenEnv!=nullptr ? std::string_view(screenEnv) : screenMips,1,"texture_screen_mips",1);
  const auto reserve=integer(reserveEnv!=nullptr ? std::string_view(reserveEnv) : std::string_view(),1,
                             "OPENGOTHIC_TEXTURE_MIP_RESERVE",16);
  const auto jumpMs=integer(jumpEnv!=nullptr ? std::string_view(jumpEnv) : std::string_view(),300,"OPENGOTHIC_TEXTURE_JUMP_MS",60000);
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  inst->requeueTextures([&] {
    inst->textureStreaming=enabled==nullptr || std::strcmp(enabled,"0")!=0;
    inst->textureSkipMips=inst->textureStreaming && (mipsEnv==nullptr || std::strcmp(mipsEnv,"0")!=0) ? uint32_t(skip) : 0;
    inst->textureScreen=screen!=0;
    inst->textureReserve=uint32_t(reserve);
    inst->textureJumpMs=uint32_t(jumpMs);
    inst->textureOverBudget=false;
    inst->setTextureBudget(budgetChoice,targetChoice,vram);
    });
  }

bool Resources::textureScreenMips() {
  return inst!=nullptr && inst->textureScreen;
  }

uint32_t Resources::textureMipReserve() {
  return inst!=nullptr ? inst->textureReserve.load() : 1;
  }

float Resources::textureViewScaleLog2() {
  return inst!=nullptr ? inst->textureViewScale.load() : 11.f;
  }

void Resources::setTextureView(float height, float fovYDegrees) {
  if(inst==nullptr || inst->dev==nullptr)
    return;
  const auto scale=TextureLevels::viewScale(height,fovYDegrees);
  if(!(scale>0))
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  // A larger scale needs sharper levels at once; a smaller one waits for three eighths of a level, so an aim that
  // narrows the view over several frames requeues the textures a few times, not every frame.
  constexpr float Slack=0.125f;
  const float current=inst->textureViewScale.load();
  if(scale<=current && scale>=current-3*Slack)
    return;
  inst->requeueTextures([&] { inst->textureViewScale=scale+Slack; });
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
  stats.shortfallCount=inst->textureMissing.size()+inst->texturePromotions.size();
  stats.shortfallSeconds=inst->shortfallSeconds;
  stats.loadsInFlight=inst->textureReloads.size();
  stats.demandCalls=inst->textureDemandCalls;
  stats.requeues=inst->textureRequeues;
  stats.loadsStarted=inst->textureLoadsStarted;
  stats.jumps=inst->textureJumps;
  stats.jumpsOpen=inst->textureJumpsUnresolved+(inst->textureJump.open ? 1 : 0);
  stats.jumpSyncMsMax=inst->textureJumpSyncMsMax;
  stats.jumpZeroMsMax=inst->textureJumpZeroMsMax;
  for(const auto& queue:inst->recycled)
    stats.retiredBytes+=queue.textureBytes;
  stats.liveBytes=Texture2d::liveBytes();
  stats.count=Texture2d::liveCount();
  stats.peakBytes=Texture2d::peakBytes();
  stats.evicted=inst->texturesEvicted;
  stats.reloaded=inst->texturesReloaded;
  stats.categoryBytes[size_t(TextureCategory::Other)]=stats.liveBytes;
  stats.categoryCount[size_t(TextureCategory::Other)]=stats.count;
  uint64_t coarser=0;
  for(const auto& entry:inst->textureRecords) {
    const auto& record=entry.second;
    if(!record.resident)
      continue;
    const auto bytes=record.bytes;
    (record.tail() ? stats.tailBytes : stats.fullBytes)+=bytes;
    if(record.firstMip!=0) {
      stats.levelBytes[std::min<size_t>(record.firstMip,stats.levelBytes.size()-1)]+=bytes;
      coarser+=bytes;
      }
    stats.sourceBytes[size_t(record.sourceKind)]+=bytes;
    if(!record.tail()) {
      stats.sourceFullBytes[size_t(record.sourceKind)]+=bytes;
      if(record.hasTail() && inst->textureStreaming && inst->textureSkipMips!=0 &&
         !record.pinned && record.distances.count(TextureAlwaysFull)==0 && // ring-rule owners: measured ones key by class
         !(inst->textureScreen ? record.ringNeeds : record.distances).empty() &&
         (inst->textureScreen ? record.ringNeeds : record.distances).begin()->first<=2)
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
  // Untracked payloads (UI, colours, a replacement not yet published) count as whole chains.
  stats.levelBytes[0]=stats.liveBytes>coarser ? stats.liveBytes-coarser : 0;
  stats.sourceBytes[size_t(TextureSource::Other)]+=stats.liveBytes-tracked;
  stats.sourceFullBytes[size_t(TextureSource::Other)]+=stats.fullBytes-trackedFull;
  inst->textureBudgetStats(stats);
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
// The level lowering runs on a wall-clock delay and on memory pressure: with `benchTexturesSettle` texturesReady()
// also waits for the timed demotions, loads in flight and, above the texture target, the demotion queue (below it
// the queue never moves); while `benchTexturesFrozen` updateTextures() changes nothing, so levels depend on the view.
bool benchTexturesSettle = false;
bool benchTexturesFrozen = false;

bool Resources::texturesReady() {
  if(inst==nullptr || inst->dev==nullptr)
    return true;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  if(benchTexturesSettle && (!inst->textureDemotions.empty() || !inst->textureReloads.empty() ||
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
         " reload="+std::to_string(inst->textureReloads.size())+" settle="+std::to_string(int(benchTexturesSettle));
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
  textureDemand(material,category,add,TextureNeed{distance,TextureLevels::NoExponent});
  }

void Resources::textureDemand(const Material& material, TextureCategory category, bool add, const TextureNeed& need) {
  LoadProbe::Vobs::Scope probe("texture_demand");
  if(inst==nullptr || inst->dev==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  inst->implTextureDemand(material,category,add ? nullptr : &need,add ? &need : nullptr);
  }

void Resources::textureDemand(const Material& material, TextureCategory category, const TextureNeed& from, const TextureNeed& to) {
  LoadProbe::Vobs::Scope probe("texture_demand");
  if(inst==nullptr || inst->dev==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  inst->implTextureDemand(material,category,&from,&to);
  }

void Resources::implTextureDemand(const Material& material, TextureCategory category, const TextureNeed* release,
                                  const TextureNeed* add) {
  ++textureDemandCalls;
  std::array<const Texture2d*,3> maps={material.tex,material.normal,material.orm};
  const auto unique=[&](const Texture2d* texture, size_t index) {
    for(size_t i=0;i<index && i<maps.size();++i)
      if(maps[i]==texture)
        return false;
    return texture!=nullptr;
    };
  const auto take=[](auto& owners, auto key) {
    const auto found=owners.find(key);
    assert(found!=owners.end() && found->second!=0);
    if(found!=owners.end() && --found->second==0)
      owners.erase(found);
    };
  bool unmatched=false;
  const auto visit=[&](const Texture2d* texture) {
    auto entry=textureRecords.find(texture);
    if(entry==textureRecords.end())
      return;
    auto& record=entry->second;
    auto& count=record.demand[size_t(category)];
    if(release!=nullptr && count==0) {
      if(!unmatched) {
        Log::e("textures: demand released without an owner");
        unmatched=true;
        }
      assert(count!=0 && "texture demand released without an owner");
      return;
      }
    const bool neededFull=record.wanted() && wantsFull(record);
    unqueueTexture(record);
    if(release!=nullptr) {
      --count;
      take(record.distances,release->ring);
      if(release->resolution==TextureLevels::NoExponent)
        take(record.ringNeeds,release->ring);
      else
        take(record.resolutions,release->resolution);
      }
    if(add!=nullptr) {
      ++count;
      ++record.distances[add->ring];
      if(add->resolution==TextureLevels::NoExponent)
        ++record.ringNeeds[add->ring];
      else
        ++record.resolutions[add->resolution];
      }
    record.lastDemand=++textureClock;
    if(neededFull || (record.wanted() && wantsFull(record)))
      record.lastNeededFull=record.lastDemand;
    for(size_t i=0;i<record.demand.size();++i)
      if(record.demand[i]!=0) {
        record.category=TextureCategory(i);
        break;
        }
    queueTexture(record);
    };
  // Each identity once: animation frames, then the maps not among them.
  for(size_t i=0;i<material.frames.size();++i)
    if(std::find(material.frames.begin(),material.frames.begin()+ptrdiff_t(i),material.frames[i])==material.frames.begin()+ptrdiff_t(i) &&
       material.frames[i]!=nullptr)
      visit(material.frames[i]);
  for(size_t i=0;i<maps.size();++i)
    if(unique(maps[i],i) && std::find(material.frames.begin(),material.frames.end(),maps[i])==material.frames.end())
      visit(maps[i]);
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

void Resources::publishTexture(const std::shared_ptr<TextureReload>& reload) {
  if(!reload || reload->ready.isEmpty())
    return;
  auto& record=textureRecords.at(reload->identity);
  unqueueTexture(record);
  record.loading=false;
  const auto wanted=wantedLevel(record);
  const auto level=reload->level;
  const auto& rings=textureScreen ? record.ringNeeds : record.distances;
  const bool hold=!rings.empty() && rings.begin()->first<=3;
  const bool delayElapsed=record.demoteAt!=std::chrono::steady_clock::time_point{} &&
                          record.demoteAt<=std::chrono::steady_clock::now();
  // The unpublished replacement is already included in the live counter.
  const auto liveWithoutReplacement=Texture2d::liveBytes()-LoadProbe::textureBytes(reload->ready);
  const bool pressure=liveWithoutReplacement>textureTarget;
  bool publish=false;
  if(record.wanted()) {
    if(!record.resident)
      publish=textureScreen || level<=wanted; // the ring rule fills a placeholder with a tail only where it is wanted
    else if(level<record.firstMip)
      publish=true;
    else if(level>record.firstMip)
      publish=level<=wanted && !hold && delayElapsed && pressure;
    }
  if(publish) {
    const bool wasResident=record.resident;
    const auto previous=record.firstMip;
    if(wasResident)
      retireTexture(std::move(*record.texture));
    *record.texture=std::move(reload->ready);
    record.bytes=LoadProbe::textureBytes(*record.texture);
    record.resident=true;
    record.firstMip=level;
    notifyTextureChanged(record);
    FrameProfile::textureEvent(FrameProfile::TextureActivity::Publish);
    if(!wasResident) {
      ++texturesReloaded;
      ++record.reloads;
      }
    else if(level<previous)
      ++texturesPromoted;
    else if(level>previous)
      ++texturesDemoted;
    }
  queueTexture(record);
  textureReloads.erase(std::remove(textureReloads.begin(),textureReloads.end(),reload),textureReloads.end());
  }

void Resources::startTextureLoad(TextureRecord& record, uint8_t level, bool lowering) {
  auto reload=std::make_shared<TextureReload>();
  reload->identity=record.texture;
  reload->level=level;
  reload->lowering=lowering;
  ++textureLoadsStarted;
  record.loading=true;
  reload->pending=std::async(std::launch::async,[path=record.source,mipmaps=record.mipmaps,level,device=dev] {
    const auto started=LoadProbe::now();
    FrameProfile::TextureWork reading(FrameProfile::TextureActivity::Read);
    auto data=Resources::decodeBaseTexture(path,mipmaps,level);
    reading.finish();
    FrameProfile::TextureWork uploading(FrameProfile::TextureActivity::Upload);
    auto texture=device->textureAsync(data.pixmap,data.useMipmap);
    uploading.finish();
    if(texture.isEmpty())
      throw std::runtime_error("texture reload returned an empty image");
    LoadProbe::countTexture(texture,started);
    FrameProfile::textureEvent(FrameProfile::TextureActivity::Complete);
    return TextureReload::Payload{std::move(texture),data.firstMip};
    });
  textureReloads.push_back(std::move(reload));
  }

void Resources::updateTextures(ChunkResidency* residency) {
  FrameProfile::Scope timing(FrameProfile::Zone::TextureUpdate);
  if(inst==nullptr || inst->dev==nullptr || benchTexturesFrozen)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  auto& resources=*inst;
  resources.updateTextureBudget(false);
  resources.updateNearTailTime();
  auto liveBytes=Texture2d::liveBytes();
  const auto finish=[&] {
    if(resources.texturePayloadChanged) {
      ++resources.textureEpoch;
      resources.texturePayloadChanged=false;
      }
    if(liveBytes<=resources.textureBudget) {
      resources.textureOverBudget=false;
      } else if(resources.textureStreaming && resources.textureIdle.empty() && !resources.textureOverBudget &&
              !resources.textureBudgetFollows) {
      Log::i("textures: budget exceeded with no unrequested resident payloads (",liveBytes," > ",resources.textureBudget," bytes)");
      resources.textureOverBudget=true;
      }
    };
  const auto deadline=ChunkResidency::Clock::now()+ChunkResidency::WorkBudget;

  // Loads in flight: at most one publication a frame, through the residency's frame queue.
  bool publishing=std::any_of(resources.textureReloads.begin(),resources.textureReloads.end(),
                              [](const auto& reload) { return !reload->queued.expired(); });
  for(size_t i=0;i<resources.textureReloads.size();) {
    const auto reload=resources.textureReloads[i];
    try {
      if(reload->pending.valid() && reload->pending.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto payload=reload->pending.get();
        if(payload.level<reload->level) {
          // No tail at the level asked for: the file has none above the minimum side.
          auto& record=resources.textureRecords.at(reload->identity);
          resources.unqueueTexture(record);
          record.lowest=std::min(record.lowest,payload.level);
          resources.queueTexture(record);
          }
        reload->level=payload.level;
        reload->ready=std::move(payload.texture);
        }
      if(!publishing && !reload->ready.isEmpty() && reload->queued.expired() && reload->ready.uploadReady()) {
        FrameProfile::textureEvent(FrameProfile::TextureActivity::Ready);
        publishing=true;
        if(residency==nullptr) {
          resources.publishTexture(reload);
          continue;
          }
        auto ticket=std::make_shared<int>(0);
        reload->queued=ticket;
        residency->enqueue([reload,ticket](ChunkResidency::Clock::time_point deadline) {
          if(ChunkResidency::Clock::now()>=deadline)
            return false;
          if(Resources::inst!=nullptr) {
            std::lock_guard<std::recursive_mutex> guard(Resources::inst->sync);
            const auto& reloads=Resources::inst->textureReloads;
            if(std::find(reloads.begin(),reloads.end(),reload)!=reloads.end())
              Resources::inst->publishTexture(reload);
            }
          return true;
          });
        }
      }
    catch(const std::exception& error) {
      auto& record=resources.textureRecords.at(reload->identity);
      Log::e("textures: unable to reload \"",record.source.string(),"\": ",error.what());
      resources.unqueueTexture(record);
      record.failed=true;
      record.loading=false;
      resources.queueTexture(record);
      resources.textureReloads.erase(resources.textureReloads.begin()+ptrdiff_t(i));
      continue;
      }
    ++i;
    }

  // Levels keep two loads in flight, the ring rule one.
  const size_t inFlight=resources.textureScreen ? 2 : 1;
  if(resources.textureReloads.size()<inFlight) {
    const auto now=ChunkResidency::Clock::now();
    while(!resources.textureDemotions.empty() && resources.textureDemotions.begin()->first.first<=now) {
      auto* record=resources.textureDemotions.begin()->second;
      record->demotionKey=resources.textureDemotionKey(*record);
      resources.textureDemotionLru.emplace(record->demotionKey,record);
      resources.textureDemotions.erase(resources.textureDemotions.begin());
      }
    }
  while(resources.textureReloads.size()<inFlight) {
    TextureRecord* selected=nullptr;
    // The ring rule: the nearest first. Levels: the largest shortfall in levels, then the nearest.
    const auto ring=[](const TextureRecord& record) {
      return record.pinned || record.distances.count(TextureAlwaysFull)!=0 ? uint32_t(0) :
             record.distances.empty() ? UINT32_MAX : record.distances.begin()->first;
      };
    const auto shortfall=[&](const TextureRecord& record) {
      const uint32_t resident=record.resident ? record.firstMip : std::max<uint32_t>(record.mipCount,1);
      return int64_t(resident)-int64_t(resources.wantedLevel(record));
      };
    const auto consider=[&](TextureRecord* candidate) {
      if(candidate->loading)
        return;
      if(selected==nullptr)
        selected=candidate;
      else if(!resources.textureScreen ? ring(*candidate)<ring(*selected) :
              TextureLevels::loadsBefore(shortfall(*candidate),ring(*candidate),shortfall(*selected),ring(*selected)))
        selected=candidate;
      };
    for(auto* record:resources.textureMissing)
      consider(record);
    for(auto* record:resources.texturePromotions)
      consider(record);
    bool lowering=false;
    // Above the target, unrequested textures leave first: no lowering while the LRU budget can still evict.
    if(selected==nullptr && Texture2d::liveBytes()>resources.textureTarget &&
       (resources.textureIdle.empty() || Texture2d::liveBytes()<=resources.textureBudget) &&
       std::none_of(resources.textureReloads.begin(),resources.textureReloads.end(),[](const auto& r) { return r->lowering; })) {
      // Lowering, one at a time and only under memory pressure: the ring rule's LRU or the most bytes to give back.
      for(const auto& entry:resources.textureDemotionLru)
        if(!entry.second->loading) {
          selected=entry.second;
          lowering=true;
          break;
          }
      }
    if(selected==nullptr)
      break;
    resources.startTextureLoad(*selected,resources.wantedLevel(*selected),lowering);
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
    ++record->evictions;
    record->evictionsUnowned+=record->lastDemand==0;
    resources.notifyTextureChanged(*record);
    ++resources.texturesEvicted;
    ++evictions;
    liveBytes=Texture2d::liveBytes();
    }
  resources.updateTextureJump();
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
  ret.strand = files.strand;
  ret.eye = files.eye;
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
