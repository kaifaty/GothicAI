#include "gamescript_startupfloor.h"

#include <Tempest/Log>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unordered_set>

#include "game/gamescript.h"
#include "physics/dynamicworld.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "loadprobe.h"

namespace {

// GameScript::fixNpcPosition tests points up to distBias + 800 cm away: 150 at insertion, 100 in the final placement.
constexpr float FixReach   = 150.f + 800.f;
constexpr float PlaceReach = 100.f + 800.f;

enum class Mode : uint8_t { Off, Exact, Loose };

// OPENGOTHIC_STARTUP_FLOOR_DEFER: unset or `loose` - the default; `exact` - with the final placement's barriers;
// `0` - the fix at insertion, as before.
Mode mode() {
  static const Mode value = [] {
    const char* env = std::getenv("OPENGOTHIC_STARTUP_FLOOR_DEFER");
    if(env==nullptr || std::strcmp(env,"loose")==0 || std::strcmp(env,"1")==0)
      return Mode::Loose;
    if(std::strcmp(env,"0")==0)
      return Mode::Off;
    if(std::strcmp(env,"exact")==0)
      return Mode::Exact;
    Tempest::Log::e("OPENGOTHIC_STARTUP_FLOOR_DEFER=",env,": expected 0, exact or loose; using loose");
    return Mode::Loose;
    }();
  return value;
  }

// Not exact (the default): the final placement's barriers are skipped, so an NPC placed earlier may see a pending
// neighbour at its insert position instead of its fixed one. On NEWWORLD 21 of 1053 NPCs start 0.5-2 m apart,
// mostly monster packs; the placement runs 1.1-1.3 s faster (reports/engine/2026-11-03-streaming-optimisation).
bool loosePlacement() {
  return mode()==Mode::Loose;
  }

float distXZ(const Tempest::Vec3& a, const Tempest::Vec3& b) {
  const float dx = a.x-b.x, dz = a.z-b.z;
  return std::sqrt(dx*dx+dz*dz);
  }

// Externals that neither read nor change any NPC's position, rotation, point, body or the collision world, and
// change nothing a fix reads: script data, talents, attitudes, guilds, inventory counts, routine tables,
// perception settings, log and info.
constexpr const char* noneExternals[] = {
  "hlp_random", "hlp_isvalidnpc", "hlp_isvaliditem", "hlp_isitem", "hlp_getnpc", "hlp_getinstanceid",
  "wld_getday", "wld_setguildattitude", "wld_getguildattitude", "wld_exchangeguildattitudes", "wld_istime",
  "wld_israining", "wld_assignroomtoguild",
  // mob routines are applied after the NPCs (WorldObjects::resetPositionToTA); world items are C_Item bodies,
  // which neither the fix's ray nor its capsule query hits
  "wld_setmobroutine", "wld_insertitem",
  "npc_isinstate", "npc_isinroutine", "npc_wasinstate", "npc_isdead", "npc_knowsinfo",
  "npc_settalentskill", "npc_gettalentskill", "npc_settalentvalue", "npc_gettalentvalue",
  "npc_setrefusetalk", "npc_refusetalk", "npc_hasitems", "npc_hasspell", "npc_getinvitem", "npc_getinvitembyslot",
  "npc_hasequippedarmor", "npc_setperctime", "npc_percenable", "npc_percdisable", "npc_isplayer",
  "npc_getstatetime", "npc_setstatetime", "npc_getequippedmeleeweapon", "npc_getequippedrangedweapon",
  "npc_getequippedarmor", "npc_hasequippedweapon", "npc_hasequippedmeleeweapon", "npc_hasequippedrangedweapon",
  "npc_getactivespell", "npc_getactivespellisscroll", "npc_getactivespellcat", "npc_setactivespellinfo",
  "npc_getactivespelllevel", "npc_settrueguild", "npc_gettrueguild", "npc_getattitude", "npc_getpermattitude",
  "npc_setattitude", "npc_settempattitude", "npc_hasbodyflag", "npc_getlasthitspellid", "npc_getlasthitspellcat",
  "npc_checkinfo", "npc_isinfightmode", "npc_getreadiedweapon", "npc_hasreadiedweapon",
  "npc_hasreadiedmeleeweapon", "npc_hasreadiedrangedweapon", "npc_hasrangedweaponwithammo",
  "npc_isdrawingspell", "npc_isdrawingweapon", "npc_isincutscene", "npc_gettarget",
  "ta_min", "createinvitem", "createinvitems", "perc_setrange", "mob_hasitems",
  "log_createtopic", "log_settopicstatus", "log_addentry", "info_addchoice", "info_clearchoices",
  "infomanager_hasfinished", "snd_play", "game_initgerman", "game_initenglish",
  };

// Externals that read or change only the NPCs they are given: visual and body (collision size), equipment,
// routine exchange, distances, points, sight and rooms by position, attributes (a death disables the body).
constexpr const char* argsExternals[] = {
  "npc_changeattribute",
  "mdl_setvisual", "mdl_setvisualbody", "mdl_setmodelfatness", "mdl_applyoverlaymds", "mdl_applyoverlaymdstimed",
  "mdl_removeoverlaymds", "mdl_setmodelscale", "mdl_startfaceani", "mdl_applyrandomani",
  "mdl_applyrandomanifreq", "mdl_applyrandomfaceani",
  "npc_settofightmode", "npc_settofistmode",
  "equipitem", "npc_removeinvitem", "npc_removeinvitems", "npc_clearinventory", "npc_exchangeroutine",
  "npc_getdisttowp", "npc_getdisttonpc", "npc_getnearestwp", "npc_getnextwp", "npc_isonfp", "npc_getheighttonpc",
  "npc_getdisttoitem", "npc_getheighttoitem", "npc_getdisttoplayer", "npc_canseenpc", "npc_canseenpcfreelos",
  "npc_canseeitem", "npc_getportalguild", "npc_isinplayersroom", "npc_getbodystate", "npc_getlookattarget",
  "wld_getmobstate", "snd_play3d",
  };

}

