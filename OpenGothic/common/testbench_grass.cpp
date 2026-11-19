#include "testbench_internal.h"

#include <chrono>

#include "graphics/grass/grassdiag.h"
#include "graphics/grass/grassfield.h"
#include "graphics/grass/grasswaste.h"
#include "graphics/worldview.h"
#include "world/world.h"

namespace testbench_detail {

namespace {

std::string grassFields(const GrassField* field) {
  if(field==nullptr)
    return "\"field\":false";
  const auto st = field->stats();
  std::string waste;
  if(Grass::wasteOn()) {
    const auto& w = st.waste;
    waste = ",\"draw_us\":"+std::to_string(st.drawUs)+",\"waste\":{\"gone\":"+std::to_string(w.gone)+",\"gone_triangles\":"+std::to_string(w.goneTriangles)+
            ",\"out\":"+std::to_string(w.out)+",\"out_triangles\":"+std::to_string(w.outTriangles)+",\"squares\":[";
    for(size_t i=0; i<Grass::Waste::Subs; ++i)
      waste += std::string(i==0 ? "" : ",")+"{\"cm\":"+std::to_string(int(Grass::Waste::SubCm[i]))+
               ",\"blades\":"+std::to_string(w.subDrawn[i])+",\"triangles\":"+std::to_string(w.subTriangles[i])+
               ",\"draws\":"+std::to_string(w.subDraws[i])+"}";
    waste += "],\"bands\":[";
    auto sets = [](const size_t (&v)[Grass::Waste::LinkSets][Grass::Waste::Bands], size_t band) {
      std::string s = "[";
      for(size_t i=0; i<Grass::Waste::LinkSets; ++i)
        s += std::string(i==0 ? "" : ",")+std::to_string(v[i][band]);
      return s+"]";
      };
    for(size_t i=0; i<Grass::Waste::Bands; ++i)
      waste += std::string(i==0 ? "" : ",")+"{\"from_cm\":"+std::to_string(i==0 ? 0 : int(Grass::Waste::BandCm[i-1]))+
               ",\"blades\":"+std::to_string(w.bandBlades[i])+",\"fine\":"+std::to_string(w.bandFine[i])+
               ",\"shown\":"+std::to_string(w.bandShown[i])+",\"triangles\":"+sets(w.bandTriangles,i)+
               ",\"shown_triangles\":"+sets(w.bandShownTriangles,i)+"}";
    waste += "]}";
    }
  return "\"field\":true,\"ready\":"+std::string(field->ready() ? "true" : "false")+
         ",\"cells\":"+std::to_string(st.cells)+",\"blades_stored\":"+std::to_string(st.stored)+
         ",\"blades_drawn\":"+std::to_string(st.drawn)+",\"blades_fine\":"+std::to_string(st.fine)+
         ",\"triangles\":"+std::to_string(st.triangles)+",\"draws\":"+std::to_string(st.draws)+
         ",\"buffer_bytes\":"+std::to_string(st.bytes)+",\"cells_waiting\":"+std::to_string(st.waiting)+waste;
  }

}

// The grass round the camera (reports/engine/2026-11-14-grass-system) is built over several frames on its own
// thread. `grass wait [timeout ms]` holds the scenario until the ring of the camera's place is whole (real time:
// the command is taken again every 30 ms); `grass stats` writes the counts of the last frame. Both write an event.
// `grass parts <mask>` switches parts of the look off or paints the cover (Grass::Part); 0 gives the look back
bool grassCommand(const std::vector<std::string>& a,World& w) {
  if(a.size()<2 || a[0]!="grass" || (a[1]!="wait" && a[1]!="stats" && a[1]!="parts"))
    return false;
  if(a[1]=="parts") {
    Grass::benchPartsOff() = (a.size()>2 ? uint32_t(std::strtoul(a[2].c_str(),nullptr,10)) : 0u) & Grass::PartAll;
    return true;
    }
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
