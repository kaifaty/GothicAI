#include "testbench_internal.h"

#include "game/herodeed.h"

#include <algorithm>
#include <cmath>

#include "game/gamescript.h"
#include "gothic.h"
#include "world/aiqueue.h"
#include "world/npcsimtier.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

void NpcDuel::hit(Npc& victim, Npc& by, int32_t loss, std::string_view) {
  if(!active || loss<=0) return;
  for(size_t i=0;i<2;++i)
    if(actors[i]==&by && actors[1-i]==&victim) {
      ++hits[i]; damage[i]+=loss;
      if(loser<0 && victim.isDown())loser=int(1-i);
      }
  }

namespace {

// Raw script arrays match story_eval; weapon arrays are retained separately, without combining them.
template<class T>
std::string npcDuelArray(const T& values) {
  std::string out="[";
  for(const auto value:values) { if(out.size()>1)out+=','; out+=std::to_string(value); }
  return out+"]";
  }

}

std::string npcDuelSnapshot(World& world, Npc& npc) {
  const auto& h=npc.handle();
  auto* weapon=npc.currentMeleeWeapon();
  auto* symbol=weapon ? world.script().findSymbol(weapon->clsId()) : nullptr;
  std::string skills="[", values="[";
  for(int i=0;i<TALENT_MAX_G2;++i) {
    if(i>0) {skills+=',';values+=',';}
    skills+=std::to_string(npc.talentSkill(Talent(i)));
    values+=std::to_string(npc.talentValue(Talent(i)));
    }
  return "\"instance\":"+jsonStr(symName(world,&npc))+",\"level\":"+std::to_string(h.level)+
    ",\"hp_before\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
    ",\"hp_max\":"+std::to_string(npc.attribute(ATR_HITPOINTSMAX))+
    ",\"str\":"+std::to_string(npc.attribute(ATR_STRENGTH))+",\"dex\":"+std::to_string(npc.attribute(ATR_DEXTERITY))+
    ",\"hitchance\":{\"fist\":"+std::to_string(npc.hitChance(TALENT_UNKNOWN))+
    ",\"1h\":"+std::to_string(npc.hitChance(TALENT_1H))+",\"2h\":"+std::to_string(npc.hitChance(TALENT_2H))+"}"+
    ",\"talent_skills\":"+skills+"],\"talent_values\":"+values+"]"+
    ",\"protection\":"+npcDuelArray(h.protection)+",\"damage\":"+npcDuelArray(h.damage)+
    ",\"damagetype\":"+std::to_string(h.damage_type)+",\"weapon\":"+jsonStr(symbol ? symbol->name() : std::string_view{})+
    ",\"two_handed\":"+(weapon && weapon->is2H() ? "true" : "false")+
    ",\"weapon_damage\":"+(weapon ? npcDuelArray(weapon->handle().damage) : "[0,0,0,0,0,0,0,0]")+
    ",\"weapon_damage_total\":"+std::to_string(weapon ? weapon->handle().damage_total : 0)+
    ",\"weapon_damagetype\":"+std::to_string(weapon ? weapon->handle().damage_type : 0)+
    ",\"immortal_before\":"+(npc.isImmortal() ? "true" : "false")+
    ",\"guild\":"+std::to_string(h.guild)+",\"fight_tactic\":"+std::to_string(h.fight_tactic);
  }

