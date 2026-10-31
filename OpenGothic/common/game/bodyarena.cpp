#include "bodyarena.h"
#include "bodyarenageometry.h"

#include "bodycontroller.h"
#include "bodyobservations.h"
#include "gamescript.h"
#include "gothic.h"
#include "testbench.h"
#include "graphics/mesh/pose.h"
#include "physics/dynamicworld.h"
#include "world/world.h"
#include "world/waypoint.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>
#include <stdexcept>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {
using Tempest::Vec3;
struct State final {
  BodyArena::Config config;
  bool enabled=false, ready=false, started=false, finished=false;
  bool reactionArmed=false,reactionActivated=false,reactionActivating=false,reactionHit=false;
  uint64_t reactionGame=0,reactionStep=0,reactionObservation=0,reactionActivationGame=0,reactionActivationStep=0;
  uint64_t reactionCommand=0,reactionExecution=0,reactionStartedGame=0,reactionStartedStep=0,reactionStartedEvent=0;
  std::string reactionProvider;
  uint64_t episode=0, generation=0, setupTime=0, startTime=0, distanceFrom=0, finalTime=0;
  unsigned actorCount=2;
  Npc* actors[4]={};
  uint64_t blocked[4]={}, damage[4]={};
  std::string initial, finalActors, provider, outcome="running", reason, pendingLossReason, pendingInfrastructureReason;
  std::string geometryProof = "null";
  std::function<void(std::string_view)> sink;
  };
State& state() { static State value; return value; }
// OPENGOTHIC_BODY_SCRIPT_RNG=isolated gives each arena actor its own script random stream,
// reseeded from the arena seed wherever the shared stream is: the rest of the world (live,
// frozen or other arenas) then cannot change the actors' native choices or damage rolls.
bool isolatedScriptRandom() {
  static const bool isolated = [] {
    const char* mode = std::getenv("OPENGOTHIC_BODY_SCRIPT_RNG");
    if(mode==nullptr || std::string_view(mode)=="shared")
      return false;
    if(std::string_view(mode)!="isolated")
      throw std::runtime_error("unknown OPENGOTHIC_BODY_SCRIPT_RNG mode");
    return true;
    }();
  return isolated;
  }
void reseedActors(GameScript& script) {
  auto& s=state();
  if(!isolatedScriptRandom())
    return;
  for(unsigned i=0;i<s.actorCount;++i)
    if(s.actors[i]!=nullptr)
      script.isolateRandom(*s.actors[i],s.config.seed,i);
  }
uint64_t gameTime() {
  auto world=Gothic::inst().world();
  return world==nullptr ? 0 : world->tickCount();
  }
uint64_t remaining(uint64_t until, uint64_t now) { return until>now ? until-now : 0; }
std::string quoted(std::string_view value) { return TestBench::bodyJson(value); }
std::ostringstream jsonStream() {
  std::ostringstream out;
  out.imbue(std::locale::classic()); out << std::boolalpha << std::setprecision(9);
  return out;
  }
bool customSetup() {
  const auto& config=state().config;
  return config.loadFixture || config.reactionHold || config.geometry!="open" || config.yawOffsetDeg!=0 || config.opponentYawOffsetDeg!=0 ||
         config.separationYawDeg!=0 || config.centerOffsetCm[0]!=0 || config.centerOffsetCm[1]!=0;
  }
void vectorJson(std::ostringstream& out,const Vec3& vector) { out << '[' << vector.x << ',' << vector.y << ',' << vector.z << ']'; }
}

void BodyArena::configure(const Config& config, uint64_t generation) {
  if(config.scenario!="distance" && config.scenario!="attack" && config.scenario!="block" &&
     config.scenario!="native_duel" && config.scenario!="reset_stress")
    throw std::invalid_argument("unknown arena scenario");
  if((config.weapon!="1h" && config.weapon!="2h") || (config.weapon=="2h" && config.scenario!="native_duel") ||
     config.side>1 || !std::isfinite(config.distanceCm) || config.distanceCm<80 || config.distanceCm>2000 ||
     config.timeoutMs<33 || config.timeoutMs>60000 ||
     !std::isfinite(config.yawOffsetDeg) || std::abs(config.yawOffsetDeg)>45 ||
     !std::isfinite(config.opponentYawOffsetDeg) || std::abs(config.opponentYawOffsetDeg)>45 ||
     !std::isfinite(config.separationYawDeg) || std::abs(config.separationYawDeg)>180 ||
     !std::isfinite(config.centerOffsetCm[0]) || std::abs(config.centerOffsetCm[0])>1000 ||
     !std::isfinite(config.centerOffsetCm[1]) || std::abs(config.centerOffsetCm[1])>1000 ||
     (config.geometry!="open" && config.geometry!="near_wall") ||
     (config.geometry=="near_wall" && config.separationYawDeg!=0) ||
     (config.wallClearanceCm && (config.geometry!="near_wall" || !std::isfinite(*config.wallClearanceCm) ||
                                *config.wallClearanceCm<60.f || *config.wallClearanceCm>180.f)) ||
     !BodyArenaGeometry::validTangentSelection(config.wallTangentSign,config.geometry=="near_wall",bool(config.wallClearanceCm)) ||
     (config.reactionHold && (!acceptsReactionScenario(config.scenario) || config.loadFixture)) ||
     (config.opponentOwned && (config.scenario!="native_duel" || config.loadFixture || config.reactionHold)) ||
     (config.loadFixture && (config.scenario!="native_duel" || config.geometry!="open" ||
                            config.distanceCm>400 || config.yawOffsetDeg!=0 || config.opponentYawOffsetDeg!=0 ||
                            config.separationYawDeg!=0 || config.centerOffsetCm[0]!=0 || config.centerOffsetCm[1]!=0)))
    throw std::invalid_argument("arena configuration outside bounds");
  static uint64_t nextEpisode=0;
  auto sink=std::move(state().sink);
  state()=State(); state().sink=std::move(sink);
  state().config=config; state().generation=generation; state().episode=++nextEpisode; state().enabled=true;
  state().actorCount=config.loadFixture ? 4 : 2;
  }
void BodyArena::disable() { state()=State(); }
bool BodyArena::enabled() { return state().enabled; }
bool BodyArena::frozenWorld() {
  static const bool frozen = [] {
    const char* v = std::getenv("OPENGOTHIC_FREEZE_WORLD");
    return v!=nullptr && v[0]=='1';
    }();
  return frozen && state().enabled;
  }
bool BodyArena::isActor(const Npc& npc) {
  const auto& s=state();
  for(unsigned i=0;i<s.actorCount;++i)
    if(s.actors[i]==&npc)
      return true;
  return false;
  }
