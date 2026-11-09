#include "testbench_internal.h"
#include "testbench.h"

#include <algorithm>
#include <cmath>

#include "game/gamescript.h"
#include "gothic.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "appcadence.h"
#include "world/aiqueue.h"
#include "world/objects/item.h"

using namespace Tempest;

namespace testbench_detail {

bool dodgeDirection(std::string_view name, MeleeDodge::Direction& dir) {
  if(name=="back" || name=="none")
    dir = MeleeDodge::Direction::Back;
  else if(name=="left")
    dir = MeleeDodge::Direction::Left;
  else if(name=="right")
    dir = MeleeDodge::Direction::Right;
  else if(name=="forward")
    dir = MeleeDodge::Direction::Forward;
  else
    return false;
  return true;
  }

// `dodge ... before`: fires as the attacker's next blow comes within leadMs of its contact
void meleeDodgeFields(World& world) {
  const auto context=meleeContext();
  auto& state=context.melee;
  if(!state.dodgeArm)
    return;
  const auto arm = *state.dodgeArm;
  if(world.tickCount()>arm.until) {
    say("dodge " + symName(world,arm.npc) + ": no blow of " + symName(world,arm.attacker) + " came in time");
    state.dodgeArm.reset();
    return;
    }
  const auto contact = arm.attacker->nextMeleeContactMs();
  if(!contact || *contact>arm.leadMs)
    return;
  state.dodgeArm.reset();
  const bool ok = arm.npc->meleeDodge(arm.dir);
  say("dodge " + symName(world,arm.npc) + " " + std::string(MeleeDodge::name(arm.dir)) + " before the blow of " +
      symName(world,arm.attacker) + ", its contact in " + std::to_string(*contact) + " ms: " +
      (ok ? "started" : "not started"));
  }

void meleeAiResponseFields(World& world) {
  const auto context=meleeContext();
  auto& state=context.melee;
  if(!state.meleeAiResponse)
    return;
  const auto arm = *state.meleeAiResponse;
  if(world.tickCount()>arm.until) {
    say("melee AI response: no charged contact of " + symName(world,arm.attacker) + " came in time");
    state.meleeAiResponse.reset();
    return;
    }
  if(!arm.attacker->isMeleeChargeReleasing())
    return;
  const auto contact = arm.attacker->nextMeleeContactMs();
  if(!contact || *contact>120)
    return;
  state.meleeAiResponse.reset();
  if(arm.parry) {
    arm.npc->setDirection(arm.attacker->position()-arm.npc->position());
    arm.npc->updateTransform();
    }
  const bool started = arm.parry ? arm.npc->blockSword() : arm.npc->meleeDodge(MeleeDodge::Direction::Left);
  const std::string response = arm.parry ? "parry" : "dodge";
  say("melee AI response " + response + " before the charged contact of " + symName(world,arm.attacker) +
      " in " + std::to_string(*contact) + " ms: " + (started ? "started" : "refused"));
  TestBench::bodyEvent(*arm.npc,arm.attacker,"ai_charge_response",world.tickCount(),
    "\"response\":"+jsonStr(response)+",\"contact_in_ms\":"+std::to_string(*contact)+
    ",\"started\":"+(started ? "true" : "false"));
  }

bool meleeCommand(const MeleeContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  auto& state=context.melee;
  const auto& c=a[0];
  if(c=="melee-shadow") {
    // The fixture prepares stock actors and geometry for the ordinary melee entry points.
    const auto equip=[&](std::string_view weapon) {
      if(weapon!="1h" && weapon!="2h" && weapon!="fist") {arenaAbort("melee shadow expects 1h, 2h or fist");return false;}
      pl.clearAiQueue();pl.clearState(false);pl.clearGoTo();pl.stopWalking();pl.closeWeapon(true);
      pl.handle().attribute[ATR_STRENGTH]=100;
      pl.setTalentSkill(TALENT_1H,2);pl.setTalentSkill(TALENT_2H,2);
      if(weapon=="fist") pl.setToFistMode();
      else {
        const auto symbol=w.script().findSymbolIndex(weapon=="1h"?"ITMW_1H_COMMON_01":"ITMW_2H_BAU_AXE");
        if(symbol==size_t(-1)) {arenaAbort("melee shadow stock weapon is absent");return false;}
        pl.setToFightMode(symbol);
        if(!pl.currentMeleeWeapon() || pl.currentMeleeWeapon()->clsId()!=symbol) {
          arenaAbort("melee shadow weapon did not equip");return false;
          }
        }
      state.meleeShadowWeapon=weapon;
      return true;
      };
    if(a.size()==3 && a[1]=="setup") {
      Gothic::settingsSetI("GAME","USEGOTHIC1CONTROLS",1);
      const Vec3 forward=facing(pl),side(-forward.z,0,forward.x);
      const auto wall=w.physic()->ray(pl.position()+Vec3(0,148,0),pl.position()+Vec3(0,148,0)+forward*450.f);
      if(!wall.hasCol || std::abs(wall.n.y)>.35f) {arenaAbort("melee shadow needs a real wall ahead (corner right go 0)");return true;}
      for(auto* npc:state.meleeShadowActors) if(w.npcId(npc)!=uint32_t(-1)) w.removeNpc(*npc);
      state.meleeShadowActors.clear();state.meleeShadowPlaces.clear();
      state.meleeShadowWall=wall.v;state.meleeShadowForward=forward;
      const char* symbols[]={"WOLF","WOLF","BDT_1013_BANDIT_L"};
      const Vec3 center=Vec3(wall.v.x,pl.position().y,wall.v.z)-forward*75.f;
      const Vec3 places[]={center-side*125.f-forward*30.f,center+side*125.f-forward*30.f,center};
      for(size_t i=0;i<std::size(symbols);++i) {
        Vec3 pos=places[i];
        const auto floor=w.physic()->landRay(pos+Vec3(0,200,0),500.f);
        const auto path=w.physic()->ray(pl.position()+Vec3(0,90,0),pos+Vec3(0,90,0));
        const auto symbol=w.script().findSymbolIndex(symbols[i]);
        if(!floor.hasCol || floor.n.y<.8f || path.hasCol || symbol==size_t(-1)) {
          arenaAbort("melee shadow target lacks supported unobstructed real ground");return true;
          }
        pos.y=floor.v.y;
        auto* npc=w.addNpc(symbol,pos);
        if(!npc) {arenaAbort("melee shadow target insertion failed");return true;}
        npc->clearAiQueue();npc->clearState(true);npc->clearGoTo();npc->stopWalking();npc->setExternalControl(true);
        npc->handle().attribute[ATR_HITPOINTSMAX]=100000;
        npc->handle().attribute[ATR_HITPOINTS]=100000;
        npc->setDirection(forward*-1.f);npc->updateTransform();
        if(i==2) {
          const auto sword=w.script().findSymbolIndex("ITMW_1H_COMMON_01");
          if(sword==size_t(-1)) {arenaAbort("melee shadow bandit weapon is absent");return true;}
          npc->setToFightMode(sword);
          }
        state.meleeShadowActors.push_back(npc);state.meleeShadowPlaces.push_back(pos);
        event("melee_shadow_actor","\"index\":"+std::to_string(i)+",\"npc\":"+jsonStr(TestBench::who(npc))+
          ",\"npc_id\":"+std::to_string(w.npcId(npc))+
          ",\"position_cm\":"+ragdollVector(pos)+",\"wall_cm\":"+ragdollVector(wall.v));
        }
      if(!equip(a[2])) return true;
      event("melee_shadow_setup","\"world\":"+jsonStr(w.name())+",\"weapon\":"+jsonStr(a[2])+
        ",\"wall_cm\":"+ragdollVector(wall.v)+",\"wall_normal\":"+ragdollVector(wall.n));
      }
    else if(a.size()==3 && a[1]=="weapon") equip(a[2]);
    else if(a.size()==3 && a[1]=="controls" && (a[2]=="g2" || a[2]=="classic")) {
      Gothic::settingsSetI("GAME","USEGOTHIC1CONTROLS",a[2]=="classic");
      say("melee shadow controls " + a[2]);
      }
    else if((a.size()==3 || a.size()==4) && a[1]=="bandit") {
      // melee-shadow bandit 1h|2h|none [npc]: the fixture's bandit takes the stock one- or two-hander in hand, or puts
      // his away (a clear gap between a blade and his body, no blade of his in between)
      if(state.meleeShadowActors.size()!=3) {arenaAbort("melee shadow setup is required");return true;}
      if(a[2]!="1h" && a[2]!="2h" && a[2]!="none") {arenaAbort("melee shadow bandit expects 1h, 2h or none");return true;}
      auto* actor=a.size()==4 ? findNpc(w,pl,a[3]) : state.meleeShadowActors[2];
      if(actor==nullptr) {arenaAbort("melee shadow bandit is absent");return true;}
      auto& npc=*actor;
      if(a[2]=="none") {npc.closeWeapon(true);return true;}
      const auto symbol=w.script().findSymbolIndex(a[2]=="1h"?"ITMW_1H_COMMON_01":"ITMW_2H_BAU_AXE");
      if(symbol==size_t(-1)) {arenaAbort("melee shadow bandit weapon is absent");return true;}
      npc.closeWeapon(true);
      npc.setToFightMode(symbol);
      if(!npc.currentMeleeWeapon() || npc.currentMeleeWeapon()->clsId()!=symbol) {
        arenaAbort("melee shadow bandit weapon did not equip");return true;
        }
      }
    else if(a.size()==2 && a[1]=="stamina-backstep") {
      if(state.meleeShadowActors.size()!=3) {arenaAbort("melee shadow setup is required");return true;}
      auto& npc=*state.meleeShadowActors[2];
      if(!npc.hasStamina()) {arenaAbort("stamina backstep requires human melee stamina");return true;}
      npc.handle().attribute[ATR_STRENGTH]=10;
      npc.closeWeapon(true);npc.setToFistMode();
      for(unsigned i=0;i<16 && !npc.isStaminaExhausted();++i) {
        npc.stopAnim("");
        if(!npc.doAttack(Npc::Anim::Attack,BS_HIT)) {arenaAbort("stamina backstep could not drain the NPC");return true;}
        }
      if(npc.stamina()!=0.f || !npc.isStaminaExhausted()) {arenaAbort("stamina backstep requires an exhausted NPC");return true;}
      npc.stopAnim("");
      const bool meleeRefused=!npc.setAnim(Npc::Anim::MoveBack);
      npc.closeWeapon(true);npc.stopAnim("");
      const bool unarmedAccepted=npc.setAnim(Npc::Anim::MoveBack);
      const bool pass=meleeRefused && unarmedAccepted && npc.weaponState()==WeaponState::NoWeapon &&
                      npc.stamina()==0.f && npc.isStaminaExhausted();
      event("melee_stamina_backstep","\"npc\":"+jsonStr(TestBench::who(&npc))+
        ",\"melee_refused\":"+(meleeRefused?"true":"false")+
        ",\"unarmed_accepted\":"+(unarmedAccepted?"true":"false")+
        ",\"stamina\":"+ragdollNumber(npc.stamina())+",\"pass\":"+(pass?"true":"false"));
      if(!pass) arenaAbort("exhausted unarmed NPC backstep failed");
      }
    else if(a.size()==3 && a[1]=="target") {
      const int index=int(num(a,2,-1));
      if(index<0 || size_t(index)>=state.meleeShadowActors.size()) {arenaAbort("melee shadow target is absent");return true;}
      for(size_t i=0;i<state.meleeShadowActors.size();++i) {
        auto& npc=*state.meleeShadowActors[i];
        npc.clearAiQueue();npc.clearState(true);npc.clearGoTo();npc.stopWalking();npc.setExternalControl(true);
        npc.setPosition(state.meleeShadowPlaces[i]);npc.setDirection(state.meleeShadowForward*-1.f);npc.updateTransform();
        }
      auto* target=state.meleeShadowActors[size_t(index)];
      const float distance=state.meleeShadowWeapon=="fist"?90.f:state.meleeShadowWeapon=="2h"?180.f:140.f;
      Vec3 at=target->position()-state.meleeShadowForward*distance;
      const auto floor=w.physic()->landRay(at+Vec3(0,200,0),500.f);
      if(!floor.hasCol || floor.n.y<.8f) {arenaAbort("melee shadow attacker has no supported ground");return true;}
      at.y=floor.v.y;
      pl.clearGoTo();pl.stopWalking();pl.setPosition(at);pl.setDirection(state.meleeShadowForward);pl.setTarget(target);pl.updateTransform();
      event("melee_shadow_target","\"index\":"+std::to_string(index)+",\"weapon\":"+jsonStr(state.meleeShadowWeapon)+
        ",\"attacker_cm\":"+ragdollVector(at)+",\"target_cm\":"+ragdollVector(target->position()));
      }
    else if(a.size()==2 && a[1]=="wall") {
      if(state.meleeShadowActors.size()!=3) {arenaAbort("melee shadow setup is required");return true;}
      const auto forward=state.meleeShadowForward;
      const auto parking=pl.position(); // The preceding target command verified this stance's ground.
      const auto at=Vec3(state.meleeShadowWall.x,pl.position().y,state.meleeShadowWall.z)-forward*65.f;
      const auto floor=w.physic()->landRay(at+Vec3(0,200,0),500.f);
      if(!floor.hasCol || floor.n.y<.8f) {arenaAbort("melee shadow wall stance lacks supported ground");return true;}
      pl.setPosition(Vec3(at.x,floor.v.y,at.z));pl.setDirection(forward);pl.setTarget(nullptr);pl.updateTransform();
      for(auto* npc:state.meleeShadowActors) {
        npc->setExternalControl(true);npc->clearAiQueue();npc->clearState(true);npc->clearGoTo();npc->stopWalking();
        npc->setPosition(parking);npc->updateTransform();
        }
      event("melee_shadow_wall","\"weapon\":"+jsonStr(state.meleeShadowWeapon)+",\"attacker_cm\":"+
        ragdollVector(pl.position())+",\"wall_cm\":"+ragdollVector(state.meleeShadowWall));
      }
    else if(a.size()==2 && a[1]=="live") {
      if(state.meleeShadowActors.size()!=3) {arenaAbort("melee shadow setup is required");return true;}
      for(auto* npc:state.meleeShadowActors) exec("fight id:"+std::to_string(w.npcId(npc))+" player",w,pl,ctrl);
      event("melee_shadow_live","\"weapon\":"+jsonStr(state.meleeShadowWeapon)+",\"attacker_cm\":"+ragdollVector(pl.position()));
      }
    else if(a.size()==2 && a[1]=="clear") {
      pl.setTarget(nullptr);
      for(auto* npc:state.meleeShadowActors) if(w.npcId(npc)!=uint32_t(-1)) w.removeNpc(*npc);
      state.meleeShadowActors.clear();state.meleeShadowPlaces.clear();
      }
    else {arenaAbort("melee shadow expects setup/weapon <1h|2h|fist>, controls <g2|classic>, target <0|1|2>, wall, live or clear");return true;}
    }
  else if(c=="melee-physics" && a.size()==6 && a[1]=="setup") {
    const auto* enabled=std::getenv("OPENGOTHIC_MELEE_BENCH");
    const int count=int(num(a,2,-1)),substeps=int(num(a,5,0));
    const auto& mode=a[3];const auto& pelvis=a[4];
    if(enabled==nullptr || std::string_view(enabled)!="fixture" ||
       (count!=0 && count!=1 && count!=2 && count!=4 && count!=6 && count!=10) ||
       (mode!="base" && mode!="full" && mode!="chain" && mode!="blade" && mode!="game") ||
       (pelvis!="kinematic" && pelvis!="dynamic") || (substeps!=1 && substeps!=2) ||
       (count==0 && mode!="base")) {
      arenaAbort("melee setup requires fixture flag, count 0/1/2/4/6/10, mode base/full/chain/blade/game, pelvis kinematic/dynamic, substeps 1/2");return true;
      }
    if(!chooseRagdollArena(w,pl)) {arenaAbort("melee fixture needs an empty supported arena");return true;}
    exec("clear 2500",w,pl,ctrl);
    // "game" keeps the ordinary living bodies the fighters have in play
    if(mode!="game")
      Gothic::settingsSetI("GAME","living_bodies",0);
    w.physic()->configureMeleeBench(substeps);
    state.meleeMode=mode;state.meleePelvis=pelvis;
    const auto symbol=w.script().findSymbolIndex("BDT_1013_BANDIT_L");
    const auto sword=w.script().findSymbolIndex("ItMw_1h_Common_01");
    if(symbol==size_t(-1) || sword==size_t(-1)) {arenaAbort("melee fixture symbols missing");return true;}
    const auto bodyMode=mode=="full" ? Npc::MeleeBenchMode::Full : mode=="chain" ? Npc::MeleeBenchMode::Chain :
                        mode=="blade" ? Npc::MeleeBenchMode::Blade : mode=="game" ? Npc::MeleeBenchMode::Off : Npc::MeleeBenchMode::Base;
    for(int i=0;i<count;++i) {
      auto* npc=w.addNpc(symbol,context.ragdollArenaPlaces[size_t(i)+5]);
      if(!npc) {arenaAbort("melee NPC insertion failed");return true;}
      npc->clearAiQueue();npc->clearState(false);npc->setTarget(nullptr);npc->setExternalControl(true);
      npc->setDirection(pl.position()-npc->position());npc->updateTransform();
      if(npc->currentMeleeWeapon()==nullptr) {npc->addItem(sword,1);npc->useItem(sword,Item::NSLOT,true);}
      if(npc->currentMeleeWeapon()==nullptr) {arenaAbort("melee fixture actor has no equipped sword");return true;}
      npc->configureMeleeBench(bodyMode,pelvis=="dynamic");
      context.crew.push_back(npc);context.livingWalkers.emplace_back();
      auto& walker=context.livingWalkers.back();walker.npc=npc;
      walker.ends[0]=WayPoint(npc->position()-Vec3(0,0,200),"FP_MELEE_BENCH_A");
      walker.ends[1]=WayPoint(npc->position(),"FP_MELEE_BENCH_B");
      for(auto& point:walker.ends) {
        const auto floor=w.physic()->landRay(point.pos+Vec3(0,200,0),500);
        if(!floor.hasCol || floor.n.y<.98f) {arenaAbort("melee walking path has no level floor");return true;}
        point.pos.y=floor.v.y;point.groundPos=point.pos;
        }
      npc->setWalkMode(WalkBit::WM_Walk);
      }
    meleePhysicsCamera();
    state.meleeFixture=true;state.meleeMotionFrom=w.tickCount();
    event("melee_physics_setup","\"actors\":"+std::to_string(count)+",\"mode\":"+jsonStr(mode)+
      ",\"pelvis\":"+jsonStr(pelvis)+",\"substeps\":"+std::to_string(substeps)+",\"animation_cycle_ms\":12000");
    }
  else if(c=="melee-physics" && a.size()==2 && a[1]=="steady") {
    if(!state.meleeFixture || state.meleeMode!="base") {
      arenaAbort("steady melee measurement requires the base fixture");return true;
      }
    // Keep four walkers, three idle actors and three attackers throughout the
    // comparison; long frames must not change the mix by delaying a phase.
    state.meleeSteady=true;
    }
  else if(c=="melee-physics" && a.size()==2 && a[1]=="brawl") {
    if(!state.meleeFixture || (state.meleeMode!="base" && state.meleeMode!="game") || context.crew.size()!=6) {
      arenaAbort("melee brawl requires the six-actor base or game fixture");return true;
      }
    const auto attack=w.script().findSymbolIndex("ZS_ATTACK");
    const auto tactic=w.script().findSymbol("FAI_HUMAN_MASTER");
    const auto reason=w.script().findSymbol("AIV_ATTACKREASON");
    const auto kill=w.script().findSymbol("AR_KILL");
    if(attack==size_t(-1) || !tactic || !reason || !kill || reason->get_int()<0 ||
       size_t(reason->get_int())>=daedalus::INpc::aivar_count) {
      arenaAbort("melee brawl script symbols are absent");return true;
      }
    for(size_t i=0;i<context.crew.size();++i) {
      auto& npc=*context.crew[i];
      const float angle=float(i)*float(M_PI)/3.f;
      Vec3 position=context.ragdollArenaTarget+Vec3(std::cos(angle)*220.f,0,std::sin(angle)*220.f);
      const auto floor=w.physic()->landRay(position+Vec3(0,300,0),600.f);
      if(!floor.hasCol || floor.n.y<.98f) {arenaAbort("melee brawl actor has no level ground");return true;}
      position.y=floor.v.y;
      npc.clearAiQueue();npc.clearState(true);npc.clearGoTo();npc.stopWalking();
      auto& h=npc.handle();
      h.flags=daedalus::NpcFlag(uint32_t(h.flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
      h.attribute[ATR_HITPOINTSMAX]=100000;h.attribute[ATR_HITPOINTS]=100000;
      h.attribute[ATR_STRENGTH]=60;h.hitchance[TALENT_1H]=60;npc.setTalentSkill(TALENT_1H,2);
      h.guild=i%2==0 ? GIL_BDT : GIL_PAL;h.fight_tactic=tactic->get_int();
      h.aivar[size_t(reason->get_int())]=kill->get_int();
      npc.setPosition(position);npc.updateTransform();
      }
    for(size_t i=0;i<context.crew.size();++i) {
      auto& npc=*context.crew[i];auto* target=context.crew[(i+3)%context.crew.size()];
      npc.setDirection(target->position()-npc.position());npc.updateTransform();
      npc.setExternalControl(false);npc.setTarget(target);
      npc.aiPush(AiQueue::aiStartState(ScriptFn(attack),0,target,nullptr,""));
      }
    state.meleeBrawl=true;
    event("melee_physics_brawl","\"actors\":6,\"hp_each\":100000,\"radius_cm\":220");
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  else if(c=="melee-physics" && a.size()==2 && a[1]=="measure") {
    const auto counts=livingCounts();
    const size_t expected=state.meleeMode=="base" ? 0 : context.crew.size()*24;
    size_t rigs=0,dynamicParts=0;bool motorsReady=true;
    for(const auto* npc:context.crew) if(const auto* body=npc->livingBody()) {
      ++rigs;const auto stats=body->benchStats();dynamicParts+=stats.dynamicParts;
      if(state.meleeMode=="full")
        motorsReady&=stats.dynamicParts==(state.meleePelvis=="dynamic" ? 24u : 23u);
      else if(state.meleeMode=="chain") motorsReady&=stats.dynamicParts>0 && stats.dynamicParts<24;
      }
    event("melee_physics_precondition",livingCountFields(counts,w)+",\"expected_parts\":"+std::to_string(expected)+
      ",\"rigs\":"+std::to_string(rigs)+",\"motor_dynamic_parts\":"+std::to_string(dynamicParts));
    const bool fixtureBodies=state.meleeBrawl ?
      std::all_of(context.crew.begin(),context.crew.end(),[](const Npc* npc) {return npc->attribute(ATR_HITPOINTS)>0;}) :
      counts.alive==context.crew.size() && counts.parts==expected && motorsReady &&
      rigs==(state.meleeMode=="base" ? 0 : context.crew.size());
    if(!state.meleeFixture || counts.models!=context.crew.size() || !fixtureBodies || counts.duplicates!=0 ||
       !AppCadence::normalClock() || !AppCadence::arm(state.meleeMode!="base")) {
      arenaAbort("melee measurement precondition failed");return true;
      }
    state.meleeAttackStarts=0;
    AppCadence::counts(true,uint32_t(counts.models),0,uint32_t(counts.parts),0,0);
    w.physic()->measureMeleeBench(outDir+"/melee-physics.csv");state.meleeMeasuring=true;
    event("melee_physics_measure_before",livingCountFields(counts,w)+
      ",\"steady_mix\":"+(state.meleeSteady ? "true" : "false")+",\"brawl\":"+(state.meleeBrawl ? "true" : "false"));
    }
  else if(c=="melee-physics" && a.size()==2 && a[1]=="report") {
    if(!state.meleeMeasuring || !AppCadence::done()) {arenaAbort("melee measurement window is incomplete");return true;}
    state.meleeMeasuring=false;w.physic()->reportMeleeBench();const auto counts=livingCounts();
    AppCadence::counts(false,uint32_t(counts.models),0,uint32_t(counts.parts),0,0);
    event("melee_physics_measure_after",livingCountFields(counts,w)+
      ",\"attack_starts\":"+std::to_string(state.meleeAttackStarts));
    if((!state.meleeBrawl && counts.alive!=context.crew.size()) || counts.duplicates!=0 ||
       (state.meleeBrawl && std::any_of(context.crew.begin(),context.crew.end(),[](const Npc* npc) {return npc->attribute(ATR_HITPOINTS)<=0;})))
      arenaAbort("melee actors changed during measurement");
    }
#endif
  else if(c=="melee-ai-response") {
    // melee-ai-response dodge|parry <attacker>: respond to a real charged release within 45 seconds.
    auto* attacker = a.size()>2 ? findNpc(w,pl,a[2]) : nullptr;
    if(a.size()!=3 || (a[1]!="dodge" && a[1]!="parry") || !attacker || attacker==&pl) {
      arenaAbort("melee AI response expects dodge|parry attacker");return true;
      }
    state.meleeAiResponse = Melee::MeleeAiResponse{&pl,attacker,a[1]=="parry",w.tickCount()+45000};
    say("melee AI response " + a[1] + " armed against " + symName(w,attacker));
    }
  else return false;
  return true;
  }

}
