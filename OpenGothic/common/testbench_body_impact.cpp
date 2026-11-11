#include "testbench_internal.h"
#include "testbench.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "game/bodyarena.h"
#include "game/bodycontroller.h"
#include "game/bodyobservations.h"
#include "game/damagecalculator.h"
#include "game/gamescript.h"
#include "game/herodeed.h"
#include "gothic.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "world/aiqueue.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

void BodyImpact::deed(const HeroDeed& value) {
  if(npc && value.victim.npc==TestBench::who(npc) && value.kind=="murder")++deeds;
  }
void BodyImpact::perception(Npc& observer,Npc& other,Npc* victim,PercType value) {
  if(&observer!=witness || victim!=npc || &other!=culprit)return;
  if(value==PERC_ASSESSMURDER)++murders;
  if(value==PERC_ASSESSDEFEAT)++defeats;
  }

void bodyImpactSurface(BodyImpact& state,World& world,const std::vector<Vec3>& vertices,
                       std::unique_ptr<PhysicMeshShape>& shape,DynamicWorld::Item& body) {
  body=DynamicWorld::Item();shape.reset();
  asset::WorldMesh mesh;mesh.materials.resize(1);mesh.materials[0].group=asset::MaterialGroup::STONE;
  for(const auto& point:vertices)mesh.vertices.push_back({point.x,point.y,point.z});
  for(uint32_t i=0;i<vertices.size();i+=4) {
    mesh.polygons.vertex_indices.insert(mesh.polygons.vertex_indices.end(),{i,i+2,i+1,i,i+3,i+2});
    mesh.polygons.material_indices.insert(mesh.polygons.material_indices.end(),{0,0});
    }
  shape.reset(PhysicMeshShape::load(PackedMesh(mesh,PackedMesh::PK_Physic)));
  auto matrix=Matrix4x4::mkIdentity();matrix.translate(state.anchor);
  body=world.physic()->addStaticObj(shape.get(),matrix,"BENCH_BODY_IMPACT");
  if(body.isEmpty())arenaAbort("body impact fixture has no native support");
  }

void bodyImpactClear(BodyImpact& state,const BodyImpactContext& context,World& world,Npc& player) {
  state.tracing=false;
  if(state.guardSaved) {Gothic::settingsSetI("GAME","ragdoll_quest_guard",state.questGuard);state.guardSaved=false;}
  if(state.observing) {
    Gothic::inst().onHeroDeed.ubind(&state,&BodyImpact::deed);
    Gothic::inst().onObservedPerception.ubind(&state,&BodyImpact::perception);
    Gothic::inst().onExternalPerception.ubind(&state,&BodyImpact::perception);
    state.observing=false;
    }
  player.setTarget(nullptr);player.closeWeapon(true);
  for(auto* npc:{state.npc,state.animated,state.witness,state.culprit==state.player ? nullptr : state.culprit})if(npc) {
    if(context.subject==npc)context.subject=nullptr;
    std::erase_if(context.spellActors,[npc](const auto& actor){return actor.npc==npc;});
    std::erase(context.crew,npc);
    world.removeNpc(*npc);
    }
  state.npc=state.animated=state.witness=nullptr;
  state.culprit=nullptr;
  state.wall=DynamicWorld::Item();state.wallShape.reset();
  state.ledge=DynamicWorld::Item();state.ledgeShape.reset();
  context.spellTrace=false;
  }