bool BodyArena::ready() { return state().ready; }
bool BodyArena::started() { return state().started; }
bool BodyArena::complete() { return state().enabled && state().finished; }
bool BodyArena::reactionHeld() { return state().enabled && state().config.reactionHold && state().started && !state().reactionActivated; }
bool BodyArena::reactionArmed() { return reactionHeld() && state().reactionArmed; }
bool BodyArena::reactionConfigured() { return state().enabled && state().config.reactionHold; }
bool BodyArena::acceptsReactionScenario(std::string_view scenario) { return scenario=="block" || scenario=="reset_stress"; }
uint32_t BodyArena::seed() { return state().config.seed; }
std::vector<Npc*> BodyArena::actors() {
  if(!state().ready) return {};
  return std::vector<Npc*>(state().actors,state().actors+state().actorCount);
  }
void BodyArena::setSink(std::function<void(std::string_view)> sink) { state().sink=std::move(sink); }

void BodyArena::setup(World& world, Npc& player) {
  auto& s=state();
  if(!s.enabled || s.ready) throw std::runtime_error("arena setup requires a fresh configured reset");
  const bool twoHanded=s.config.weapon=="2h";
  const char* names[]={"BDT_1003_BANDIT_M","PAL_255_RITTER",twoHanded ? "ITMW_2H_BAU_AXE" : "ItMw_1h_Common_01","ItAr_Leather_L",
                       "ZS_ATTACK","FAI_HUMAN_MASTER","AIV_ATTACKREASON","AR_KILL"};
  size_t ids[std::size(names)]={};
  for(size_t i=0;i<std::size(names);++i) {
    ids[i]=world.script().findSymbolIndex(names[i]);
    if(ids[i]==size_t(-1)) throw std::runtime_error(std::string("arena missing symbol ")+names[i]);
    }
  const auto attackReason=world.script().findSymbol(names[6])->get_int();
  if(attackReason<0 || size_t(attackReason)>=zenkit::INpc::aivar_count)
    throw std::runtime_error("arena invalid attack reason");
  world.script().setSeed(s.config.seed);
  world.script().clearIsolatedRandom();
  const float angle=player.rotation()*float(M_PI)/180.f;
  const Vec3 forward(std::cos(angle),0,std::sin(angle)), side(-forward.z,0,forward.x);
  const auto center=player.position()+forward*(1200.f+s.config.centerOffsetCm[0])+side*s.config.centerOffsetCm[1];
  const float separation=s.config.separationYawDeg*float(M_PI)/180.f;
  const auto axis=s.config.separationYawDeg==0 ? side : side*std::cos(separation)+forward*std::sin(separation);
  Vec3 positions[2]={center-axis*(s.config.distanceCm/2),center+axis*(s.config.distanceCm/2)};
  for(auto& pos:positions) {
    DynamicWorld::RayLandResult ground;
    for(float y=pos.y+1500;y>pos.y-1500 && !ground.hasCol;y-=400)
      ground=world.physic()->ray({pos.x,y,pos.z},{pos.x,y-450,pos.z});
    if(!ground.hasCol) throw std::runtime_error("arena ground not found");
    pos.y=ground.v.y;
    }
  for(unsigned i=0;i<s.actorCount;++i) {
    const auto role=i%2;
    if(customSetup() && (positions[role]-player.position()).quadLength()>2800.f*2800.f)
      throw std::runtime_error("arena custom placement exceeds active observer radius");
    Npc* npc=world.addNpc(ids[role],positions[role^s.config.side]);
    if(npc==nullptr) throw std::runtime_error("arena NPC creation failed");
    s.actors[i]=npc;
    if(isolatedScriptRandom())
      world.script().isolateRandom(*npc,s.config.seed,i);
    npc->setExternalControl(true); npc->setInteraction(nullptr,true);
    npc->clearAiQueue(); npc->clearState(true); npc->clearGoTo(); npc->clearSpeed();
    std::vector<size_t> equipped;
    for(auto it=npc->inventory().iterator(Inventory::T_Inventory);it.isValid();++it)
      if(it.isEquipped()) equipped.push_back(it->clsId());
    for(auto item:equipped) npc->unequipItem(item);
    npc->clearInventory();
    npc->addItem(ids[2],1); npc->useItem(ids[2],Item::NSLOT,true);
    npc->addItem(ids[3],1); npc->useItem(ids[3],Item::NSLOT,true);
    auto& handle=npc->handle();
    handle.flags=zenkit::NpcFlag(uint32_t(handle.flags)&~uint32_t(zenkit::NpcFlag::IMMORTAL));
    handle.attribute[ATR_HITPOINTS]=handle.attribute[ATR_HITPOINTSMAX]=s.config.loadFixture ? 3000 : 300;
    handle.attribute[ATR_STRENGTH]=handle.attribute[ATR_DEXTERITY]=60;
    const auto talent=twoHanded ? TALENT_2H : TALENT_1H;
    handle.hitchance[talent]=60; npc->setTalentSkill(talent,2);
    handle.protection[PROT_EDGE]=handle.protection[PROT_BLUNT]=30;
    handle.guild=role==0 ? GIL_BDT : GIL_PAL;
    handle.fight_tactic=world.script().findSymbol(names[5])->get_int();
    handle.aivar[size_t(attackReason)]=world.script().findSymbol(names[7])->get_int();
    const auto sword=npc->currentMeleeWeapon();
    if(sword==nullptr || sword->is2H()!=twoHanded || npc->currentArmor()==nullptr || npc->currentRangedWeapon()!=nullptr || npc->isImmortal())
      throw std::runtime_error("arena melee profile validation failed");
    // A canonical control-save setup includes already drawn swords. This initializer has no
    // runtime command counterpart: subsequent motion, defence and damage use the normal executor.
    const_cast<Pose&>(npc->visual.pose()).stopAllAnim();
    npc->visual.setToFightMode(twoHanded ? WeaponState::W2H : WeaponState::W1H);
    npc->invent.switchActiveWeapon(*npc,1); npc->hnpc->weapon=twoHanded ? 4 : 3; npc->updateWeaponSkeleton();
    npc->waitTime=npc->aniWaitTime=npc->faiWaitTime=npc->outWaitTime=0;
    npc->fghAlgo.onClearTarget(); npc->setAnimRotate(0); npc->setAnim(Npc::Anim::Idle);
    npc->setPosition(positions[role^s.config.side]);
    }
  const auto place=[&](const Vec3& first,const Vec3& second) {
    const Vec3 physical[2]={first,second};
    for(unsigned i=0;i<2;++i) {
      auto& npc=*s.actors[i]; const auto index=i^s.config.side;
      npc.setPosition(physical[index]); npc.setDirection(physical[index^1]-physical[index]);
      npc.setDirection(npc.rotation()+(i==0 ? s.config.yawOffsetDeg : s.config.opponentYawOffsetDeg));
      npc.updateTransform();
      }
    };
  if(s.config.loadFixture) {
    // A CPU/inference-load scene has four distinct runtime actors in this one world.
    // Search only eight fixed pair layouts; setup translations occur before observation
    // installation or control acquisition, and never rescue a running policy episode.
    bool found=false;
    for(unsigned layout=0;layout<8 && !found;++layout) {
      const float a=float(layout)*float(M_PI)/4.f;
      const auto offset=(side*std::cos(a)+forward*std::sin(a))*1400.f;
      const Vec3 pairCenter[2]={center-offset,center+offset};
      Vec3 placement[4]; bool valid=true;
      for(unsigned i=0;i<4;++i) {
        const auto role=i%2, physical=role^s.config.side;
        auto pos=pairCenter[i/2]+axis*(physical==0 ? -s.config.distanceCm/2 : s.config.distanceCm/2);
        DynamicWorld::RayLandResult hit;
        for(float y=pos.y+1500;y>pos.y-1500 && !hit.hasCol;y-=400)
          hit=world.physic()->ray({pos.x,y,pos.z},{pos.x,y-450,pos.z});
        if(!hit.hasCol) { valid=false; break; }
        pos.y=hit.v.y; placement[i]=pos;
        if((pos-player.position()).quadLength()>2800.f*2800.f) { valid=false; break; }
        }
      if(!valid) continue;
      for(unsigned i=0;i<4;++i) {
        auto& npc=*s.actors[i]; npc.setPosition(placement[i]);
        npc.setDirection(placement[i^1]-placement[i]); npc.updateTransform();
        }
      float minimumCrossPair=std::numeric_limits<float>::max();
      for(unsigned i=0;i<4 && valid;++i) {
        auto& npc=*s.actors[i]; auto& target=*s.actors[i^1];
        DynamicWorld::CollisionTest free;
        if(std::abs(placement[i].y-placement[i^1].y)>50 || !npc.physic.testMove(placement[i],placement[i],free) ||
           !npc.canSeeNpc(target,false) || world.physic()->ray(npc.collosionCenter(),target.collosionCenter()).hasCol)
          valid=false;
        for(unsigned j=0;j<4;++j) if(i/2!=j/2)
          minimumCrossPair=std::min(minimumCrossPair,(placement[i]-placement[j]).length());
        }
      if(!valid || minimumCrossPair<2400) continue;
      auto proof=jsonStream();
      proof << "{\"kind\":\"four_actor_load_layout\",\"layout_index\":" << layout
            << ",\"real_actor_count\":4,\"pair_count\":2,\"hp_per_actor\":3000,\"quality_credit\":false"
            << ",\"observer_radius_limit_cm\":2800,\"cross_pair_distance_cm\":" << minimumCrossPair
            << ",\"initial_capsules_free\":true,\"grounded_placements\":true,\"pair_visibility\":true,\"melee_line_clear\":true}";
      s.geometryProof=proof.str(); found=true;
      }
    if(!found) throw std::runtime_error("no verified four-actor load layout among eight bounded candidates");
    } else if(s.config.geometry=="near_wall") {
    // Search existing collision geometry. No obstacle is fabricated and all actor relocations
    // happen only in this fresh-save initializer, before observations, ownership or game steps.
    std::vector<std::pair<Vec3,std::string>> candidates;
    world.findWayPoint(center,[&](const WayPoint& point) {
      if((point.pos-player.position()).quadLength()<=2500.f*2500.f)
        candidates.emplace_back(point.pos,point.name);
      return false;
      });
    for(unsigned ring=0;ring<5;++ring)
      for(unsigned direction=0;direction<16;++direction) {
        const float a=float(direction)*float(M_PI)/8.f;
        const auto point=center+Vec3(std::cos(a),0,std::sin(a))*float(ring*400);
        if((point-player.position()).quadLength()<=2500.f*2500.f)
          candidates.emplace_back(point,"grid-"+std::to_string(ring)+"-"+std::to_string(direction));
        }
    std::sort(candidates.begin(),candidates.end(),[&](const auto& first,const auto& second) {
      const auto a=(first.first-center).quadLength(),b=(second.first-center).quadLength();
      return a!=b ? a<b : first.second<second.second;
      });
    const auto ground=[&](Vec3 point) {
      const auto hit=world.physic()->ray({point.x,point.y+300,point.z},{point.x,point.y-400,point.z});
      if(!hit.hasCol || std::abs(hit.n.y)<0.7f) return Vec3(NAN,NAN,NAN);
      point.y=hit.v.y; return point;
      };
    bool found=false;
    size_t attempts=0;
    for(const auto& candidate:candidates) {
      if(found || attempts>=4096) break;
      const auto origin=ground(candidate.first);
      if(!std::isfinite(origin.y)) continue;
      for(unsigned direction=0;direction<16 && !found && attempts<4096;++direction) {
        ++attempts;
        const float a=float(direction)*float(M_PI)/8.f;
        const Vec3 searchToward(std::cos(a),0,std::sin(a));
        auto toward=searchToward;
        const auto rayStart=origin+Vec3(0,85,0);
        const auto wall=world.physic()->ray(rayStart,rayStart+toward*300.f);
        if(!wall.hasCol || wall.hitFraction<0.1f || wall.hitFraction>0.95f || std::abs(wall.n.y)>0.35f) continue;
        std::optional<BodyArenaGeometry::WallBasis> placementBasis;
        if(s.config.wallClearanceCm) {
          // The legacy omitted option intentionally retains its search-ray offset/tangent.
          // A requested distance instead binds the real wall plane, including for side 1.
          // Object ray hits without a reported normal cannot prove this optional contract.
          placementBasis=BodyArenaGeometry::horizontalBasis(wall.n,searchToward);
          if(!placementBasis) continue;
          toward=placementBasis->toward;
          }
        const auto first=ground(wall.v-toward*s.config.wallClearanceCm.value_or(120.f)-Vec3(0,85,0));
        if(!std::isfinite(first.y)) continue;
        const Vec3 tangent(-toward.z,0,toward.x);
        for(float sign:{1.f,-1.f}) {
          if(!BodyArenaGeometry::acceptsTangentSign(s.config.wallTangentSign,int(sign))) continue;
          const auto second=ground(first+tangent*(s.config.distanceCm*sign));
          if(!std::isfinite(second.y) || std::abs(second.y-first.y)>50.f ||
             (first-player.position()).quadLength()>2800.f*2800.f || (second-player.position()).quadLength()>2800.f*2800.f) continue;
          place(first,second);
          auto& primary=*s.actors[0]; auto& opponent=*s.actors[1];
          const auto p=primary.position();
          const auto sight=primary.canSeeNpc(opponent,false) && opponent.canSeeNpc(primary,false);
          const auto melee=world.physic()->ray(primary.collosionCenter(),opponent.collosionCenter());
          const auto actualWall=world.physic()->ray(primary.collosionCenter(),primary.collosionCenter()+toward*300.f);
          if(!sight || melee.hasCol || !actualWall.hasCol || actualWall.hitFraction<0.1f ||
             actualWall.hitFraction>0.95f || std::abs(actualWall.n.y)>0.35f) continue;
          const float clearance=actualWall.hitFraction*300.f;
          std::optional<BodyArenaGeometry::WallBasis> actualBasis;
          float normalClearance=0;
          if(s.config.wallClearanceCm) {
            actualBasis=BodyArenaGeometry::horizontalBasis(actualWall.n,toward);
            if(!actualBasis || Vec3::dotProduct(actualBasis->outward,placementBasis->outward)<0.95f) continue;
            normalClearance=BodyArenaGeometry::normalClearance(primary.collosionCenter(),actualWall.v,actualBasis->outward);
            if(!BodyArenaGeometry::acceptsClearance(*s.config.wallClearanceCm,clearance,normalClearance)) continue;
            }
          DynamicWorld::CollisionTest firstFree,secondFree,blocked,safe;
          const bool freePrimary=primary.physic.testMove(p,p,firstFree);
          const auto other=opponent.position();
          const bool freeOpponent=opponent.physic.testMove(other,other,secondFree);
          const auto blockedTo=p+toward*(actualWall.hitFraction*300.f+80.f);
          const auto safeTo=ground(p-toward*120.f);
          const bool wallMoveAllowed=primary.physic.testMove(blockedTo,p,blocked);
          const bool safeMoveAllowed=std::isfinite(safeTo.y) && std::abs(safeTo.y-p.y)<=50.f && primary.physic.testMove(safeTo,p,safe);
          if(!freePrimary || !freeOpponent || wallMoveAllowed || !blocked.landCol || std::abs(blocked.normal.y)>0.5f || !safeMoveAllowed) continue;
          auto proof=jsonStream();
          proof << "{\"kind\":\"existing_static_wall\",\"candidate\":" << quoted(candidate.second)
                << ",\"search_attempts\":" << attempts << ",\"obstacle_distance_cm\":" << actualWall.hitFraction*300.f
                << ",\"wall_ray_hit\":true,\"wall_point_cm\":";
          vectorJson(proof,actualWall.v); proof << ",\"wall_normal\":"; vectorJson(proof,actualWall.n);
          proof << ",\"wall_direction_world\":"; vectorJson(proof,toward);
          proof << ",\"blocked_probe_to_cm\":"; vectorJson(proof,blockedTo);
          proof << ",\"safe_probe_to_cm\":"; vectorJson(proof,safeTo);
          proof << ",\"safe_height_delta_cm\":" << safeTo.y-p.y
                << ",\"placement_height_delta_cm\":" << opponent.position().y-p.y;
          proof << ",\"capsule_move_blocked\":true,\"capsule_block_is_land\":true,\"capsule_block_normal\":";
          vectorJson(proof,blocked.normal);
          if(s.config.wallClearanceCm) {
            proof << ",\"requested_wall_clearance_cm\":" << *s.config.wallClearanceCm
                  << ",\"actual_wall_clearance_cm\":" << clearance
                  << ",\"wall_clearance_error_cm\":" << clearance-*s.config.wallClearanceCm
                  << ",\"wall_clearance_tolerance_cm\":20";
            proof << ",\"placement_basis\":\"wall_normal_xz\",\"search_ray_direction_world\":";
            vectorJson(proof,searchToward);
            proof << ",\"placement_wall_anchor_cm\":"; vectorJson(proof,wall.v);
            proof << ",\"placement_wall_normal_xz_world\":"; vectorJson(proof,placementBasis->outward);
            proof << ",\"placement_wall_tangent_world\":"; vectorJson(proof,tangent);
            proof << ",\"primary_collision_center_cm\":"; vectorJson(proof,primary.collosionCenter());
            proof << ",\"actual_wall_normal_xz_world\":"; vectorJson(proof,actualBasis->outward);
            proof << ",\"requested_wall_normal_clearance_cm\":" << *s.config.wallClearanceCm
                  << ",\"actual_wall_normal_clearance_cm\":" << normalClearance
                  << ",\"wall_normal_clearance_error_cm\":" << normalClearance-*s.config.wallClearanceCm
                  << ",\"wall_normal_clearance_tolerance_cm\":20";
            if(s.config.wallTangentSign)
              proof << ",\"requested_wall_tangent_sign\":" << *s.config.wallTangentSign
                    << ",\"placement_tangent_sign\":" << int(sign);
            }
          proof << ",\"initial_capsules_free\":true,\"safe_outward_move\":true,\"grounded_placements\":true,\"target_visible\":true,\"melee_line_clear\":true}";
          s.geometryProof=proof.str(); found=true; break;
          }
        }
      }
    if(!found) throw std::runtime_error("bounded arena search found no verified near-wall placement");
    } else {
    place(positions[0],positions[1]);
    }
  for(unsigned i=0;i<s.actorCount;++i) {
    auto& npc=*s.actors[i]; auto& target=*s.actors[i^1];
    npc.setTarget(&target);
    npc.setExternalControl(false);
    TestBench::bodyEvent(npc,&target,"arena_actor_mapping",world.tickCount(),
                        "\"observation_entity_id\":"+std::to_string(BodyObservations::entityId(npc))+ ",\"role\":"+
                        quoted(i%2==0 ? "primary" : "opponent")+",\"physical_side\":"+std::to_string((i%2)^s.config.side));
    }
  world.script().setSeed(s.config.seed);
  reseedActors(world.script());
  s.setupTime=world.tickCount(); s.ready=true;
  }

