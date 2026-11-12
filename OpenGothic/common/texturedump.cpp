#include "texturedump.h"

#include <Tempest/Log>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "graphics/worldview.h"
#include "resources.h"

void TextureDump::write(std::string_view stage, WorldView* view) {
  const char* dir = std::getenv("OPENGOTHIC_TEXTURE_DUMP");
  if(dir==nullptr || dir[0]=='\0')
    return;
  std::string name(stage);
  for(auto& c:name)
    if(c=='/' || c=='\\' || c==' ')
      c = '-';
  std::error_code error;
  std::filesystem::create_directories(dir,error);
  const std::filesystem::path root(dir);
  Resources::dumpTextures(root/(name+"-textures.csv"));
  if(view!=nullptr)
    view->dumpTextureOwners(root/(name+"-owners.csv"));
  Tempest::Log::i("texture dump: ",name," in \"",dir,"\"");
  }

std::string TextureDump::flowFields(const Resources::TextureStats& after, const Resources::TextureStats& before,
                                    const TextureFlow::Distributions& now, const TextureFlow::Distributions& was) {
  const auto& a=after.flow;
  const auto& b=before.flow;
  std::string out;
  char field[256];
  for(size_t i=0;i<TextureFlow::States;++i) {
    std::snprintf(field,sizeof(field),",\"tex_shortfall_%s_seconds\":%.9g",TextureFlow::name(TextureFlow::State(i)),
                  a.seconds[i]-b.seconds[i]);
    out+=field;
    }
  std::snprintf(field,sizeof(field),",\"tex_visible_shortfall_seconds\":%.9g",a.visibleSeconds-b.visibleSeconds);
  out+=field;
  // part C: texture-seconds queued for the look-ahead alone, and its horizon at the end of the pass
  std::snprintf(field,sizeof(field),",\"tex_ahead_seconds\":%.9g,\"tex_lookahead_ms\":%.9g",a.aheadSeconds-b.aheadSeconds,
                1000*a.horizon);
  out+=field;
  std::snprintf(field,sizeof(field),",\"tex_loads_started\":%llu,\"tex_short_entries\":%llu",
                (unsigned long long)(after.loadsStarted-before.loadsStarted),(unsigned long long)(a.entries-b.entries));
  out+=field;
  const auto times=[&](const char* name, const TextureFlow::Histogram& now, const TextureFlow::Histogram& was) {
    std::snprintf(field,sizeof(field),",\"tex_%s_n\":%llu,\"tex_%s_p50_ms\":%.1f,\"tex_%s_p90_ms\":%.1f,\"tex_%s_max_ms\":%.1f",
                  name,(unsigned long long)now.count(was),name,now.quantile(0.5,was),name,now.quantile(0.9,was),name,
                  now.max(was));
    out+=field;
    };
  // the publications' own cost on the main thread (part B): count and milliseconds of the pass, the session's longest
  std::snprintf(field,sizeof(field),",\"tex_publications\":%llu,\"tex_publish_ms\":%.9g,\"tex_publish_max_ms\":%.9g",
                (unsigned long long)(a.publications-b.publications),a.publishMs-b.publishMs,a.publishMaxMs);
  out+=field;
  times("latency",now.latency,was.latency);
  times("wait",now.wait,was.wait);
  std::snprintf(field,sizeof(field),",\"tex_bursts\":%llu,\"tex_burst_p50\":%.0f,\"tex_burst_p90\":%.0f,\"tex_burst_max\":%u",
                (unsigned long long)now.bursts.count(was.bursts),now.bursts.quantile(0.5,was.bursts),
                now.bursts.quantile(0.9,was.bursts),now.bursts.max(was.bursts));
  out+=field;
  return out;
  }
