#include "islandflags.h"

#include "islandservices.h"

#include <charconv>
#include <cstdlib>
#include <string_view>

namespace island {

namespace {
template<class T>
bool number(std::string_view text, T& out) {
  const auto end = text.data()+text.size();
  return !text.empty() && std::from_chars(text.data(), end, out).ptr==end;
  }

// not from_chars: older C++ libraries have it for integers only
bool number(std::string_view text, float& out) {
  const std::string copy(text);
  char* end = nullptr;
  out = std::strtof(copy.c_str(), &end);
  return !copy.empty() && end==copy.c_str()+copy.size();
  }
}

const char* usage() {
  return "usage: engine-island [--data <dir>] [--frames N] [--capture <dir>] [--demo] [--time HH:MM]\n"
         "                     [--time-pace K] [--size WxH]\n"
         "  --data <dir>      the island (default: the build's island-data folder)\n"
         "  --frames N        quit after N frames\n"
         "  --capture <dir>   save every frame there as PNG, 30 frames a second of island time\n"
         "  --demo            play the built-in 13.5 s track instead of the keys and the mouse\n"
         "  --time HH:MM      the time of day (default 12:00; it stands still)\n"
         "  --time-pace K     let the time run, K island seconds a real second\n"
         "  --size WxH        the frame (default 1280x720)\n"
         "keys: WASD walk, Shift run, Space jump, mouse look, Esc quit\n";
  }

bool parseFlags(int argc, const char* const* argv, Flags& flags, std::string& error) {
  for(int i=1; i<argc; ++i) {
    const std::string_view arg = argv[i];
    auto value = [&](std::string_view& out) {
      if(i+1>=argc) {
        error = std::string(arg)+" needs a value";
        return false;
        }
      out = argv[++i];
      return true;
      };
    std::string_view v;
    if(arg=="--help" || arg=="-h") {
      flags.help = true;
      }
    else if(arg=="--demo") {
      flags.demo = true;
      }
    else if(arg=="--data") {
      if(!value(v))
        return false;
      flags.data = std::filesystem::path(v);
      }
    else if(arg=="--capture") {
      if(!value(v))
        return false;
      flags.capture = std::filesystem::path(v);
      }
    else if(arg=="--frames") {
      if(!value(v) || !number(v, flags.frames) || flags.frames==0) {
        error = "--frames needs a number above 0";
        return false;
        }
      }
    else if(arg=="--time") {
      if(!value(v))
        return false;
      flags.dayTime = parseDayTime(v);
      if(flags.dayTime<0) {
        error = "--time needs HH:MM";
        return false;
        }
      }
    else if(arg=="--time-pace") {
      if(!value(v) || !number(v, flags.pace) || flags.pace<0) {
        error = "--time-pace needs a number, 0 or above";
        return false;
        }
      }
    else if(arg=="--size") {
      if(!value(v))
        return false;
      const auto x = v.find('x');
      if(x==std::string_view::npos || !number(v.substr(0, x), flags.width) || !number(v.substr(x+1), flags.height) ||
         flags.width<64 || flags.height<64) {
        error = "--size needs WxH, 64 and more";
        return false;
        }
      }
    else {
      error = "unknown argument "+std::string(arg);
      return false;
      }
    }
  return true;
  }

}