std::string BodyArena::actorJson(Npc& actor, unsigned i) {
  const auto now=gameTime();
  auto out=jsonStream();
  const auto& s=state(); const auto pos=actor.position();
  const auto weapon=actor.currentMeleeWeapon(); const auto armor=actor.currentArmor();
  const auto* weaponSymbol=weapon==nullptr ? nullptr : actor.world().script().findSymbol(weapon->clsId());
  // Retain the original snapshot spelling for the stock one-handed sword.
  const auto weaponName=weaponSymbol==nullptr ? std::string_view() :
                        weapon->clsId()==actor.world().script().findSymbolIndex("ItMw_1h_Common_01") ?
                        std::string_view("ItMw_1h_Common_01") : std::string_view(weaponSymbol->name());
  const auto body=actor.bodyObservationState(); const auto control=BodyController::snapshot(actor);
  const auto* target=actor.currentTarget;
  out << "{\"entity_id\":" << BodyObservations::entityId(actor)
      << ",\"session_epoch\":" << BodyObservations::sessionEpoch() << ",\"world_epoch\":" << BodyObservations::worldEpoch()
      << ",\"role\":" << quoted(i%2==0 ? "primary" : "opponent")
      << ",\"symbol\":" << quoted(i%2==0 ? "BDT_1003_BANDIT_M" : "PAL_255_RITTER")
      << ",\"physical_side\":" << ((i%2)^s.config.side)
      << ",\"hp\":" << actor.attribute(ATR_HITPOINTS) << ",\"hp_max\":" << actor.attribute(ATR_HITPOINTSMAX)
      << ",\"str\":" << actor.attribute(ATR_STRENGTH) << ",\"dex\":" << actor.attribute(ATR_DEXTERITY)
      << ",\"onehand\":" << actor.hitChance(TALENT_1H) << ",\"onehand_talent\":" << unsigned(actor.talentSkill(TALENT_1H))
      << ",\"edge_protection\":" << actor.protection(PROT_EDGE) << ",\"blunt_protection\":" << actor.protection(PROT_BLUNT)
      << ",\"weapon_symbol\":" << (weapon==nullptr ? 0 : weapon->clsId()) << ",\"weapon\":" << quoted(weaponName)
      << ",\"armor_symbol\":" << (armor==nullptr ? 0 : armor->clsId()) << ",\"armor\":" << quoted(armor==nullptr ? "" : "ItAr_Leather_L")
      << ",\"weapon_damage\":" << (weapon==nullptr ? 0 : weapon->handle().damage_total)
      << ",\"weapon_length\":" << (weapon==nullptr ? 0 : weapon->handle().range)
      << ",\"weapon_state\":" << unsigned(actor.weaponState())
      << ",\"body_state\":" << body.bodyState << ",\"pose\":" << quoted(actor.animNames())
      << ",\"interruptible\":" << body.interruptible << ",\"segment_remaining_ms\":" << body.segmentRemainingMs
      << ",\"native_eligible\":" << body.nativeEligible << ",\"attack_anim\":" << body.attackAnim
      << ",\"prehit\":" << body.prehit << ",\"defence\":" << body.defence << ",\"jump_back\":" << body.jumpBack
      << ",\"immortal\":" << actor.isImmortal() << ",\"pos\":[" << pos.x << ',' << pos.y << ',' << pos.z << ']'
      << ",\"yaw_deg\":" << actor.rotation() << ",\"target_entity_id\":" << (target==nullptr ? 0 : BodyObservations::entityId(*target))
      << ",\"external_control\":" << actor.externalCtrl
      << ",\"current_state\":" << (actor.currentState().isValid() ? actor.currentState().ptr : 0)
      << ",\"ai_queue_size\":" << actor.aiQueue.size() << ",\"ai_overlay_queue_size\":" << actor.aiQueueOverlay.size()
      << ",\"fight_queue\":[";
  bool comma=false;
  for(auto primitive:actor.fghAlgo.queueSnapshot()) { if(comma) out << ','; comma=true; out << unsigned(primitive); }
  out << "],\"primitive\":" << body.primitive << ",\"waits_ms\":{\"fai\":" << remaining(actor.faiWaitTime,now)
      << ",\"ani\":" << remaining(actor.aniWaitTime,now) << ",\"ai\":" << remaining(actor.waitTime,now)
      << ",\"output\":" << remaining(actor.outWaitTime,now) << '}'
      << ",\"down\":" << actor.isDown() << ",\"dead\":" << actor.isDead() << ",\"unconscious\":" << actor.isUnconscious()
      << ",\"owner\":" << quoted(control.owned ? "scripted" : "native")
      << ",\"control_generation\":" << control.controlGeneration << ",\"goal_version\":" << control.goalVersion
      << ",\"command_id\":" << control.commandId << ",\"execution_id\":" << control.executionId
      << ",\"release_pending\":" << control.releasePending << ",\"control_allowed\":" << control.controlAllowed
      << ",\"can_decide\":" << control.canDecide << ",\"action_mask\":[";
  for(size_t k=0;k<control.mask.size();++k) { if(k>0) out << ','; out << control.mask[k]; }
  out << "],\"blocked_hits\":" << s.blocked[i] << ",\"received_damage\":" << s.damage[i]
      << ",\"outgoing_damage\":" << s.damage[i^1];
  if(s.config.weapon=="2h")
    out << ",\"twohand\":" << actor.hitChance(TALENT_2H) << ",\"twohand_talent\":" << unsigned(actor.talentSkill(TALENT_2H));
  if(s.config.loadFixture) out << ",\"pair_index\":" << i/2;
  out << '}';
  return out.str();
  }
