#include "appcadence.h"
// resources.cpp: texture residency of a deterministic bench (`deterministic on`)
extern bool benchTexturesSettle;
extern bool benchTexturesFrozen;

#include "testbench.h"
#include "testbench_internal.h"
#include "ui/gamemenu.h"
#include "navbench.h"
#include "navdemos.h"
#include "npcdayjournal.h"
#include "frameprofile.h"
#include "loadprobe.h"

#include <Tempest/Pixmap>
#include <Tempest/SoundDevice>
#include <Tempest/SystemApi>
#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "game/gamescript.h"
#include "game/damagecalculator.h"
#include "game/herodeed.h"
#include "game/playercontrol.h"
#include "game/bodyobservations.h"
#include "game/bodycontroller.h"
#include "game/bodyarena.h"
#include "game/bodyreplay.h"
#include "game/navigationeyebench.h"
#include "game/navigationmemorybench.h"
#include "game/knownroadsbench.h"
#include "game/npckeysbench.h"
#include "game/bodytracesbench.h"
#include "game/bloodstainsbench.h"
#include "game/serialize.h"
#include "game/ragdollsave.h"
#include "game/definitions/sounddefinitions.h"
#include "resources.h"
#include "graphics/drawcommands.h"
#include "graphics/mesh/footik.h"
#include "graphics/mesh/gaze.h"
#include "graphics/mesh/lipsync.h"
#include "graphics/mesh/talkgestures.h"
#include "graphics/mesh/eyeblink.h"
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/lean.h"
#include "graphics/mesh/pose.h"
#include "graphics/mesh/animblend.h"
#include "graphics/mesh/aim.h"
#include "graphics/mesh/animationsolver.h"
#include "graphics/mesh/bowdraw.h"
#include "graphics/mesh/hitzones.h"
#include "game/archery.h"
#include "game/archerai.h"
#include "graphics/mesh/flinch.h"
#include "graphics/mesh/skeleton.h"
#include "physics/dynamicworld.h"
#include "physics/physicmesh.h"
#include "physics/physicmeshshape.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "world/triggers/movetrigger.h"
#include "physics/ragdoll.h"
#include "physics/physicsnoise.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/objects/sound.h"
#include "graphics/objvisual.h"
#include "graphics/worldview.h"
#include "graphics/mesh/protomesh.h"
#include "world/movablevobs.h"
#include "world/world.h"
#include "world/chunkresidency.h"
#include "world/npcsimtier.h"
#include "world/bullet.h"
#include "world/waypoint.h"
#include "world/aiqueue.h"
#include "ui/crosshair.h"
#include "utils/keycodec.h"
#include "camera.h"
#include "gothic.h"
#include "marvin.h"

using namespace Tempest;
using namespace testbench_detail;

namespace testbench_detail {

const char* envStr(const char* name) {
  const char* v = std::getenv(name);
  return (v!=nullptr && v[0]!='\0') ? v : nullptr;
  }

}

namespace testbench_detail {

const char*       scenarioPath = envStr("OPENGOTHIC_BENCH");
}

namespace testbench_detail {

std::string       outDir       = envStr("OPENGOTHIC_BENCH_OUT") ? envStr("OPENGOTHIC_BENCH_OUT") : ".";

}

namespace testbench_detail {
const bool        keepRunning  = envStr("OPENGOTHIC_BENCH_KEEP")!=nullptr;

// the fixed steps keep to the wall clock (never faster): the AI server and its voices live in real time
const bool        realtime     = envStr("OPENGOTHIC_BENCH_REALTIME")!=nullptr;
bool              bodyTrace    = envStr("OPENGOTHIC_BODY_TRACE")!=nullptr &&
                                std::string_view(envStr("OPENGOTHIC_BODY_TRACE"))=="1";
// Trivial process-lifetime guard: NPCs may be destroyed after Bench's nontrivial members at shutdown.
bool              bodyTraceAlive = true;
// Keep the established regression CSV unchanged unless the stage 2 trace is requested.
const bool        ragdollTrace = envStr("OPENGOTHIC_RAGDOLL_TRACE")!=nullptr &&
                                std::string_view(envStr("OPENGOTHIC_RAGDOLL_TRACE"))=="1";
const bool        ragdollPerf = envStr("OPENGOTHIC_RAGDOLL_PERF")!=nullptr &&
                               std::string_view(envStr("OPENGOTHIC_RAGDOLL_PERF"))=="1";
}

namespace testbench_detail {

const bool        npcCharacterTrace = envStr("OPENGOTHIC_NPC_CHARACTER_TRACE")!=nullptr &&
                                     std::string_view(envStr("OPENGOTHIC_NPC_CHARACTER_TRACE"))=="1";
}

namespace {

std::FILE* logFile = nullptr;

}

namespace testbench_detail {

void say(const std::string& s) {
  std::printf("[bench] %s\n",s.c_str());
  std::fflush(stdout);
  if(logFile==nullptr)
    logFile = std::fopen((outDir+"/bench.log").c_str(),"w");
  if(logFile!=nullptr) {
    std::fprintf(logFile,"%s\n",s.c_str());
    std::fflush(logFile);
    }
  }

}

namespace testbench_detail {

std::vector<std::string> split(std::string_view s) {
  std::vector<std::string> ret;
  size_t i = 0;
  while(i<s.size()) {
    while(i<s.size() && std::isspace(uint8_t(s[i])))
      ++i;
    size_t e = i;
    while(e<s.size() && !std::isspace(uint8_t(s[e])))
      ++e;
    if(e>i)
      ret.emplace_back(s.substr(i,e-i));
    i = e;
    }
  return ret;
  }

}

namespace testbench_detail {

float num(const std::vector<std::string>& a, size_t i, float def) {
  return i<a.size() ? std::strtof(a[i].c_str(),nullptr) : def;
  }

bool isNumber(const std::string& s) {
  char* end = nullptr;
  std::strtof(s.c_str(),&end);
  return end!=s.c_str() && *end=='\0';
  }

}


namespace testbench_detail {

std::string jsonStr(std::string_view s) {
  std::string r = "\"";
  for(char ch:s) {
    switch(ch) {
      case '"':  r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\r': r += "\\r"; break;
      case '\t': r += "\\t"; break;
      default:
        if(uint8_t(ch)<0x20) {
          char buf[8];
          std::snprintf(buf,sizeof(buf),"\\u%04x",unsigned(uint8_t(ch)));
          r += buf;
          } else {
          r += ch;
          }
      }
    }
  return r + "\"";
  }

}

namespace testbench_detail {

float groundY(World& w, float x, float z, float y) {
  auto r = w.physic()->ray({x,y+150,z},{x,y-300,z});
  return r.hasCol ? r.v.y : NAN;
  }

// slope at a point: angle in degrees, horizontal direction down the slope, roughness (steps)
bool slopeAt(World& w, const Vec3& p, float& angle, Vec3& down, float& rough) {
  const float d  = 40;
  const float h0 = groundY(w,p.x,p.z,p.y);
  const float x0 = groundY(w,p.x-d,p.z,p.y), x1 = groundY(w,p.x+d,p.z,p.y);
  const float z0 = groundY(w,p.x,p.z-d,p.y), z1 = groundY(w,p.x,p.z+d,p.y);
  if(std::isnan(h0) || std::isnan(x0) || std::isnan(x1) || std::isnan(z0) || std::isnan(z1))
    return false;
  const float gx = (x1-x0)/(2*d), gz = (z1-z0)/(2*d);
  const float g  = std::sqrt(gx*gx+gz*gz);
  angle = std::atan(g)*180.f/float(M_PI);
  down  = g>0 ? Vec3(-gx/g,0,-gz/g) : Vec3(1,0,0);
  rough = std::max(std::abs((x0+x1)*0.5f-h0), std::abs((z0+z1)*0.5f-h0));
  return true;
  }

}

namespace testbench_detail {

Vec3 facing(const Npc& pl) {
  const float a = pl.rotation()*float(M_PI)/180.f;
  return {std::cos(a),0,std::sin(a)};
  }



}

