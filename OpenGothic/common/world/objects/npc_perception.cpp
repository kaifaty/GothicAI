#include "npc.h"

#include <algorithm>
#include <cmath>

#include "game/bodycontroller.h"
#include "game/deedwitness.h"
#include "game/herodeed.h"
#include "graphics/mesh/skeleton.h"
#include "gothic.h"
#include "physics/physicsnoise.h"
#include "world/benchhooks.h"
#include "world/objects/item.h"
#include "world/world.h"

using namespace Tempest;

void Npc::setPerceptionTime(uint64_t time) {
  perceptionTime = time;
  }

uint64_t Npc::perceptionTimeClampt() const {
  return std::max<uint64_t>(perceptionTime, 1);
  }

void Npc::setPerceptionEnable(PercType t, size_t fn) {
  if(t>0 && t<PERC_Count)
    perception[t].func = fn;
  }

void Npc::setPerceptionDisable(PercType t) {
  if(t>0 && t<PERC_Count)
    perception[t].func = ScriptFn();
  }

void Npc::startDialog(Npc& pl) {
  if(BodyController::owns(*this)) {
    BodyController::legacyRejected(*this,"dialogue_owned");
    return;
    }
  if(pl.isDown() || pl.isInAir() || isPlayer())
    return;
  stopPhysicsInvestigation(false);
  if(perceptionProcess(pl,nullptr,0,PERC_ASSESSTALK))
    setOther(&pl);
  }

bool Npc::perceptionProcess(Npc &pl) {
  static bool dbg = false;
  static int  kId = -1;
  if(dbg && hnpc->id!=kId)
    return false;

  if(isPlayer())
    return true;

  bool ret=false;
  SensesBit sensedPlayer = SensesBit::SENSE_NONE;
  const auto trace = [&](std::string_view gate) {
    if(!Bench::bodyTracing() || currentTarget==nullptr)
      return;
    Bench::bodyEvent(*this,&pl,"native_perception",owner.tickCount(),
                         "\"perception_kind\":\"periodic\",\"gate\":"+Bench::bodyJson(gate)+
                         ",\"process_policy\":"+std::to_string(int(processPolicy()))+
                         ",\"external_control\":"+(externalCtrl ? "true" : "false")+
                         ",\"sensed_player\":"+std::to_string(int(sensedPlayer))+
                         ",\"result\":"+(ret ? "true" : "false")+
                         ",\"next_time\":"+std::to_string(perceptionNextTime));
    };
  if(processPolicy()!=NpcProcessPolicy::AiNormal) {
    perceptionNextTime = owner.tickCount()+perceptionTimeClampt();
    trace("policy_disabled");
    return ret;
    }

  const float quadDist = pl.qDistTo(*this);
  const bool assessPlayer = hasPerc(PERC_ASSESSPLAYER);
  if(assessPlayer)
    sensedPlayer = canSenseNpc(pl,false);
  if(assessPlayer && sensedPlayer!=SensesBit::SENSE_NONE) {
    if(perceptionProcess(pl,nullptr,quadDist,PERC_ASSESSPLAYER)) {
      ret = true;
      }
    }

  Npc* enem=hasPerc(PERC_ASSESSENEMY) ? updateNearestEnemy() : nullptr;
  if(enem!=nullptr){
    float dist=qDistTo(*enem);
    if(perceptionProcess(*enem,nullptr,dist,PERC_ASSESSENEMY)){
      ret          = true;
      } else {
      nearestEnemy = nullptr;
      }
    }

  Npc* body=hasPerc(PERC_ASSESSBODY) ? updateNearestBody() : nullptr;
  if(body!=nullptr){
    float dist=qDistTo(*body);
    if(perceptionProcess(*body,nullptr,dist,PERC_ASSESSBODY)) {
      ret = true;
      }
    }

  // if(aiQueue.size()==0) // NOTE: Gothic1 fights
  perceptionNextTime = owner.tickCount()+perceptionTimeClampt();
  trace("normal");
  return ret;
  }