StartupFloor::StartupFloor(GameScript& owner):owner(owner) {
  }

StartupFloor::Access StartupFloor::classify(std::string_view external) {
  for(auto n:noneExternals)
    if(external==n)
      return Access::None;
  for(auto n:argsExternals)
    if(external==n)
      return Access::Args;
  if(external=="wld_insertnpc")
    return Access::None; // handled by inserted()
  return Access::All;
  }

bool StartupFloor::enabled() {
  return mode()!=Mode::Off;
  }

size_t StartupFloor::registerExternal(std::string_view name, Access a) {
  ExternalStat st;
  st.name   = std::string(name);
  st.access = a;
  externals.push_back(std::move(st));
  return externals.size()-1;
  }

void StartupFloor::begin() {
  pending.clear();
  inserts = deferred = cleared = 0;
  std::fill(std::begin(byReason),std::end(byReason),0);
  for(auto& e:externals)
    e = ExternalStat{e.name,e.access};
  window.clear();
  globalReads = 0;
  isActive = enabled();
  }

void StartupFloor::end() {
  if(!isActive)
    return;
  // Not moved by the placement (removed or invalid): it keeps its insert-time fix.
  while(!pending.empty())
    materialise(*pending.front().npc,R_End);
  if(LoadProbe::enabled())
    report();
  isActive = false;
  }

void StartupFloor::inserted(Npc& npc) {
  ++inserts;
  if(LoadProbe::enabled())
    window[&npc] = Window{};
  Entry e;
  e.npc    = &npc;
  e.anchor = npc.position();
  e.seq    = npc.creationSeq();
  pending.push_back(e);
  npc.setStartupFloorPending(true);
  if(npc.isDead()) {
    // A dead body may be kept where it lies by the final placement.
    materialise(npc,R_Dead);
    return;
    }
  ++deferred;
  }

