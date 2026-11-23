#include "stucklogger.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gothic.h"
#include "jsonwriter.h"
#include "stuckprogress.h"
#include "world/benchhooks.h"
#include "textcodec.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

namespace {
constexpr uint64_t SampleEveryMs = 250;

StuckProgress::Point point(const Tempest::Vec3& p) { return {p.x,p.y,p.z}; }

std::string symbol(World& world, const Npc& npc) {
  if(auto* sym = world.script().findSymbol(npc.instanceSymbol()))
    return sym->name();
  return {};
  }

void position(JsonWriter& out, std::string_view key, StuckProgress::Point p) {
  out.object(key).num("x",p.x).num("y",p.y).num("z",p.z).end();
  }
}

struct StuckLogger::Impl final {
  struct Goal {
    std::string kind, name;
    StuckProgress::Point position;
    bool same(const Goal& other) const {
      if(kind!=other.kind || name!=other.name)
        return false;
      if(!name.empty())
        return true; // the NPC being followed may itself move
      return std::abs(position.x-other.position.x)<1.f &&
             std::abs(position.y-other.position.y)<1.f && std::abs(position.z-other.position.z)<1.f;
      }
    };
  struct Walker {
    StuckProgress progress;
    Goal goal;
    std::string symbol, name, waypoint, routine;
    uint32_t instance = 0, id = 0;
    int processPolicy = 0;
    uint64_t seen = 0;
    };

  explicit Impl(const char* output):path(output),file(std::fopen(output,"a")) {}
  ~Impl() { if(file!=nullptr) std::fclose(file); }

  bool begin(World& world, std::string_view name, float range) {
    if(file==nullptr || world.player()==nullptr || !std::isfinite(range) || range<=0)
      return false;
    // Deliberately discard warm-up candidates: every observation starts with a fresh 3 s window.
    if(observing)
      finish("observation_restarted");
    walkers.clear();
    uniqueWalkers.clear();
    label = name;
    center = point(world.player()->position());
    radius = range;
    start = now = world.tickCount();
    startGame = gameTime = world.time().toInt();
    worldName = world.name();
    lastWorld = &world;
    nextSample = 0;
    observing = true;
    paused = false;
    events = censoredEvents = 0;
    return true;
    }

  void tick(uint64_t) {
    auto* world = Gothic::inst().world();
    if(world==nullptr || paused)
      return;
    if(lastWorld!=world || world->tickCount()<now) {
      finish("world_changed");
      if(!begin(*world,"gameplay",StuckLogger::NativeAiRadiusCm))
        return;
      // In normal play the hero can move; radius restriction is only for a named observation.
      fixedCenter = false;
      }
    now = world->tickCount();
    gameTime = world->time().toInt();
    if(now<nextSample)
      return;
    nextSample = now+SampleEveryMs;
    for(uint32_t id=0; id<world->npcCount(); ++id) {
      auto* npc = world->npcById(id);
      if(npc==nullptr)
        continue;
      const auto p = npc->position();
      const float dx = p.x-center.x, dy = p.y-center.y, dz = p.z-center.z;
      const bool inRange = !fixedCenter || dx*dx+dy*dy+dz*dz<=radius*radius;
      const auto* activity = npc->interactive();
      const bool occupied = activity!=nullptr && !activity->isLadder() && !activity->isTrueDoor(*npc);
      const bool dialog = Gothic::inst().isNpcInDialogFn && Gothic::inst().isNpcInDialog(*npc);
      // A queued destination may already be visible while an uninterruptible
      // drink/eat animation finishes. Its root sway is not attempted walking.
      const bool itemInteraction = (npc->bodyState() & BS_MAX)==(BS_ITEMINTERACT & BS_MAX);
      const bool eligible = inRange && !occupied && !dialog && !itemInteraction &&
                            !npc->isPlayer() && !npc->isDown() && !npc->isTalk() &&
                            npc->talkPartner()==nullptr && !npc->isAttack() && npc->target()==nullptr &&
                            npc->moveHint()!=Npc::GT_Flee && npc->moveHint()!=Npc::GT_Enemy &&
                            npc->moveHint()!=Npc::GT_EnemyG;
      if(!eligible) {
        auto it = walkers.find(npc);
        if(it!=walkers.end()) {
          emit(it->second,!inRange ? "out_of_range" : "interrupted",!inRange);
          walkers.erase(it);
          }
        continue;
        }
      const auto navigation = npc->navigationGoal();
      if(!navigation.active) {
        auto it = walkers.find(npc);
        if(it!=walkers.end()) {
          emit(it->second,"movement_ended",false);
          walkers.erase(it);
          }
        continue;
        }
      Goal goal{std::string(navigation.kind),std::string(navigation.name),point(navigation.position)};
      if(navigation.npc!=nullptr)
        goal.name = symbol(*world,*navigation.npc);
      auto it = walkers.find(npc);
      if(it!=walkers.end() && (!it->second.goal.same(goal) || it->second.instance!=npc->instanceSymbol())) {
        emit(it->second,"goal_changed",false);
        walkers.erase(it);
        }
      auto& walker = walkers[npc];
      walker.goal = std::move(goal);
      walker.instance = npc->instanceSymbol();
      walker.id = id;
      walker.seen = now;
      uniqueWalkers.insert(npc);
      const auto change = walker.progress.update(now,point(p),gameTime);
      if(change==StuckProgress::Detected) {
        walker.symbol = symbol(*world,*npc);
        walker.name = TextCodec::toUtf8(npc->displayName());
        walker.processPolicy = int(npc->processPolicy());
        const auto origin = walker.progress.position();
        if(const auto* wp = world->findWayPoint(Tempest::Vec3{origin.x,origin.y,origin.z}))
          walker.waypoint = wp->name;
        if(const auto* wp = npc->currentTaPoint())
          walker.routine = wp->name;
        }
      else if(change==StuckProgress::Recovered) {
        // update() keeps the episode metadata available after reporting recovery.
        emit(walker,"moved",false,true);
        }
      }
    for(auto it=walkers.begin(); it!=walkers.end();) {
      if(it->second.seen!=now) {
        emit(it->second,"npc_removed",true);
        it = walkers.erase(it);
        } else {
        ++it;
        }
      }
    }