bool Npc::passPerception(Npc& other, Npc* victim, PercType perc) {
  percPassThrough = true;
  const bool ret = perceptionProcess(other,victim,0,perc);
  percPassThrough = false;
  return ret;
  }

bool Npc::perceptionProcess(Npc &pl, Npc* victim, float quadDist, PercType perc, const std::shared_ptr<HeroDeed>& deed) {
  const auto trace = [&](std::string_view gate, bool result, bool dispatched) {
    if(!Bench::bodyTracing() || currentTarget==nullptr)
      return;
    Bench::bodyEvent(*this,&pl,"native_perception",owner.tickCount(),
                         "\"perception_kind\":\"dispatch\",\"gate\":"+Bench::bodyJson(gate)+
                         ",\"process_policy\":"+std::to_string(int(processPolicy()))+
                         ",\"perception\":"+std::to_string(int(perc))+
                         ",\"quad_distance_cm\":"+std::to_string(quadDist)+
                         ",\"external_control\":"+(externalCtrl ? "true" : "false")+
                         ",\"native_callback_dispatched\":"+(dispatched ? "true" : "false")+
                         ",\"result\":"+(result ? "true" : "false"));
    };
  if(BodyController::owns(*this)) {
    if(deed!=nullptr)
      deed->witnessed(*this);
    Gothic::inst().onObservedPerception(*this,pl,victim,perc);
    trace("body_control_owned",false,false);
    return false;
    }
  if(!aiState.started && aiState.funcIni.isValid()) {
    // avoid ugly soft-lock (ZS_MM_Attack <-> B_MM_AssessWarn) for the orks near ramp
    trace("waiting_for_state",false,false);
    return false;
    }

  float r = perceptionRange(perc);
  r = r*r;

  if(quadDist>r) {
    trace("out_of_range",false,false);
    return false;
    }

  if(externalCtrl) {
    if(deed!=nullptr)
      deed->witnessed(*this);
    Gothic::inst().onExternalPerception(*this,pl,victim,perc);
    trace("external_control",false,false);
    return false;
    }
  Gothic::inst().onObservedPerception(*this,pl,victim,perc);

  if(isPhysicsInvestigating()) {
    constexpr auto interrupts = [](PercType observed) {
      switch(observed) {
      case PERC_ASSESSENEMY:
      case PERC_ASSESSFIGHTER:
      case PERC_ASSESSDAMAGE:
      case PERC_ASSESSOTHERSDAMAGE:
      case PERC_ASSESSTHREAT:
      case PERC_ASSESSFIGHTSOUND:
      case PERC_ASSESSWARN:
      case PERC_ASSESSCALL:
      case PERC_ASSESSTALK:
      case PERC_DRAWWEAPON:
      case PERC_ASSESSMURDER:
      case PERC_ASSESSMAGIC:
      case PERC_ASSESSCASTER:
      case PERC_CATCHTHIEF:
      case PERC_ASSESSTHEFT:
      case PERC_ASSESSUSEMOB:
      case PERC_ASSESSENTERROOM:
      case PERC_MOVEMOB:
      case PERC_OBSERVEINTRUDER:
      case PERC_OBSERVESUSPECT:
      case PERC_ASSESSBODY:
      case PERC_ASSESSDEFEAT:
        return true;
      default:
        return false;
        }
      };
    // Contract for this active-investigation gate: witnessed crimes reach the scripts,
    // while ordinary proximity and quiet sounds keep the ongoing check intact.
    static_assert(interrupts(PERC_CATCHTHIEF) && interrupts(PERC_ASSESSTHEFT));
    static_assert(interrupts(PERC_ASSESSENTERROOM));
    static_assert(!interrupts(PERC_ASSESSPLAYER) && !interrupts(PERC_ASSESSQUIETSOUND));
    if(!interrupts(perc)) {
      // The planner has observed it above. Routine chatter and ordinary hero
      // activity must not replace the ongoing check through a script callback.
      trace("investigating",false,false);
      return false;
      }
    stopPhysicsInvestigation(false);
    }

  if(hasPerc(perc)) {
    // Receipt makes a witness, even when the server takes the reaction. A late
    // passPerception has no deed, so returning it to scripts cannot add one twice.
    if(deed!=nullptr)
      deed->witnessed(*this);
    auto& intercept = Gothic::inst().interceptPerception;
    if(!percPassThrough && intercept && intercept(*this,pl,victim,perc,deed.get())) {
      trace("intercepted",true,false);
      return true;  // taken over by an outside AI (ai_bridge); it may hand it back
      }
    trace("native_callback",true,true);
    owner.script().invokeState(this,&pl,victim,perception[perc].func);
    return true;
    }
  if(perc==PERC_ASSESSMAGIC && isPlayer()) {
    auto defaultFn = owner.script().playerPercAssessMagic();
    if(defaultFn.isValid()) {
      trace("native_magic_callback",true,true);
      owner.script().invokeState(this,&pl,victim,defaultFn);
      }
    return true;
    }
  trace("no_callback",false,false);
  return false;
  }