std::string BodyArena::actorsJson() {
  if(!state().ready) return "[]";
  std::string result="[";
  for(unsigned i=0;i<state().actorCount;++i) { if(i>0) result+=','; result+=actorJson(*state().actors[i],i); }
  return result+']';
  }
std::string BodyArena::setupJson() {
  const auto& config=state().config; auto out=jsonStream();
  out << "{\"schema_version\":1,\"geometry\":" << quoted(config.geometry)
      << ",\"yaw_offset_deg\":" << config.yawOffsetDeg << ",\"opponent_yaw_offset_deg\":" << config.opponentYawOffsetDeg
      << ",\"center_offset_cm\":[" << config.centerOffsetCm[0] << ',' << config.centerOffsetCm[1] << ']'
      << ",\"separation_yaw_deg\":" << config.separationYawDeg << ",\"geometry_proof\":" << state().geometryProof;
  if(config.wallClearanceCm) out << ",\"wall_clearance_cm\":" << *config.wallClearanceCm;
  if(config.wallTangentSign) out << ",\"wall_tangent_sign\":" << *config.wallTangentSign;
  if(config.reactionHold) out << ",\"reaction_hold\":true";
  if(config.loadFixture) out << ",\"load_fixture\":true,\"quality_credit\":false,\"real_actor_count\":4";
  out << '}';
  return out.str();
  }