namespace testbench_detail {


Bench bench;


OverlayContext overlayContext() {
  return {bench,bench,bench.living,bench.crew,bench.ctrl,bench.subject,bench.time,bench.label};
  }

CameraContext cameraContext() {
  return {bench,bench,overlayContext(),bench.water,bench.ragdoll,bench.monster,bench.character,bench.living,
          bench.archery,bench.movables,bench.routine,bench.crew,bench.subject,bench.ctrl,bench.time,bench.prog,bench.pc,
          bench.hasPrev,bench.anim1,bench.anim2,bench.ordered,bench.bodyLive,ragdollPerf,realtime,subjectOf,waterFilmFraming};
  }

}

namespace testbench_detail {

bool frameRagdollArena(size_t expected) { return testbench_detail::frameRagdollArena(cameraContext(),expected); }
void updateCamera(World& w, Npc& pl) { testbench_detail::updateCamera(cameraContext(),w,pl); }
void placeHero(World& w, Npc& pl) { testbench_detail::placeHero(cameraContext(),w,pl); }

}

namespace testbench_detail {


DestructionContext destructionContext() {
  return {bench.cam,bench.movables.movableCenter,bench.movables.movableIds,bench.movables.movableMeasuring,bench.movables.movableReady,
          bench.movables.movableWalkStart,bench.movables.noiseHost,bench.movables.noiseSource,bench.movables.noiseTrace,bench.ragdoll.ragdollArenaTarget,
          bench.ragdoll.ragdollArenaWaypoint,bench.movables.spellActors,bench.time,bench.waitUntil,bench.brokenSignals,scenarioPath};
  }

}

namespace testbench_detail {

ControlContext controlContext() {
  return {bench.time,bench.waitUntil,bench.realWaitUntil,bench.waitArrive,bench.arriveEnd,bench.label,
          bench.hasPrev,bench.holds,bench.taps,bench.healing,bench.inputTraceUntil,bench.routePoints,
          bench.routeNext,bench.routePosition,bench.routeSpeed,bench.routeLength,bench.routeStarted,
          bench.routeActive,bench.routeWasExternal,subjectOf};
  }

DialogContext dialogContext() {
  return {bench.time,bench.hearNpc,bench.hearUntil,bench.hearEvery,bench.hearNext,bench.thinkNpc,bench.thinkUntil,
          bench.speaking,bench.speakUntil,bench.heroEndAt,bench.waitReply,bench.replyNeed,bench.replyFrom,
          bench.replyEnd,bench.replyQuiet,bench.waitAiKey,bench.waitAiStage,bench.waitAiEnd,subjectOf};
  }

WorldContext worldContext() {
  return {bench.world,bench.ragdoll,bench.crew,bench.subject,bench.time,bench.waitUntil,bench.camPos,subjectOf};
  }

NpcCommandsContext npcCommandsContext() {
  return {bench.npcCommands,bench.healing,bench.holds,bench.taps,bench.pinned,bench.pinAt,
          bench.scenarioSeed,bench.lcg,bench.monster.monsterCharacterSeed,bench.crew,bench.time,
          bench.melee,bench.archery,bench.subject,stunDriftSample};
  }

ArcheryContext archeryContext() {
  return {bench.archery,bench.living,bench.crew,bench.subject,bench.ragdoll.ragdollArenaTarget,bench.cam,
          bench.trails,bench.camAngle,bench.camDist,bench.camHeight,bench.camLook};
  }

MeleeContext meleeContext() {
  return {bench.melee,bench.crew,bench.living.livingWalkers,bench.ragdoll.ragdollArenaPlaces,bench.ragdoll.ragdollArenaTarget};
  }

RoutineContext routineContext() {
  return {bench.routine,bench.pinned,bench.pinAt,bench.csv,bench.subject,bench.time,subjectOf};
  }

void meleePhysicsCamera() {
    // The same camera and world geometry are used for zero, one and ten actors.
    bench.camPos=bench.ragdoll.ragdollArenaTarget+Vec3(0,2600,1100);
    bench.cam=C_RagdollArena;bench.ragdoll.ragdollArenaCamera=true;bench.camAuto=false;bench.smValid=false;
    lookFrom(bench.camPos,bench.ragdoll.ragdollArenaTarget);
  }

OwnMeshContext ownMeshContext() {
  return {bench.ownMesh,bench.cam,bench.camAuto};
  }

RagdollContext ragdollContext() {
  return {bench.ragdoll,bench.crew,bench.camPos,bench.csv,bench.subject,bench.bodyIds,bench.nextBodyId,
          bench.step,bench.routine.questNpc,bench.recFrame,bench.recName,ragdollTrace,ragdollPerf,subjectOf};
  }

ItemWakeContext itemWakeContext() {
  return {bench.itemWake,bench.destruction,bench.cam,bench.ragdoll.ragdollArenaTarget};
  }

LivingContext livingContext() {
  return {bench.living,bench.crew,bench.subject,bench.csv,bench.ragdoll.ragdollArenaPlaces,frameRagdollArena};
  }

MovablesContext movablesContext() {
  return {bench.movables,bench.cam,bench.crew,bench.routine.questCliff,bench.routine.questWater,
          bench.ragdoll.ragdollArenaTarget,bench.shot,bench.subject,scenarioPath};
  }

PhysicsTracesContext physicsTracesContext() {
  return {bench.traceHost,bench.traceTable,bench.traceOrigin,bench.traceCenter,bench.traceVisible,
          bench.traceBlocked,bench.traceFar,bench.traceWall,bench.traceShotDirection,bench.traceReady};
  }

BodyImpactContext bodyImpactContext() {
  return {bench.bodyImpact,bench.subject,bench.movables.spellActors,bench.crew,bench.movables.spellTrace,bodyTrace,
          bench.time,bench.waitUntil};
  }

BodyContext bodyContext() {
  return {bench.bodyLive,bench.bodyLiveWait,bench.bodyLiveDeadline,bench.bodyAnimation,bench.duelA,
          bench.duelB,bench.duelFrom,bench.duelEnd,bench.duelWaiting,bench.observeActors,
          bench.observeFrom,bench.observeEnd,bench.observeActive,bench.observeVisibleA,bench.observeVisibleB,
          bench.observeVisibleSaved,bench.observeProbeCase,bench.probeSound,bench.probeSoundStart,bench.probeSoundDuration,
          bench.probeOccluded,bench.subject,bench.lcg,bench.pc,bench.prog,
          bench.time,bench.waitUntil};
  }

uintptr_t destructionAdd(World& world,std::string_view name,const Vec3& support) {
  return destructionAdd(bench.destruction,destructionContext(),world,name,support);
  }

void destructionCheckpoint(World& world) {
  destructionCheckpoint(bench.destruction,destructionContext(),world);
  }

}

namespace testbench_detail {

CharacterContext characterContext() {
  return {bench.cam,bench.camAngle,bench.camAuto,bench.camDist,bench.camHeight,bench.frameMs,
          bench.hasPrev,bench.holds,bench.movables.movableFixtures,bench.movables.movableReady,bench.movables.movableWalkEnd,
          bench.movables.movableWalkStart,bench.movables.noiseHost,bench.rec,bench.recVideo,bench.step,bench.subject,bench.taps};
  }

MonsterCharacterContext monsterCharacterContext() {
  return {bench.character,characterContext(),bench.cam,bench.camAuto,bench.subject,
          bench.ragdoll.ragdollArenaTarget,bench.ragdoll.ragdollArenaPlaces,bench.ragdoll.ragdollArenaWaypoint,bench.rec,bench.recVideo};
  }

WaterContext waterContext() {
  return {bench.cam,bench.camPos,bench.movables.movableMeasuring,bench.movables.movableIds,bench.movables.movableMapIds,
          bench.movables.movableReady,bench.movables.movableCenter,bench.routine.questWater,bench.destruction.destructionFixtures,
          bench.holds,bench.taps,bench.ctrl,bench.crew,bench.movables.spellActors,bench.movables.spellGuardTrace,
          bench.movables.spellRunes,bench.movables.spellCastingId,bench.movables.spellEquipPending,bench.movables.spellEquipUntil,
          bench.ragdoll.expectedRagdollLoot,bench.ragdoll.ragdollLootDidOpen};
  }

ClothContext clothContext() {
  return {bench.cloth,bench.cam,bench.camPos,bench.camHeldAngles,bench.camTurnAngles,bench.camTurnDegrees,
          bench.camTurnStart,bench.camTurnDuration,bench.subject,bench.crew,bench.time,bench.waitUntil,updateCamera};
  }

WaterFilmContext waterFilmContext() {
  return {bench.waterFilm,bench.water,bench.movables,bench.destruction,waterContext(),destructionContext(),
          bench.cam,bench.camPos,bench.subject,bench.crew,bench.rec,bench.time,bench.recStart};
  }

void waterFilmFraming(World& world,uint32_t width,uint32_t height) { testbench_detail::waterFilmFraming(waterFilmContext(),world,width,height); }


}

