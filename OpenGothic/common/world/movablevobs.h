#pragma once

#include <string>
#include <string_view>

#include "asset/worldtypes.h"

namespace MovableVobs {

struct Rule {
  std::string visualPattern;
  std::string className;
  float       massMultiplier = 1.f;
  bool        blocksWalking  = false;
  };

// Only ordinary, visible mesh vobs are eligible. Interactive objects and other
// vob types retain their original physics regardless of their visual name.
const Rule* classify(const asset::VirtualObject& vob);
const Rule* classifyVisual(std::string_view visual);

}