std::string BodyArena::reactionJson() {
  const auto& s=state(); auto out=jsonStream();
  out << "{\"enabled\":true,\"phase\":"
        << quoted(s.reactionActivated ? "active" : s.reactionArmed ? "armed" : "held")
        << ",\"held_game_ms\":" << s.startTime << ",\"preparation_max_game_ms\":1000"
        << ",\"game_ms\":" << s.reactionGame << ",\"sim_step\":" << s.reactionStep
        << ",\"observation_seq\":" << s.reactionObservation << ",\"periodic\":" << s.reactionArmed
        << ",\"opponent_command_id\":" << s.reactionCommand << ",\"opponent_execution_id\":" << s.reactionExecution
        << ",\"opponent_started_game_ms\":" << s.reactionStartedGame
        << ",\"opponent_started_sim_step\":" << s.reactionStartedStep
        << ",\"opponent_started_event_seq\":" << s.reactionStartedEvent
        << ",\"activation_game_ms\":" << s.reactionActivationGame
        << ",\"activation_sim_step\":" << s.reactionActivationStep
        << ",\"activation_provider\":" << quoted(s.reactionProvider) << '}';
  return out.str();
  }
void BodyArena::sealInitialState() {
  if(!ready() || !state().initial.empty()) throw std::runtime_error("arena initial snapshot already sealed or unavailable");
  const auto& s=state();
  auto out=jsonStream();
  out << "{\"actors\":" << actorsJson() << ",\"game_ms\":" << gameTime()
      << ",\"runner_generation\":" << state().generation
      << ",\"session_epoch\":" << BodyObservations::sessionEpoch() << ",\"world_epoch\":" << BodyObservations::worldEpoch();
  if(customSetup()) out << ",\"setup\":" << setupJson();
  if(s.config.opponentOwned) out << ",\"opponent_owned\":true";
  if(s.config.reactionHold) out << ",\"reaction\":" << reactionJson();
  if(s.config.loadFixture) {
    out << ",\"load_fixture\":true,\"quality_credit\":false,\"real_actor_count\":4,\"actor_pairs\":[";
    for(unsigned pair=0;pair<2;++pair) {
      if(pair>0) out << ',';
      out << "{\"pair_index\":" << pair << ",\"primary_entity_id\":"
          << (s.ready ? BodyObservations::entityId(*s.actors[pair*2]) : 0)
          << ",\"opponent_entity_id\":" << (s.ready ? BodyObservations::entityId(*s.actors[pair*2+1]) : 0) << '}';
      }
    out << ']';
    }
  out << '}';
  state().initial=out.str(); journal("reset_ready");
  }