namespace testbench_detail {

// a line of events.jsonl: what happened when, in the recording's frames (the cut of a short lines up its
// subtitles and sound by them); `fields` are more json members, without braces
void event(std::string_view kind, const std::string& fields) {
  if(bench.events==nullptr)
    bench.events = std::fopen((outDir+"/events.jsonl").c_str(),"w");
  if(bench.events==nullptr)
    return;
  std::fprintf(bench.events,"{\"t\":%llu,\"seg\":%s,\"frame\":%lld,\"kind\":%s%s%s}\n",
               (unsigned long long)bench.time,jsonStr(bench.rec ? bench.recName : "").c_str(),
               bench.rec ? (long long)bench.recFrame : -1LL,jsonStr(kind).c_str(),fields.empty() ? "" : ",",fields.c_str());
  std::fflush(bench.events);
  }

}

namespace testbench_detail {

void loadScenario() {
  bench.loaded = true;
  std::ifstream fin(scenarioPath);
  if(!fin) {
    say(std::string("can not read scenario: ") + scenarioPath);
    return;
    }
  const char*       tv   = std::getenv("OPENGOTHIC_BENCH_TAKE");
  const std::string take = tv!=nullptr && tv[0]!='\0' ? tv : "on";
  std::string line;
  while(std::getline(fin,line)) {
    const size_t hash = line.find('#');
    if(hash!=std::string::npos)
      line.resize(hash);
    while(!line.empty() && std::isspace(uint8_t(line.back())))
      line.pop_back();
    size_t b = 0;
    while(b<line.size() && std::isspace(uint8_t(line[b])))
      ++b;
    // `off: <command>` / `on: <command>`: in that take of a before/after short only (gothic-shorts compare/film.sh
    // sets OPENGOTHIC_BENCH_TAKE; without it the take is `on`)
    for(const std::string_view tk:{std::string_view("off:"),std::string_view("on:")}) {
      if(line.compare(b,tk.size(),tk)!=0)
        continue;
      if(tk.substr(0,tk.size()-1)!=take)
        b = line.size(); else
        b += tk.size();
      while(b<line.size() && std::isspace(uint8_t(line[b])))
        ++b;
      break;
      }
    if(b<line.size())
      bench.prog.push_back(line.substr(b));
    }
  say("scenario " + std::string(scenarioPath) + ", " + std::to_string(bench.prog.size()) + " lines");
  }

}

namespace testbench_detail {

void press(PlayerControl& ctrl, KeyCodec::Action a) {
  if(a==KeyCodec::ActionGeneric || a==KeyCodec::Forward) {
    bench.inputTraceUntil = bench.time+2000;
    const auto* world = Gothic::inst().world();
    say("input press " + std::string(a==KeyCodec::Forward ? "forward" : "attack") + " at " +
        std::to_string(world ? world->tickCount() : 0) + " ms");
    }
  ctrl.onKeyPressed(a,KeyEvent::K_NoKey,KeyCodec::Mapping::Primary);
  }

}

namespace testbench_detail {

void release(PlayerControl& ctrl, KeyCodec::Action a) {
  if(a==KeyCodec::ActionGeneric || a==KeyCodec::Forward) {
    bench.inputTraceUntil = bench.time+2000;
    const auto* world = Gothic::inst().world();
    say("input release " + std::string(a==KeyCodec::Forward ? "forward" : "attack") + " at " +
        std::to_string(world ? world->tickCount() : 0) + " ms");
    }
  ctrl.onKeyReleased(a,KeyCodec::Mapping::Primary);
  }

}

namespace testbench_detail {


void stopRecording() { testbench_detail::stopRecording(cameraContext()); }


Npc& subjectOf(Npc& pl) {
  return bench.subject!=nullptr ? *bench.subject : pl;
  }

}

namespace testbench_detail {

// "nearest": the nearest living npc but the player; else a script symbol (NONE_100_XARDAS)
Npc* findNpc(World& w, Npc& pl, const std::string& name) {
  if(name=="player")
    return &pl;
  if(name=="watched")
    return &subjectOf(pl);
  if(name.starts_with("id:")) {
    // Archive fixtures address the persisted world slot, including dead NPCs.
    // Reject signs, whitespace, trailing text and overflow rather than falling
    // back to a script symbol that might select another actor.
    if(name.size()==3)
      return nullptr;
    uint64_t id=0;
    for(size_t i=3; i<name.size(); ++i) {
      if(name[i]<'0' || name[i]>'9')
        return nullptr;
      id=id*10+uint64_t(name[i]-'0');
      if(id>uint64_t(UINT32_MAX))
        return nullptr;
      }
    return w.npcById(uint32_t(id));
    }
  if(name=="nearest") {
    Npc*  best = nullptr;
    float dist = 0;
    for(uint32_t i=0; i<w.npcCount(); ++i) {
      Npc* n = w.npcById(i);
      if(n==nullptr || n==&pl || n->isDead())
        continue;
      const float d = (n->position()-pl.position()).quadLength();
      if(best==nullptr || d<dist) {
        best = n;
        dist = d;
        }
      }
    return best;
    }
  // by its script symbol; of several (two sheep inserted) a living one, the nearest
  const size_t id = w.script().findSymbolIndex(name);
  if(id==size_t(-1))
    return nullptr;
  Npc*  best = nullptr;
  float dist = 0;
  for(uint32_t i=0; i<w.npcCount(); ++i) {
    Npc* n = w.npcById(i);
    if(n==nullptr || n->instanceSymbol()!=id)
      continue;
    const float d = (n->position()-pl.position()).quadLength() + (n->isDead() ? 1e12f : 0.f);
    if(best==nullptr || d<dist) {
      best = n;
      dist = d;
      }
    }
  return best!=nullptr ? best : w.findNpcByInstance(id);
  }

}

namespace testbench_detail {

std::string symName(World& w, const Npc* n) {
  if(n==nullptr)
    return "-";
  if(auto sym = w.script().findSymbol(size_t(n->instanceSymbol())))
    return sym->name();
  return nameUtf8(*n);
  }

}

namespace testbench_detail {

// how much of its AI runs, by its distance to the hero (worldobjects.cpp): far ones say nothing, far2 ones
// barely move
const char* policyName(NpcProcessPolicy p) {
  switch(p) {
    case NpcProcessPolicy::Player:   return "player";
    case NpcProcessPolicy::AiNormal: return "normal";
    case NpcProcessPolicy::AiFar:    return "far";
    case NpcProcessPolicy::AiFar2:   return "far2";
    }
  return "?";
  }

}

namespace testbench_detail {

// the script state it is in (ZS_SMALLTALK), "-" for none
std::string stateName(World& w, const Npc& n) {
  const ScriptFn fn  = n.currentState();
  auto           sym = fn.isValid() ? w.script().findSymbol(fn.ptr) : nullptr;
  return sym!=nullptr ? sym->name() : "-";
  }

// a point is hidden if something is between it and the camera (not counting the ground right at the point)
bool hidden(World& w, const Vec3& from, const Vec3& to) {
  auto r = w.physic()->ray(from,to);
  return r.hasCol && (r.v-to).length()>15.f;
  }

}

namespace testbench_detail {


void lookFrom(const Vec3& org, const Vec3& tgt) { lookFrom(cameraContext(),org,tgt); }

Vec3 rotY(const Vec3& v, float deg) {
  const float a = deg*float(M_PI)/180.f, c = std::cos(a), s = std::sin(a);
  return {v.x*c - v.z*s, v.y, v.x*s + v.z*c};
  }

}


