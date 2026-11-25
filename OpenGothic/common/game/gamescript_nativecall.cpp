#include "gamescript_nativecall.h"

#include <algorithm>
#include <cctype>

using namespace storygraph;

std::string storygraph::nativeOperationName(std::string_view name) {
  std::string key(name);
  std::ranges::transform(key,key.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  return key;
  }
