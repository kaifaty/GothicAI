#include "npcdayjournal.h"

#include <Tempest/Log>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "game/gamescript.h"
#include "game/inventory.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/npcsimtier.h"
#include "world/waypoint.h"
#include "world/world.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Entry {
  std::string key, symbol;
  size_t      state    = size_t(-1);
  int         ta       = -2;
  bool        dead     = false;
  uint64_t    lastSeen = 0;
  };

struct Journal {
  bool                                     active = false;
  NpcDayJournal::Options                   opt;
  std::FILE*                               samples = nullptr;
  std::FILE*                               events  = nullptr;
  std::FILE*                               cost    = nullptr;
  std::unordered_map<const Npc*,Entry>     entries;
  std::map<std::string,uint32_t>           nextOrdinal;
  int64_t                                  startMs = 0, endMs = 0, nextSampleMs = 0;
  uint64_t                                 tick = 0, sampleCount = 0, rowCount = 0, eventCount = 0;
  uint64_t                                 inserted = 0, removed = 0, deaths = 0;
  Clock::time_point                        wallStart;
  NpcSimTiers::Stats                       tiersAtStart, tiersAtSample;
  std::string                              header;
  };

Journal& journal() { static Journal j; return j; }

const char* actionName(uint32_t act) {
  static const char* names[] = {
    "None","LookAtNpc","StopLookAt","RemoveWeapon","TurnToNpc","GoToNpc","GoToNextFp","GoToPoint",
    "StartState","PlayAnim","PlayAnimBs","Wait","StandUp","StandUpQuick","EquipArmor","EquipBestArmor",
    "EquipMelee","EquipRange","UseMob","UseItem","UseItemToState","Teleport","DrawWeaponMelee",
    "DrawWeaponRange","DrawSpell","Attack","Flee","Dodge","UnEquipWeapons","UnEquipArmor","Output",
    "OutputSvm","OutputSvmOverlay","ProcessInfo","StopProcessInfo","ContinueRoutine","AlignToFp",
    "AlignToWp","SetNpcsToState","SetWalkMode","FinishingMove","DrawWeapon","TakeItem","GotoItem",
    "PointAtNpc","PointAt","StopPointAt","PrintScreen","LookAt","WhirlToNpc","TurnAway",
    };
  return act<sizeof(names)/sizeof(names[0]) ? names[act] : "?";
  }

std::string clean(std::string_view s) {
  std::string ret(s.empty() ? std::string_view("-") : s);
  for(auto& c:ret)
    if(c==',' || c=='\n' || c=='\r' || c=='"')
      c = '_';
  return ret;
  }

std::string symbolName(World& w, size_t symbol) {
  if(symbol==size_t(-1))
    return "-";
  auto* sym = w.script().findSymbol(symbol);
  return sym!=nullptr ? clean(sym->name()) : std::to_string(symbol);
  }

uint32_t hash32(std::string_view s) {
  uint32_t h = 2166136261u;
  for(char c:s) {
    h ^= uint8_t(c);
    h *= 16777619u;
    }
  return h;
  }

std::pair<uint64_t,uint64_t> inventoryHash(const Npc& npc) {
  std::map<size_t,size_t> counts;
  for(auto it=npc.inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it)
    counts[it->clsId()] += it.count();
  uint64_t h = 1469598103934665603ull, total = 0;
  auto mix = [&h](uint64_t v) {
    for(int i=0; i<8; ++i) {
      h ^= (v>>(i*8)) & 0xff;
      h *= 1099511628211ull;
      }
    };
  for(auto& [cls,count]:counts) {
    mix(cls);
    mix(count);
    total += count;
    }
  return {h,total};
  }

double relativeMinutes(const Journal& j, int64_t ms) {
  return double(ms-j.startMs)/60000.0;
  }

void event(World& w, Journal& j, const Npc& npc, const Entry& e, const char* kind, std::string_view value) {
  if(j.events==nullptr)
    return;
  const auto p = npc.position();
  std::fprintf(j.events,"%.4f,%s,%s,%s,%.1f,%.1f,%.1f\n",relativeMinutes(j,w.time().toInt()),e.key.c_str(),kind,
               clean(value).c_str(),double(p.x),double(p.y),double(p.z));
  ++j.eventCount;
  }

std::string routineText(World& w, const Npc& npc, int index) {
  std::string ret = "-";
  int i = 0;
  npc.forEachRoutine([&](const Npc::RoutineInfo& r) {
    if(i++!=index)
      return;
    char buf[48];
    std::snprintf(buf,sizeof(buf),"%02lld:%02lld-%02lld:%02lld",(long long)r.start.hour(),(long long)r.start.minute(),
                  (long long)r.end.hour(),(long long)r.end.minute());
    ret = std::to_string(index)+" "+buf+" "+symbolName(w,r.state.ptr)+" "+clean(r.point);
    });
  return ret;
  }