std::string BodyArena::statusJson() {
  const auto& s=state(); auto out=jsonStream();
  const auto now=s.finished ? s.finalTime : gameTime();
  const uint64_t elapsed=s.started && now>=s.startTime ? now-s.startTime : 0;
  out << "{\"schema_version\":1,\"enabled\":" << s.enabled << ",\"episode_id\":" << s.episode
      << ",\"runner_generation\":" << s.generation << ",\"session_epoch\":" << BodyObservations::sessionEpoch()
      << ",\"generation\":" << s.generation
      << ",\"world_epoch\":" << BodyObservations::worldEpoch() << ",\"scenario\":" << quoted(s.config.scenario)
      << ",\"seed\":" << s.config.seed << ",\"side\":" << s.config.side
      << ",\"distance_cm\":" << s.config.distanceCm << ",\"timeout_ms\":" << s.config.timeoutMs
      << ",\"distance_goal_cm\":220,\"distance_hold_ms\":200,\"distance_held_ms\":" << (s.distanceFrom==0 ? 0 : now-s.distanceFrom)
      << ",\"primary_entity_id\":" << (s.ready ? BodyObservations::entityId(*s.actors[0]) : 0)
      << ",\"opponent_entity_id\":" << (s.ready ? BodyObservations::entityId(*s.actors[1]) : 0)
      << ",\"ready\":" << s.ready << ",\"started\":" << s.started << ",\"provider\":" << quoted(s.provider)
      << ",\"outcome\":" << quoted(s.outcome) << ",\"reason\":" << quoted(s.reason)
      << ",\"terminated\":" << (s.finished && (s.outcome=="task_success" || s.outcome=="death_or_down"))
      << ",\"truncated\":" << (s.finished && (s.outcome=="time_limit" || s.outcome=="control_lost"))
      << ",\"infrastructure\":" << (s.finished && s.outcome=="infrastructure")
      << ",\"elapsed_game_ms\":" << elapsed << ",\"start_game_ms\":" << s.startTime << ",\"game_ms\":" << now
      << ",\"actors\":" << (s.finished ? s.finalActors : actorsJson()) << ",\"initial_state\":" << (s.initial.empty() ? "null" : s.initial);
  if(customSetup()) out << ",\"setup\":" << setupJson();
  if(s.config.opponentOwned) out << ",\"opponent_owned\":true";
  if(s.config.reactionHold) out << ",\"reaction\":" << reactionJson();
  if(s.config.loadFixture) {
    out << ",\"load_fixture\":true,\"quality_credit\":false,\"real_actor_count\":4,\"actor_pairs\":[";
    for(unsigned pair=0;pair<2;++pair) {
      if(pair>0) out << ',';
      out << "{\"pair_index\":" << pair << ",\"primary_entity_id\":"
          << (s.ready ? BodyObservations::entityId(*s.actors[pair*2]) : 0)
          << ",\"opponent_entity_id\":" << (s.ready ? BodyObservations::entityId(*s.actors[pair*2+1]) : 0) << '}';
      }
    out << ']';
    }
  out << '}';
  return out.str();
  }
void BodyArena::journal(std::string_view kind) {
  if(!enabled() || !state().sink) return;
  const auto value="{\"schema_version\":1,\"kind\":"+quoted(kind)+",\"arena\":"+statusJson()+"}";
  state().sink(value);
  }
void BodyArena::start(std::string_view provider) {
  auto& s=state();
  if(!s.ready || s.started || s.finished || s.initial.empty()) throw std::runtime_error("arena is not ready to start");
  if(provider!="native" && provider!="scripted" && provider!="native_with_scripted_opponent")
    throw std::runtime_error("arena provider must be native, scripted or native_with_scripted_opponent");
  if(s.config.reactionHold && provider!="scripted") throw std::runtime_error("reaction preparation requires both scripted leases");
  const bool first=BodyController::owns(*s.actors[0]), second=BodyController::owns(*s.actors[1]);
  if(s.config.loadFixture) {
    if(provider=="native_with_scripted_opponent") throw std::runtime_error("load fixture requires all-native or all-owned actors");
    for(unsigned i=0;i<s.actorCount;++i)
      if(BodyController::owns(*s.actors[i])!=(provider=="scripted"))
        throw std::runtime_error("four-actor load ownership must match provider for every actual actor");
    }
  if(provider=="native" && (first || second)) throw std::runtime_error("native arena requires native ownership");
  if(s.config.opponentOwned && (provider!="scripted" || !first || !second))
    throw std::runtime_error("owned-opponent duel requires both scripted leases");
  if(provider=="native_with_scripted_opponent" && (first || !second || s.config.scenario=="native_duel"))
    throw std::runtime_error("native micro baseline requires native primary and scripted opponent");
  if(!s.config.loadFixture && provider=="scripted" && (!first || (s.config.scenario!="native_duel" && !second) ||
                               (s.config.scenario=="native_duel" && second!=s.config.opponentOwned)))
    throw std::runtime_error("scripted arena ownership does not match scenario");
  s.provider=provider; s.started=true; s.startTime=gameTime();
  auto& script=Gothic::inst().world()->script(); const auto attack=script.findSymbolIndex("ZS_ATTACK");
  script.setSeed(s.config.seed);
  reseedActors(script);
  for(unsigned i=0;i<s.actorCount;++i)
    if(!BodyController::owns(*s.actors[i]))
      s.actors[i]->aiPush(AiQueue::aiStartState(ScriptFn(attack),0,s.actors[i^1],nullptr,""));
  journal("start");
  if(s.config.reactionHold) journal("reaction_held");
  }