bool tickNpcDuel(NpcDuel& state,World& world) {
  auto& duel=state;
  if(!duel.active)return false;
  const uint64_t now=world.tickCount();
  const bool down[]={duel.actors[0]->isDown(),duel.actors[1]->isDown()};
  if(!down[0] && !down[1] && now<duel.end)return true;
  Gothic::inst().onCombatHit.ubind(&duel,&NpcDuel::hit);
  duel.active=false;
  std::string fields="\"seed\":"+std::to_string(duel.seed)+",\"duration_ms\":"+std::to_string(now-duel.from)+
    ",\"hit_log\":"+duel.hitLog+"],\"defender_first_swing_ms\":"+
    (duel.firstResponseMs<0 ? "null" : std::to_string(duel.firstResponseMs));
  if(down[0] || down[1]) {
    const size_t loser=duel.loser>=0 ? size_t(duel.loser) : (down[0] ? 0 : 1);
    fields+=",\"winner\":"+jsonStr(symName(world,duel.actors[1-loser]))+
            ",\"loser\":"+jsonStr(symName(world,duel.actors[loser]));
    } else fields+=",\"timeout\":true";
  std::vector<Item*> dropped;
  for(size_t i=0;i<2;++i) {
    Npc& npc=*duel.actors[i];
    bool weaponDropped=false;
    world.detectItem(npc.position(),3000.f,[&](Item& item) {
      if(item.handle().owner!=int32_t(npc.instanceSymbol()) || !item.isDynamic())return;
      if(item.clsId()==duel.weapons[i])weaponDropped=true;
      dropped.push_back(&item);
      });
    fields+=std::string(i==0 ? ",\"attacker\":{" : ",\"defender\":{")+duel.before[i]+
      ",\"hp_after\":"+std::to_string(npc.attribute(ATR_HITPOINTS))+
      ",\"hits\":"+std::to_string(duel.hits[i])+",\"damage_dealt\":"+std::to_string(duel.damage[i])+
      ",\"swings\":"+std::to_string(duel.swings[i])+",\"blocked\":"+std::to_string(duel.blocked[i])+
      ",\"misses\":"+std::to_string(duel.misses[i])+
      ",\"hp_one\":"+(npc.attribute(ATR_HITPOINTS)==1 ? "true" : "false")+
      ",\"unconscious\":"+(stateName(world,npc)=="ZS_UNCONSCIOUS" ? "true" : "false")+
      ",\"weapon_dropped\":"+(weaponDropped ? "true" : "false")+"}";
    }
  event("npc_duel",fields);
  for(auto* item:dropped)world.removeItem(*item);
  for(auto*& npc:duel.actors) {world.removeNpc(*npc);npc=nullptr;}
  return false;
  }

namespace {

std::vector<const WayPoint*> npcTierPath(const Npc& npc) {
  std::vector<const WayPoint*> result;
  if(const auto* current=npc.currentGoToPoint())result.push_back(current);
  auto path=npc.navigationPath();
  while(const auto* point=path.pop())
    if(result.empty() || result.back()!=point)result.push_back(point);
  return result;
  }

}