void writeSample(World& w, Journal& j, int64_t scheduledMs) {
  if(j.samples==nullptr)
    return;
  std::vector<std::pair<const std::string*,Npc*>> order;
  order.reserve(w.npcCount());
  for(uint32_t i=0; i<w.npcCount(); ++i) {
    auto* npc = w.npcById(i);
    auto  it  = j.entries.find(npc);
    if(it!=j.entries.end())
      order.emplace_back(&it->second.key,npc);
    }
  std::sort(order.begin(),order.end(),[](const auto& a, const auto& b) { return *a.first<*b.first; });
  const double t = relativeMinutes(j,scheduledMs);
  const double at = relativeMinutes(j,w.time().toInt());
  for(auto& [key,npc]:order) {
    const auto& e    = j.entries[npc];
    const auto  p    = npc->position();
    const auto* wp   = w.findWayPoint(p);
    const auto* head = npc->aiQueueFront();
    std::string headText = "-";
    if(head!=nullptr) {
      headText = actionName(uint32_t(head->act));
      if(head->point!=nullptr)
        headText += " "+clean(head->point->name);
      else if(head->target!=nullptr)
        headText += " "+symbolName(w,size_t(head->target->instanceSymbol()));
      else if(head->func.isValid())
        headText += " "+symbolName(w,head->func.ptr);
      }
    const auto  inv   = inventoryHash(*npc);
    const int   ta    = npc->currentRoutineIndex();
    const auto* mob   = npc->interactive();
    std::fprintf(j.samples,
                 "%.0f,%.4f,%s,%s,%d,%d,%d,%s,%s,%llu,%.1f,%.1f,%.1f,%s,%.1f,%zu,%s,%u,%016llx,%llu,%s,%s,%s,%s\n",
                 t,at,key->c_str(),e.symbol.c_str(),npc->isDead() ? 0 : 1,
                 npc->attribute(ATR_HITPOINTS),npc->attribute(ATR_HITPOINTSMAX),
                 routineText(w,*npc,ta).c_str(),symbolName(w,npc->currentState().ptr).c_str(),
                 (unsigned long long)npc->stateTime(),double(p.x),double(p.y),double(p.z),
                 wp!=nullptr ? clean(wp->name).c_str() : "-",
                 wp!=nullptr ? double((wp->position()-p).length()) : -1.0,
                 npc->aiQueueSize(),headText.c_str(),unsigned(npc->bodyStateMasked() & BS_MAX),
                 (unsigned long long)inv.first,(unsigned long long)inv.second,
                 mob!=nullptr ? clean(mob->schemeName()).c_str() : "-",
                 NpcSimTiers::name(npc->simTier()),
                 npc->processPolicy()==NpcProcessPolicy::Player   ? "player" :
                 npc->processPolicy()==NpcProcessPolicy::AiNormal ? "normal" :
                 npc->processPolicy()==NpcProcessPolicy::AiFar    ? "far" : "far2",
                 npc->isPlayer() ? "hero" : "-");
    ++j.rowCount;
    }
  std::fflush(j.samples);
  if(j.events!=nullptr)
    std::fflush(j.events);
  if(j.cost!=nullptr) {
    const auto& s    = w.npcSimTierStats();
    const auto& prev = j.tiersAtSample;
    const double wall = std::chrono::duration<double>(Clock::now()-j.wallStart).count();
    // NPCs near the hero (30/60 m: the AiNormal and AiFar policy radii) say how much of the
    // population the hero's presence can touch at this sample.
    size_t near30 = 0, near60 = 0;
    if(const auto* hero = w.player()) {
      for(uint32_t i=0; i<w.npcCount(); ++i) {
        const auto* npc = w.npcById(i);
        if(npc==hero)
          continue;
        const float d = (npc->position()-hero->position()).quadLength();
        near30 += d<3000.f*3000.f;
        near60 += d<6000.f*6000.f;
        }
      }
    std::fprintf(j.cost,"%.0f,%.3f,%zu,%zu,%llu,%zu,%zu,%llu,%llu,%.6f,%.6f\n",t,wall,near30,near60,
                 (unsigned long long)(s.ticks-prev.ticks),s.full,s.simplified,
                 (unsigned long long)(s.toFull-prev.toFull),(unsigned long long)(s.toSimplified-prev.toSimplified),
                 s.fullTickMsTotal-prev.fullTickMsTotal,s.simplifiedTickMsTotal-prev.simplifiedTickMsTotal);
    std::fflush(j.cost);
    j.tiersAtSample = s;
    }
  ++j.sampleCount;
  }

