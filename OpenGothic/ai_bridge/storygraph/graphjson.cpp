#include "graphjson.h"

#include "outputunits.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>

storygraph::LineTexts storygraph::loadLines(const WorldData::ZenFile& library) {
  LineTexts lines;
  for(const auto& block : OutputUnits::sortedBlocks(library)) {
    if(auto message = OutputUnits::message(*block)) {
      auto name = block->name;
      for(auto& c : name)
        c = char(std::toupper(uint8_t(c)));
      lines[std::move(name)] = message->text;
      }
    }
  return lines;
  }

std::string storygraph::toJson(const Model& graph, const GraphSources& sources, bool pretty) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> json(buffer);
  const auto digest = [&](const char* key, std::string_view value) {
    json.Key(key);
    if(value.empty())
      json.Null();
    else
      json.String(value.data(),rapidjson::SizeType(value.size()));
    };
  json.StartObject();
  digest("program_native_sha256",sources.programNative);
  digest("program_source_sha256",sources.programSource);
  digest("ou_native_sha256",sources.ouNative);
  digest("ou_source_sha256",sources.ouSource);
  digest("converter_sha256",sources.converter);
  json.EndObject();

  auto text = toJson(graph,pretty);
  text.insert(1,"\"sources\":"+std::string(buffer.GetString(),buffer.GetSize())+",");
  return text;
  }