bool npcTierTestTick(NpcTierTest& state,World& world,Npc& player,uint64_t frameMs) {
  auto& test=state;
  if(!test.npc)return false;
  auto& npc=*test.npc;
  const auto now=world.tickCount();
  const bool turning=test.phase==1;
  const float moved=(npc.position()-test.lastPosition).length();
  const float turned=std::abs(std::remainder(npc.rotation()-test.lastAngle,360.f));
  test.lastPosition=npc.position();test.lastAngle=npc.rotation();
  if(now>=test.deadline) {
    test.npc=nullptr;
    arenaAbort("NPC tier transition test timed out before every action arrived");
    return true;
    }
  if(test.pending) {
    if(now==test.switchedAt)return true;
    const auto position=npc.position();
    const auto delta=position-test.before;
    const float horizontal=std::sqrt(delta.x*delta.x+delta.z*delta.z);
    const auto elapsed=now-test.switchedAt;
    const float travel=std::max(test.travelPerMs*float(elapsed),npc.animMoveSpeed(elapsed).length());
    const auto path=npcTierPath(npc);
    const bool suffix=path.size()<=test.path.size() && path.size()+1>=test.path.size() &&
      std::equal(path.rbegin(),path.rend(),test.path.rbegin());
    const auto goal=npc.navigationGoal();
    const bool sameGoal=turning || (goal.active && goal.point==test.destination);
    const auto* head=npc.aiQueueFront();
    const bool sameTurn=!turning || (head && head->act==AI_TurnToNpc);
    const float afterHeight=std::hypot(horizontal,std::max(0.f,std::abs(delta.y)-30.f));
    const bool pass=npc.simTier()!=test.previous && suffix && sameGoal && sameTurn && afterHeight<=travel+0.1f;
    test.maximumJump=std::max(test.maximumJump,delta.length());
    test.maximumHeight=std::max(test.maximumHeight,std::abs(delta.y));
    if(npc.simTier()==NpcSimTier::Full)++test.toFull[turning?1:0];
    else ++test.toSimplified[turning?1:0];
    event("npc_tier_transition","\"npc\":"+jsonStr(symName(world,&npc))+
      ",\"action\":"+jsonStr(turning?"turn":"edge")+",\"from\":"+jsonStr(NpcSimTiers::name(test.previous))+
      ",\"to\":"+jsonStr(NpcSimTiers::name(npc.simTier()))+",\"jump_cm\":"+ragdollNumber(delta.length())+
      ",\"horizontal_cm\":"+ragdollNumber(horizontal)+",\"height_cm\":"+ragdollNumber(std::abs(delta.y))+
      ",\"tick_travel_cm\":"+ragdollNumber(travel)+",\"after_height_correction_cm\":"+ragdollNumber(afterHeight)+",\"angle_delta_deg\":"+
      ragdollNumber(std::abs(std::remainder(npc.rotation()-test.beforeAngle,360.f)))+
      ",\"path_preserved\":"+(suffix?"true":"false")+",\"destination_preserved\":"+(sameGoal?"true":"false")+
      ",\"pass\":"+(pass?"true":"false"));
    test.pending=false;
    if(!pass) {
      test.npc=nullptr;
      arenaAbort("NPC tier transition changed the remaining action or exceeded one tick of travel");
      return true;
      }
    ++test.switches;
    }
  if(test.switches<2) {
    const auto* head=npc.aiQueueFront();
    const auto goal=npc.navigationGoal();
    const auto* current=npc.currentGoToPoint();
    const auto fromStart=npc.position()-test.started;
    const auto toWaypoint=current ? current->groundPos-npc.position() : Vec3();
    const bool ready=turning ? head && head->act==AI_TurnToNpc && turned>0.5f :
      current && goal.active && moved>0.1f && std::hypot(fromStart.x,fromStart.z)>50.f &&
      std::hypot(toWaypoint.x,toWaypoint.z)>200.f;
    if(!ready)return true;
    test.before=npc.position();
    test.beforeAngle=npc.rotation();
    test.destination=goal.point;
    test.path=npcTierPath(npc);
    test.previous=npc.simTier();
    const auto tick=std::max<uint64_t>(frameMs,1);
    test.travelPerMs=turning?0.f:std::max(npc.simplifiedMoveSpeed(),npc.animMoveSpeed(tick).length()/float(tick));
    test.switchedAt=now;
    test.pending=true;
    npc.setSimTierOverride(test.previous==NpcSimTier::Full?NpcSimTier::Simplified:NpcSimTier::Full);
    return true;
    }
  if(!npc.isAiQueueEmpty())return true;
  if(!turning) {
    const auto* destination=test.ends[(test.legs+1)%2];
    const bool arrived=(npc.position()-destination->position()).length()<=150.f;
    event("npc_tier_arrival","\"leg\":"+std::to_string(test.legs)+",\"destination\":"+jsonStr(destination->name)+
      ",\"distance_cm\":"+ragdollNumber((npc.position()-destination->position()).length())+
      ",\"pass\":"+(arrived?"true":"false"));
    if(!arrived) {test.npc=nullptr;arenaAbort("NPC tier transition action finished away from its destination");return true;}
    ++test.legs;
    if(test.legs<20) {
      test.started=npc.position();test.switches=0;
      npc.aiPush(AiQueue::aiGoToPoint(*test.ends[(test.legs+1)%2]));
      return true;
      }
    test.phase=1;
    }
  else ++test.turns;
  if(test.turns<20) {
    npc.setDirection(player.position()-npc.position());
    npc.setDirection(npc.rotation()+170.f);
    test.beforeAngle=npc.rotation();test.lastAngle=npc.rotation();test.switches=0;
    npc.aiPush(AiQueue::aiTurnToNpc(&player));
    return true;
    }
  const bool enough=test.toFull[0]>=20 && test.toSimplified[0]>=20 &&
                    test.toFull[1]>=20 && test.toSimplified[1]>=20 && test.legs==20 && test.turns==20;
  event("npc_tier_transition_result","\"edge_to_full\":"+std::to_string(test.toFull[0])+
    ",\"edge_to_simplified\":"+std::to_string(test.toSimplified[0])+
    ",\"turn_to_full\":"+std::to_string(test.toFull[1])+
    ",\"turn_to_simplified\":"+std::to_string(test.toSimplified[1])+
    ",\"arrivals\":"+std::to_string(test.legs)+",\"turns_completed\":"+std::to_string(test.turns)+
    ",\"max_jump_cm\":"+ragdollNumber(test.maximumJump)+",\"max_height_cm\":"+ragdollNumber(test.maximumHeight)+
    ",\"pass\":"+(enough?"true":"false"));
  npc.setSimTierOverride(std::nullopt);
  test.npc=nullptr;
  if(!enough)arenaAbort("NPC tier transition test did not observe 20 switches each way during both actions");
  return false;
  }