Entry& track(World& w, Journal& j, Npc& npc, bool initial) {
  auto it = j.entries.find(&npc);
  if(it!=j.entries.end())
    return it->second;
  Entry e;
  e.symbol = symbolName(w,size_t(npc.instanceSymbol()));
  const auto ord = j.nextOrdinal[e.symbol]++;
  e.key   = e.symbol+"#"+std::to_string(ord);
  e.state = npc.currentState().ptr;
  e.ta    = npc.currentRoutineIndex();
  e.dead  = npc.isDead();
  auto& ret = j.entries.emplace(&npc,std::move(e)).first->second;
  if(j.opt.isolate)
    w.script().isolateRandom(npc,j.opt.seed,hash32(ret.key));
  if(!initial) {
    ++j.inserted;
    event(w,j,npc,ret,"insert",symbolName(w,ret.state));
    }
  return ret;
  }

void writeHeader(World& w, Journal& j, const std::string& summary) {
  const std::string path = j.opt.directory+"/npc-day.json";
  if(auto* f = std::fopen(path.c_str(),"w")) {
    std::fprintf(f,"{%s%s}\n",j.header.c_str(),summary.c_str());
    std::fclose(f);
    }
  (void)w;
  }

void finish(World& w, Journal& j, const char* reason) {
  const auto& s = w.npcSimTierStats();
  const double wall = std::chrono::duration<double>(Clock::now()-j.wallStart).count();
  const double fullMs = s.fullTickMsTotal-j.tiersAtStart.fullTickMsTotal;
  const double simpMs = s.simplifiedTickMsTotal-j.tiersAtStart.simplifiedTickMsTotal;
  const uint64_t ticks = s.ticks-j.tiersAtStart.ticks;
  char buf[1024];
  std::snprintf(buf,sizeof(buf),
                ",\"end_reason\":\"%s\",\"end_world_ms\":%lld,\"elapsed_game_min\":%.4f,\"wall_s\":%.3f,"
                "\"samples\":%llu,\"rows\":%llu,\"events\":%llu,\"inserted\":%llu,\"removed\":%llu,\"deaths\":%llu,"
                "\"world_ticks\":%llu,\"npc_full_tick_ms\":%.3f,\"npc_simplified_tick_ms\":%.3f,"
                "\"npc_to_full\":%llu,\"npc_to_simplified\":%llu,\"npc_full_end\":%zu,\"npc_simplified_end\":%zu",
                reason,(long long)w.time().toInt(),relativeMinutes(j,w.time().toInt()),wall,
                (unsigned long long)j.sampleCount,(unsigned long long)j.rowCount,(unsigned long long)j.eventCount,
                (unsigned long long)j.inserted,(unsigned long long)j.removed,(unsigned long long)j.deaths,
                (unsigned long long)ticks,fullMs,simpMs,
                (unsigned long long)(s.toFull-j.tiersAtStart.toFull),
                (unsigned long long)(s.toSimplified-j.tiersAtStart.toSimplified),s.full,s.simplified);
  writeHeader(w,j,buf);
  for(auto** f:{&j.samples,&j.events,&j.cost})
    if(*f!=nullptr) {
      std::fclose(*f);
      *f = nullptr;
      }
  Tempest::Log::i("npcday: ",reason,", ",j.sampleCount," samples, ",j.rowCount," rows, ",j.eventCount," events, ",wall," s");
  j.active = false;
  j.entries.clear();
  j.nextOrdinal.clear();
  }

}

bool NpcDayJournal::running() {
  return journal().active;
  }

