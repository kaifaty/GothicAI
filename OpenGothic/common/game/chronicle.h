#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The hero's chronicle as the AI server sends it (protocol v57): this branch's fate and the
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

struct ChronicleEnd final {
  std::string kind, title, text, name;
  int64_t     chapter = 1, day = 0;
  };

struct ChronicleFork final {
  int64_t                    chapter = 1;
  int64_t                    day = 0;
  std::vector<ChronicleFeat> feats;
  std::vector<ChronicleEnd>  ends;
  };

struct ChronicleData final {
  std::vector<ChronicleFeat> fate;
  std::vector<ChronicleFork> others;
  };

struct FateCard final {
  bool                       off = true;
  std::string                title;
  std::string                text;
  std::vector<ChronicleFeat> feats;
  int64_t                    chapter = 1;
  int64_t                    day = 0;
  int64_t                    count = 0;
  };