Npc* bodyImpactActor(World& world,std::string_view symbol,const Vec3& position) {
  auto* npc=world.addNpc(world.script().findSymbolIndex(symbol),position);
  if(!npc) {arenaAbort("body impact actor insertion failed");return nullptr;}
  npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);npc->clearGoTo();npc->clearSpeed();
  npc->handle().flags=daedalus::NpcFlag(uint32_t(npc->handle().flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
  npc->handle().attribute[ATR_HITPOINTS]=npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
  npc->setDirection(Vec3(0,0,-1));npc->updateTransform();
  return npc;
  }

void bodyImpactSetup(BodyImpact& state,const BodyImpactContext& context,World& world,Npc& player,std::string_view mode,float value) {
  bodyImpactClear(state,context,world,player);context.bodyTrace=true;
  if(!state.ready) {
    state.anchor=player.position()+Vec3(0,10000,0);
    bodyImpactSurface(state,world,{{-2200,0,-1800},{-2200,0,1800},{2200,0,1800},{2200,0,-1800}},state.floorShape,state.floor);
    state.ready=true;
    }
  state.mode=mode;state.value=value;state.player=state.culprit=&player;state.damage=state.capsuleDamage=0;
  state.impacts=state.deeds=state.murders=state.defeats=0;state.weapon=size_t(-1);
  // The numeric fall must reach its support rather than the separate quest return.
  state.questGuard=Gothic::settingsGetI("GAME","ragdoll_quest_guard");state.guardSaved=true;
  Gothic::settingsSetI("GAME","ragdoll_quest_guard",0);
  state.maximumSpeed=0;state.launchSpeed=0;state.sawAir=false;
  const auto origin=state.anchor+Vec3(-500,0,0);
  player.clearAiQueue();player.stopWalking();player.clearSpeed();player.setPosition(origin-Vec3(0,0,180));
  player.setDirection(Vec3(0,0,1));player.updateTransform();
  state.npc=bodyImpactActor(world,"BAU_904_BAUER",origin);if(!state.npc)return;
  auto& npc=*state.npc;
  const bool paired=mode=="nonlethal" || mode=="lethal";
  if(paired) {
    state.culprit=bodyImpactActor(world,"SLD_826_SOELDNER",origin-Vec3(350,0,0));
    if(!state.culprit)return;
    }
  Gothic::settingsSetI("GAME","ragdoll",1);Gothic::settingsSetI("GAME","spell_physics",1);
  if(mode=="unconscious" || mode=="dead") {
    npc.setRagdollDown(mode=="dead");npc.prepareRagdoll();
    }
  else {
    // Exercise the actual posture knockdown, then exclude preparation weapon damage
    // from the observation. No transient body flag or health state is fabricated.
    const auto stagger=Gothic::settingsGetI("GAME","melee_stagger");
    Gothic::settingsSetI("GAME","melee_posture",1);Gothic::settingsSetI("GAME","melee_stagger",0);
    state.culprit->setToFistMode();
    MeleeBladeContact contact;contact.npc=&npc;contact.point=npc.collosionCenter();
    contact.velocityCmS=Vec3(0,0,4000);contact.speedCmS=4000;contact.surfaceAngleDeg=90;
    for(unsigned i=0;i<12 && !npc.isMeleeKnockedDown();++i)npc.takeDamage(*state.culprit,nullptr,true,1.f,&contact);
    Gothic::settingsSetI("GAME","melee_stagger",stagger);
    if(!npc.isMeleeKnockedDown() || !npc.ragdollBody() || npc.isDown()) {
      arenaAbort("body impact preparation did not create a living posture knockdown");return;
      }
    npc.handle().attribute[ATR_HITPOINTS]=50000;
    }
  if(!npc.ragdollBody()) {arenaAbort("body impact preparation has no native ragdoll");return;}
  if(paired) {
    if(mode=="nonlethal")state.culprit->setNonlethalFight(npc);
    // Enable NPC deed capture in both controls, so lethal proves that the
    // nonlethal case's empty murder count is meaningful.
    state.culprit->setServerDeed("murder",&npc);
    state.weapon=world.script().findSymbolIndex("ITMW_1H_MACE_L_01");
    npc.setToFightMode(state.weapon);
    if(!npc.currentMeleeWeapon() || npc.currentMeleeWeapon()->clsId()!=state.weapon) {
      arenaAbort("body impact paired victim has no drawn weapon");return;
      }
    }
  if(mode=="fall") {
    const float h=value*100.f;
    bodyImpactSurface(state,world,{{500,h,-160},{500,h,160},{750,h,160},{750,h,-160}},state.ledgeShape,state.ledge);
    state.animated=bodyImpactActor(world,"BAU_904_BAUER",state.anchor+Vec3(650,h,0));
    if(!state.animated)return;
    if(state.animated->guild()!=npc.guild()) {arenaAbort("body impact fall comparators have different guilds");return;}
    }
  if(mode=="wall" || mode=="fatal" || paired)
    bodyImpactSurface(state,world,{{-1600,0,180},{-1600,1600,180},{1600,1600,180},{1600,0,180}},state.wallShape,state.wall);
  state.witness=bodyImpactActor(world,"BAU_904_BAUER",state.anchor+Vec3(200,0,-200));
  Gothic::inst().onHeroDeed.bind(&state,&BodyImpact::deed);
  Gothic::inst().onObservedPerception.bind(&state,&BodyImpact::perception);
  Gothic::inst().onExternalPerception.bind(&state,&BodyImpact::perception);state.observing=true;
  context.subject=state.npc;
  event("body_impact_setup","\"case\":"+jsonStr(state.mode)+",\"value\":"+ragdollNumber(value)+
    ",\"guild\":"+std::to_string(npc.guild())+",\"mass_kg\":"+ragdollNumber(npc.physicsMass())+
    ",\"falldown_height_cm\":"+std::to_string(world.script().guildVal().falldown_height[npc.guild()])+
    ",\"falldown_damage_per_m\":"+std::to_string(world.script().guildVal().falldown_damage[npc.guild()])+
    ",\"prot_fall\":"+std::to_string(npc.protection(PROT_FALL))+",\"quest_guard\":false"+
    ",\"knockdown\":"+(npc.isMeleeKnockedDown()?"true":"false"));
  }

void bodyImpactLaunch(BodyImpact& state,World& world,Npc& player) {
  auto* npc=state.npc;
  if(!npc || !npc->ragdollBody()) {arenaAbort("body impact launch requires prepared native body");return;}
  const bool paired=state.mode=="nonlethal" || state.mode=="lethal";
  const bool wall=state.mode=="wall" || state.mode=="fatal" || paired;
  npc->setPosition(npc->position()+Vec3(0,wall ? 200.f : state.value*100.f,0));
  auto saved=npc->ragdollBody()->state();
  for(auto& part:saved.parts) {
    part.linearVelocity=part.linearVelocityMeters=part.angularVelocity={};part.sleeping=false;
    }
  if(!const_cast<Ragdoll*>(npc->ragdollBody())->restore(saved)) {arenaAbort("body impact launch cannot reset native velocities");return;}
  if(state.mode=="immortal")npc->handle().flags=daedalus::NpcFlag(uint32_t(npc->handle().flags)|uint32_t(daedalus::NpcFlag::IMMORTAL));
  if(state.mode=="fatal" || paired)npc->handle().attribute[ATR_HITPOINTS]=20;
  state.hpBefore=npc->attribute(ATR_HITPOINTS);state.animatedBefore=state.animated ? state.animated->attribute(ATR_HITPOINTS) : 0;
  state.damage=state.capsuleDamage=0;state.impacts=state.deeds=state.murders=state.defeats=0;
  state.maximumSpeed=0;state.started=world.tickCount();state.tracing=true;
  // Ragdoll's existing .12/s damping acts once before the first 120 Hz contact.
  // Prepare the requested contact speed without changing the native damping.
  state.launchSpeed=wall ? state.value/(1.f-.12f/120.f) : 0.f;
  if(wall && !npc->addSpellImpulse(Vec3(0,0,npc->physicsMass()*state.launchSpeed),true,state.culprit)) {
    arenaAbort("body impact native wall impulse was refused");return;
    }
  if(state.animated) {
    // Sneak steps over the edge without Run's horizontal fall-energy surplus.
    state.animated->setWalkMode(WalkBit::WM_Sneak);
    state.animated->setExternalControl(false);
    state.animated->setDirection(Vec3(1,0,0));state.animated->updateTransform();
    if(!state.animated->goToPosition(state.anchor+Vec3(1400,state.value*100.f,0))) {
      arenaAbort("body impact animated comparator could not step off its ledge");return;
      }
    }
  event("body_impact_launch","\"case\":"+jsonStr(state.mode)+",\"value\":"+ragdollNumber(state.value)+
    ",\"hp_before\":"+std::to_string(state.hpBefore)+",\"normal_speed_m_s\":"+ragdollNumber(wall?state.value:0)+
    ",\"launch_speed_m_s\":"+ragdollNumber(state.launchSpeed)+
    ",\"drop_m\":"+ragdollNumber(wall?0:state.value)+",\"feature\":"+(Gothic::physicsFlags().bodyImpactDamage?"true":"false"));
  }

void bodyImpactWind(BodyImpact& state,const BodyImpactContext& context,World& world,Npc& player) {
  bodyImpactClear(state,context,world,player);
  prepareSpellFixture(world,player,"wind");
  for(auto& actor:context.spellActors)world.removeNpc(*actor.npc);
  context.spellActors.clear();context.crew.clear();
  if(!state.ready) {arenaAbort("body impact wind requires the body impact floor");return;}
  state.mode="wind_goblin";state.value=0;state.player=state.culprit=&player;state.damage=state.capsuleDamage=0;
  state.impacts=state.deeds=state.murders=state.defeats=0;
  state.maximumSpeed=0;state.launchSpeed=0;state.sawAir=false;context.bodyTrace=true;
  bodyImpactSurface(state,world,{{-1600,0,80},{-1600,1600,80},{1600,1600,80},{1600,0,80}},state.wallShape,state.wall);
  state.npc=bodyImpactActor(world,"GOBBO_GREEN",state.anchor);if(!state.npc)return;
  player.setPosition(state.anchor-Vec3(0,0,230));player.setDirection(Vec3(0,0,1));player.updateTransform();
  context.spellActors.push_back({state.npc,"body_impact_target"});context.subject=state.npc;
  state.hpBefore=state.npc->attribute(ATR_HITPOINTS);state.started=world.tickCount();state.tracing=true;
  event("body_impact_wind_setup","\"case\":\"wind_goblin\",\"mass_kg\":"+ragdollNumber(state.npc->physicsMass())+
    ",\"guild\":"+std::to_string(state.npc->guild())+",\"height_scale\":1,\"cast_distance_cm\":230,\"wall_distance_cm\":80");
  }

void bodyImpactReport(BodyImpact& state,World& world,Npc& player) {
  const auto* npc=state.npc;
  if(!npc || !state.tracing) {arenaAbort("body impact report requires a launched case");return;}
  const bool enabled=Gothic::physicsFlags().bodyImpactDamage;
  const bool nonlethal=state.mode=="nonlethal",paired=nonlethal || state.mode=="lethal";
  const bool fall=state.mode=="fall",wind=state.mode=="wind_goblin",fatal=state.mode=="fatal" || state.mode=="lethal";
  const bool excluded=state.mode=="unconscious" || state.mode=="dead" || state.mode=="immortal";
  const bool wall=state.mode=="wall" || fatal || nonlethal || wind;
  const float wallZ=state.anchor.z+(wind ? 80.f : 180.f);
  float frontZ=npc->navigationPhysicsPosition().z;
  if(const auto* body=npc->ragdollBody())if(const auto* skeleton=npc->visualSkeleton()) {
    frontZ=-std::numeric_limits<float>::infinity();
    for(const auto& part:body->state().parts) {
      auto name=skeleton->nodes[part.bone].name;
      for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name.find("PELVIS")!=std::string::npos || name.find("SPINE")!=std::string::npos || name.find("HEAD")!=std::string::npos)
        frontZ=std::max(frontZ,part.centerOfMassMeters.z*100.f);
      }
    }
  const bool wallBlocked=wall && frontZ<=wallZ+5.f;
  const float normalSpeedError=wall && !wind && enabled ? std::abs(state.maximumSpeed-state.value*100.f)/(state.value*100.f)*100.f : 0.f;
  const float freefallSpeed=fall ? std::sqrt(2.f*DynamicWorld::gravity*state.value*100.f)*1000.f : 0.f;
  const float freefallError=fall && enabled ? std::abs(state.maximumSpeed-freefallSpeed)/freefallSpeed*100.f : 0.f;
  const float speed=fall || wind ? state.maximumSpeed*.001f : state.value*.1f;
  const int formula=DamageCalculator::damageFall(*state.npc,speed).value;
  const int damage=state.hpBefore-npc->attribute(ATR_HITPOINTS);
  const int animated=state.animated ? state.animatedBefore-state.animated->attribute(ATR_HITPOINTS) : 0;
  const int expected=enabled && !excluded ? formula : 0;
  const float tolerance=fall ? std::max(2.f,float(expected)*.1f) : float(expected)*.1f;
  const bool attributed=npc->bodyImpactCulprit()==state.culprit;
  bool weaponDropped=false;
  if(paired)world.detectItem(npc->position(),1200.f,[&](Item& item) {
    weaponDropped|=item.clsId()==state.weapon;
    });
  bool pass=wind ? (enabled ? state.capsuleDamage>0 && attributed && state.sawAir : state.damage==0 && state.capsuleDamage==0) :
            float(std::abs(damage-expected))<=tolerance;
  if(fall)pass&=state.sawAir && (!enabled || freefallError<=10.f);
  if(excluded || (!enabled && !wind))pass&=damage==0 && state.deeds==0 && state.murders==0;
  if(fatal && enabled)pass=npc->isDead() && npc->lastHitBy()==state.culprit && attributed && state.deeds>0 && state.murders>0;
  if(nonlethal && enabled)pass=npc->attribute(ATR_HITPOINTS)==1 && npc->isUnconscious() && !npc->isDead() &&
      !npc->isMeleeKnockedDown() && npc->ragdollBody()!=nullptr && npc->lastHitBy()==state.culprit &&
      npc->stateOther()==state.culprit && attributed && state.deeds==0 && state.murders==0 && state.defeats>0 &&
      npc->bodyTrace().id==0 && weaponDropped && npc->itemCount(state.weapon)==0;
  if(state.mode=="lethal" && enabled)pass&=npc->bodyTrace().id!=0;
  if(!fall && !excluded && !wind && enabled)pass&=attributed;
  if(wall && !wind && enabled)pass&=normalSpeedError<=1.f;
  if(wall)pass&=wallBlocked;
  event("body_impact_result","\"case\":"+jsonStr(state.mode)+",\"value\":"+ragdollNumber(state.value)+
    ",\"feature\":"+(enabled?"true":"false")+",\"elapsed_ms\":"+std::to_string(world.tickCount()-state.started)+
    ",\"hp_before\":"+std::to_string(state.hpBefore)+",\"hp_after\":"+std::to_string(npc->attribute(ATR_HITPOINTS))+
    ",\"damage\":"+std::to_string(damage)+",\"body_damage\":"+std::to_string(state.damage)+
    ",\"capsule_wall_damage\":"+std::to_string(state.capsuleDamage)+
    ",\"formula_damage\":"+std::to_string(formula)+",\"expected_damage\":"+std::to_string(expected)+
    ",\"animated_damage\":"+std::to_string(animated)+",\"animated_airborne\":"+(state.sawAir?"true":"false")+
    ",\"impacts\":"+std::to_string(state.impacts)+",\"maximum_normal_speed_cm_s\":"+ragdollNumber(state.maximumSpeed)+
    ",\"expected_freefall_speed_cm_s\":"+(fall?ragdollNumber(freefallSpeed):"null")+
    ",\"freefall_speed_error_percent\":"+(fall && enabled?ragdollNumber(freefallError):"null")+
    ",\"launch_speed_m_s\":"+ragdollNumber(state.launchSpeed)+
    ",\"normal_speed_error_percent\":"+(wall && !wind && enabled?ragdollNumber(normalSpeedError):"null")+
    ",\"wall_z_cm\":"+(wall?ragdollNumber(wallZ):"null")+
    ",\"front_z_cm\":"+(wall?ragdollNumber(frontZ):"null")+",\"wall_blocked\":"+(wallBlocked?"true":"false")+
    ",\"root_position_cm\":"+ragdollVector(npc->position())+
    ",\"culprit_hero\":"+(attributed && state.culprit==&player?"true":"false")+",\"killer_hero\":"+(npc->lastHitBy()==&player?"true":"false")+
    ",\"deeds\":"+std::to_string(state.deeds)+",\"murder_perceptions\":"+std::to_string(state.murders)+
    ",\"defeat_perceptions\":"+std::to_string(state.defeats)+",\"culprit_npc\":"+(attributed && state.culprit!=&player?"true":"false")+
    ",\"last_hit_culprit\":"+(npc->lastHitBy()==state.culprit?"true":"false")+
    ",\"other_culprit\":"+(npc->stateOther()==state.culprit?"true":"false")+
    ",\"death_trace_id\":"+std::to_string(npc->bodyTrace().id)+",\"weapon_dropped\":"+(weaponDropped?"true":"false")+
    ",\"unconscious\":"+(npc->isUnconscious()?"true":"false")+",\"dead\":"+(npc->isDead()?"true":"false")+
    ",\"immortal\":"+(npc->isImmortal()?"true":"false")+",\"pass\":"+(pass?"true":"false"));
  state.tracing=false;
  if(!pass)arenaAbort("body impact acceptance failed");
  }