bool NpcDayJournal::start(World& w, const Options& options, std::string& error) {
  auto& j = journal();
  if(j.active) {
    error = "a journal is already running";
    return false;
    }
  if(options.period==0 || options.minutes==0) {
    error = "period and duration must be positive";
    return false;
    }
  j = Journal();
  j.opt = options;
  const std::string base = options.directory+"/";
  j.samples = std::fopen((base+"npc-day.csv").c_str(),"w");
  j.events  = std::fopen((base+"npc-day-events.csv").c_str(),"w");
  j.cost    = std::fopen((base+"npc-day-cost.csv").c_str(),"w");
  if(j.samples==nullptr || j.events==nullptr || j.cost==nullptr) {
    for(auto** f:{&j.samples,&j.events,&j.cost})
      if(*f!=nullptr) {
        std::fclose(*f);
        *f = nullptr;
        }
    error = "cannot write the journal in "+options.directory;
    return false;
    }
  std::fprintf(j.samples,"t_min,at_min,key,symbol,alive,hp,hp_max,ta,zs,zs_ms,x,y,z,wp,wp_dist,queue,head,body,"
                         "inv_hash,inv_items,mob,tier,policy,hero\n");
  std::fprintf(j.events,"t_min,key,kind,value,x,y,z\n");
  std::fprintf(j.cost,"t_min,wall_s,near_hero_30m,near_hero_60m,world_ticks,npc_full,npc_simplified,to_full,"
                      "to_simplified,full_tick_ms,simplified_tick_ms\n");
  j.active       = true;
  j.startMs      = w.time().toInt();
  j.endMs        = j.startMs+int64_t(options.minutes)*60000;
  j.nextSampleMs = j.startMs;
  j.wallStart    = Clock::now();
  j.tiersAtStart = w.npcSimTierStats();
  j.tiersAtSample = j.tiersAtStart;

  // Keys: the symbol plus an ordinal among NPCs of the same symbol, ordered
  // by their position at the start, then by their place in the world array.
  struct Start { Npc* npc; std::string symbol; float x, z, y; uint32_t index; };
  std::vector<Start> all;
  for(uint32_t i=0; i<w.npcCount(); ++i) {
    auto* npc = w.npcById(i);
    const auto p = npc->position();
    all.push_back({npc,symbolName(w,size_t(npc->instanceSymbol())),p.x,p.z,p.y,i});
    }
  std::sort(all.begin(),all.end(),[](const Start& a, const Start& b) {
    return std::tie(a.symbol,a.x,a.z,a.y,a.index)<std::tie(b.symbol,b.x,b.z,b.y,b.index);
    });
  for(auto& s:all)
    track(w,j,*s.npc,true);

  const auto* hero = w.player();
  const auto  hp   = hero!=nullptr ? hero->position() : Tempest::Vec3();
  char buf[768];
  std::snprintf(buf,sizeof(buf),
                "\"world\":\"%s\",\"tier_mode\":\"%s\",\"streaming\":%s,\"seed\":%u,\"isolate\":%s,"
                "\"minutes\":%llu,\"period_min\":%u,\"start_world_ms\":%lld,\"start_day\":%lld,"
                "\"start_hour\":%lld,\"start_minute\":%lld,\"npcs\":%zu,\"hero\":[%.1f,%.1f,%.1f]",
                clean(w.name()).c_str(),NpcSimTiers::name(NpcSimTiers::mode()),
                w.chunkResidency()!=nullptr ? "true" : "false",options.seed,options.isolate ? "true" : "false",
                (unsigned long long)options.minutes,options.period,(long long)j.startMs,
                (long long)w.time().day(),(long long)w.time().hour(),(long long)w.time().minute(),
                all.size(),double(hp.x),double(hp.y),double(hp.z));
  j.header = buf;
  writeHeader(w,j,"");
  writeSample(w,j,j.nextSampleMs);
  j.nextSampleMs += int64_t(options.period)*60000;
  Tempest::Log::i("npcday: ",all.size()," npcs, ",options.minutes," game minutes, every ",options.period,
                  ", tier ",NpcSimTiers::name(NpcSimTiers::mode()));
  return true;
  }

void NpcDayJournal::tick(World& w) {
  auto& j = journal();
  if(!j.active)
    return;
  ++j.tick;
  for(uint32_t i=0; i<w.npcCount(); ++i) {
    auto* npc = w.npcById(i);
    auto& e   = track(w,j,*npc,false);
    e.lastSeen = j.tick;
    const size_t state = npc->currentState().ptr;
    if(state!=e.state) {
      if(e.state!=size_t(-1))
        event(w,j,*npc,e,"zs_end",symbolName(w,e.state));
      if(state!=size_t(-1))
        event(w,j,*npc,e,"zs_start",symbolName(w,state));
      e.state = state;
      }
    const int ta = npc->currentRoutineIndex();
    if(ta!=e.ta) {
      event(w,j,*npc,e,"ta",routineText(w,*npc,ta));
      e.ta = ta;
      }
    const bool dead = npc->isDead();
    if(dead!=e.dead) {
      if(dead)
        ++j.deaths;
      event(w,j,*npc,e,dead ? "death" : "revive",std::to_string(npc->attribute(ATR_HITPOINTS)));
      e.dead = dead;
      }
    }
  for(auto it=j.entries.begin(); it!=j.entries.end();) {
    if(it->second.lastSeen==j.tick) {
      ++it;
      continue;
      }
    // The NPC has left the world array; its object may already be gone.
    if(j.events!=nullptr) {
      std::fprintf(j.events,"%.4f,%s,remove,-,0,0,0\n",relativeMinutes(j,w.time().toInt()),it->second.key.c_str());
      ++j.eventCount;
      }
    ++j.removed;
    it = j.entries.erase(it);
    }
  const int64_t now = w.time().toInt();
  while(j.active && now>=j.nextSampleMs && j.nextSampleMs<=j.endMs) {
    writeSample(w,j,j.nextSampleMs);
    j.nextSampleMs += int64_t(j.opt.period)*60000;
    }
  if(j.active && now>=j.endMs)
    finish(w,j,"done");
  }

void NpcDayJournal::stop(World& w, const char* reason) {
  auto& j = journal();
  if(j.active)
    finish(w,j,reason);
  }
