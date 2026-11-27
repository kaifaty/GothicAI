#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace RoutineDefinitions {
struct Row {
  int startHour = 0, startMinute = 0, endHour = 0, endMinute = 0;
  std::string state, waypoint;
  };
struct Library {
  std::map<std::string,std::vector<Row>> routines;
  std::map<std::string,std::string> uncaptured;
  };

// Reads definitions/<program SHA>/routines.json without a script VM.
Library read(const std::filesystem::path& base, std::string_view programSha256);
Library readFile(const std::filesystem::path& file, std::string_view programSha256);
}
