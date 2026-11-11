#include "testbench_internal.h"
#include "testbench.h"
#include "texturedump.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

#include "appcadence.h"
#include "frameprofile.h"
#include "game/gamescript.h"
#include "gothic.h"
#include "graphics/mesh/footik.h"
#include "graphics/mesh/skeleton.h"
#include "loadprobe.h"
#include "world/aiqueue.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "world/chunkresidency.h"

// resources.cpp: texture residency of a deterministic bench (`deterministic on`)
extern bool benchTexturesSettle;
extern bool benchTexturesFrozen;

using namespace Tempest;
using namespace testbench_detail;

namespace {

// The migration oracle deliberately uses the public game API. A generated input file is replayed
// verbatim by the other backend: even waypoint ground adjustment must not change the queries.
void physicsDump(World& w, const Npc& pl, size_t count, uint32_t seed, const std::string& input) {
  struct Query { Vec3 from, to; float radius=0; };
  std::vector<Query> queries;
  if(!input.empty()) {
    std::ifstream file(input);
    Query q;
    while(file >> q.from.x >> q.from.y >> q.from.z >> q.to.x >> q.to.y >> q.to.z >> q.radius)
      queries.push_back(q);
    if(queries.empty() || !file.eof()) {
      say("physics dump failed: malformed or empty query file " + input);
      return;
      }
    } else {
    std::vector<Spot> points;
    w.findWayPoint(pl.position(),[&](const WayPoint& wp) {
      Spot point;
      point.name=wp.name;
      point.pos=wp.pos;
      points.push_back(point);
      return false;
      });
    std::sort(points.begin(),points.end(),[](const Spot& a, const Spot& b) { return a.name<b.name; });
    if(points.empty()) {
      say("physics dump failed: no waypoints");
      return;
      }
    auto next = [&]() {
      seed ^= seed<<13;
      seed ^= seed>>17;
      seed ^= seed<<5;
      return seed;
      };
    if(seed==0) seed=1;
    auto random = [&](float lo, float hi) { return lo+(hi-lo)*(float(next()>>8)/16777216.f); };
    queries.reserve(count);
    for(size_t i=0;i<count;++i) {
      const auto& point=points[next()%points.size()];
      Query q;
      q.from=point.pos+Vec3(random(-500,500),random(50,1500),random(-500,500));
      if(i%4<2) {
        // Vertical ground probes cover the directions used by Foot IK and locomotion.
        q.to=q.from-Vec3(0,random(500,4000),0);
        }
      else if(i%4==2) {
        q.to=q.from+Vec3(random(-5000,5000),random(-1000,1000),random(-5000,5000));
        }
      else {
        q.to=points[next()%points.size()].pos+Vec3(random(-500,500),random(50,1500),random(-500,500));
        }
      static constexpr float radii[]={0.1f,1.f,5.f,25.f,50.f,100.f};
      q.radius=radii[next()%6];
      queries.push_back(q);
      }
    say("physics dump waypoints " + std::to_string(points.size()));
    }
  std::FILE* queryFile=std::fopen((outDir+"/physics-queries.tsv").c_str(),"w");
  std::FILE* resultFile=std::fopen((outDir+"/physics-results.jsonl").c_str(),"w");
  if(queryFile==nullptr || resultFile==nullptr) {
    if(queryFile!=nullptr) std::fclose(queryFile);
    if(resultFile!=nullptr) std::fclose(resultFile);
    say("physics dump failed: cannot create output files");
    return;
    }
  for(const auto& q:queries)
    std::fprintf(queryFile,"%.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",double(q.from.x),double(q.from.y),double(q.from.z),
                 double(q.to.x),double(q.to.y),double(q.to.z),double(q.radius));
  std::fclose(queryFile);
  for(int kind=0;kind<2;++kind) {
    const char* name=kind==0 ? "ray" : "sphere";
    auto cast = [&](const Query& q) { return kind==0 ? w.physic()->ray(q.from,q.to) : w.physic()->sweep(q.from,q.to,q.radius); };
    for(size_t i=0;i<std::min<size_t>(4096,queries.size());++i)
      (void)cast(queries[i]);
    std::vector<double> batchTimes;
    // Timing batches avoids the clock overhead dominating cheap individual queries. JSON/file IO
    // happens outside the timed block, and every batch has the same mixture in both backends.
    constexpr size_t batchSize=256;
    for(size_t begin=0;begin<queries.size();begin+=batchSize) {
      const size_t end=std::min(begin+batchSize,queries.size());
      std::vector<DynamicWorld::RayLandResult> results(end-begin);
      const auto start=std::chrono::steady_clock::now();
      for(size_t i=begin;i<end;++i)
        results[i-begin]=cast(queries[i]);
      const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
      batchTimes.push_back(double(elapsed)/double(end-begin));
      for(size_t i=begin;i<end;++i) {
        const auto& r=results[i-begin];
        const std::string sector=jsonStr(r.sector!=nullptr ? r.sector : "");
        std::fprintf(resultFile,"{\"kind\":\"%s\",\"id\":%zu,\"hit\":%s,\"point\":[%.9g,%.9g,%.9g],\"normal\":[%.9g,%.9g,%.9g],\"material\":%u,\"sector\":%s,\"fraction\":%.9g}\n",
                     name,i,r.hasCol ? "true" : "false",r.hasCol ? double(r.v.x) : 0.0,r.hasCol ? double(r.v.y) : 0.0,r.hasCol ? double(r.v.z) : 0.0,
                     double(r.n.x),double(r.n.y),double(r.n.z),unsigned(r.mat),sector.c_str(),double(r.hitFraction));
        }
      }
    std::sort(batchTimes.begin(),batchTimes.end());
    if(!batchTimes.empty()) {
      const auto percentile=[&](double p) { return batchTimes[size_t(p*double(batchTimes.size()-1))]; };
      say(std::string("physics timing ")+name+" count "+std::to_string(queries.size())+
          " batch 256 ns/query p50 "+std::to_string(percentile(0.5))+" p95 "+std::to_string(percentile(0.95)));
      }
    }
  std::fclose(resultFile);
  say("physics dump complete " + std::to_string(queries.size()) + " rays and spheres");
  }

}