bool Npc::canHearPhysicsNoise() const {
  if(isPlayer() || processPolicy()!=NpcProcessPolicy::AiNormal || externalCtrl || isDown() ||
     isLie() || isTalk() || isAttack() || currentTarget!=nullptr || talkPartner()!=nullptr ||
     Gothic::inst().isNpcInDialog(*this))
    return false;
  const auto* state = owner.script().findSymbol(aiState.funcIni.ptr);
  return state==nullptr || (state->name()!="ZS_SLEEP" && state->name()!="ZS_MAGICSLEEP" &&
                            state->name()!="ZS_MM_RTN_SLEEP");
  }

float Npc::perceptionRange(PercType perc) const {
  return float(world().script().percRanges().at(perc,hnpc->senses_range));
  }

bool Npc::canWitnessDeed(const Npc& culprit, const Npc* victim, PercType perc) const {
  const auto* state = owner.script().findSymbol(aiState.funcIni.ptr);
  DeedWitness gates;
  gates.culprit = this==&culprit;
  gates.victim = this==victim;
  gates.player = isPlayer();
  gates.guild = guild();
  gates.perceptionEnabled = hasPerc(perc);
  gates.down = isDown();
  gates.lying = isLie();
  gates.simulated = processPolicy()==NpcProcessPolicy::AiNormal;
  gates.stateStarted = aiState.started || !aiState.funcIni.isValid();
  gates.sleeping = state!=nullptr && (state->name()=="ZS_SLEEP" || state->name()=="ZS_MAGICSLEEP" ||
                                    state->name()=="ZS_MM_RTN_SLEEP");
  // A participant in an unrelated fight is occupied. A defender already fighting
  // this culprit may still see a fresh deed against somebody else.
  gates.foreignFight = DeedWitness::foreignCombat(isAttack(),currentTarget==&culprit);
  gates.range = perceptionRange(perc);
  // Native ASSESSOTHERSDAMAGE is sent by the damaged body (PerceptionMsg::self),
  // so delivery measures range from the victim, while OTHER names the culprit.
  gates.distanceSquared = perc==PERC_ASSESSOTHERSDAMAGE && victim!=nullptr ?
                          qDistTo(*victim) : qDistTo(culprit);
  return gates.admitted() && canSeeNpc(culprit,false);
  }

void Npc::setServerDeed(std::string_view kind, Npc* victim, size_t count, std::string_view expectedOwner) {
  serverDeedKind = kind;
  serverDeedVictim = victim;
  serverAttackRecorded = false;
  serverDeedSimulationLost = false;
  serverDeedCount = count;
  serverDeedOwner = expectedOwner;
  }

void Npc::clearServerDeed() {
  serverDeedKind.clear();
  serverDeedVictim = nullptr;
  serverAttackRecorded = false;
  serverDeedSimulationLost = false;
  serverDeedCount = 0;
  serverDeedOwner.clear();
  }

bool Npc::isServerDeed(std::string_view kind, const Npc* victim) const {
  return kind==serverDeedKind && (kind!="attack" || (victim==serverDeedVictim && !serverAttackRecorded));
  }