const char* BodyArena::validateReactionActivation(const ReactionGate& gate,const ReactionRequest& request) {
  if(!gate.enabled || !gate.armed || gate.finished) return "reaction_not_armed";
  if(request.provider!="native" && request.provider!="scripted") return "invalid_reaction_provider";
  if(request.mode!="lockstep" && request.mode!="realtime") return "invalid_reaction_mode";
  const auto& actual=gate.actual;
  if(request.session!=actual.session) return "stale_session";
  if(request.world!=actual.world) return "stale_world";
  if(request.primary!=actual.primary || request.opponent!=actual.opponent) return "reaction_actor_mismatch";
  if(request.primaryGeneration!=actual.primaryGeneration || request.opponentGeneration!=actual.opponentGeneration)
    return "stale_generation";
  if(request.observation==0 || request.observation!=actual.observation || request.game!=actual.game || !gate.fresh || !gate.periodic)
    return "stale_reaction_observation";
  if(!gate.owned || !gate.idle || !gate.eligible) return "reaction_defender_unavailable";
  if(!gate.visible || !gate.inRange) return "reaction_target_unavailable";
  if(!gate.attack || !gate.prehit || !gate.started) return "reaction_windup_missing";
  if(!gate.unhurt) return "reaction_preparation_hit";
  return "allowed";
  }
BodyArena::ReactionGate BodyArena::reactionGate() {
  const auto& s=state(); ReactionGate gate;
  gate.enabled=s.enabled && s.config.reactionHold && s.started && !s.reactionActivated;
  gate.armed=s.reactionArmed; gate.finished=s.finished;
  if(!s.ready) return gate;
  auto& primary=*s.actors[0]; auto& opponent=*s.actors[1];
  const auto first=BodyController::snapshot(primary),second=BodyController::snapshot(opponent);
  gate.actual.session=BodyObservations::sessionEpoch(); gate.actual.world=BodyObservations::worldEpoch();
  gate.actual.primary=BodyObservations::entityId(primary); gate.actual.opponent=BodyObservations::entityId(opponent);
  gate.actual.primaryGeneration=first.controlGeneration; gate.actual.opponentGeneration=second.controlGeneration;
  gate.actual.observation=BodyObservations::latestSequence(primary); gate.actual.game=gameTime();
  gate.owned=first.owned && second.owned && !first.releasePending && !second.releasePending;
  const auto self=primary.bodyObservationState(),target=opponent.bodyObservationState();
  gate.idle=first.commandId==0 && first.executionId==0 && primary.aiQueue.size()==0 && primary.go2.empty() &&
            self.interruptible && self.segmentRemainingMs==0 && !self.attackAnim && !self.prehit && !self.defence && !self.jumpBack;
  gate.eligible=first.canDecide && self.nativeEligible;
  gate.visible=primary.currentTarget==&opponent && opponent.currentTarget==&primary && primary.canSeeNpc(opponent,false);
  gate.inRange=gate.visible && primary.bodyObservationTarget(opponent).inWRange;
  gate.attack=target.attackAnim; gate.prehit=target.prehit;
  gate.started=s.reactionCommand!=0 && s.reactionExecution!=0 && second.commandId==s.reactionCommand && second.executionId==s.reactionExecution;
  gate.unhurt=!s.reactionHit && primary.attribute(ATR_HITPOINTS)==300 && opponent.attribute(ATR_HITPOINTS)==300 &&
              s.damage[0]==0 && s.damage[1]==0 && s.blocked[0]==0 && s.blocked[1]==0;
  gate.periodic=BodyObservations::latestPeriodic(primary);
  gate.fresh=BodyObservations::latestGameMs(primary)==gameTime() &&
             (!s.reactionArmed || (s.reactionGame==gameTime() && s.reactionStep==BodyObservations::currentStep() &&
                                  s.reactionObservation==gate.actual.observation));
  return gate;
  }
void BodyArena::activateReaction(const ReactionRequest& request) {
  auto& s=state(); const auto gate=reactionGate();
  const auto reason=validateReactionActivation(gate,request);
  if(std::string_view(reason)!="allowed") throw std::runtime_error(std::string("reaction activation rejected: ")+reason);
  auto& script=Gothic::inst().world()->script(); const auto attack=script.findSymbolIndex("ZS_ATTACK");
  if(attack==size_t(-1)) throw std::runtime_error("reaction activation missing ZS_ATTACK");
  s.reactionActivating=true;
  try { BodyController::activateBenchReaction(*s.actors[0],*s.actors[1],request.provider=="native",request.mode); }
  catch(...) { s.reactionActivating=false; throw; }
  s.reactionActivated=true; s.reactionActivating=false; s.reactionProvider=request.provider;
  s.reactionActivationGame=gameTime(); s.reactionActivationStep=BodyObservations::currentStep();
  s.provider=request.provider=="native" ? "native_with_scripted_opponent" : "scripted";
  if(request.provider=="native")
    s.actors[0]->aiPush(AiQueue::aiStartState(ScriptFn(attack),0,s.actors[1],nullptr,""));
  journal("reaction_active");
  }
void BodyArena::finish(std::string_view outcome, std::string_view reason) {
  auto& s=state(); if(s.finished) return;
  s.finished=true; s.outcome=outcome; s.reason=reason; s.finalTime=gameTime();
  // Capture the final observation at the same real post-animation state as the terminal
  // snapshot. No periodic bucket is fabricated and an existing sample is never duplicated.
  if(s.ready) BodyObservations::sampleAll(true);
  s.finalActors=actorsJson();
  journal("terminal");
  }
void BodyArena::interrupt(std::string_view reason) {
  if(!enabled()) return;
  if(ready() && state().started && !state().finished) finish("infrastructure",reason);
  journal("pre_reset");
  }
