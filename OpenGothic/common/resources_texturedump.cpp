// OPENGOTHIC_TEXTURE_DUMP (texturedump.h): every tracked texture with its levels and demand.
#include "resources.h"
#include "texturelevels.h"

#include <Tempest/Log>

#include <cstdio>

void Resources::dumpTextures(const std::filesystem::path& file) {
  if(inst==nullptr || inst->dev==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  auto& r=*inst;
  std::FILE* out=std::fopen(file.string().c_str(),"w");
  if(out==nullptr) {
    Tempest::Log::e("texture dump: unable to write \"",file.string(),"\"");
    return;
    }
  const auto now=std::chrono::steady_clock::now();
  std::fprintf(out,"# view_scale=%.4f reserve=%u screen=%d skip=%u target=%llu budget=%llu live=%llu loads=%zu missing=%zu "
                   "promotions=%zu delay=%zu queued=%zu\n",
               double(r.textureViewScale.load()),r.textureReserve.load(),int(r.textureScreen.load()),r.textureSkipMips,
               (unsigned long long)r.textureTarget,(unsigned long long)r.textureBudget,
               (unsigned long long)Tempest::Texture2d::liveBytes(),r.textureReloads.size(),r.textureMissing.size(),
               r.texturePromotions.size(),r.textureDemotions.size(),r.textureDemotionLru.size());
  std::fprintf(out,"id,name,kind,source_kind,format,width,height,mips,lowest,first_mip,resident,failed,loading,pinned,bytes,"
                   "wanted_bytes,category,demand_landscape,demand_static,demand_dynamic,demand_other,wanted,wanted_level,"
                   "ring_level,resolution_level,resolution_max,resolution_owners,whole_chain_owners,ring_need_min,"
                   "ring_need_owners,ring_min,always_full,lowering,delay_ms,missing,promotion\n");
  for(const auto& entry:r.textureRecords) {
    const auto& t=entry.second;
    const auto id=reinterpret_cast<uintptr_t>(t.texture);
    const bool wanted=t.wanted();
    const int wantedLevel=wanted ? int(r.wantedLevel(t)) : -1;
    int ringLevel=-1, resolutionLevel=-1;
    uint32_t resolutionOwners=0, wholeChain=0, ringOwners=0, alwaysFull=0;
    for(const auto& n:t.resolutions) {
      resolutionOwners+=n.second;
      if(n.first==std::numeric_limits<int32_t>::max())
        wholeChain+=n.second;
      }
    for(const auto& n:t.ringNeeds)
      ringOwners+=n.second;
    if(auto it=t.distances.find(TextureAlwaysFull); it!=t.distances.end())
      alwaysFull=it->second;
    if(!t.ringNeeds.empty())
      ringLevel=t.ringNeeds.begin()->first<=2 ? 0 : int(r.tailLevel(t));
    if(!t.resolutions.empty())
      resolutionLevel=TextureLevels::level(t.width,t.height,r.textureViewScale,t.resolutions.rbegin()->first,
                                           r.textureReserve,t.lowest);
    const char* lowering="none";
    double delayMs=0;
    if(r.textureDemotions.count({t.demoteAt,id})!=0) {
      lowering="delay";
      delayMs=std::chrono::duration<double,std::milli>(t.demoteAt-now).count();
      } else if(auto it=r.textureDemotionLru.find(t.demotionKey); it!=r.textureDemotionLru.end() && it->second==&t) {
      lowering="queued";
      }
    const auto wantedBytes=wanted ? TextureLevels::chainBytes(t.format,t.width,t.height,t.mipCount,uint32_t(wantedLevel)) : 0;
    std::string name=t.source.filename().string();
    for(auto& c:name)
      if(c==',')
        c=';';
    std::fprintf(out,"%llx,%s,%d,%d,%d,%u,%u,%u,%u,%u,%d,%d,%d,%d,%llu,%llu,%d,%u,%u,%u,%u,%d,%d,%d,%d,",
                 (unsigned long long)id,name.c_str(),int(t.kind),int(t.sourceKind),int(t.format),t.width,t.height,t.mipCount,
                 unsigned(t.lowest),unsigned(t.firstMip),int(t.resident),int(t.failed),int(t.loading),int(t.pinned),
                 (unsigned long long)t.bytes,(unsigned long long)wantedBytes,int(t.category),
                 t.demand[0],t.demand[1],t.demand[2],t.demand[3],int(wanted),wantedLevel,ringLevel,resolutionLevel);
    if(t.resolutions.empty())
      std::fprintf(out,",");
    else
      std::fprintf(out,"%d,",t.resolutions.rbegin()->first);
    std::fprintf(out,"%u,%u,",resolutionOwners,wholeChain);
    if(t.ringNeeds.empty())
      std::fprintf(out,",");
    else
      std::fprintf(out,"%u,",t.ringNeeds.begin()->first);
    std::fprintf(out,"%u,",ringOwners);
    if(t.distances.empty())
      std::fprintf(out,",");
    else
      std::fprintf(out,"%u,",t.distances.begin()->first);
    std::fprintf(out,"%u,%s,%.1f,%d,%d\n",alwaysFull,lowering,delayMs,int(r.textureMissing.count(const_cast<TextureRecord*>(&t))!=0),
                 int(r.texturePromotions.count(const_cast<TextureRecord*>(&t))!=0));
    }
  std::fclose(out);
  }
