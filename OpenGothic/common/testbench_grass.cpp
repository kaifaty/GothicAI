#include "testbench_internal.h"

#include <chrono>

#include "graphics/grass/grassfield.h"
#include "graphics/worldview.h"
#include "world/world.h"

namespace testbench_detail {

namespace {

std::string grassFields(const GrassField* field) {
  if(field==nullptr)
    return "\"field\":false";
  const auto st = field->stats();
  return "\"field\":true,\"ready\":"+std::string(field->ready() ? "true" : "false")+
         ",\"cells\":"+std::to_string(st.cells)+",\"blades_stored\":"+std::to_string(st.stored)+
         ",\"blades_drawn\":"+std::to_string(st.drawn)+",\"triangles\":"+std::to_string(st.triangles)+
         ",\"buffer_bytes\":"+std::to_string(st.bytes)+",\"cells_waiting\":"+std::to_string(st.waiting);
  }

}

// The grass round the camera (reports/engine/2026-11-14-grass-system) is built over several frames on its own
// thread. `grass wait [timeout ms]` holds the scenario until the ring of the camera's place is whole (real time:
// the command is taken again every 30 ms); `grass stats` writes the counts of the last frame. Both write an event
bool grassCommand(const std::vector<std::string>& a,World& w) {
  if(a.size()<2 || a[0]!="grass" || (a[1]!="wait" && a[1]!="stats"))
    return false;
  const GrassField* field = w.view()!=nullptr ? w.view()->grassField() : nullptr;
  if(a[1]=="stats") {
    event("grass_stats",grassFields(field));
    return true;
    }
  using Clock = std::chrono::steady_clock;
  static Clock::time_point deadline;
  static int               polls = 0;
  const auto               now   = Clock::now();
  if(polls==0)
    deadline = now + std::chrono::milliseconds(int64_t(num(a,2,20000)));
  ++polls;
  // the first frames after a jump of the camera have not asked for the new place yet
  if(field!=nullptr && (polls<4 || !field->ready()) && now<deadline) {
    --bench.pc;
    bench.realWaitUntil = now + std::chrono::milliseconds(30);
    return true;
    }
  polls = 0;
  event("grass_ready",grassFields(field));
  if(field!=nullptr && !field->ready())
    arenaAbort("grass wait timed out before the ring was built");
  return true;
  }

}
