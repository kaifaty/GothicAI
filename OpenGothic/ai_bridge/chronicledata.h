#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ChronicleFeat final {
  std::string id;
  std::string title;
  std::string text;
  std::string category;
  int64_t     chapter = 1;
  int64_t     day = 0;
  std::string engraving;
  std::string icon;
  std::string epilogue;
  };

struct ChronicleFork final {
  int64_t                    chapter = 1;
  int64_t                    day = 0;
  std::vector<ChronicleFeat> feats;
  };

struct ChronicleData final {
  std::vector<ChronicleFeat> fate;
  std::vector<ChronicleFork> others;
  };

// Display text is converted to the game codepage; identifiers and paths retain UTF-8.
ChronicleData parseChronicle(std::string_view json);