void BodyArena::setupFailed(std::string_view reason) { if(enabled()) finish("infrastructure",reason); }
void BodyArena::postStep() {
  auto& s=state(); if(!s.enabled || !s.ready || !s.started || s.finished) return;
  if(BodyController::infrastructureFailure()) { finish("infrastructure",BodyController::infrastructureReason()); return; }
  auto& primary=*s.actors[0]; auto& opponent=*s.actors[1];
  if(s.config.loadFixture) for(unsigned i=2;i<s.actorCount;++i)
    if(s.actors[i]->isDown() || s.actors[i]->attribute(ATR_HITPOINTS)<=0) {
      finish("death_or_down",i%2==0 ? "pair_1_primary_down" : "pair_1_opponent_down"); return;
      }
  if(primary.isDown() || opponent.isDown() || primary.attribute(ATR_HITPOINTS)<=0 || opponent.attribute(ATR_HITPOINTS)<=0) {
    finish("death_or_down",primary.isDown() || primary.attribute(ATR_HITPOINTS)<=0 ? "primary_down" : "opponent_down"); return;
    }
  if(!s.pendingInfrastructureReason.empty()) { finish("infrastructure",s.pendingInfrastructureReason); return; }
  if(!s.pendingLossReason.empty()) { finish("control_lost",s.pendingLossReason); return; }
  if(s.config.reactionHold && !s.reactionActivated) {
    if(s.reactionArmed) return;
    auto gate=reactionGate(); gate.armed=true;
    if(!gate.owned || !gate.idle || !gate.unhurt) { finish("infrastructure","reaction_preparation_invalidated"); return; }
    if(std::string_view(validateReactionActivation(gate,gate.actual))=="allowed") {
      s.reactionArmed=true; s.reactionGame=gameTime(); s.reactionStep=BodyObservations::currentStep();
      s.reactionObservation=gate.actual.observation; journal("reaction_armed");
      return;
      }
    if(gameTime()-s.startTime>=1000 || gameTime()-s.startTime>=s.config.timeoutMs)
      finish("infrastructure","reaction_preparation_timeout");
    return;
    }
  const auto difference=primary.position()-opponent.position();
  if(s.config.scenario=="distance") {
    if(difference.x*difference.x+difference.z*difference.z<=220.f*220.f) {
      if(s.distanceFrom==0) s.distanceFrom=gameTime();
      if(gameTime()-s.distanceFrom>=200) { finish("task_success","distance_reached"); return; }
    } else s.distanceFrom=0;
    }
  if(s.config.scenario=="attack" && s.damage[1]>0 && opponent.attribute(ATR_HITPOINTS)<300) {
    finish("task_success","melee_damage_applied"); return;
    }
  if(s.config.scenario=="block" && s.blocked[0]>0 && primary.attribute(ATR_HITPOINTS)==300 && s.damage[0]==0) {
    finish("task_success","melee_hit_blocked"); return;
    }
  if(gameTime()-s.startTime>=s.config.timeoutMs) finish("time_limit","time_limit");
  }
void BodyArena::noteControl(std::string_view encoded) {
  auto& s=state();
  if(!s.enabled || !s.ready || !s.started || s.finished ||
     (!s.config.reactionHold && (s.provider!="scripted" || s.config.scenario!="native_duel")))
    return;
  rapidjson::Document row; row.Parse(encoded.data(),encoded.size());
  if(row.HasParseError() || !row.IsObject()) throw std::runtime_error("arena invalid native control telemetry");
  const auto text=[&](const char* key) -> std::string_view {
    if(!row.HasMember(key) || !row[key].IsString()) return {};
    return {row[key].GetString(),row[key].GetStringLength()};
    };
  const auto matches=[&](const char* key,uint64_t value) {
    return row.HasMember(key) && row[key].IsUint64() && row[key].GetUint64()==value;
    };
  if(text("event")!="body_control_event") return;
  if(s.config.reactionHold && !s.reactionActivated) {
    if(!matches("session_epoch",BodyObservations::sessionEpoch()) || !matches("world_epoch",BodyObservations::worldEpoch())) return;
    const bool primary=matches("entity_id",BodyObservations::entityId(*s.actors[0]));
    const bool opponent=matches("entity_id",BodyObservations::entityId(*s.actors[1]));
    if(primary && text("phase")=="accepted") s.pendingInfrastructureReason="reaction_defender_command_before_activation";
    if((primary || opponent) && !s.reactionActivating && text("phase")=="ownership" &&
       (text("reason")=="release_pending" || text("reason")=="native_restored"))
      s.pendingInfrastructureReason="reaction_preparation_ownership_lost";
    if(opponent && text("phase")=="started" && matches("action_id",6)) {
      const auto integer=[&](const char* key) { return row.HasMember(key) && row[key].IsUint64() ? row[key].GetUint64() : uint64_t(0); };
      s.reactionCommand=integer("command_id"); s.reactionExecution=integer("execution_id");
      s.reactionStartedGame=integer("t_game_ms"); s.reactionStartedStep=integer("sim_step"); s.reactionStartedEvent=integer("event_seq");
      }
    }
  if(s.provider!="scripted" || s.config.scenario!="native_duel" || text("phase")!="ownership" ||
     (text("reason")!="release_pending" && text("reason")!="native_restored")) return;
  bool actorMatches=matches("entity_id",BodyObservations::entityId(*s.actors[0]));
  if(s.config.opponentOwned) actorMatches=actorMatches || matches("entity_id",BodyObservations::entityId(*s.actors[1]));
  if(s.config.loadFixture) for(unsigned i=1;i<s.actorCount;++i)
    actorMatches=actorMatches || matches("entity_id",BodyObservations::entityId(*s.actors[i]));
  if(!actorMatches || !matches("session_epoch",BodyObservations::sessionEpoch()) || !matches("world_epoch",BodyObservations::worldEpoch())) return;
  const auto releaseReason=text("release_reason");
  if(releaseReason=="target_visibility_lost") s.pendingLossReason=releaseReason;
  else s.pendingInfrastructureReason="unexpected_control_loss:"+std::string(releaseReason);
  }
void BodyArena::noteCombat(const Npc& actor, const Npc* target, std::string_view phase, const std::string& fields) {
  auto& s=state(); if(!s.enabled || !s.started || s.finished) return;
  unsigned i=0; while(i<s.actorCount && &actor!=s.actors[i]) ++i;
  if(i==s.actorCount || target!=s.actors[i^1]) return;
  if(s.config.reactionHold && !s.reactionActivated && phase=="hit_attempt") s.reactionHit=true;
  if(phase!="defence_result" && phase!="damage") return;
  rapidjson::Document row; const auto text='{'+fields+'}'; row.Parse(text.c_str());
  if(row.HasParseError() || !row.IsObject()) throw std::runtime_error("arena invalid native combat telemetry");
  if(phase=="defence_result" && row.HasMember("blocked") && row["blocked"].IsBool() && row["blocked"].GetBool())
    ++s.blocked[i];
  if(phase=="damage" && row.HasMember("hp_before") && row["hp_before"].IsInt() && row.HasMember("hp_after") && row["hp_after"].IsInt()) {
    const int loss=row["hp_before"].GetInt()-row["hp_after"].GetInt();
    if(loss>0) s.damage[i]+=uint64_t(loss);
    }
  }
