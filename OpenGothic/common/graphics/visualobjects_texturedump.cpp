// OPENGOTHIC_TEXTURE_DUMP (texturedump.h): every texture owner of VisualObjects.
#include "visualobjects.h"

#include <Tempest/Log>

#include <cmath>
#include <cstdio>

void VisualObjects::dumpTextureOwners(const std::filesystem::path& file) const {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  std::FILE* out=std::fopen(file.string().c_str(),"w");
  if(out==nullptr) {
    Tempest::Log::e("texture dump: unable to write \"",file.string(),"\"");
    return;
    }
  const auto& v=textureView;
  std::fprintf(out,"# hero=%.1f;%.1f;%.1f camera=%.1f;%.1f;%.1f odometer=%.3f known=%d coarsest=%d screen=%d queue=%zu scheduled=%zu\n",
               double(v.hero.x),double(v.hero.y),double(v.hero.z),double(v.camera.x),double(v.camera.y),double(v.camera.z),
               v.odometer,int(v.known),v.coarsest,int(textureScreenMode),textureScreen.size(),textureScheduledCount);
  std::fprintf(out,"id,type,category,world_owner,chunked_landscape,measured,placed,screen_owner,demand,scheduled,ring,resolution,"
                   "local_uv,uv,min_x,min_y,min_z,max_x,max_y,max_z,pos_x,pos_y,pos_z,radius,reach,distance,tex,normal,orm,frames,"
                   "ibo_len\n");
  for(size_t id=0;id<objects.size();++id) {
    const auto& o=objects[id];
    if(o.isEmpty())
      continue;
    const auto& t=o.textureTexel;
    const auto& mat=const_cast<Object&>(o).bucketId->mat;
    const double distance=textureDistance(o);
    std::fprintf(out,"%zu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,",id,int(o.type),int(o.textureCategory),int(o.textureWorldOwner),
                 int(o.textureChunkedLandscape),int(o.textureMeasured),int(o.texturePlaced),int(textureScreenOwner(o)),
                 int(o.textureDemand),int(o.textureScheduled),o.textureNeed.ring);
    if(o.textureNeed.resolution==TextureLevels::NoExponent)
      std::fprintf(out,",");
    else
      std::fprintf(out,"%d,",o.textureNeed.resolution);
    std::fprintf(out,"%.6g,%.6g,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,",
                 double(o.textureLocal.uvPerMetre),double(t.uvPerMetre),double(t.min.x),double(t.min.y),double(t.min.z),
                 double(t.max.x),double(t.max.y),double(t.max.z),double(o.texturePosition.x),double(o.texturePosition.y),
                 double(o.texturePosition.z));
    // radius: the sphere of an owner placed by a position, cm; reach: how far it may move before it is evaluated again
    if(o.textureRadius>=0)
      std::fprintf(out,"%.1f,",double(o.textureRadius));
    else
      std::fprintf(out,",");
    if(o.textureWorldOwner && std::isfinite(o.textureReach))
      std::fprintf(out,"%.1f,",double(o.textureReach));
    else
      std::fprintf(out,",");
    std::fprintf(out,"%.2f,%llx,%llx,%llx,%zu,%u\n",distance,
                 (unsigned long long)reinterpret_cast<uintptr_t>(mat.tex),(unsigned long long)reinterpret_cast<uintptr_t>(mat.normal),
                 (unsigned long long)reinterpret_cast<uintptr_t>(mat.orm),mat.frames.size(),o.iboLen);
    }
  std::fclose(out);
  }

void VisualObjects::reportTextureCensus() {
  auto& c=textureCensus;
  const auto now=std::chrono::steady_clock::now();
  if(c.since==std::chrono::steady_clock::time_point{}) {
    c.since=now;
    return;
    }
  if(now-c.since<std::chrono::seconds(1))
    return;
  const auto r=Resources::textureStats();
  char line[256];
  std::snprintf(line,sizeof(line),"[texture-demand] s=%.2f calls=%llu placements=%llu moves=%llu evaluations=%llu stale=%llu "
                "compactions=%llu focus_owners=%llu classes=%llu sweep=%llu heap=%zu scheduled=%zu",
                std::chrono::duration<double>(now-c.since).count(),(unsigned long long)c.demandCalls,
                (unsigned long long)c.placements,(unsigned long long)c.moves,(unsigned long long)c.evaluations,
                (unsigned long long)c.stale,
                (unsigned long long)c.compactions,(unsigned long long)c.focusOwners,(unsigned long long)c.classes,
                (unsigned long long)c.sweep,
                textureScreen.size(),textureScheduledCount);
  Tempest::Log::i(line);
  std::snprintf(line,sizeof(line),"[texture-demand] view_ms=%.3f view_max_ms=%.3f focus_ms=%.3f focus_max_ms=%.3f "
                "resource_calls=%llu requeues=%llu loads=%llu odometer=%.1f",
                c.viewMs,c.viewMaxMs,c.focusMs,c.focusMaxMs,(unsigned long long)(r.demandCalls-c.resourceCalls),
                (unsigned long long)(r.requeues-c.requeues),(unsigned long long)(r.loadsStarted-c.loads),textureView.odometer);
  Tempest::Log::i(line);
  const bool enabled=c.enabled;
  c=TextureCensus();
  c.enabled=enabled;
  c.since=now;
  c.resourceCalls=r.demandCalls;
  c.requeues=r.requeues;
  c.loads=r.loadsStarted;
  }

std::pair<uint64_t,uint64_t> VisualObjects::textureQueueDroppedPastEnd() const {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  return {textureDroppedPastEnd[0],textureDroppedPastEnd[1]};
  }
