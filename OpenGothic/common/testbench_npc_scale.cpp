#include "testbench_internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "game/gamescript.h"
#include "physics/dynamicworld.h"
#include "world/npcsimtier.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;
using namespace testbench_detail;

// Bench-only population probe: how the world simulates a large NPC population while the hero
// stands alone, far from everyone. `npcscale <count>` spreads peaceful civilians with their
// native routines over the map waypoints; `npcscale stats <label>` dumps a one-pass life
// histogram and the tier tick counters. No game path reads this. The day journal is deliberately
// not used here: its per-tick poll of every NPC would dominate the frame at this scale.

namespace {

constexpr std::string_view kDefaultSymbols =
  "BAU_904_BAUER,BAU_951_HILDA,BAU_950_LOBART,VLK_451_BUERGER,VLK_452_BUERGER";
constexpr float    kHeroExcludeCm = 15000.f; // 150 m around the hero stay empty at spawn
constexpr uint32_t kJitterSeed    = 0x5EEDu; // fixed: the same count spawns the same layout

std::vector<std::string> splitCsv(std::string_view s) {
  std::vector<std::string> ret;
  for(size_t at=0; at<s.size();) {
    const auto end   = s.find(',',at);
    const auto piece = s.substr(at,end==std::string_view::npos ? s.size() : end-at);
    if(!piece.empty())
      ret.emplace_back(piece);
    at = end==std::string_view::npos ? s.size() : end+1;
    }
  return ret;
  }

float jitterUnit(uint32_t& state) {
  state = state*1664525u + 1013904223u;
  return float(state>>8)/float(1u<<24)*2.f - 1.f;
  }

bool spawnScale(World& w, Npc& pl, const std::vector<std::string>& a) {
  const int count = int(num(a,1,0));
  if(count<=0) {
    say("npcscale: usage: npcscale <count> [spread cm 2000] [symbols=CSV]");
    return true;
    }
  const float spread = num(a,2,2000);
  std::string_view symbols = kDefaultSymbols;
  for(size_t i=3; i<a.size(); ++i)
    if(a[i].starts_with("symbols="))
      symbols = std::string_view(a[i]).substr(8);
  std::vector<size_t> ids;
  for(const auto& name:splitCsv(symbols)) {
    const size_t id = w.script().findSymbolIndex(name);
    if(id==size_t(-1))
      say("npcscale: no npc symbol "+name); else
      ids.push_back(id);
    }
  if(ids.empty()) {
    say("npcscale: no valid npc symbols");
    return true;
    }
  const auto hero = pl.position();
  std::vector<Vec3> points;
  w.forEachWayPoint([&](const WayPoint& wp) {
    if(wp.underWater)
      return;
    const auto p = wp.groundPos.length()>0 ? wp.groundPos : wp.pos;
    if((p-hero).length()<kHeroExcludeCm)
      return;
    points.push_back(p);
    });
  if(points.empty()) {
    say("npcscale: no waypoints to spawn at");
    return true;
    }
  const auto t0 = std::chrono::steady_clock::now();
  uint32_t rng = kJitterSeed ^ uint32_t(count);
  size_t spawned = 0;
  for(int i=0; i<count; ++i) {
    auto p = points[size_t(i)%points.size()];
    p.x += jitterUnit(rng)*spread*0.5f;
    p.z += jitterUnit(rng)*spread*0.5f;
    DynamicWorld::RayLandResult r;
    for(float y=p.y+1500; y>p.y-1500 && !r.hasCol; y-=400)
      r = w.physic()->ray({p.x,y,p.z},{p.x,y-450,p.z});
    if(r.hasCol)
      p.y = r.v.y;
    if(w.addNpc(ids[size_t(i)%ids.size()],p)!=nullptr)
      ++spawned;
    }
  const double ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
  char buf[256];
  std::snprintf(buf,sizeof(buf),"npcscale: spawned %zu/%d in %.0f ms, %zu waypoints, spread %.0f cm",
                spawned,count,ms,points.size(),double(spread));
  say(buf);
  event("npc_scale_spawn","\"count\":"+std::to_string(count)+",\"spawned\":"+std::to_string(spawned)+
        ",\"ms\":"+std::to_string(ms)+",\"waypoints\":"+std::to_string(points.size()));
  return true;
  }

void statsScale(World& w, const std::string& label) {
  std::map<std::string,size_t> zs;
  std::map<int,size_t>         routine;
  size_t dead=0, down=0;
  for(uint32_t i=0; i<w.npcCount(); ++i) {
    const auto* npc = w.npcById(i);
    if(npc==nullptr)
      continue;
    const size_t state = npc->currentState().ptr;
    std::string name = (state==size_t(-1) || state==0) ? "-" : std::string(w.rules().symbolName(state));
    if(name.empty())
      name = std::to_string(state);
    ++zs[name];
    ++routine[npc->currentRoutineIndex()];
    dead += npc->isDead() ? 1:0;
    down += npc->isDown() ? 1:0;
    }
  const auto& tiers = w.npcSimTierStats();
  const auto  t     = w.time();
  std::ofstream out(outDir+"/npc-scale-"+label+".json");
  out << "{\"label\":" << jsonStr(label)
      << ",\"npcs\":" << w.npcCount()
      << ",\"world_ms\":" << t.toInt()
      << ",\"day\":" << t.day() << ",\"hour\":" << t.hour() << ",\"minute\":" << t.minute()
      << ",\"dead\":" << dead << ",\"down\":" << down
      << ",\"tiers\":\"" << tiers.probeFields() << "\""
      << ",\"zs\":{";
  bool first=true;
  for(const auto& [name,n]:zs) {
    out << (first ? "":",") << jsonStr(name) << ":" << n;
    first=false;
    }
  out << "},\"routine\":{";
  first=true;
  for(const auto& [idx,n]:routine) {
    out << (first ? "":",") << "\"" << idx << "\":" << n;
    first=false;
    }
  out << "}}\n";
  out.close();
  const auto top = std::max_element(zs.begin(),zs.end(),
                                    [](const auto& a, const auto& b){ return a.second<b.second; });
  char buf[256];
  std::snprintf(buf,sizeof(buf),"npcscale stats %s: %u npcs, dead %zu, down %zu, top %s=%zu -> npc-scale-%s.json",
                label.c_str(),w.npcCount(),dead,down,
                top==zs.end() ? "-":top->first.c_str(),top==zs.end() ? 0:top->second,label.c_str());
  say(buf);
  }

}

namespace testbench_detail {

bool npcScaleCommand(const std::vector<std::string>& a, World& w, Npc& pl) {
  if(a.size()>1 && a[1]=="stats") {
    std::string label = a.size()>2 ? a[2] : "stats";
    for(char& c:label)
      if(!(std::isalnum(uint8_t(c)) || c=='-' || c=='_'))
        c='-';
    statsScale(w,label);
    return true;
    }
  return spawnScale(w,pl,a);
  }

}
