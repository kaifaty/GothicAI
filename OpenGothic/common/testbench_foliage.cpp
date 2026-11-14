#include "testbench_internal.h"

#include <cmath>

#include "settings.h"
#include "physics/cloth.h"
#include "physics/dynamicworld.h"
#include "physics/worldwind.h"
#include "graphics/foliagefront.h"
#include "graphics/worldview.h"
#include "world/world.h"

namespace testbench_detail {

// Living foliage (reports/engine/2026-11-14-living-foliage): the clips show the world's own wind, which is a pure
// function of the world's tick (WorldWind::sample). Its gust is .5+.5*sin(phase*.31), phase = tick*2pi/period:
// `foliage wait-gust <lead ms>` waits until the next peak of it is <lead> ms ahead, `foliage wait-lull <lead ms>`
// the same for the middle of the calm between two gusts. Nothing in the world is changed.
// The gust runs over the world as a front (graphics/foliagefront.h): the wait is counted for the place at
// <z cm>, the third number; without it for the camera of the last frame
bool foliageCommand(const std::vector<std::string>& a,World& w) {
  if(a.size()<2 || a[0]!="foliage" || (a[1]!="wait-gust" && a[1]!="wait-lull"))
    return false;
  const double period = double(uint64_t((::Settings::getF("ENGINE","zWindCycleTime") +
                                        ::Settings::getF("ENGINE","zWindCycleTimeVar"))*1000.f));
  if(!(period>0)) {
    arenaAbort("foliage: the world has no wind period");
    return true;
    }
  const double cycle = period/0.31;                 // ms from one gust to the next
  const double at    = a[1]=="wait-gust" ? 0.25 : 0.75;
  const double lead  = double(num(a,2,4000));
  const double now   = double(w.tickCount());
  double       delay = 0; // ms the front takes from z=0 to the place
  if(auto* view = w.view()) {
    const auto& scene = view->sceneGlobals();
    if(scene.zWindEnabled && !scene.legacyWind && scene.windScale>0.f)
      delay = double(a.size()>3 ? float(num(a,3,0)) : scene.camPos().z)*1000.0/FoliageFront::Speed;
    }
  const double k     = std::ceil((now+lead-delay)/cycle - at);
  const double peak  = cycle*(at+k) + delay;
  const auto   wait  = uint64_t(std::max(0.0, peak-lead-now));
  bench.waitUntil = bench.time + wait;

  const float scale = w.physic()->cloth().windScale();
  const auto  speed = [&](double tick) {
    const auto v = WorldWind::sample(uint64_t(std::max(0.0,tick-delay)),uint64_t(period),true,scale);
    return std::sqrt(v.x*v.x + v.z*v.z);
    };
  event("foliage_wait","\"what\":"+jsonStr(a[1])+",\"lead_ms\":"+std::to_string(uint64_t(lead))+
        ",\"wait_ms\":"+std::to_string(wait)+",\"tick\":"+std::to_string(uint64_t(now))+
        ",\"at_tick\":"+std::to_string(uint64_t(peak))+",\"cycle_ms\":"+std::to_string(uint64_t(cycle))+
        ",\"front_delay_ms\":"+std::to_string(int64_t(delay))+
        ",\"wind_scale\":"+ragdollNumber(scale)+",\"speed_now_ms\":"+ragdollNumber(speed(now))+
        ",\"speed_at_ms\":"+ragdollNumber(speed(peak)));
  return true;
  }

}
