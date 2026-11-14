#include "islandevents.h"

#include "islandhero.h"
#include "islandview.h"
#include "resources.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace island {
namespace {
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
void begin(Writer& w, const char* kind, uint64_t time, const Hero& hero) {
  w.StartObject();
  w.Key("event"); w.String(kind);
  w.Key("ms"); w.Uint64(time);
  w.Key("x"); w.Double(double(hero.position().x)*0.01);
  w.Key("y"); w.Double(double(hero.position().y)*0.01);
  w.Key("z"); w.Double(double(hero.position().z)*0.01);
  w.Key("grounded"); w.Bool(hero.onGround());
  }
}

Events::Events(const std::filesystem::path& file) {
  if(file.empty())
    return;
  output.open(file);
  if(!output)
    throw std::runtime_error("cannot write events: "+file.string());
  output.exceptions(std::ios::badbit | std::ios::failbit);
  }

void Events::emit(const char* kind, uint64_t time, const Hero& hero,
                  std::initializer_list<std::pair<const char*,std::string>> fields) {
  if(!output.is_open())
    return;
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  begin(w, kind, time, hero);
  for(const auto& [name,value]:fields) {
    w.Key(name); w.String(value.c_str());
    }
  w.EndObject();
  output<<buffer.GetString()<<'\n';
  }

void Events::census(uint64_t time, const Hero& hero, bool final) {
  const char* file = std::getenv("OPENGOTHIC_CENSUS");
  if(!file || !Resources::writeCensus())
    return;
  std::ifstream in(file);
  const std::string text((std::istreambuf_iterator<char>(in)), {});
  rapidjson::Document doc;
  if(doc.Parse(text.c_str()).HasParseError() || !doc.IsObject())
    throw std::runtime_error("cannot read content census");
  for(const auto* list:{"files", "replacements"}) {
    if(!doc.HasMember(list))
      continue;
    for(const auto& item:doc[list].GetArray()) {
      const char* source = item.HasMember("source") ? "source" : "replacement";
      const std::string path = item[source].GetString();
      if((path.ends_with(".dds") || path.ends_with(".DDS")) && textures.insert(path).second)
        emit("texture", time, hero, {{"name", item["name"].GetString()}, {"source", path}});
      }
    }
  if(final) {
    rapidjson::StringBuffer buffer;
    Writer w(buffer);
    begin(w, "census", time, hero);
    w.Key("content"); doc.Accept(w);
    w.EndObject();
    output<<buffer.GetString()<<'\n';
    }
  }

void Events::observe(uint64_t time, const Hero& hero, const View& view) {
  if(!output.is_open())
    return;
  // Actual GPU publications/releases, not the desired Near/Far states. A frame may publish several chunks.
  const auto& stats = view.residency().stats();
  if(stats.published>published)
    emit("chunk_load", time, hero, {{"count", std::to_string(stats.published-published)}});
  if(stats.released>released)
    emit("chunk_unload", time, hero, {{"count", std::to_string(stats.released-released)}});
  published = stats.published;
  released = stats.released;
  if(time>=sampled) {
    census(time, hero, false);
    sampled = time+100;
    }
  }

void Events::finish(uint64_t time, const Hero& hero, uint64_t frames, const std::vector<uint32_t>& frameMs,
                    int code, bool completed) {
  if(!output.is_open())
    return;
  census(time, hero, true);
  // Match Window's statistics: exclude the first 60 intervals (shader/texture warm-up).
  std::vector<uint32_t> ms(frameMs.size()>60 ? frameMs.begin()+60 : frameMs.end(), frameMs.end());
  std::sort(ms.begin(), ms.end());
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  begin(w, "end", time, hero);
  w.Key("frames"); w.Uint64(frames);
  w.Key("frame_median_ms"); w.Uint(ms.empty() ? 0 : ms[ms.size()/2]);
  w.Key("frame_p95_ms"); w.Uint(ms.empty() ? 0 : ms[std::min(ms.size()-1, ms.size()*95/100)]);
  w.Key("exit_code"); w.Int(code);
  w.Key("completed"); w.Bool(completed);
  w.EndObject();
  output<<buffer.GetString()<<'\n';
  output.flush();
  }
}