void bodyImpactRecovered(BodyImpact& state) {
  const auto* npc=state.npc;
  const bool pass=npc && state.mode=="nonlethal" && !npc->isDown() && !npc->isMeleeKnockedDown() &&
      !npc->ragdollBody() && !npc->isRagdollBlending() && npc->bodyTrace().id==0;
  event("body_impact_recovered","\"case\":"+jsonStr(state.mode)+",\"pass\":"+(pass?"true":"false"));
  if(!pass)arenaAbort("body impact nonlethal recovery failed");
  }

void bodyImpactDuel(BodyImpact& state,const BodyImpactContext& context,World& world,Npc& player,uint32_t seed,std::string_view weapon) {
  if(BodyArena::enabled()) {
    const auto actors=BodyArena::actors();endAnimationControl();BodyArena::disable();BodyObservations::disable();
    BodyController::reset("body impact duel reset");
    for(auto* npc:actors)world.removeNpc(*npc);
    }
  context.bodyTrace=true;
  BodyArena::Config config;config.seed=seed;config.weapon=weapon;
  BodyArena::configure(config,seed);
  BodyArena::setSink([](std::string_view row){event("body_impact_arena","\"arena\":"+std::string(row));});
  setupArena(world,player);
  BodyObservations::watch(BodyArena::actors());BodyArena::sealInitialState();BodyArena::start("native");
  state.duelBodies.clear();
  for(auto* npc:BodyArena::actors())state.duelBodies.push_back({npc});
  state.duel=true;context.waitUntil=context.time+60100;
  event("body_impact_duel_start","\"seed\":"+std::to_string(seed)+",\"weapon\":"+jsonStr(weapon)+
    ",\"feature\":"+(Gothic::physicsFlags().bodyImpactDamage?"true":"false"));
  }