const char* Npc::serverDeedSimulationError() {
  // NPC queues run before WorldObjects updates the policy; the observer runs at
  // the next frame's start. Remember a refused native pickup even if the policy
  // has become normal again before that observer can report the failure.
  return NpcDeedSimulation::refusal(processPolicy(),serverDeedSimulationLost);
  }

void Npc::consumeServerAttack() {
  serverAttackRecorded = true;
  }

void Npc::setNonlethalFight(Npc& other) {
  nonlethalTarget = other.instanceSymbol();
  nonlethalUntil = owner.tickCount()+5000;
  }

bool Npc::isNonlethalFight(const Npc& other) const {
  return nonlethalTarget==other.instanceSymbol() || other.nonlethalTarget==instanceSymbol();
  }

bool Npc::hearPhysicsNoise(const PhysicsNoise& noise) {
  const uint64_t now = owner.tickCount();
  if(noise.culprit==this || !canHearPhysicsNoise() || isPhysicsInvestigating() || now<physicsNoiseNextTime)
    return false;
  // The observer checks sight now, before either the native turn or a script callback.
  Gothic::inst().onPhysicsNoiseHeard(*this,noise);
  physicsNoiseNextTime = now+2001;
  physicsNoisePosition = noise.position;
  physicsNoiseUntil = now+2000;
  // Vanilla B_AssessQuietSound dereferences OTHER. Unknown physical causes
  // still turn the listener natively, without inventing a script actor.
  const bool callbackDispatched = noise.culprit!=nullptr && hasPerc(PERC_ASSESSQUIETSOUND);
  if(callbackDispatched) {
    physicsNoiseCallback = true;
    owner.script().invokeState(this,noise.culprit,nullptr,perception[PERC_ASSESSQUIETSOUND].func);
    physicsNoiseCallback = false;
    // B_AssessQuietSound clears perceptions before queuing its state. Run only
    // that transition and its initializer now; its queued actions still wait
    // for the native turn, rather than leaving perceptions disabled for 2 s.
    auto state = aiQueue.popPhysicsNoiseState();
    if(state.act==AI_StartState && startQueuedState(state) && !aiState.started)
      tickRoutine();
    }
  Bench::physicsNoiseHeard(*this,noise,callbackDispatched);
  return true;
  }

bool Npc::startPhysicsInvestigation(const Tempest::Vec3& at, uint64_t scanMs) {
  if(!canHearPhysicsNoise() || !isHuman() || weaponState()!=WeaponState::NoWeapon)
    return false;
  clearAiQueue();
  physicsInvestigationPosition = at;
  physicsInvestigationUntil = owner.tickCount()+45000;
  physicsInvestigationScanUntil = 0;
  physicsInvestigationScanMs = std::clamp<uint64_t>(scanMs,2000,8000);
  physicsInvestigationWalking = false;
  physicsInvestigationReason = {};
  return true;
  }

void Npc::stopPhysicsInvestigation(bool resume, std::string_view reason) {
  if(physicsInvestigationUntil==0)
    return;
  physicsInvestigationUntil = 0;
  physicsInvestigationScanUntil = 0;
  physicsInvestigationReason = reason;
  physicsNoiseUntil = 0;
  // A new owner or native state may already have installed its own movement.
  if(go2.flag==GT_Point && (go2.pos-physicsInvestigationPosition).quadLength()<1.f)
    clearGoTo();
  if(resume) {
    clearAiQueue();
    resumeAiRoutine();
    }
  }

