#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Preflight for story_effect chest_take. Inspect every source before any effect runs, including
// repeated requests for the same stack. The world/VM adapters remain in StoryWorld/StoryEffect.
namespace ChestTake {
struct Request {
  std::string chest;
  std::string item;
  int64_t     count = 0;
  };

struct Stock {
  int64_t count = 0;
  bool    mission = false;
  };

template<class Inspect>
std::string check(const std::vector<Request>& requests, Inspect inspect) {
  std::map<std::pair<std::string,std::string>,int64_t> need;
  for(const auto& request:requests)
    need[{request.chest,request.item}] += request.count;
  for(const auto& [source,count]:need) {
    const auto stock = inspect(source.first,source.second);
    if(!stock)
      return "no chest " + source.first;
    if(stock->mission)
      return "a mission item: " + source.second;
    if(stock->count<count)
      return "missing: chest " + source.first + " " + source.second + " " + std::to_string(count) +
             " (has " + std::to_string(stock->count) + ")";
    }
  return {};
  }
}