void bodyImpactTick(World& world) {
  const auto context=bodyImpactContext();
  auto& state=context.bodyImpact;
  if(state.tracing) {
    if(state.animated)state.sawAir|=state.animated->isInAir() || state.animated->isFalling();
    if(state.mode=="wind_goblin" && state.npc)state.sawAir|=state.npc->isInAir();
    }
  if(state.duel) {
    BodyArena::postStep();
    for(auto& body:state.duelBodies) {
      const bool knocked=body.npc->isMeleeKnockedDown();
      if(knocked && !body.knocked) {
        body.knocked=true;body.damage=0;body.speed=0;body.since=world.tickCount();
        }
      if(body.knocked && (!knocked || BodyArena::complete())) {
        event("body_impact_duel_knockdown","\"seed\":"+std::to_string(BodyArena::seed())+
          ",\"actor\":"+jsonStr(TestBench::who(body.npc))+",\"since_ms\":"+std::to_string(body.since)+
          ",\"elapsed_ms\":"+std::to_string(world.tickCount()-body.since)+",\"body_damage\":"+std::to_string(body.damage)+
          ",\"maximum_normal_speed_cm_s\":"+ragdollNumber(body.speed)+
          ",\"terminal\":"+(BodyArena::complete()?"true":"false"));
        body.knocked=false;
        }
      }
    if(BodyArena::complete()) {
      event("body_impact_duel_result","\"feature\":"+(Gothic::physicsFlags().bodyImpactDamage?std::string("true"):std::string("false"))+
        ",\"arena\":"+BodyArena::statusJson());
      state.duel=false;context.waitUntil=context.time;endAnimationControl();
      }
    }
  }