namespace testbench_detail {

void face(Npc& pl, const Vec3& dir) {
  if(dir.x==0 && dir.z==0)
    return;
  pl.setDirection(dir);
  pl.updateTransform();
  }

}


namespace testbench_detail {

void arenaAbort(std::string_view reason) {
  say("ragdoll arena failed: "+std::string(reason));
  event("ragdoll_error","\"reason\":"+jsonStr(reason));
  bench.pc=bench.prog.size(); bench.waitUntil=0;
  }


}


bool TestBench::npcDuelActor(const Npc& npc) {
  return bench.npcDuel.active && (bench.npcDuel.actors[0]==&npc || bench.npcDuel.actors[1]==&npc);
  }

void TestBench::npcDuelSwingStart(const Npc& npc) {
  if(!npcDuelActor(npc))return;
  auto& duel=bench.npcDuel;
  const size_t i=duel.actors[0]==&npc ? 0 : 1;
  ++duel.swings[i];
  if(i==1 && duel.firstResponseMs<0)
    duel.firstResponseMs=int64_t(npc.world().tickCount()-duel.from);
  }

void TestBench::npcDuelSwingEnd(const Npc& npc, std::string_view outcome) {
  if(!npcDuelActor(npc))return;
  auto& duel=bench.npcDuel;
  const size_t i=duel.actors[0]==&npc ? 0 : 1;
  if(outcome=="blocked")++duel.blocked[i];
  else if(outcome!="hit" && outcome!="applied")++duel.misses[i];
  }

void TestBench::npcDuelHit(const Npc& victim, const Npc& by, int32_t hpBefore, int32_t hpAfter,
                           int32_t baseDamage, bool critical, float meleeScale, float stunBonus, float backstabMult) {
  if(!npcDuelActor(by) || !npcDuelActor(victim) || &by==&victim || hpBefore<=hpAfter)return;
  auto& duel=bench.npcDuel;
  auto& log=duel.hitLog;
  if(log.size()>1)log+=',';
  log+="{\"time_ms\":"+std::to_string(by.world().tickCount()-duel.from)+
    ",\"attacker\":"+jsonStr(who(&by))+",\"victim\":"+jsonStr(who(&victim))+
    ",\"hp_before\":"+std::to_string(hpBefore)+",\"hp_after\":"+std::to_string(hpAfter)+
    ",\"base_damage\":"+std::to_string(baseDamage)+",\"critical\":"+(critical ? "true" : "false")+
    ",\"melee_scale\":"+std::to_string(meleeScale)+",\"stun_bonus\":"+std::to_string(stunBonus)+
    ",\"backstab_mult\":"+std::to_string(backstabMult)+",\"hp_loss\":"+std::to_string(hpBefore-hpAfter)+"}";
  }

bool TestBench::isActive() {
  return scenarioPath!=nullptr;
  }

bool TestBench::strictGraphMovement(const Npc& npc) {
  return isActive() && (bench.npcTierTest.npc==&npc || bench.npcSpeedTest.npc==&npc);
  }

bool TestBench::isPhysicsNoiseDebug() {
  return bodyTraceAlive && bench.movables.noiseBridgeDebug;
  }

void TestBench::physicsNoiseBridgeEvent(const Npc& listener,uint64_t tick) {
  if(isPhysicsNoiseDebug() && &listener==bench.movables.noiseHost)bench.movables.noiseBridgeEvents.push_back(tick);
  }

bool TestBench::physicsNoiseDebug(World& world,Npc& player,std::string_view command,
                                  std::string& value,std::string& error) {
  return testbench_detail::physicsNoiseDebug(movablesContext(),world,player,command,value,error);
  }

bool TestBench::physicsTracesDebug(World& world,Npc& player,std::string_view command,
                                  std::string& value,std::string& error) {
  return testbench_detail::physicsTracesDebug(physicsTracesContext(),world,player,command,value,error);
  }

bool TestBench::destructionTracesDebug(World& world,Npc& player,std::string_view command,
                                      std::string& value,std::string& error) {
  return testbench_detail::destructionTracesDebug(bench.destruction,destructionContext(),world,player,command,value,error);
  }

void TestBench::destructionBreak(uintptr_t body,size_t pieces,float milliseconds,bool remnant) {
  if(scenarioPath==nullptr || !bench.destruction.destructionTrace || !destructionOwns(bench.destruction,body))return;
  auto* world=Gothic::inst().world();
  if(world)destructionFields(bench.destruction,destructionContext(),*world);
  bench.destruction.destructionLastBreak=world?world->tickCount():0;bench.destruction.destructionSleepSampled=false;
  event("destruction_break","\"case\":"+jsonStr(bench.destruction.destructionCase)+",\"body\":"+std::to_string(body)+
    ",\"pieces\":"+std::to_string(pieces)+",\"remnant\":"+(remnant?"true":"false")+",\"break_ms\":"+ragdollNumber(milliseconds)+
    ",\"wall_ns\":"+std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count())+
    ",\"t_game_ms\":"+std::to_string(world?world->tickCount():0)+",\"arrows\":"+std::to_string(bench.destruction.destructionArrows));
  // Not inside a timed window: loading the pieces would land in the measured break frame.
  if(world && !bench.movables.movableMeasuring)fracturePiecesBreak(bench.fracturePieces,*world,body,bench.destruction.destructionCase);
  }

void TestBench::destructionRetire(uintptr_t body,size_t piece,bool expired,float observerDistanceCm) {
  if(scenarioPath==nullptr || !bench.destruction.destructionTrace || !destructionOwns(bench.destruction,body))return;
  auto* world=Gothic::inst().world();
  event("destruction_fragment_retired","\"case\":"+jsonStr(bench.destruction.destructionCase)+",\"parent_id\":"+std::to_string(body)+
    ",\"piece\":"+std::to_string(piece)+",\"sleeping\":true,\"reason\":"+jsonStr(expired?"expired":"distant")+
    ",\"observer_distance_cm\":"+ragdollNumber(observerDistanceCm)+",\"t_game_ms\":"+std::to_string(world?world->tickCount():0));
  }

void TestBench::destructionMeleeHit(const Npc& actor,size_t affected,float damage) {
  if(scenarioPath==nullptr || !bench.destruction.destructionMeleeRunning || bench.destruction.destructionMeleeActor!=&actor)return;
  if(affected)++bench.destruction.destructionMeleeHits;
  auto* world=Gothic::inst().world();
  event("destruction2_melee_hit","\"case\":"+jsonStr(bench.destruction.destructionCase)+",\"affected\":"+
    std::to_string(affected)+",\"hits\":"+std::to_string(bench.destruction.destructionMeleeHits)+
    ",\"raw_hit_power\":"+ragdollNumber(damage)+",\"bodies\":"+(world?destructionRows(bench.destruction,*world):"[]"));
  }

void TestBench::movementImpulseHandoff(const Npc& actor,std::string_view backend,uint32_t fromState,
                                      uint32_t toState,const Vec3& velocity) {
  if(scenarioPath==nullptr || !bench.started || bench.done)return;
  auto* world=Gothic::inst().world();
  if(!world)return;
  event("movement_impulse_handoff","\"actor\":"+jsonStr(symName(*world,&actor))+
    ",\"actor_id\":"+std::to_string(world->npcId(&actor))+",\"backend\":"+jsonStr(backend)+
    ",\"from_state\":"+std::to_string(fromState)+",\"to_state\":"+std::to_string(toState)+
    ",\"impulse_velocity_cm_s\":"+ragdollVector(velocity));
  }

void TestBench::physicsNoise(const PhysicsNoise& noise,std::string_view sound,bool played) {
  testbench_detail::physicsNoise(movablesContext(),noise,sound,played);
  }

void TestBench::physicsNoiseHeard(const Npc& npc,const PhysicsNoise& noise,bool callbackDispatched) {
  testbench_detail::physicsNoiseHeard(movablesContext(),npc,noise,callbackDispatched);
  }