void StartupFloor::external(size_t id, Npc* const* args, size_t count) {
  auto& st = externals[id];
  if(LoadProbe::enabled()) {
    ++st.calls;
    if(!pending.empty())
      ++st.callsPending;
    for(size_t i=0; i<count; ++i) {
      auto it = window.find(args[i]);
      if(it==window.end() || it->second.placed)
        continue;
      if(st.access==Access::None)
        ++it->second.dataCalls; else
        ++it->second.reads;
      if(args[i]->startupFloorPending())
        ++st.pendingArgs;
      }
    if(st.access==Access::All)
      ++globalReads;
    }
  if(pending.empty() || st.access==Access::None)
    return;
  if(st.access==Access::All) {
    st.materialised += pending.size();
    while(!pending.empty())
      materialise(*pending.front().npc,R_External);
    return;
    }
  // An argument's own pending fix runs first, then those of newer NPCs that saw its body: the call may change it.
  for(size_t i=0; i<count; ++i) {
    Npc* npc = args[i];
    if(npc==nullptr)
      continue;
    const size_t before = pending.size();
    if(npc->startupFloorPending())
      materialise(*npc,R_External);
    materialiseNewerNear(npc->position(),FixReach+2.f*radius(),npc->creationSeq(),R_Neighbour);
    st.materialised += before-pending.size();
    }
  }

void StartupFloor::beforeResetMove(Npc& npc, const Tempest::Vec3& dest) {
  if(!isActive)
    return;
  placed(npc);
  if(pending.empty())
    return;
  if(loosePlacement()) {
    if(npc.startupFloorPending()) {
      pending.erase(pending.begin()+ptrdiff_t(find(npc)));
      npc.setStartupFloorPending(false);
      ++cleared;
      }
    return;
    }
  const float r = radius();
  // Pending NPCs that see npc in their own fix need it where it is now; npc's own fix needs them in place.
  const bool          self   = npc.startupFloorPending();
  const Tempest::Vec3 from   = self ? pending[find(npc)].anchor : npc.position();
  const float         reachF = (self ? FixReach : 0.f) + FixReach + 2.f*r;
  const float         reachT = PlaceReach + FixReach + 2.f*r;
  const uint32_t      seq    = npc.creationSeq();
  for(size_t i=0; i<pending.size();) {
    auto& e = pending[i];
    if(e.npc==&npc) {
      ++i;
      continue;
      }
    bool need = distXZ(e.anchor,dest)<=reachT;
    if(!need && e.seq>seq)
      need = distXZ(e.anchor,from)<=reachF;
    if(!need) {
      ++i;
      continue;
      }
    const size_t before = pending.size();
    materialise(*e.npc,R_ResetMove);
    // materialise removes at least this entry and possibly older ones: rescan from the start
    i = (pending.size()<before) ? 0 : i+1;
    }
  if(npc.startupFloorPending()) {
    // The final placement runs its own fix from the routine point.
    const size_t at = find(npc);
    pending.erase(pending.begin()+ptrdiff_t(at));
    npc.setStartupFloorPending(false);
    ++cleared;
    }
  }

void StartupFloor::beforeKeep(Npc& npc) {
  if(!isActive)
    return;
  placed(npc);
  if(npc.startupFloorPending())
    materialise(npc,R_Keep);
  }

void StartupFloor::placed(const Npc& npc) {
  auto it = window.find(&npc);
  if(it!=window.end())
    it->second.placed = true;
  }

void StartupFloor::forget(Npc& npc) {
  const size_t at = find(npc);
  if(at<pending.size())
    pending.erase(pending.begin()+ptrdiff_t(at));
  npc.setStartupFloorPending(false);
  window.erase(&npc);
  }