bool bodyImpactCommand(BodyImpact& state,const BodyImpactContext& context,const std::vector<std::string>& a,World& w,Npc& pl) {
  const std::string& c = a[0];
  if(c=="body-impact" && a.size()==4 && a[1]=="setup") {
    if(a[2]!="fall" && a[2]!="wall" && a[2]!="fatal" && a[2]!="nonlethal" && a[2]!="lethal" &&
       a[2]!="unconscious" && a[2]!="dead" && a[2]!="immortal") {
      arenaAbort("body impact setup expects fall, wall, fatal, nonlethal, lethal, unconscious, dead or immortal");return true;
      }
    bodyImpactSetup(state,context,w,pl,a[2],num(a,3,0));
    }
  else if(c=="body-impact" && a.size()==2 && a[1]=="launch")bodyImpactLaunch(state,w,pl);
  else if(c=="body-impact" && a.size()==2 && a[1]=="report")bodyImpactReport(state,w,pl);
  else if(c=="body-impact" && a.size()==2 && a[1]=="standup") {
    if(Gothic::physicsFlags().bodyImpactDamage) {
      auto* npc=state.npc;
      if(!npc || state.mode!="nonlethal" || !npc->isUnconscious()) {
        arenaAbort("body impact standup requires the nonlethal knockout");return true;
        }
      npc->clearAiQueue();npc->setExternalControl(false);npc->clearState(false);
      if(npc->isAiQueueEmpty())npc->aiPush(AiQueue::aiStandup());
      }
    }
  else if(c=="body-impact" && a.size()==2 && a[1]=="recovered")bodyImpactRecovered(state);
  else if(c=="body-impact" && a.size()==2 && a[1]=="wind")bodyImpactWind(state,context,w,pl);
  else if(c=="body-impact" && a.size()==2 && a[1]=="clear")bodyImpactClear(state,context,w,pl);
  else if(c=="body-impact" && a.size()==4 && a[1]=="duel")bodyImpactDuel(state,context,w,pl,uint32_t(std::stoul(a[2])),a[3]);
  else return false;
  return true;
  }

}
