#pragma once

#include "model.h"
#include "worlddata.h"

#include <string_view>

namespace storygraph {

struct GraphSources {
  std::string_view programNative;
  std::string_view programSource;
  std::string_view ouNative;
  std::string_view ouSource;
  std::string_view converter;
  };

LineTexts loadLines(const WorldData::ZenFile& library);
std::string toJson(const Model& graph, const GraphSources& sources, bool pretty);

}