bool Npc::tickPhysicsInvestigation(uint64_t dt) {
  if(physicsInvestigationUntil==0)
    return false;
  if(!canHearPhysicsNoise() || weaponState()!=WeaponState::NoWeapon) {
    stopPhysicsInvestigation(false);
    return false;
    }
  const uint64_t now = owner.tickCount();
  if(now>=physicsInvestigationUntil) {
    stopPhysicsInvestigation(true,"timeout");
    return false;
    }
  if(physicsInvestigationScanUntil!=0) {
    if(now>=physicsInvestigationScanUntil) {
      stopPhysicsInvestigation(true,"done");
      return false;
      }
    // Look to either side of the impact, then back, while standing at the spot.
    const uint64_t elapsed = physicsInvestigationScanMs-(physicsInvestigationScanUntil-now);
    const float offset = elapsed<physicsInvestigationScanMs/3 ? -.7f :
                         elapsed<2*physicsInvestigationScanMs/3 ? .7f : 0.f;
    const float facing = physicsInvestigationFacing+offset;
    implTurnTo(std::cos(facing),std::sin(facing),AnimationSolver::TurnType::Std,dt);
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return true;
    }
  if(!setInteraction(nullptr)) {
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return true;
    }
  if((position()-physicsInvestigationPosition).quadLength()<140.f*140.f) {
    clearGoTo();
    stopWalking();
    physicsInvestigationFacing = rotationRad();
    physicsInvestigationScanUntil = now+physicsInvestigationScanMs;
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return true;
    }
  if(!physicsInvestigationWalking) {
    wayPath.clear();
    attachToPoint(nullptr);
    go2.clear();
    go2.set(physicsInvestigationPosition);
    physicsInvestigationWalking = true;
    }
  // Position targets use the same native movement/collision checks as AI_UseMob.
  // A blocked approach ends at the deadline; it never teleports the listener.
  implGoTo(dt,60.f);
  return true;
  }

bool Npc::hasPerc(PercType perc) const {
  return perception[perc].func.isValid();
  }

uint64_t Npc::percNextTime() const {
  return perceptionNextTime;
  }

bool Npc::canSeeNpc(const Npc &oth, bool freeLos) const {
  const auto mid = oth.physic.center();
  if(canRayHitPoint(mid,freeLos)) {
    // mid of dead npc may endedup inside a wall; extra check for physical center
    return true;
    }
  if(oth.visual.visualSkeleton()==nullptr)
    return false;
  if(oth.visual.visualSkeleton()->BIP01_HEAD==size_t(-1))
    return false;
  auto head = oth.mapHeadBone();
  if(canRayHitPoint(head,freeLos))
    return true;
  return false;
  }

bool Npc::canSeeNpcOverEdge(const Npc& oth) const {
  const auto ws = weaponState();
  if(!Aim::isEnabled() || (ws!=WeaponState::Bow && ws!=WeaponState::CBow && ws!=WeaponState::Mage))
    return false;
  const Vec3  head = mapHeadBone();
  const Vec3  tg   = oth.physic.center();
  const Vec3  d    = tg - head;
  const float h    = std::sqrt(d.x*d.x+d.z*d.z);
  const float lean = Aim::leanForNpc(-std::atan2(d.y,h));
  if(lean<=0.f)
    return false;
  // Classic focus keeps its original angular rule and head estimate.
  static constexpr float reach = 58.f, drop = 28.f;
  const float a   = rotationRad();
  const Vec3  dir = Vec3::normalize(Vec3(d.x,0,d.z)*(1.f/std::max(h,1.f)) + Vec3(std::cos(a),0,std::sin(a))*0.25f);
  const float k   = lean/(Aim::NpcLeanMax*float(M_PI)/180.f);
  const Vec3  eye = head + dir*(reach*k) - Vec3(0,drop*k,0);
  if(owner.physic()->ray(head,eye).hasCol)
    return false; // no room to lean
  return canRayHitPoint(eye,tg,180.f) || canRayHitPoint(eye,oth.mapHeadBone(),180.f);
  }

bool Npc::canSeeSource() const {
  const auto head = mapHeadBone();
  const bool ret  = owner.sound()->canSeeSource(head);
  if(ret)
    return ret;
  if(currentLookAtNpc!=nullptr)
    return canSeeNpc(*currentLookAtNpc, false);
  return false;
  }

bool Npc::canRayHitPoint(const Tempest::Vec3 pos, bool freeLos, float extRange) const {
  float ang = freeLos ? 180.f : -1;
  return canRayHitPoint(pos, ang, extRange);
  }