namespace testbench_detail {

bool worldCommand(const WorldContext& context,const std::string& line,const std::vector<std::string>& a,
                  World& w,Npc& pl,PlayerControl& ctrl) {
  auto& state=context.world;
  const auto& c=a[0];
  if(c=="physics" && a.size()>1 && a[1]=="dump") {
    physicsDump(w,pl,size_t(std::clamp(num(a,2,100000),1.f,1000000.f)),uint32_t(num(a,3,73013)),a.size()>4 ? a[4] : "");
    }
  else if(c=="physics" && a.size()==2 && a[1]=="flags-check") {
    struct Flag {const char* key;bool Gothic::PhysicsFlags::*member;};
    const Flag flags[]{
      {"ragdoll",&Gothic::PhysicsFlags::ragdoll},
      {"ragdoll_quest_guard",&Gothic::PhysicsFlags::questGuard},
      {"living_bodies",&Gothic::PhysicsFlags::livingBodies},
      {"arrow_physics",&Gothic::PhysicsFlags::arrowPhysics},
      {"arrow_sticking",&Gothic::PhysicsFlags::arrowSticking},
      {"spell_physics",&Gothic::PhysicsFlags::spellPhysics},
      {"body_impact_damage",&Gothic::PhysicsFlags::bodyImpactDamage},
      {"body_traces",&Gothic::PhysicsFlags::bodyTraces},
      {"blood",&Gothic::PhysicsFlags::blood},
      {"movable_vobs",&Gothic::PhysicsFlags::movableVobs},
      {"item_hulls",&Gothic::PhysicsFlags::itemHulls},
      {"item_deepest_contact",&Gothic::PhysicsFlags::itemDeepestContact},
      {"ragdoll_birth_guard",&Gothic::PhysicsFlags::ragdollBirthGuard},
      {"furniture_parts",&Gothic::PhysicsFlags::furnitureParts},
      {"placed_items",&Gothic::PhysicsFlags::placedItems},
      {"cloth",&Gothic::PhysicsFlags::cloth}
      };
    auto& npc=context.subjectOf(pl);
    const auto tick=w.tickCount();
    bool pass=true;
    for(const auto& flag:flags) {
      const int original=Gothic::settingsGetI("GAME",flag.key);
      pass&=(Gothic::physicsFlags().*flag.member)==(original!=0);
      for(const std::string_view phase:{"off","on","restore"}) {
        const int value=phase=="restore" ? original : int(phase=="on");
        Gothic::settingsSetI("GAME",flag.key,value);
        const bool immediate=Gothic::physicsFlags().*flag.member;
        npc.prepareRagdoll();npc.prepareLivingBody();
        const int configured=Gothic::settingsGetI("GAME",flag.key);
        const bool cached=Gothic::physicsFlags().*flag.member;
        const bool same=w.tickCount()==tick;
        const bool ok=immediate==(value!=0) && cached==(configured!=0) && configured==value && same;
        pass&=ok;
        event("physics_flags_check","\"flag\":"+jsonStr(flag.key)+",\"phase\":"+jsonStr(phase)+
          ",\"original\":"+std::to_string(original)+",\"configured\":"+std::to_string(configured)+
          ",\"cached\":"+(cached?"true":"false")+",\"prepared\":true,\"same_dispatch\":"+(same?"true":"false")+
          ",\"pass\":"+(ok?"true":"false"));
        }
      }
    if(!pass) arenaAbort("physics flag snapshot differs in same dispatch");
    }
  else if(c=="physics" && a.size()==2 && a[1]=="stats") {
    const auto stats=w.physic()->physicsStats();
    char text[200];
    std::snprintf(text,sizeof(text),"physics stats step_ms=%.6f bodies=%zu rays=%llu",
                  double(stats.stepMilliseconds),stats.bodies,(unsigned long long)stats.rays);
    say(text);
    }
  else if(c=="world-data" && a.size()==2 && (a[1]=="prepare" || a[1]=="check")) {
    Interactive* door=nullptr;
    for(uint32_t id=0;auto* mob=w.mobsiById(id);++id)
      if(mob->isDoor() && mob->stateCount()>0 && (!door || mob->zenId()<door->zenId()))
        door=mob;
    if(!door) {arenaAbort("world-data needs an authored door with an open state");return true;}
    if(a[1]=="prepare") {
      door->setAsCracked(true);
      door->resetPositionToTA(1);
      }
    const bool pass=door->stateId()==1 && door->isCracked() && !door->isLocked();
    event("world_data_door","\"operation\":"+jsonStr(a[1])+",\"pass\":"+(pass?"true":"false")+
      ",\"zen_id\":"+std::to_string(door->zenId())+",\"mob_id\":"+std::to_string(w.mobsiId(door))+
      ",\"state\":"+std::to_string(door->stateId())+",\"cracked\":"+(door->isCracked()?"true":"false")+
      ",\"locked\":"+(door->isLocked()?"true":"false")+
      ",\"world_load_ms\":"+ragdollNumber(float(w.loadMilliseconds())));
    if(!pass)arenaAbort("world-data door lost its open or cracked state");
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="cadence" && a.size()==2 && a[1]=="normal") {
    AppCadence::enable(outDir+"/appcadence.csv");FootIk::setProbing(false);
    }
  else if(c=="cadence" && a.size()==2 && a[1]=="check") {
    uint32_t models=0,dead=0,parts=0;
    const auto* skeleton=context.crew.empty() ? nullptr : context.crew.front()->visualSkeleton();
    for(const auto* npc:context.crew) {
      if(skeleton!=nullptr && npc->visualSkeleton()==skeleton && TestBench::ragdollBodyDrawable(*npc)) ++models;
      if(npc->isDead()) ++dead;
      if(const auto* body=npc->ragdollBody()) parts+=uint32_t(body->partCount());
      }
    if(context.crew.size()!=20 || models!=20 || dead!=0 || parts!=0) { arenaAbort("cadence requires twenty loaded live humans");return true; }
    state.cadenceVerified=true;AppCadence::counts(true,models,dead,parts,0,0);
    FrameProfile::setActors(context.crew);
    }
  else if(c=="cadence" && a.size()==3 && a[1]=="die") {
    if(!state.cadenceVerified || !AppCadence::normalClock() || (a[2]!="on" && a[2]!="off") || !AppCadence::arm(a[2]=="on")) { arenaAbort("cadence death start failed");return true; }
    for(auto* npc:context.crew) { npc->setLastHitBy(&pl);if(npc->setRagdollDown(true)) ++state.cadenceDown;npc->prepareRagdoll(); }
    }
  else if(c=="cadence" && a.size()==2 && a[1]=="wait") state.cadenceWait=true;
  else if(c=="cadence" && a.size()==2 && a[1]=="report") {
    uint32_t models=0,dead=0,parts=0,asleep=0;
    for(const auto* npc:context.crew) {
      if(TestBench::ragdollBodyDrawable(*npc)) ++models;
      if(npc->isDead()) ++dead;
      if(const auto* body=npc->ragdollBody()) { parts+=uint32_t(body->partCount());if(body->isSleeping())++asleep; }
      }
    AppCadence::counts(false,models,dead,parts,asleep,state.cadenceDown);
    }
#else
  else if(c=="cadence") {
    arenaAbort("cadence requires a build with OPENGOTHIC_ENABLE_APP_CADENCE=ON");
    }
#endif
  else if(c=="waitragdoll") {
    Npc* npc=a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    const std::string state=a.size()>2 ? a[2] : "";
    const float timeout=num(a,3,30000);
    if(npc==nullptr || a.size()<3 || a.size()>4 || !std::isfinite(timeout) || timeout<=0 ||
       (state!="sleep" && state!="recovery" && state!="getup" && state!="knockdown")) {
      say("waitragdoll failed: expected <npc> sleep|recovery|getup|knockdown [timeout ms]");
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return true;
      }
    context.ragdoll.waitRagdollNpc=npc; context.ragdoll.waitRagdollState=state;
    context.ragdoll.waitRagdollEnd=context.time+uint64_t(std::min(timeout,300000.f));
    }
  else if(c=="standup" && a.size()==2) {
    Npc* npc=findNpc(w,pl,a[1]);
    if(npc==nullptr || npc->isDead() || !npc->isUnconscious()) {
      say("standup failed: expected an unconscious NPC");
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return true;
      }
    npc->clearAiQueue();
    npc->setExternalControl(false);
    npc->clearState(false); // The normal ZS_Unconscious End hook may enqueue AI_StandUp.
    if(npc->isAiQueueEmpty())
      npc->aiPush(AiQueue::aiStandup());
    event("ragdoll_standup","\"actor\":"+jsonStr(TestBench::who(npc))+
          ",\"t_game_ms\":"+std::to_string(w.tickCount())+
          ",\"control\":\"normal unconscious End hook and native AI_StandUp after sleep\"");
    say("standup "+symName(w,npc));
    }
  else if(c=="crowd" && a.size()==3) {
    const size_t instance=w.script().findSymbolIndex(a[1]);
    const float count=num(a,2,0);
    if(instance==size_t(-1) || count!=20 || !context.crew.empty()) {
      say("crowd failed: expected one fresh twenty-NPC fixture");
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return true;
      }
    const Vec3 center=pl.position()+facing(pl)*1000.f;
    for(int i=0;i<20;++i) {
      Vec3 at=context.ragdoll.ragdollArenaPlaces.size()==20 ? context.ragdoll.ragdollArenaPlaces[size_t(i)] :
              center+Vec3(float(i%5)*250.f-500.f,0,float(i/5)*250.f-375.f);
      const float ground=groundY(w,at.x,at.z,at.y+200);
      if(!std::isfinite(ground)) {
        event("ragdoll_error","\"reason\":\"twenty-NPC fixture has no ground\"");
        continue;
        }
      at.y=ground;
      Npc* npc=w.addNpc(instance,at);
      if(npc==nullptr) { event("ragdoll_error","\"reason\":\"twenty-NPC fixture insertion failed\""); continue; }
      npc->setExternalControl(true); npc->clearAiQueue(); npc->clearState(false);
      npc->setDirection(pl.position()-at); npc->updateTransform();
      context.crew.push_back(npc);
      }
    if(context.ragdoll.ragdollArenaPlaces.size()==20 && context.crew.size()!=20) { arenaAbort("arena insertion must retain all twenty actors"); return true; }
    if(context.crew.size()==20) {
      context.subject=context.crew[7];
      exec("watch watched",w,pl,ctrl);
      }
    say("crowd "+std::to_string(context.crew.size()));
    event("ragdoll_crowd","\"actors\":"+std::to_string(context.crew.size()));
    }
  else if(c=="down" && a.size()==3 && a[1]=="all" && (a[2]=="dead" || a[2]=="ko")) {
    size_t down=0;
    std::string bodies="[";
    for(auto* npc:context.crew) {
      npc->setLastHitBy(&pl);
      if(npc->setRagdollDown(a[2]=="dead")) ++down;
      npc->prepareRagdoll();
      if(bodies.size()>1) bodies+=",";
      const auto* body=npc->ragdollBody();
      const auto* skeleton=npc->visualSkeleton();
      bodies+="{\"actor_id\":"+std::to_string(w.npcId(npc))+",\"symbol\":"+jsonStr(symName(w,npc))+
              ",\"skeleton\":"+jsonStr(skeleton!=nullptr ? skeleton->name() : std::string_view())+
              ",\"part_count\":"+std::to_string(body!=nullptr ? body->partCount() : 0)+",\"bone_indices\":[";
      if(body!=nullptr)
        for(size_t i=0;i<body->boneIndices().size();++i)
          bodies+=(i>0 ? "," : "")+std::to_string(body->boneIndices()[i]);
      bodies+="]}";
      }
    event("ragdoll_down_bulk","\"actors\":"+std::to_string(context.crew.size())+",\"down\":"+std::to_string(down)+",\"bodies\":"+bodies+"]");
    say("down all "+std::to_string(down));
    }
  else if(c=="down") {
    Npc* npc=a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    if(a.size()!=3 || npc==nullptr || (a[2]!="dead" && a[2]!="ko")) {
      say("down failed: expected down <npc> dead|ko: "+line);
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return true;
      }
    const bool inAirBefore=npc->isInAir();
    if(npc!=&pl)
      npc->setLastHitBy(&pl);
    npc->setRagdollDown(a[2]=="dead");
    npc->prepareRagdoll();
    say("down "+symName(w,npc)+" "+a[2]+(npc->isDown() ? "" : " failed"));
    event("ragdoll_down","\"actor\":"+jsonStr(TestBench::who(npc))+",\"death\":"+
          (a[2]=="dead" ? "true" : "false")+",\"down\":"+(npc->isDown() ? "true" : "false")+
          ",\"in_air_before\":"+(inAirBefore ? "true" : "false")+
          ",\"parts\":"+std::to_string(npc->ragdollBody()!=nullptr ? npc->ragdollBody()->partCount() : 0));
    }
  else if(c=="push") {
    Npc* npc=a.size()>1 ? findNpc(w,pl,a[1]) : nullptr;
    const Vec3 direction(num(a,2,0),num(a,3,0),num(a,4,0));
    const float magnitude=num(a,5,0), length=direction.length();
    const bool numbers=a.size()==6 && isNumber(a[2]) && isNumber(a[3]) && isNumber(a[4]) && isNumber(a[5]);
    if(!numbers || npc==nullptr || npc->ragdollBody()==nullptr || !std::isfinite(length) || length<1e-6f ||
       !std::isfinite(magnitude) || magnitude<0) {
      say("push failed: expected a physical body and finite direction/impulse: "+line);
      event("ragdoll_error","\"command\":"+jsonStr(line));
      return true;
      }
    const Vec3 impulse=direction*(magnitude/length);
    npc->addRagdollImpulse(impulse);
    say("push "+symName(w,npc)+" impulse="+ragdollVector(impulse));
    event("ragdoll_push","\"actor\":"+jsonStr(TestBench::who(npc))+",\"impulse_ns\":"+ragdollVector(impulse));
    }
  else if(c=="world") {
    if(a.size()<2 || a.size()>3 || !Resources::hasWorld(a[1])) {
      arenaAbort("world expects an indexed ZEN and an optional waypoint");
      return true;
      }
    say("world change requested: "+a[1]);
    w.triggerChangeWorld(a[1],a.size()==3 ? std::string_view(a[2]) : std::string_view{});
    // Yield until the next native tick schedules loading; tick() then waits for its completion.
    context.waitUntil = context.time+1;
    }
  else if(c=="world-check") {
    if(a.size()!=2 || !std::equal(w.name().begin(),w.name().end(),a[1].begin(),a[1].end(),[](char x,char y) {
        return std::toupper(static_cast<unsigned char>(x))==std::toupper(static_cast<unsigned char>(y));
        })) {
      arenaAbort("world-check did not reach the requested world");
      return true;
      }
    say("world check passed: "+a[1]);
    event("world_check","\"world\":"+jsonStr(w.name())+",\"pass\":true");
    }
  else if(c=="perf") {
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE && defined(OPENGOTHIC_ENABLE_APP_CADENCE)
    if(a.size()==3 && a[1]=="sample" && a[2].size()<64 && AppCadence::normalClock()) {
      if(!state.perfArmed) {
        if(!AppCadence::arm(false)) {arenaAbort("perf sample could not arm app cadence");return true;}
        state.perfArmed=true;
        }
      state.perfSegment=a[2];state.perfFrame=0;
      state.streamingTexturesBefore=Resources::textureStats();
      if(auto* residency=w.chunkResidency()) {
        state.streamingNearBefore=residency->stats().transitionsNear;
        state.streamingFarBefore=residency->stats().transitionsFar;
        state.streamingPublishedBefore=residency->stats().published;
        state.streamingReleasedBefore=residency->stats().released;
        state.streamingVobCreatedBefore=residency->stats().vobCreated;
        state.streamingVobReleasedBefore=residency->stats().vobReleased;
        state.streamingVobMissingBefore=residency->stats().vobNearMissingSeconds;
        state.streamingNpcTransitionsBefore=w.npcSimTierStats().toFull+w.npcSimTierStats().toSimplified;
        }
      FrameProfile::setActors(context.crew);
      event("perf_segment","\"segment\":"+jsonStr(state.perfSegment)+
        ",\"camera_cm\":"+ragdollVector(context.camPos)+",\"actors\":"+std::to_string(context.crew.size())+
        ",\"residency_manager\":"+(w.chunkResidency()!=nullptr ? "true" : "false"));
      }
    else if(a.size()==2 && a[1]=="stop") {
      if(!state.perfSegment.empty())
        TextureDump::write(state.perfSegment,w.view());
      if(state.perfSegment.starts_with("streaming-flythrough/")) {
        const auto textures=Resources::textureStats();
        char textureSeconds[48], shortfallSeconds[48];
        std::snprintf(textureSeconds,sizeof(textureSeconds),"%.9g",textures.nearTailSeconds-state.streamingTexturesBefore.nearTailSeconds);
        std::snprintf(shortfallSeconds,sizeof(shortfallSeconds),"%.9g",
                      textures.shortfallSeconds-state.streamingTexturesBefore.shortfallSeconds);
        event("streaming_end","\"segment\":"+jsonStr(state.perfSegment)+
          ",\"gpu_mb\":"+std::to_string(double(tempestProbeGpuBytes())/1048576.0)+
          ",\"tex_promoted\":"+std::to_string(textures.promoted-state.streamingTexturesBefore.promoted)+
          ",\"tex_demoted\":"+std::to_string(textures.demoted-state.streamingTexturesBefore.demoted)+
          ",\"tex_near_tail_seconds\":"+textureSeconds+",\"tex_shortfall_seconds\":"+shortfallSeconds);
        }
      state.perfSegment.clear();
      }
    else arenaAbort("perf expects sample <segment under 64 bytes> or stop, after cadence normal");
#else
    arenaAbort("perf requires OPENGOTHIC_ENABLE_FRAME_PROFILE=ON and OPENGOTHIC_ENABLE_APP_CADENCE=ON");
#endif
    }
  else if(c=="streaming") {
    if(a.size()<2 || a.size()>3 || a[1]!="wait" || (a.size()==3 && !isNumber(a[2]))) {
      arenaAbort("streaming expects wait [timeout_ms]");
      return true;
      }
    const float timeout=num(a,2,60000);
    if(!std::isfinite(timeout) || timeout<=0 || timeout>300000) {
      arenaAbort("streaming wait timeout must be in (0, 300000] ms");
      return true;
      }
    state.waitStreaming=true;
    state.streamingDeadline=std::chrono::steady_clock::now()+
      std::chrono::milliseconds(uint64_t(timeout));
    // Camera and desired residency are updated after this bench tick.
    context.waitUntil=context.time+1;
    }
  else if(c=="deterministic") {
    if(a.size()!=2 || (a[1]!="on" && a[1]!="off")) {
      arenaAbort("deterministic expects on or off");
      return true;
      }
    state.deterministic=a[1]=="on";
    benchTexturesSettle=state.deterministic;
    benchTexturesFrozen=false;
    say("deterministic "+a[1]);
    }
  else return false;
  return true;
  }

}