bool npcSpeedPrepare(NpcSpeedTest& state,World& world) {
  auto& test=state;
  static constexpr const char* instances[]={"BAU_904_BAUER","OrcWarrior_Roam","Wolf"};
  static constexpr WalkBit modes[]={WalkBit::WM_Walk,WalkBit::WM_Run,WalkBit::WM_Sneak};
  if(test.leg%6==0) {
    if(test.npc)world.removeNpc(*test.npc);
    const auto instance=world.script().findSymbolIndex(instances[test.leg/6]);
    test.npc=instance==size_t(-1) ? nullptr : world.addNpc(instance,test.start->groundPos);
    if(!test.npc || !test.npc->visualSkeleton() || test.npc->isDown()) {
      test.npc=nullptr;arenaAbort("NPC speed fixture instance is unavailable");return false;
      }
    }
  auto& npc=*test.npc;
  npc.setInteraction(nullptr,true);npc.clearAiQueue();npc.clearState(true);npc.clearGoTo();
  npc.setTarget(nullptr);npc.setExternalControl(true);npc.stopWalking();npc.closeWeapon(true);npc.clearSpeed();
  npc.setSimTierOverride(test.leg%2==0 ? NpcSimTier::Full : NpcSimTier::Simplified);
  npc.setPosition(test.start->groundPos);npc.setDirection(test.end->groundPos-test.start->groundPos);
  npc.attachToPoint(test.start);npc.updateTransform();
  npc.aiPush(AiQueue::aiSetWalkMode(modes[(test.leg/2)%3]));
  npc.aiPush(AiQueue::aiGoToPoint(*test.end));
  test.began=test.previousTick=world.tickCount();test.previous=npc.position();
  test.measuredMs=0;test.samples=0;test.distance=0;test.expectedDistance=0;
  test.firstAnimation.clear();test.lastAnimation.clear();
  return true;
  }

bool npcSpeedTick(NpcSpeedTest& state,World& world) {
  auto& test=state;
  if(!test.npc)return false;
  auto& npc=*test.npc;
  const auto now=world.tickCount();
  if(now-test.began>600000) {
    test.npc=nullptr;arenaAbort("NPC speed fixture did not arrive on the authored edge");return true;
    }
  const auto edge=test.end->groundPos-test.start->groundPos;
  const auto offset=npc.position()-test.start->groundPos,previous=test.previous-test.start->groundPos;
  const auto progress=[&](const Vec3& p) {return (p.x*edge.x+p.z*edge.z)/(test.length*test.length);};
  if(now>test.previousTick && progress(previous)>=.2f && progress(offset)<=.8f) {
    const auto expected=test.leg%2==0 ? NpcSimTier::Full : NpcSimTier::Simplified;
    if(npc.simTier()!=expected || npc.currentGoToPoint()!=test.end) {
      test.npc=nullptr;arenaAbort("NPC speed fixture left the requested tier or authored edge");return true;
      }
    const auto dt=now-test.previousTick;
    const auto moved=npc.position()-test.previous;
    test.distance+=(moved.x*edge.x+moved.z*edge.z)/test.length;
    test.expectedDistance+=npc.simplifiedMoveSpeed()*float(dt);
    test.measuredMs+=dt;++test.samples;
    if(test.firstAnimation.empty())test.firstAnimation=npc.animNames();
    test.lastAnimation=npc.animNames();
    }
  test.previous=npc.position();test.previousTick=now;
  if(!npc.isAiQueueEmpty())return true;
  static constexpr const char* species[]={"human","orc","wolf"};
  static constexpr const char* modes[]={"walk","run","sneak"};
  const bool arrived=(npc.position()-test.end->groundPos).length()<=150.f;
  const bool pass=arrived && test.samples>=3 && test.distance>0;
  event("npc_speed_sample","\"species\":"+jsonStr(species[test.leg/6])+
    ",\"npc\":"+jsonStr(symName(world,&npc))+",\"walk_mode\":"+jsonStr(modes[(test.leg/2)%3])+
    ",\"tier\":"+jsonStr(NpcSimTiers::name(npc.simTier()))+
    ",\"from\":"+jsonStr(test.start->name)+",\"to\":"+jsonStr(test.end->name)+
    ",\"edge_cm\":"+ragdollNumber(test.length)+",\"edge_height_cm\":"+ragdollNumber(edge.y)+
    ",\"elapsed_ms\":"+std::to_string(now-test.began)+",\"measured_ms\":"+std::to_string(test.measuredMs)+
    ",\"samples\":"+std::to_string(test.samples)+",\"distance_cm\":"+ragdollNumber(test.distance)+
    ",\"sequence_speed_m_s\":"+ragdollNumber(test.measuredMs ? test.expectedDistance*10.f/float(test.measuredMs) : 0.f)+
    ",\"observed_speed_m_s\":"+ragdollNumber(test.measuredMs ? test.distance*10.f/float(test.measuredMs) : 0.f)+
    ",\"first_animation\":"+jsonStr(test.firstAnimation)+",\"last_animation\":"+jsonStr(test.lastAnimation)+
    ",\"arrived\":"+(arrived?"true":"false")+",\"pass\":"+(pass?"true":"false"));
  if(!pass) {test.npc=nullptr;arenaAbort("NPC speed fixture has no valid middle-edge measurement or arrival");return true;}
  if(++test.leg==18) {
    world.removeNpc(npc);test.npc=nullptr;
    event("npc_speed_result","\"rows\":18,\"species\":3,\"walk_modes\":3,\"tiers\":2,\"pass\":true");
    return false;
    }
  npcSpeedPrepare(state,world);
  return true;
  }

