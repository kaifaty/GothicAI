#include "islandservices.h"

#include <charconv>
#include <cmath>

namespace island {

Arguments::Arguments(const std::filesystem::path& root)
  : path(root.string()), argv{"engine-island", "-g", path.c_str()}, cmd(int(argv.size()), argv.data()) {
  }

Options::Options(const std::filesystem::path& root) {
  settings.load((root/"system.ini").u16string(), (root/"user.ini").u16string());
  settings.setupOptions();
  settings.refreshPhysicsFlags();
  }

CameraTable::CameraTable() {
  cam.best_range     = 3;
  cam.min_range      = 2;
  cam.max_range      = 10;
  cam.best_elevation = 25;
  cam.max_elevation  = 89;
  cam.min_azimuth    = -90;
  cam.max_azimuth    = 90;
  cam.velo_trans     = 40;
  cam.velo_rot       = 2;
  cam.translate      = 1;
  cam.rotate         = 1;
  cam.collision      = 1;
  }

void Services::setClock(int64_t dayMs, float paceOfDay) {
  clock = double(((dayMs%DayMs)+DayMs)%DayMs);
  pace  = paceOfDay;
  }

void Services::tick(uint64_t dt) {
  ticks += dt;
  clock  = std::fmod(clock+double(dt)*double(pace), double(DayMs));
  }

int64_t Services::worldTime() const {
  return int64_t(clock);
  }

int64_t parseDayTime(std::string_view text) {
  int hours = 0, minutes = 0;
  const auto colon = text.find(':');
  const auto h = text.substr(0, colon);
  if(std::from_chars(h.data(), h.data()+h.size(), hours).ptr!=h.data()+h.size() || h.empty())
    return -1;
  if(colon!=std::string_view::npos) {
    const auto m = text.substr(colon+1);
    if(m.empty() || std::from_chars(m.data(), m.data()+m.size(), minutes).ptr!=m.data()+m.size())
      return -1;
    }
  if(hours<0 || hours>23 || minutes<0 || minutes>59)
    return -1;
  return (int64_t(hours)*60+minutes)*60*1000;
  }

}