void TestBench::configure(std::string scenario, std::string output, int64_t seedOverride) {
  stopBodyLive();
  benchTexturesSettle=false;
  benchTexturesFrozen=false;
  npcCharacterStopMotionTrace(bench.character,Gothic::inst().world());
  KnownRoadsBench::reset();
  NavigationMemoryBench::reset();
  NavigationEyeBench::reset();
  NpcKeysBench::reset();
  BodyTracesBench::reset();
  BloodStainsBench::reset();
  NavBench::reset();
  NavDemos::reset();
  stopRecording();
  if(bench.csv!=nullptr)
    std::fclose(bench.csv);
  if(bench.events!=nullptr)
    std::fclose(bench.events);
  if(logFile!=nullptr)
    std::fclose(logFile);
  logFile = nullptr;
  // Reconstruct all scenario-owned buffers/counters/pointers, including terminal and RNG state.
  bench.~Bench();
  new(&bench) Bench();
  bodyTraceAlive = true;
  static std::string configuredScenario;
  configuredScenario = std::move(scenario);
  scenarioPath = configuredScenario.c_str();
  outDir = std::move(output);
  const auto traceSetting = envStr("OPENGOTHIC_BODY_TRACE");
  bodyTrace = traceSetting!=nullptr ? std::string_view(traceSetting)=="1" : bodyTrace || Resources::isHeadless();
  Pose::probeNpc = nullptr;
  loadScenario();
  if(seedOverride>=0) {
    for(auto& line:bench.prog) {
      if(line.rfind("bodyduel ",0)!=0)
        continue;
      auto args = split(line);
      line = "bodyduel " + std::to_string(seedOverride) + " " + (args.size()>2 ? args[2] : "60000");
      }
    }
  }

void TestBench::holdBefore(std::string command) {
  bench.holdCommand = std::move(command);
  bench.heldNow = false;
  }

bool TestBench::held() { return bench.heldNow; }

void TestBench::flushOutput() {
  for(std::FILE* file:{bench.events,bench.csv,logFile})
    if(file!=nullptr)
      std::fflush(file);
  }

void TestBench::retarget(std::string output) {
  // The template flushed these before forking: closing the fork's copies writes nothing twice.
  for(std::FILE** file:{&bench.events,&bench.csv,&logFile})
    if(*file!=nullptr) {
      std::fclose(*file);
      *file = nullptr;
      }
  outDir = std::move(output);
  }

void TestBench::resumeHeld() {
  auto world  = Gothic::inst().world();
  auto player = Gothic::inst().player();
  if(!bench.heldNow || world==nullptr || player==nullptr || bench.ctrl==nullptr)
    throw std::runtime_error("no held scenario step to resume");
  bench.holdCommand.clear();
  bench.heldNow = false;
  run(*world,*player,*bench.ctrl);
  }

bool TestBench::done() { return bench.done; }

void TestBench::endWait(std::string_view reason) {
  if(scenarioPath==nullptr || bench.done)
    return;
  bench.waitUntil = bench.time;
  bench.realWaitUntil={};
  say("wait ended: " + std::string(reason));
  }

uint64_t TestBench::stepCount() { return bench.step; }
uint64_t TestBench::nativeChoices() { return bench.nativeChoices; }

std::vector<Npc*> TestBench::bodyObservationActors() {
  if(BodyArena::enabled())
    return BodyArena::actors();
  if(!bench.observeActors.empty())
    return bench.observeActors;
  if(bench.duelA!=nullptr && bench.duelB!=nullptr)
    return {bench.duelA,bench.duelB};
  return {};
  }

bool TestBench::bodyTracing() {
  return bodyTrace && !bench.navigationOnly && bodyTraceAlive && scenarioPath!=nullptr && bench.started && !bench.done;
  }

void TestBench::bodyEvent(const Npc& actor, const Npc* target, std::string_view phase, uint64_t gameTime,
                          const std::string& fields, std::string_view animation) {
  if(!bodyTracing())
    return;
  if(phase=="native_choice")
    ++bench.nativeChoices;
  if(phase=="body_impact" && (bench.bodyImpact.duel || (bench.bodyImpact.tracing && bench.bodyImpact.npc==&actor))) {
    rapidjson::Document sample;
    const std::string row="{"+fields+"}";sample.Parse(row.c_str());
    if(!sample.HasParseError() && sample.IsObject() && sample.HasMember("damage") && sample["damage"].IsInt() &&
       sample.HasMember("speed_cm_s") && sample["speed_cm_s"].IsNumber()) {
      if(bench.bodyImpact.tracing && bench.bodyImpact.npc==&actor) {
        bench.bodyImpact.damage+=sample["damage"].GetInt();++bench.bodyImpact.impacts;
        if(sample.HasMember("source") && sample["source"].IsString() &&
           std::string_view(sample["source"].GetString())=="spell_capsule")bench.bodyImpact.capsuleDamage+=sample["damage"].GetInt();
        bench.bodyImpact.maximumSpeed=std::max(bench.bodyImpact.maximumSpeed,sample["speed_cm_s"].GetFloat());
        }
      for(auto& body:bench.bodyImpact.duelBodies)if(body.npc==&actor && body.knocked) {
        body.damage+=sample["damage"].GetInt();body.speed=std::max(body.speed,sample["speed_cm_s"].GetFloat());
        }
      }
    }
  const auto id = [](const Npc* npc) -> uint64_t {
    if(npc==nullptr)
      return 0;
    auto it = bench.bodyIds.find(npc);
    if(it==bench.bodyIds.end())
      it = bench.bodyIds.emplace(npc,++bench.nextBodyId).first;
    return it->second;
    };
  const auto actorId = id(&actor);
  const auto targetId = id(target);
  event("body_combat","\"schema_version\":1,\"seq\":" + std::to_string(++bench.bodySeq) +
                     ",\"t_game\":" + std::to_string(gameTime) + ",\"step\":" + std::to_string(bench.step) +
                     ",\"actor_id\":" + std::to_string(actorId) + ",\"actor\":" + jsonStr(who(&actor)) +
                     ",\"target_id\":" + std::to_string(targetId) + ",\"target\":" + jsonStr(who(target)) +
                     ",\"phase\":" + jsonStr(phase) + ",\"animation\":" + jsonStr(animation) +
                     (fields.empty() ? "" : ",") + fields);
  if(phase=="native_attempt")
    observeAnimationAttempt(actor,fields,gameTime);
  BodyArena::noteCombat(actor,target,phase,fields);
  }

void TestBench::bodyForget(const Npc* npc) {
  if(bodyTraceAlive && bench.melee.meleeAiResponse &&
     (bench.melee.meleeAiResponse->npc==npc || bench.melee.meleeAiResponse->attacker==npc)) bench.melee.meleeAiResponse.reset();
  if(bodyTraceAlive && (bench.npcCommands.stunDrift.target==npc || bench.npcCommands.stunDrift.attacker==npc)) bench.npcCommands.stunDrift={};
  if(bodyTraceAlive) {
    if(bench.monster.monsterCharacterRoute.npc==npc)bench.monster.monsterCharacterRoute.npc=nullptr;
    for(auto& actor:bench.monster.monsterCharacterExtras)if(actor.npc==npc)actor.npc=nullptr;
    if(bench.character.characterIdle.actor==npc)bench.character.characterIdle.actor=nullptr;
    if(bench.character.characterIdle.fixtureNpc==npc)bench.character.characterIdle.fixtureNpc=nullptr;
    bench.character.npcCharacterGoals.erase(npc);bench.character.npcCharacterWalkers.erase(npc);
    std::erase_if(bench.character.npcCharacterTraceActors,[npc](const auto& actor){return actor.npc==npc;});
    }
  if(bodyTraceAlive && bench.movables.noiseTrace)bench.movables.noiseNpcIds.erase(npc);
  if(bodyTracing() || (ragdollTrace && bodyTraceAlive && bench.started && !bench.done)) {
    bench.bodyIds.erase(npc);
    for(auto it=bench.ragdoll.savedRagdollNpcs.begin();it!=bench.ragdoll.savedRagdollNpcs.end();) {
      if(it->second.npc==npc) it=bench.ragdoll.savedRagdollNpcs.erase(it); else ++it;
      }
    for(auto& actor:bench.bodyAnimation.actors)
      if(actor.npc==npc)
        actor.npc = nullptr;
    }
  }

std::string TestBench::bodyJson(std::string_view text) {
  return jsonStr(text);
  }

