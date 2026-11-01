#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The hero's chronicle as the AI server sends it (protocol v55): this branch's fate and the
// other fates of the playthrough by save fork. The bridge fills it, the diary reads it.
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