bool npcSimCommand(NpcDuel& duelState,NpcTierTest& tierState,NpcSpeedTest& speedState,size_t& pc,const std::vector<std::string>& prog,const std::vector<std::string>& a,World& w,Npc& pl) {
  const std::string& c = a[0];
  if(c=="npctier") {
    // npctier <npc> full|simplified|auto: a runtime-only override of one actor.
    Npc* npc=a.size()==3?findNpc(w,pl,a[1]):nullptr;
    if(!npc || (a[2]!="full" && a[2]!="simplified" && a[2]!="auto")) {
      arenaAbort("npctier expects an existing NPC and full, simplified or auto");
      return true;
      }
    npc->setSimTierOverride(a[2]=="auto"?std::nullopt:
      std::optional<NpcSimTier>(a[2]=="full"?NpcSimTier::Full:NpcSimTier::Simplified));
    event("npc_tier_override","\"npc\":"+jsonStr(symName(w,npc))+",\"mode\":"+jsonStr(a[2]));
    }
  else if(c=="npctier-test") {
    // npctier-test <npc> <start waypoint> <end waypoint>: 20 walking legs and
    // 20 turns, each with one switch in either direction while still active.
    Npc* npc=a.size()==4?findNpc(w,pl,a[1]):nullptr;
    const auto* start=a.size()==4?w.findPoint(a[2],false):nullptr;
    const auto* end=a.size()==4?w.findPoint(a[3],false):nullptr;
    if(!npc || npc==&pl || !start || !end || start==end) {
      arenaAbort("npctier-test expects an NPC and two distinct existing waypoints");
      return true;
      }
    tierState=NpcTierTest();
    auto& test=tierState;
    test.npc=npc;test.ends[0]=start;test.ends[1]=end;
    test.deadline=w.tickCount()+1800000;
    npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);
    npc->setExternalControl(true);npc->stopWalking();npc->closeWeapon(true);
    npc->setSimTierOverride(NpcSimTier::Full);npc->setWalkMode(WalkBit::WM_Walk);
    const auto floor=w.physic()->landRay(start->position()+Vec3(0,100,0),300.f);
    if(!floor.hasCol) {test.npc=nullptr;arenaAbort("npctier-test start waypoint has no ground");return true;}
    npc->setPosition(floor.v);npc->updateTransform();
    test.started=npc->position();test.lastPosition=npc->position();test.lastAngle=npc->rotation();
    npc->aiPush(AiQueue::aiGoToPoint(*end));
    event("npc_tier_transition_start","\"npc\":"+jsonStr(symName(w,npc))+",\"from\":"+jsonStr(start->name)+
      ",\"to\":"+jsonStr(end->name));
    }
  else if(c=="npcspeed") {
    const auto* start=a.size()==2?w.findPoint(a[1],false):nullptr;
    const WayPoint* end=nullptr;
    float length=0;
    if(start)for(const auto& connection:start->connections()) {
      const auto edge=connection.point->groundPos-start->groundPos;
      const float distance=std::hypot(edge.x,edge.z);
      if(distance>length) {end=connection.point;length=distance;}
      }
    if(!start || !end || length<500.f) {
      arenaAbort("npcspeed expects an authored waypoint with a connected edge at least 5 m long");return true;
      }
    // This fresh calibration world has no ambient combat, crowding or other
    // actors using the edge; the player's distant position is retained.
    for(uint32_t i=w.npcCount();i>0;--i)
      if(auto* npc=w.npcById(i-1);npc!=&pl)w.removeNpc(*npc);
    speedState=NpcSpeedTest();
    auto& test=speedState;test.start=start;test.end=end;test.length=length;
    npcSpeedPrepare(speedState,w);
    }
  else if(c=="npcduel") {
    auto fail=[&](const std::string& reason) {
      say("npcduel: "+reason);event("npc_duel_error","\"reason\":"+jsonStr(reason));
      pc=prog.size();
      };
    if(a.size()<4 || a.size()>5) {fail("expected attacker defender seed [timeout_ms]");return true;}
    const auto first=w.script().findSymbolIndex(a[1]), second=w.script().findSymbolIndex(a[2]);
    const auto attack=w.script().findSymbolIndex("ZS_ATTACK");
    if(first==size_t(-1) || second==size_t(-1) || attack==size_t(-1)) {fail("missing script instance or ZS_ATTACK");return true;}
    const float an=pl.rotation()*float(M_PI)/180.f;
    const Vec3 forward(std::cos(an),0,std::sin(an)), side(-forward.z,0,forward.x);
    const Vec3 center=pl.position()+forward*1200.f;
    Vec3 pos[]={center-side*150.f,center+side*150.f};
    for(auto& p:pos) {
      DynamicWorld::RayLandResult ground;
      for(float y=p.y+1500;y>p.y-1500 && !ground.hasCol;y-=400)
        ground=w.physic()->ray({p.x,y,p.z},{p.x,y-450,p.z});
      if(!ground.hasCol) {fail("arena ground not found");return true;}
      p.y=ground.v.y;
      }
    auto& duel=duelState;
    duel=NpcDuel{};
    duel.seed=uint32_t(std::stoul(a[3]));
    const uint64_t timeout=a.size()>4 ? std::stoull(a[4]) : 180000;
    w.script().setSeed(duel.seed);
    duel.actors[0]=w.addNpc(first,pos[0]);duel.actors[1]=w.addNpc(second,pos[1]);
    for(size_t i=0;i<2;++i) {
      auto* npc=duel.actors[i];
      if(npc==nullptr || npc->isMonster() || npc->handle().guild>=16) {
        for(auto*& copy:duel.actors)if(copy) {w.removeNpc(*copy);copy=nullptr;}
        fail("two human NPCs with guild < 16 are required");return true;
        }
      duel.before[i]=npcDuelSnapshot(w,*npc);
      if(auto* weapon=npc->currentMeleeWeapon())duel.weapons[i]=weapon->clsId();
      auto& flags=npc->handle().flags;
      flags=daedalus::NpcFlag(uint32_t(flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
      npc->setDirection(pos[1-i]-pos[i]);npc->updateTransform();
      npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
      }
    pl.handle().flags=daedalus::NpcFlag(uint32_t(pl.handle().flags)|uint32_t(daedalus::NpcFlag::IMMORTAL));
    pl.clearAiQueue();pl.clearGoTo();pl.clearSpeed();
    Npc& npc=*duel.actors[0];Npc& target=*duel.actors[1];
    // Same nonlethal start as npc_act fight{deed:true}; the defender retains its constructor state.
    auto& aiv=npc.handle().aivar;
    aiv[size_t(HeroDeed::constant(npc,"AIV_ATTACKREASON"))]=HeroDeed::constant(npc,"AR_NONE");
    aiv[size_t(HeroDeed::constant(npc,"AIV_LASTFIGHTAGAINSTPLAYER"))]=HeroDeed::constant(npc,"FIGHT_CANCEL",0);
    aiv[size_t(HeroDeed::constant(npc,"AIV_LASTFIGHTCOMMENT"))]=0;
    npc.stopListening();npc.thinkOfAnswer(0);npc.clearAiQueue();
    if(!npc.startState(ScriptFn(attack),"",gtime::endOfTime(),true)) {
      for(auto*& copy:duel.actors) {w.removeNpc(*copy);copy=nullptr;}
      fail("cannot start ZS_ATTACK");return true;
      }
    npc.setOther(&target);npc.setVictim(nullptr);npc.setTarget(&target);
    npc.setNonlethalFight(target);npc.setServerDeed("attack",&target);
    if(npc.weaponState()==WeaponState::NoWeapon)npc.aiPush(AiQueue::aiDrawWeapon());
    duel.from=w.tickCount();duel.end=duel.from+timeout;duel.active=true;
    Gothic::inst().onCombatHit.bind(&duel,&NpcDuel::hit);
    }
  else return false;
  return true;
  }

}