int TestBench::bodyBlockVariant(const Npc& npc, int fallback) {
  auto& control = bench.bodyAnimation;
  if(!bodyTracing() || !control.active || control.mode==BodyAnimationControl::Default)
    return fallback;
  const unsigned side = animationActorIndex(npc);
  if(side>=control.actorCount)
    return fallback;
  auto& actor = control.actors[side];
  ++actor.solverCalls;
  const uint64_t now = Gothic::inst().gameSession()->tickCount();
  const uint64_t time = now-bench.duelFrom;
  int variant = fallback;
  BodyAnimationEntry* entry = nullptr;
  if(control.mode==BodyAnimationControl::Isolated) {
    actor.rng ^= actor.rng<<13;
    actor.rng ^= actor.rng>>17;
    actor.rng ^= actor.rng<<5;
    variant = int(actor.rng%3);
    ++actor.controlledCalls;
    }
  else {
    entry = animationFixtureEntry(side,time);
    if(entry!=nullptr) {
      variant = entry->variant; // repeated continuation/start solves in one tick use the same entry
      ++actor.controlledCalls;
      }
    else
      ++actor.fallbackCalls;
    }
  actor.lastTime = time;
  actor.lastVariant = variant;
  bodyEvent(npc,npc.target(),"animation_branch_draw",now,
            "\"mode\":"+jsonStr(animationModeName())+",\"seed\":"+std::to_string(control.seed)+
            ",\"role\":"+jsonStr(side%2==0 ? "actor" : "target")+",\"relative_time_ms\":"+std::to_string(time)+
            ",\"fallback_variant\":"+std::to_string(fallback)+",\"selected_variant\":"+std::to_string(variant)+
            ",\"fixture_matched\":"+(entry==nullptr ? "false" : "true")+
            ",\"source_seq\":"+std::to_string(entry==nullptr ? 0 : entry->sourceSeq)+animationActorFields(side));
  return variant;
  }

void TestBench::ragdollBirthPeak(const Npc& npc, const Npc* culprit, const std::string& fields) {
  if(!isActive() || !bench.started || bench.done) return;
  event("ragdoll_birth_peak","\"actor\":"+jsonStr(who(&npc))+",\"culprit\":"+(culprit ? jsonStr(who(culprit)) : "null")+
    ",\"guard_enabled\":"+(Gothic::physicsFlags().ragdollBirthGuard ? "true" : "false")+","+fields);
  }

void TestBench::note(const char* text) {
  if(scenarioPath!=nullptr && bench.started && !bench.done) {
    say(text);
    event("note","\"text\":"+jsonStr(text));
    // releaseArrow reports this note immediately after releasing the real projectile.
    // Sample the feet and the applied aim pose here, rather than at the script's
    // earlier `release action` (which can precede the shot by an animation frame).
    auto* player = Gothic::inst().player();
    if(player!=nullptr && player->hasManualAim() &&
       std::string_view(text).starts_with("arrow shot: ") &&
       std::string_view(text).ends_with(", by "+std::string(who(player)))) {
      const float yaw = player->rotationRad();
      const auto edge = player->aimEdge(Vec3(std::cos(yaw),0,std::sin(yaw)),2000.f);
      char buf[256];
      std::snprintf(buf,sizeof(buf),"shot pose %s: edge_facing_cm %.2f edge_found %d facing_yaw_deg %.2f a_lean_deg %.2f",
                    bench.recName.c_str(),edge.found ? double(edge.distance) : -1.0,int(edge.found),
                    double(player->rotation()),double(Aim::playerProbe.lean));
      say(buf);
      event("note","\"text\":"+jsonStr(buf));
      }
    if(bench.archery.arrowKoOnHit && std::string_view(text).starts_with("arrow hit ") && bench.archery.arrowNpc!=nullptr &&
       bench.archery.arrowNpc->arrowPhysicsProbe().hits>bench.archery.arrowSeenHits && !bench.archery.arrowNpc->isDown()) {
      // This note runs inside real projectile damage, after queueArrowPhysics
      // and before the next sequential native prepare. Ordinary arrows kill;
      // only the benchmark forces KO to exercise the same pending-hit handoff.
      bench.archery.arrowKoOnHit=false;
      bench.archery.arrowNpc->setRagdollDown(false);
      event("arrow_reaction_forced_ko","\"case\":"+jsonStr(bench.archery.arrowCase)+
        ",\"hits\":"+std::to_string(bench.archery.arrowNpc->arrowPhysicsProbe().hits));
      }
    }
  }

void TestBench::ownMeshArrowHit(uint32_t zenId,const Vec3& point,const Vec3& normal) {
  if(!isActive() || !bench.started || bench.done || bench.ownMesh.ownMeshZen==uint32_t(-1))return;
  const auto delta=point-bench.ownMesh.ownMeshAxis;
  const bool matched=zenId==bench.ownMesh.ownMeshZen;
  if(matched)++bench.ownMesh.ownMeshArrowHits;
  event("own_mesh_arrow_hit","\"zen_id\":"+std::to_string(zenId)+
    ",\"expected_zen_id\":"+std::to_string(bench.ownMesh.ownMeshZen)+",\"matched\":"+(matched?"true":"false")+
    ",\"point_cm\":"+ragdollVector(point)+",\"normal\":"+ragdollVector(normal)+
    ",\"axis_distance_cm\":"+ragdollNumber(std::hypot(delta.x,delta.z))+
    ",\"half_height_error_cm\":"+ragdollNumber(delta.y));
  }

std::string_view TestBench::who(const Npc* npc) {
  auto w = Gothic::inst().world();
  if(npc==nullptr || w==nullptr)
    return "none";
  auto sym = w->script().findSymbol(size_t(npc->instanceSymbol()));
  return sym!=nullptr ? std::string_view(sym->name()) : std::string_view("npc");
  }

bool TestBench::filming() {
  return scenarioPath!=nullptr && realtime;
  }

bool TestBench::deterministic() { return bench.started && !bench.done && bench.world.deterministic; }

bool TestBench::simulationFrozen() { return bench.started && !bench.done && bench.frozen; }

bool TestBench::orderedVisibility() { return bench.ordered; }

bool TestBench::takeRenderReset() {
  const bool r=bench.renderReset;
  bench.renderReset=false;
  return r;
  }

uint64_t TestBench::frameDt(uint64_t dt) {
  if(!bench.started || bench.done)
    return dt;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  if(AppCadence::normalClock()) { ++bench.step;bench.frameMs=dt;return dt; }
#endif
  ++bench.step;
  if(realtime) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if(bench.step==1)
      bench.wallStart = now;
    const auto due = bench.wallStart + microseconds(int64_t(bench.step)*1000000/Fps);
    if(now<due)
      std::this_thread::sleep_until(due); else
      bench.lagMs = std::max<int64_t>(bench.lagMs,duration_cast<milliseconds>(now-due).count());
    }
  bench.frameMs = (bench.step*1000)/Fps - ((bench.step-1)*1000)/Fps;
  const auto healingStep=[&](uint64_t step) {
    const auto* world=Gothic::inst().world();
    if(world && bench.healing.test.strikeAt>world->tickCount())
      return std::min(step,bench.healing.test.strikeAt-world->tickCount());
    return step;
    };
  if(bench.slow<=1.f)
    return healingStep(bench.frameMs);
  bench.slowAcc += uint64_t(float(bench.frameMs*1000)/bench.slow);
  const uint64_t dtGame = bench.slowAcc/1000;
  bench.slowAcc -= dtGame*1000;
  return healingStep(dtGame);
  }

void TestBench::beforeNpcTick(Npc& npc) {
  if(scenarioPath==nullptr || bench.done || !bench.healing.test.strikeAt || !bench.ctrl || !npc.isPlayer())
    return;
  auto& world=npc.world();
  if(world.tickCount()<bench.healing.test.strikeAt)
    return;
  bench.healing.test.strikeAt=0;
  exec("strike player BDT_1013_BANDIT_L",world,npc,*bench.ctrl);
  healingObserve(bench.healing,world,npc);
  }