bool Npc::canRayHitPoint(const Tempest::Vec3 pos, float angOverride, float extRange) const {
  const float range = float(hnpc->senses_range) + extRange;
  if(qDistTo(pos)>range*range)
    return false;
  // npc eyesight height by default
  return canRayHitPoint(mapHeadBone(), pos, angOverride, extRange);
  }

bool Npc::canRayHitPoint(const Tempest::Vec3 self, const Tempest::Vec3 pos, float angOverride, float extRange) const {
  const float range = float(hnpc->senses_range) + extRange;
  if(qDistTo(pos)>range*range)
    return false;

  static const double ref = std::cos(100*M_PI/180.0); // spec requires +-100 view angle range
  const DynamicWorld* w   = owner.physic();
  bool freeLos = angOverride>=180.f;
  if(freeLos) {
    return !w->ray(self, pos).hasCol;
    }

  float dx  = self.x-pos.x, dz=self.z-pos.z;
  float dir = angleDir(dx,dz);
  syncDeferredAnimation();
  float da  = float(M_PI)*(visual.viewDirection()-dir)/180.f;
  auto  ca  = angOverride > 0 ? std::cos(angOverride*M_PI/180.0) : ref;
  if(double(std::cos(da))<=ca) {
    if(!w->ray(self, pos).hasCol)
      return true;
    }
  return false;
  }

SensesBit Npc::canSenseNpc(const Npc &oth, bool freeLos, float extRange) const {
  // NOTE1: https://github.com/Try/OpenGothic/pull/589#issuecomment-2045897394
  // NOTE2: interacting with chest(lockpicking) or some MOBSI should not produce 'noise'
  // NOTE3: seem npc can't hear player in general case, and hearing relevant only for sendImmediatePerc cases
  const bool isNoisy = false;
  const auto mid     = oth.centerPosition();
  return canSenseNpc(mid,freeLos,isNoisy,extRange);
  }

SensesBit Npc::canSenseNpc(const Tempest::Vec3 pos, bool freeLos, bool isNoisy, float extRange) const {
  const float range = float(hnpc->senses_range)+extRange;
  if(qDistTo(pos)>range*range)
    return SensesBit::SENSE_NONE;

  SensesBit ret = SensesBit::SENSE_SMELL;

  if(isNoisy) {
    // no need to be in same room: https://github.com/Try/OpenGothic/issues/420
    ret = ret | SensesBit::SENSE_HEAR;
    }

  if((hnpc->senses & int32_t(SensesBit::SENSE_SEE))!=0 && canRayHitPoint(pos, freeLos, extRange)) {
    ret = ret | SensesBit::SENSE_SEE;
    }

  return ret & SensesBit(hnpc->senses);
  }

bool Npc::canSeeItem(const Item& it, bool freeLos) const {
  static const double ref = std::cos(100*M_PI/180.0); // spec requires +-100 view angle range

  const auto  itMid = it.midPosition();
  const auto  cen   = mapHeadBone();
  const auto  dir   = itMid - cen;
  const float range = float(hnpc->senses_range);

  if(dir.quadLength()>range*range)
    return false;

  if(!freeLos) {
    float dx  = dir.x, dz = dir.z;
    float dir = angleDir(dx,dz);
    syncDeferredAnimation();
    float da  = float(M_PI)*(visual.viewDirection()-dir)/180.f;
    if(double(std::cos(da))>ref)
      return false;
    }

  if(auto bbox = it.bBox()) {
    // npc eyesight height
    auto  at     = it.midPosition();
    auto  tMax   = (at - cen).length();
    auto  dir    = (at - cen)/tMax;
    float tHit   = DynamicWorld::rayBox(cen, dir, tMax, it.transform(), bbox[0], bbox[1]);

    const auto r = owner.physic()->ray(cen, cen+dir*tHit);
    if(r.hasCol)
      return false;
    } else {
    const auto r = owner.physic()->ray(cen, itMid);
    if(r.hasCol)
      return false;
    }

  return true;
  }