void StartupFloor::materialise(Npc& npc, Reason r) {
  const size_t at = find(npc);
  if(at>=pending.size())
    return;
  const Entry e = pending[at];
  // Older pending NPCs near it stood at their insert-time positions when its fix ran.
  materialiseNear(e.anchor,2.f*FixReach+2.f*radius(),e.seq,R_Neighbour);

  const size_t now = find(npc);
  pending.erase(pending.begin()+ptrdiff_t(now));
  npc.setStartupFloorPending(false);
  ++byReason[r];

  auto& phys = *owner.world().physic();
  phys.setNpcReplaySeq(e.seq);
  owner.fixNpcPosition(npc,0,150);
  phys.setNpcReplaySeq(0);
  }

void StartupFloor::materialiseNear(const Tempest::Vec3& at, float reach, uint32_t olderThan, Reason r) {
  for(size_t i=0; i<pending.size();) {
    auto& e = pending[i];
    if(e.seq>=olderThan || distXZ(e.anchor,at)>reach) {
      ++i;
      continue;
      }
    materialise(*e.npc,r);
    i = 0;
    }
  }

void StartupFloor::materialiseNewerNear(const Tempest::Vec3& at, float reach, uint32_t newerThan, Reason r) {
  for(size_t i=0; i<pending.size();) {
    auto& e = pending[i];
    if(e.seq<=newerThan || distXZ(e.anchor,at)>reach) {
      ++i;
      continue;
      }
    materialise(*e.npc,r);
    i = 0;
    }
  }

size_t StartupFloor::find(const Npc& npc) const {
  for(size_t i=0; i<pending.size(); ++i)
    if(pending[i].npc==&npc)
      return i;
  return pending.size();
  }

float StartupFloor::radius() const {
  return owner.world().physic()->npcMaxRadiusXZ();
  }

void StartupFloor::report() const {
  char line[512];
  std::snprintf(line,sizeof(line),
                "[loadprobe-floor] inserts=%llu deferred=%llu cleared=%llu external=%llu neighbour=%llu reset_move=%llu keep=%llu dead=%llu end=%llu",
                (unsigned long long)inserts,(unsigned long long)deferred,(unsigned long long)cleared,
                (unsigned long long)byReason[R_External],(unsigned long long)byReason[R_Neighbour],
                (unsigned long long)byReason[R_ResetMove],(unsigned long long)byReason[R_Keep],
                (unsigned long long)byReason[R_Dead],(unsigned long long)byReason[R_End]);
  Tempest::Log::i(line);
  for(auto& e:externals) {
    if(e.calls==0)
      continue;
    const char* access = e.access==Access::None ? "none" : (e.access==Access::Args ? "args" : "all");
    std::snprintf(line,sizeof(line),
                  "[loadprobe-floor-external] name=%s access=%s calls=%llu calls_pending=%llu pending_args=%llu materialised=%llu",
                  e.name.c_str(),access,(unsigned long long)e.calls,(unsigned long long)e.callsPending,
                  (unsigned long long)e.pendingArgs,(unsigned long long)e.materialised);
    Tempest::Log::i(line);
    }
  // Reads of each inserted NPC between its insertion and its final placement (the brief's step 1 census).
  std::vector<uint32_t> reads, data;
  for(auto& w:window) {
    reads.push_back(w.second.reads);
    data.push_back(w.second.dataCalls);
    }
  auto summary = [](std::vector<uint32_t>& v, uint32_t& nonZero, uint64_t& sum, uint32_t& mx) {
    nonZero = 0; sum = 0; mx = 0;
    for(auto x:v) {
      nonZero += (x>0 ? 1u : 0u);
      sum += x;
      mx = std::max(mx,x);
      }
    };
  uint32_t rn=0, rm=0, dn=0, dm=0;
  uint64_t rs=0, ds=0;
  summary(reads,rn,rs,rm);
  summary(data,dn,ds,dm);
  std::snprintf(line,sizeof(line),
                "[loadprobe-floor-reads] npcs=%zu read_npcs=%u reads=%llu max_per_npc=%u data_npcs=%u data_calls=%llu data_max_per_npc=%u all_calls=%llu",
                window.size(),rn,(unsigned long long)rs,rm,dn,(unsigned long long)ds,dm,(unsigned long long)globalReads);
  Tempest::Log::i(line);
  }