void TestBench::beforeWorldTick(uint64_t dt) {
  // Keep the native observer at the fixture's supported point. Nearby enemies
  // retain their ordinary behaviour and cannot push this measurement out of range.
  if(scenarioPath!=nullptr && bench.started && !bench.done && bench.cloth.clothTrace && bench.cloth.clothHold) {
    if(auto* player=Gothic::inst().player()) {
      player->stopWalking();player->clearAiQueue();
      player->setPosition(bench.cloth.clothHeldPosition);player->updateTransform();
      }
    }
  // A reload observer accepts no live keyboard or mouse movement. Its actor
  // remains subject to ordinary native support and gravity, with no pose pin.
  if(scenarioPath!=nullptr && bench.started && !bench.done && bench.water.observer && bench.ctrl)
    bench.ctrl->clearInput();
  // The live bridge stand has no bench program, but uses the same real sword
  // animation and its normal hit frame as the scripted destruction fixture.
  if(scenarioPath==nullptr && bench.destruction.destructionTraceDebug && bench.destruction.destructionMeleeRunning && dt!=0) {
    auto* world=Gothic::inst().world();
    if(world && world==bench.destruction.destructionTraceWorld)destructionMeleeTick(bench.destruction,destructionContext(),*world);
    else {bench.destruction.destructionTraceDebug=false;bench.destruction.destructionMeleeRunning=false;}
    }
  const bool destructionCast=bench.destruction.destructionTrace &&
                             (bench.destruction.destructionCase=="fireball" || bench.destruction.destructionCase=="five");
  const bool movableCast=bench.movables.movableTrace && bench.movables.movablePhase=="blast";
  const bool waterCast=bench.water.ready && !bench.movables.spellActors.empty() && bench.movables.spellActors[0].role=="water_target";
  if(scenarioPath!=nullptr && bench.started && !bench.done && (destructionCast || movableCast || waterCast) && !bench.movables.spellActors.empty()) {
    // PlayerControl refreshes auto-focus after each world step. Keep this
    // fixture's selected native target until commitSpell has emitted the real
    // projectile, so a nearby tavern host cannot steal the cast mid-animation.
    if(auto* world=Gothic::inst().world();world && world->player())world->player()->setTarget(bench.movables.spellActors[0].npc);
    }
  if(scenarioPath==nullptr || !bench.started || bench.done || !bench.character.characterRideMoving || !bench.character.characterPlatform || dt==0)return;
  auto matrix=bench.character.characterRideMatrixStart;
  if(bench.character.characterRideRotating) {
    bench.character.characterRideAngle+=25.f*float(dt)*.001f;
    matrix.translate(bench.character.characterRidePivot);matrix.rotate(bench.character.characterRideAngle,0,1,0);matrix.translate(-bench.character.characterRidePivot);
    }
  else {
    bench.character.characterRideTravel+=80.f*float(dt)*.001f;
    matrix.translate(bench.character.characterRideDirection*bench.character.characterRideTravel);
    }
  bench.character.characterPlatformMatrix=matrix;
  bench.character.characterPlatform->setObjMatrix(matrix);bench.character.characterPlatformVisual->setObjMatrix(matrix);
  }


std::function<bool()> TestBench::aiBusy;
std::function<void(std::string_view)> TestBench::openMenu;
std::function<bool(std::string_view,std::string_view)> TestBench::aiReached;
std::function<bool(World&,std::string_view,float)> TestBench::stuckBegin;
std::function<std::string(World&,std::string_view)> TestBench::npcView;

namespace {
bool benchBodySweep = false;
}

void TestBench::allowBodySweep(bool allow) {
  benchBodySweep = allow;
  }

bool TestBench::bodySweepAllowed() {
  return benchBodySweep;
  }
std::function<bool(World&)> TestBench::stuckEnd;

bool TestBench::voiceStems() {
  static const bool enabled = envStr("OPENGOTHIC_BENCH_VOICE_STEM")!=nullptr &&
                             std::string_view(envStr("OPENGOTHIC_BENCH_VOICE_STEM"))=="1";
  return enabled;
  }

std::function<void(const int16_t*,size_t)> TestBench::aiVoiceStem(std::string_view npc, std::string_view line, uint32_t rate) {
  if(!voiceStems() || !bench.rec || bench.audio==nullptr || SoundDevice::loopbackRate()==0)
    return {};
  auto stem = std::make_shared<Bench::VoiceStem>();
  char suffix[48];
  std::snprintf(suffix,sizeof(suffix),".voice-%02zu.wav",bench.voiceStems.size()+1);
  stem->path = std::filesystem::absolute(outDir+"/"+bench.recName+suffix).string();
  stem->rate = rate;
  // Sources start in the next loopback render, immediately after the recorded mix so far.
  stem->sample = bench.audioFrames;
  bench.voiceStems.push_back(stem);
  event("npc_voice","\"npc\":"+jsonStr(npc)+",\"line\":"+jsonStr(line)+
                    ",\"wav\":"+jsonStr(stem->path)+",\"sample\":"+std::to_string(stem->sample));
  return [stem](const int16_t* pcm, size_t n) {
    std::lock_guard<std::mutex> guard(stem->sync);
    if(!stem->stopped)
      stem->pcm.insert(stem->pcm.end(),pcm,pcm+n);
    };
  }

bool TestBench::ragdollBodyDrawable(const Npc& npc) {
  const auto& mesh=npc.visual.view;
  if(bench.monster.monsterCharacterRenderRequest==&npc) {
    std::string parts="[";
    for(size_t i=0;i<mesh.subCount;++i) {
      const auto& item=mesh.sub[i];if(item.isEmpty())continue;
      const auto matrix=item.position();const auto& bounds=item.bounds();const auto& material=item.material();
      auto center=(bounds.bbox[0]+bounds.bbox[1])*.5f;matrix.project(center);
      const auto root=Vec3(matrix.at(3,0),matrix.at(3,1),matrix.at(3,2));
      float x=center.x,y=center.y,z=center.z,w=1;
      if(auto* camera=Gothic::inst().camera())camera->viewProj().project(x,y,z,w);
      if(parts.size()>1)parts+=',';
      parts+="{\"part\":"+std::to_string(i)+",\"indices\":"+std::to_string(item.meshSlice().second)+
        ",\"root_cm\":"+ragdollVector(root)+",\"center_cm\":"+ragdollVector(center)+
        ",\"clip_cm\":["+ragdollNumber(x)+","+ragdollNumber(y)+","+ragdollNumber(z)+","+ragdollNumber(w)+"]"+
        ",\"radius_cm\":"+ragdollNumber(bounds.rConservative)+",\"alpha\":"+ragdollNumber(material.alphaWeight)+
        ",\"ghost\":"+(material.isGhost?"true":"false")+",\"texture\":"+
        (material.tex && !material.tex->isEmpty()?"true":"false")+"}";
      }
    event("monster_character_render_mesh","\"npc_id\":"+std::to_string(npc.world().npcId(&npc))+
      ",\"cpu_only\":"+(mesh.cpuOnly?"true":"false")+",\"submeshes\":"+std::to_string(mesh.subCount)+",\"parts\":"+parts+"]");
    }
  if(mesh.cpuOnly) return false;
  if(bench.destruction.destructionSkinRequest==&npc && mesh.protoMesh()!=nullptr)
    destructionSkinSample(npc,*mesh.protoMesh(),npc.visual,npc.visual.transform(),npc.visual.pose());
  for(size_t i=0;i<mesh.subCount;++i) {
    const auto& item=mesh.sub[i];
    if(item.isEmpty()) continue;
    const auto& bounds=item.bounds(); const auto& material=item.material();
    if(item.meshSlice().second>0 && std::isfinite(bounds.rConservative) && bounds.rConservative>0 && material.tex!=nullptr &&
       !material.tex->isEmpty() && std::isfinite(material.alphaWeight) && material.alphaWeight>0 && !material.isGhost)
      return true;
    }
  return false;
  }