  void emit(Walker& walker, std::string_view reason, bool censored, bool recovered=false) {
    if(!walker.progress.isStuck() && !recovered)
      return;
    const auto duration = now-walker.progress.start();
    JsonWriter out("npc_stuck");
    out.str("sample",label).str("world",worldName).str("npc",walker.symbol)
       .str("npc_name",walker.name).integer("npc_id",walker.id).integer("npc_instance",walker.instance)
       .integer("process_policy",walker.processPolicy)
       .str("nearest_waypoint",walker.waypoint).str("routine_waypoint",walker.routine);
    position(out,"position",walker.progress.position());
    out.object("goal").str("kind",walker.goal.kind).str("name",walker.goal.name);
    position(out,"position",walker.goal.position);
    out.end().integer("start_tick_ms",int64_t(walker.progress.start()))
       .integer("detected_tick_ms",int64_t(walker.progress.detected()))
       .integer("end_tick_ms",int64_t(now)).integer("duration_ms",int64_t(duration))
       .integer("game_time_ms",walker.progress.gameTime()).integer("end_game_time_ms",gameTime)
       .str("end_reason",reason).boolean("censored",censored);
    const auto row = out.finish();
    std::fwrite(row.data(),1,row.size(),file);
    std::fputc('\n',file);
    std::fflush(file);
    if(std::ferror(file))
      Tempest::Log::e("[stuck] failed to write ",path);
    ++events;
    if(censored)
      ++censoredEvents;
    }

  void finish(std::string_view reason) {
    if(!observing)
      return;
    for(auto& entry:walkers)
      emit(entry.second,reason,true);
    walkers.clear();
    JsonWriter out("stuck_observation");
    out.str("sample",label).str("world",worldName).num("radius_cm",radius)
       .num("native_ai_radius_cm",StuckLogger::NativeAiRadiusCm)
       .integer("start_tick_ms",int64_t(start)).integer("end_tick_ms",int64_t(now))
       .integer("simulation_duration_ms",int64_t(now-start))
       .integer("start_game_time_ms",startGame).integer("end_game_time_ms",gameTime)
       .integer("unique_walkers",int64_t(uniqueWalkers.size()))
       .integer("stuck_events",int64_t(events)).integer("censored_events",int64_t(censoredEvents))
       .str("end_reason",reason);
    position(out,"center",center);
    summaries.push_back(out.finish());
    observing = false;
    writeSummary();
    }

