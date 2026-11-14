#include "islandevents.h"

#include "islandhero.h"
#include "world/chunkresidency.h"
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

void Events::census(uint64_t time, const Hero& hero) {
  const char* file = std::getenv("OPENGOTHIC_CENSUS");
  if(!file || !Resources::writeCensus())
    return;
  std::ifstream in(file);
  const std::string text((std::istreambuf_iterator<char>(in)), {});
  rapidjson::Document doc;
  if(doc.Parse(text.c_str()).HasParseError() || !doc.IsObject())
    throw std::runtime_error("cannot read content census");
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  begin(w, "census", time, hero);
  w.Key("content"); doc.Accept(w);
  w.EndObject();
  output<<buffer.GetString()<<'\n';
  }

Events::~Events() {
  if(residency)
    residency->unsubscribe(subscription);
  }

void Events::watch(ChunkResidency& chunks, const Hero& hero, const uint64_t& time) {
  if(!output.is_open())
    return;
  residency = &chunks;
  subscription = chunks.subscribe([this,&hero,&time](ChunkId id, ChunkResidency::State from, ChunkResidency::State to) {
    const auto state = [](ChunkResidency::State s) {
      switch(s) {
        case ChunkResidency::State::Near: return "Near";
        case ChunkResidency::State::Far: return "Far";
        case ChunkResidency::State::Outside: return "Outside";
        }
      return "Unknown";
      };
    emit("residency", time, hero, {{"id", std::to_string(id.x)+","+std::to_string(id.z)},
                                 {"from", state(from)}, {"to", state(to)}});
    });
  }

void Events::observe(uint64_t time, const Hero& hero) {
  if(!output.is_open())
    return;
  for(const auto& texture:Resources::textureSnapshot()) {
    const auto source = texture.source.string();
    const auto value = std::make_pair(texture.firstMip, texture.reloads);
    const auto found = textures.find(source);
    if(texture.resident && (found==textures.end() || found->second!=value))
      emit("texture", time, hero, {{"name", texture.source.filename().string()}, {"source", source},
                                  {"first_mip", std::to_string(texture.firstMip)},
                                  {"reloads", std::to_string(texture.reloads)}});
    if(texture.resident)
      textures[source] = value;
    else
      textures.erase(source);
    }
  }

void Events::finish(uint64_t time, const Hero& hero, uint64_t frames, const std::vector<uint32_t>& frameMs,
                    int code, bool completed) {
  if(!output.is_open())
    return;
  observe(time, hero);
  census(time, hero);
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