void TestBench::aiLine(std::string_view npc, std::string_view name, std::string_view text, uint64_t ms,
                       std::string_view to, bool filler) {
  if(scenarioPath==nullptr)
    return;
  bench.lastLineAt = std::max<uint64_t>(bench.time,1);
  bench.busyAt     = bench.time;
  event("npc_line","\"npc\":" + jsonStr(npc) + ",\"name\":" + jsonStr(name) + ",\"text\":" + jsonStr(text) +
                   ",\"ms\":" + std::to_string(ms) + ",\"to\":" + jsonStr(to) + (filler ? ",\"filler\":true" : ""));
  say("npc line " + std::string(npc) + (to.empty() ? "" : " -> " + std::string(to)) + ": " + std::string(text));
  }

void TestBench::npcOutput(const Npc& npc, std::string_view name, const Npc* target, uint64_t ms) {
  World* w = Gothic::inst().world();
  if(scenarioPath==nullptr || !bench.started || bench.done || w==nullptr)
    return;
  const auto who    = symName(*w,&npc);
  const auto whom   = symName(*w,target);
  const auto policy = npc.processPolicy();
  say("output " + who + " -> " + whom + " " + std::string(name) + " " + std::to_string(ms) +
      (policy==NpcProcessPolicy::AiNormal || policy==NpcProcessPolicy::Player ? "" : std::string(" ") + policyName(policy)));
  event("output","\"npc\":" + jsonStr(who) + ",\"target\":" + jsonStr(whom) + ",\"name\":" + jsonStr(name) +
                 ",\"ms\":" + std::to_string(ms) + ",\"ai\":" + jsonStr(policyName(policy)));
  }

void TestBench::aiVoiceEnd(std::string_view npc, uint64_t ms) {
  if(scenarioPath==nullptr)
    return;
  bench.busyAt = bench.time;
  event("npc_voice_end","\"npc\":" + jsonStr(npc) + ",\"ms\":" + std::to_string(ms));
  }

void TestBench::aiHeard(std::string_view text, std::string_view to) {
  if(scenarioPath==nullptr)
    return;
  event("heard","\"text\":" + jsonStr(text) + ",\"to\":" + jsonStr(to));
  say("heard: " + std::string(text) + " -> " + std::string(to));
  }


void TestBench::sampleStreaming(const World& world) {
  if(scenarioPath==nullptr || bench.done)return;
  const bool measuring=bench.world.perfSegment.starts_with("streaming-flythrough/");
  const bool shots=std::filesystem::path(scenarioPath).filename()=="streaming-shots.bench";
  if(!measuring && !shots && std::filesystem::path(scenarioPath).filename()!="streaming-flythrough.bench")return;
  const auto* residency=world.chunkResidency();
  if(residency==nullptr)return;
  const auto& stats=residency->stats();
  if(shots && !bench.shot.empty())
    event("streaming_shot","\"shot\":"+jsonStr(bench.shot)+
      ",\"vob_near\":"+std::to_string(stats.vobNear)+
      ",\"vob_near_missing\":"+std::to_string(stats.vobNearMissing)+
      ",\"vob_pending\":"+std::to_string(stats.vobPending));
  if(!measuring && !shots) {
    bench.streamingWarmupFarMs.insert(bench.streamingWarmupFarMs.end(),stats.farPublicationTimesMs.begin(),stats.farPublicationTimesMs.end());
    bench.streamingWarmupFarMaxPerFrame=std::max(bench.streamingWarmupFarMaxPerFrame,stats.farPublicationTimesMs.size());
    bench.streamingWarmupQueueMaxMs=std::max(bench.streamingWarmupQueueMaxMs,stats.workMs);
    return;
    }
  bench.streamingSamples.push_back({bench.world.perfSegment,bench.world.perfFrame,stats.updateMs,stats.workMs,
    stats.near,stats.far,stats.outside,stats.transitionsNear-bench.world.streamingNearBefore,
    stats.transitionsFar-bench.world.streamingFarBefore,stats.publicationTimesMs,
    stats.published-bench.world.streamingPublishedBefore,stats.released-bench.world.streamingReleasedBefore,
    stats.landscapePageUsedBytes,stats.landscapePageFreeBytes,stats.landscapePageRetiredBytes,stats.landscapePageCount,
    Resources::textureStats(),stats.vobNear,stats.vobFar,stats.vobOutside,stats.vobPending,stats.vobNearMissing,
    stats.vobCreated-bench.world.streamingVobCreatedBefore,stats.vobReleased-bench.world.streamingVobReleasedBefore,
    stats.vobPublicationMs,stats.vobNearMissingSeconds-bench.world.streamingVobMissingBefore});
  const auto& tiers=world.npcSimTierStats();
  auto& sample=bench.streamingSamples.back();
  sample.npcFull=tiers.full;
  sample.npcSimplified=tiers.simplified;
  // Transitions since the previous sample, like near_transitions and far_transitions.
  sample.npcTierTransitions=tiers.toFull+tiers.toSimplified-bench.world.streamingNpcTransitionsBefore;
  bench.world.streamingNpcTransitionsBefore=tiers.toFull+tiers.toSimplified;
  sample.npcFullTickMs=tiers.fullTickMs;
  sample.npcSimplifiedTickMs=tiers.simplifiedTickMs;
  bench.world.streamingNearBefore=stats.transitionsNear;bench.world.streamingFarBefore=stats.transitionsFar;
  bench.world.streamingPublishedBefore=stats.published;bench.world.streamingReleasedBefore=stats.released;
  bench.world.streamingVobCreatedBefore=stats.vobCreated;bench.world.streamingVobReleasedBefore=stats.vobReleased;
  }

bool TestBench::withUi() {
  return isActive() && bench.ui;
  }

bool TestBench::ragdollLootOpened(const Npc& player, const Npc& target) {
  if((!ragdollTrace && !bench.water.bodiesReady) || !bodyTraceAlive || scenarioPath==nullptr || !bench.started || bench.done || bench.ragdoll.expectedRagdollLoot!=&target)
    return false;
  auto* world=Gothic::inst().world();
  if(world==nullptr)
    return false;
  bench.ragdoll.ragdollLootDidOpen=true;
  event("ragdoll_loot_open","\"player_actor_id\":"+std::to_string(world->npcId(&player))+
        ",\"target_actor_id\":"+std::to_string(world->npcId(&target))+",\"state\":\"Ransack\",\"close\":\"synchronous fixture after actual open\"");
  return true;
  }


void TestBench::capture(Pixmap& frame, float uiScale) { testbench_detail::capture(cameraContext(),frame,uiScale); }

void TestBench::monsterCharacterCollision(const Npc& actor, const Npc* target) {
  auto& route=bench.monster.monsterCharacterRoute;
  if(scenarioPath==nullptr || !route.active || route.npc!=&actor)
    return;
  ++route.callbacks;
  event("monster_character_collision","\"actor\":"+std::to_string(actor.world().npcId(&actor))+
    ",\"target\":"+(target!=nullptr ? std::to_string(actor.world().npcId(target)) : "null"));
  }

void TestBench::arrowAt(const void* arrow, float x, float y, float z) {
  if(scenarioPath!=nullptr && bench.started && !bench.done && bench.cloth.clothTrace && arrow==bench.cloth.clothArrow && bench.time<=bench.cloth.clothShotAt+2000) {
    const Vec3 point(x,y,z);
    const auto downstream=Vec3::dotProduct(point-bench.cloth.clothShotTarget,-bench.cloth.clothShotNormal);
    bench.cloth.clothArrowDownstream=std::max(bench.cloth.clothArrowDownstream,downstream);
    event("cloth_arrow_trajectory","\"position_cm\":"+ragdollVector(point)+
      ",\"downstream_cm\":"+ragdollNumber(downstream));
    }
  if(scenarioPath==nullptr || !bench.trailOn)
    return;
  const Vec3 p = {x,y,z};
  // the same arrow if it moved within 200 ms (a new arrow may get the place of an old one in memory)
  for(auto& tr:bench.trails) {
    if(tr.key!=arrow || bench.time>tr.last+200)
      continue;
    if(tr.pts.empty() || (tr.pts.back()-p).length()>2.f) {
      if(tr.pts.size()<4000)
        tr.pts.push_back(p);
      }
    tr.last = bench.time;
    return;
    }
  // drop the ones long gone, start this one
  std::erase_if(bench.trails,[](const Bench::Trail& t){ return bench.time>t.last+bench.trailKeep; });
  bench.trails.push_back({arrow,{p},bench.time});
  }
