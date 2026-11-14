#pragma once

// The island program's command line (engine-island --help).

#include <cstdint>
#include <filesystem>
#include <string>

namespace island {

struct Flags {
  std::filesystem::path data;            // --data: the island-data target's folder
  uint64_t              frames  = 0;     // --frames: stop after so many frames (0: no limit)
  std::filesystem::path capture;         // --capture: a PNG of every frame there, 30 frames a second of island time
  std::filesystem::path script;          // --script: recorded controls
  std::filesystem::path events;          // --events: JSONL journal
  uint32_t              captureEvery = 1; // --capture-every: keep every first or second 30 Hz capture frame
  bool                  demo    = false; // --demo: the built-in track instead of the keys and the mouse
  int64_t               dayTime = 12ll*60*60*1000; // --time HH:MM
  float                 pace    = 0;     // --time-pace: island seconds per real second (0: the time stands)
  uint32_t              width   = 1280, height = 720; // --size WxH: the frame of --capture
  bool                  help    = false;
  };

// Fills flags from the command line; false with the reason on a mistake.
bool parseFlags(int argc, const char* const* argv, Flags& flags, std::string& error);
const char* usage();

}