  void writeSummary() {
    JsonWriter out("stuck_summary");
    std::string rows;
    for(const auto& row:summaries) {
      if(!rows.empty()) rows += ',';
      rows += row;
      }
    out.integer("schema_version",1).integer("window_ms",int64_t(StuckProgress::WindowMs))
       .num("progress_cm",StuckProgress::ProgressCm).num("recovery_cm",StuckProgress::RecoveryCm)
       .integer("sample_interval_ms",int64_t(SampleEveryMs)).raw("observations","["+rows+"]");
    const auto json = out.finish();
    std::ofstream summary(path+".summary.json",std::ios::trunc);
    summary << json << '\n';
    if(!summary)
      Tempest::Log::e("[stuck] failed to write summary ",path);
    }

  void loading() {
    if(Gothic::inst().checkLoading()!=Gothic::LoadState::Saving) {
      finish("world_unloaded");
      lastWorld = nullptr;
      }
    }
  void unloaded() { finish("world_unloaded"); lastWorld = nullptr; }

  static Impl* current;
  std::string path;
  std::FILE* file = nullptr;
  World* lastWorld = nullptr;
  std::string label, worldName;
  bool observing = false, paused = false, fixedCenter = false;
  float radius = StuckLogger::NativeAiRadiusCm;
  StuckProgress::Point center;
  uint64_t start = 0, now = 0, nextSample = 0, events = 0, censoredEvents = 0;
  int64_t startGame = 0, gameTime = 0;
  // World npc_id is a vector index and can change after a spawn/removal/sort. Object keys are
  // compared only; absent keys are finalized without dereferencing a removed NPC.
  std::unordered_map<const Npc*,Walker> walkers;
  std::unordered_set<const Npc*> uniqueWalkers;
  std::vector<std::string> summaries;
  };

StuckLogger::Impl* StuckLogger::Impl::current = nullptr;

std::unique_ptr<StuckLogger> StuckLogger::fromEnvironment() {
  const auto* path = std::getenv("OPENGOTHIC_STUCK_LOG");
  if(path==nullptr || path[0]=='\0')
    return nullptr;
  auto result = std::unique_ptr<StuckLogger>(new StuckLogger(path));
  if(result->impl->file==nullptr) {
    Tempest::Log::e("[stuck] cannot open ",path);
    return nullptr;
    }
  return result;
  }

StuckLogger::StuckLogger(const char* path):impl(std::make_unique<Impl>(path)) {
  if(impl->file==nullptr)
    return;
  Impl::current = impl.get();
  auto& gothic = Gothic::inst();
  gothic.onTick.bind(impl.get(),&Impl::tick);
  gothic.onStartLoading.bind(impl.get(),&Impl::loading);
  gothic.onSessionUnload.bind(impl.get(),&Impl::unloaded);
  Bench::stuckBegin = [](World& w, std::string_view label, float radius) { return beginObservation(w,label,radius); };
  Bench::stuckEnd = [](World& w) { return endObservation(w); };
  Tempest::Log::i("[stuck] logging to ",path);
  }

StuckLogger::~StuckLogger() {
  if(impl->file==nullptr)
    return;
  impl->finish("game_exit");
  auto& gothic = Gothic::inst();
  gothic.onTick.ubind(impl.get(),&Impl::tick);
  gothic.onStartLoading.ubind(impl.get(),&Impl::loading);
  gothic.onSessionUnload.ubind(impl.get(),&Impl::unloaded);
  Bench::stuckBegin = nullptr;
  Bench::stuckEnd = nullptr;
  Impl::current = nullptr;
  }

bool StuckLogger::beginObservation(World& world, std::string_view label, float radius) {
  if(Impl::current==nullptr || !Impl::current->begin(world,label,radius))
    return false;
  Impl::current->fixedCenter = true;
  return true;
  }

bool StuckLogger::endObservation(World& world) {
  if(Impl::current==nullptr || !Impl::current->observing)
    return false;
  auto& logger = *Impl::current;
  logger.now = world.tickCount();
  logger.gameTime = world.time().toInt();
  logger.finish("observation_end");
  logger.paused = true;
  return true;
  }
