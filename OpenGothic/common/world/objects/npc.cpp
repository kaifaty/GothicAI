#include "npc.h"
#include "npc_transformback.h"
#include "npc_melee_shadow.h"
#include "npc_melee.h"
#include "loadprobe.h"
#include "world/animationlod.h"
#include "world/bodytraces.h"
#include "world/bloodspawn.h"
#include "graphics/mesh/protomesh.h"

#include <Tempest/Matrix4x4>
#include <Tempest/Log>
#include <cctype>
#include <cstdlib>

#include "graphics/mesh/skeleton.h"
#include "graphics/visualfx.h"
#include "graphics/effect.h"
#include "world/objects/pfxemitter.h"
#include "game/damagecalculator.h"
#include "game/bodyimpact.h"
#include "game/herodeed.h"
#include "game/deedwitness.h"
#include "game/bodyobservations.h"
#include "game/bodyarena.h"
#include "game/bodycontroller.h"
#include "game/bodytrajectory.h"
#include "game/npckeys.h"
#include "game/serialize.h"
#include "physics/physicsnoise.h"
#include "game/ragdollsave.h"
#include "game/meleeparry.h"
#include "game/meleecharge.h"
#include "game/meleeriposte.h"
#include "game/gamescript.h"
#include "utils/string_frm.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "utils/fileext.h"
#include "utils/dbgpainter.h"
#include "camera.h"
#include "commandline.h"
#include "testbench.h"
#include "gothic.h"
#include "resources.h"
#include "frameprofile.h"
#include "navdemos.h"
#include "npcdayjournal.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_set>

using namespace Tempest;

static std::string_view humansTorchOverlay = "_TORCH.MDS";

namespace {

float meleeParryLeadMs() {
  static const float lead = [] {
    const char* text = std::getenv("OPENGOTHIC_MELEE_PARRY_LEAD_MS");
    const float value = text==nullptr ? 0.f : std::strtof(text,nullptr);
    return std::isfinite(value) ? value : 0.f;
    }();
  return lead;
  }

}

// Npc::tick may intentionally return without calling MoveAlgo (for example a native attack gate).
// Observe that branch explicitly instead of advancing movement just to make telemetry contiguous.
struct Npc::BodyMovementTrace final {
  BodyMovementTrace(Npc& npc, uint64_t dt)
    :npc(npc),dt(dt),serial(npc.mvAlgo.traceTickSerial()),previous(npc.bodyTraceMovementActive) {
    active = TestBench::bodyTracing() && npc.currentTarget!=nullptr;
    if(active)
      start = npc.position();
    npc.bodyTraceMovementActive = previous || active;
    }

  ~BodyMovementTrace() {
    npc.bodyTraceMovementActive = previous;
    if(!active || npc.mvAlgo.traceTickSerial()!=serial)
      return;
    const auto end = npc.position();
    const auto moved = end-start;
    const auto state = npc.mvAlgo.state();
    char fields[1500];
    std::snprintf(fields,sizeof(fields),
                  "\"sample_phase\":\"npc_post_movement\",\"movealgo_tick\":false,"
                  "\"skip_reason\":\"npc_tick_without_movealgo\",\"dt_ms\":%llu,\"move_flags\":0,"
                  "\"root_dx\":0,\"root_dy\":0,\"root_dz\":0,"
                  "\"requested_dx\":0,\"requested_dy\":0,\"requested_dz\":0,"
                  "\"moved_dx\":%.6f,\"moved_dy\":%.6f,\"moved_dz\":%.6f,"
                  "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"yaw\":%.6f,"
                  "\"grounded\":%s,\"movement_state\":%u,\"collision_scope\":\"none\","
                  "\"collision_attempts\":0,\"collision_failed_count\":0,"
                  "\"collision_land\":false,\"collision_npc\":false,\"collision_vob\":false,\"collision_prefall\":false",
                  (unsigned long long)dt,double(moved.x),double(moved.y),double(moved.z),
                  double(end.x),double(end.y),double(end.z),double(npc.rotation()),
                  state==MoveAlgo::Run ? "true" : "false",unsigned(state));
    TestBench::bodyEvent(npc,npc.currentTarget,"physics_step",npc.owner.tickCount(),fields);
    }

  Npc& npc;
  uint64_t dt = 0, serial = 0;
  bool active = false, previous = false;
  Vec3 start = {};
  };

std::string_view Npc::Routine::wayPointName() const {
  return point!=nullptr ? point->name : fallbackName;
  }


void Npc::GoTo::save(Serialize& fout) const {
  fout.write(npc, uint8_t(flag), wp, pos);
  }

void Npc::GoTo::load(Serialize& fin) {
  pointDistance = 0.f;
  nativeCombatTarget = nullptr;
  fin.read(npc, reinterpret_cast<uint8_t&>(flag), wp, pos);
  //NOTE: no real need to version check
  //if(fin.version()<53) {
    if(flag==GoToHint::GT_Enemy || flag==GoToHint::GT_EnemyG)
      clear(); // not persistent flags, and should be cleared by FAI
  //  }
  }

Vec3 Npc::GoTo::target() const {
  if(npc!=nullptr)
    return npc->centerPosition();
  if(wp!=nullptr)
    return wp->position();
  return pos;
  }

bool Npc::GoTo::isClose(const Npc& self, float dist) const {
  if(flag==GT_Enemy)
    return self.fghAlgo.isInWRange(self, *npc, self.owner.script()); // consistent with implAttack
  if(npc!=nullptr && pointDistance>0.f && !self.canSeeNpc(*npc,true))
    return false;
  if(npc!=nullptr)
    return MoveAlgo::isClose(self, *npc, dist);
  if(wp!=nullptr)
    return MoveAlgo::isClose(self, *wp, dist);
  if(flag==GT_Item) {
    // Ground and table items are approached horizontally; ItmGet handles height.
    const auto delta=pos-self.position();
    return Vec2(delta.x,delta.z).quadLength()<dist*dist;
    }
  return MoveAlgo::isClose(self, pos, dist);
  }

bool Npc::GoTo::empty() const {
  return flag==Npc::GT_No;
  }

void Npc::GoTo::clear() {
  npc  = nullptr;
  wp   = nullptr;
  flag = Npc::GT_No;
  pointDistance = 0.f;
  nativeCombatTarget = nullptr;
  }

void Npc::GoTo::set(Npc* to, Npc::GoToHint hnt) {
  nativeCombatTarget = nullptr;
  npc  = to;
  wp   = nullptr;
  flag = hnt;
  }

void Npc::GoTo::set(const WayPoint* to, GoToHint hnt) {
  nativeCombatTarget = nullptr;
  npc  = nullptr;
  wp   = to;
  flag = hnt;
  }

void Npc::GoTo::set(const Item* to) {
  nativeCombatTarget = nullptr;
  pos  = to->position();
  flag = Npc::GT_Item;
  }

void Npc::GoTo::set(const Vec3& to) {
  nativeCombatTarget = nullptr;
  pos  = to;
  flag = GT_Point;
  pointDistance = 0.f;
  }

void Npc::GoTo::setFlee() {
  nativeCombatTarget = nullptr;
  flag = GT_Flee;
  }


Npc::TransformBack::TransformBack(Npc& self) {
    hnpc        = std::make_shared<daedalus::INpc>(*self.hnpc);
    invent      = std::move(self.invent);
    self.invent = Inventory(); // cleanup

    std::memcpy(talentsSk,self.talentsSk,sizeof(talentsSk));
    std::memcpy(talentsVl,self.talentsVl,sizeof(talentsVl));

    body     = std::move(self.body);
    head     = std::move(self.head);
    vHead    = self.vHead;
    vTeeth   = self.vTeeth;
    vColor   = self.vColor;
    bdColor  = self.bdColor;

    skeleton = self.visual.visualSkeleton();
    }

Npc::TransformBack::TransformBack(Npc& owner, daedalus::DaedalusVm& vm, Serialize& fin) {
    hnpc           = std::make_shared<daedalus::INpc>();
    hnpc->user_ptr = nullptr;
    fin.readNpc(vm, hnpc);
    invent.load(fin,owner);
    fin.read(talentsSk,talentsVl);
    fin.read(body,head,vHead,vTeeth,vColor,bdColor);

    std::string sk;
    fin.read(sk);
    skeleton = Resources::loadSkeleton(sk);
    }

void Npc::TransformBack::undo(Npc& self) {
    int32_t aivar[daedalus::INpc::aivar_count]={};

    auto exp      = self.hnpc->exp;
    auto exp_next = self.hnpc->exp_next;
    auto lp       = self.hnpc->lp;
    auto level    = self.hnpc->level;
    std::memcpy(aivar,self.hnpc->aivar,sizeof(aivar));

    *self.hnpc          = *hnpc;

    self.hnpc->user_ptr = &self;
    self.hnpc->exp      = exp;
    self.hnpc->exp_next = exp_next;
    self.hnpc->lp       = lp;
    self.hnpc->level    = level;
    std::memcpy(self.hnpc->aivar,aivar,sizeof(aivar));

    self.invent = std::move(invent);
    std::memcpy(self.talentsSk,talentsSk,sizeof(talentsSk));
    std::memcpy(self.talentsVl,talentsVl,sizeof(talentsVl));

    self.body    = std::move(body);
    self.head    = std::move(head);
    self.vHead   = vHead;
    self.vTeeth  = vTeeth;
    self.vColor  = vColor;
    self.bdColor = bdColor;
    }

void Npc::TransformBack::save(Serialize& fout) {
    fout.write(*hnpc);
    invent.save(fout);
    fout.write(talentsSk,talentsVl);
    fout.write(body,head,vHead,vTeeth,vColor,bdColor);
    fout.write(skeleton!=nullptr ? skeleton->name() : "");
    }

Npc::Npc(World &owner, size_t instance, std::string_view waypoint, NpcProcessPolicy aiPolicy)
  :owner(owner),aiPolicy(aiPolicy),mvAlgo(*this) {
  animLodSeed    = AnimationLod::nextSeed();
  outputPipe     = owner.script().openAiOuput();

  hnpc           = std::make_shared<daedalus::INpc>();
  hnpc->user_ptr = this;
  hnpc->id       = int32_t(instance & 0x7FFFFFFF);
  hnpc->wp       = std::string(waypoint);

  if(instance==size_t(-1))
    return;

  owner.script().initializeInstanceNpc(hnpc, instance);

  // vanilla behavior: equip best weapon and set non-zero damage type
  if(!isPlayer())
    invent.autoEquipWeapons(*this);
  if(hnpc->damage_type==0)
    hnpc->damage_type = 2;
  setTrueGuild(hnpc->guild); // https://worldofplayers.ru/threads/12446/post-878087
  setPerceptionTime(5000);   // https://github.com/Try/OpenGothic/pull/720#issuecomment-2602908614
  }

Npc::~Npc(){
  endMeleeAiSpecial();
  clearMeleeShadow();
  if(std::getenv("OPENGOTHIC_MELEE_BENCH")!=nullptr && owner.physic()!=nullptr)
    owner.physic()->removeMeleeBlade(this);
  NpcKeys::forget(this);
  BodyObservations::forget(this);
  TestBench::bodyForget(this);
  if(currentInteract)
    currentInteract->detach(*this,true);
  if(hnpc && hnpc->user_ptr==this)
    hnpc->user_ptr=nullptr;
  }

void Npc::syncLogicalBodyState(bool rebuild) {
  logicalBody.sync(*this,rebuild);
  }

bool Npc::setPosition(float ix, float iy, float iz) {
  if(x==ix && y==iy && z==iz)
    return false;
  const bool liveTeleport = !isDown() && !ragdoll && (position()-Vec3(ix,iy,iz)).length()>300.f;
  const auto* skeleton = visual.visualSkeleton();
  std::optional<Ragdoll::State> movedBody, movedDormant;
  auto relocate = [&](Ragdoll::State& state) {
    // A disabled body's origin can differ from the legacy animated origin.
    const Vec3 at(float(double(ix)+(double(state.rootTransform.at(3,0))-double(x))),
                  float(double(iy)+(double(state.rootTransform.at(3,1))-double(y))),
                  float(double(iz)+(double(state.rootTransform.at(3,2))-double(z))));
    return skeleton!=nullptr && Ragdoll::relocateState(*skeleton,state,at);
    };
  if(ragdoll) {
    movedBody = ragdoll->state();
    if(!relocate(*movedBody))
      return false;
    }
  if(ragdollDormant) {
    movedDormant = *ragdollDormant;
    if(!relocate(*movedDormant))
      return false;
    }
  if(movedBody) {
    if(!ragdoll->restore(*movedBody))
      return false;
    ragdollObjectMatrix = ragdoll->rootTransform();
    }
  if(movedDormant)
    ragdollDormant = std::move(movedDormant);
  x = ix;
  y = iy;
  z = iz;
  visual.setTextureOwner(Vec3(x,y,z),!isPlayer());
  resetCharacterVisualHeight();
  if(!isPlayer() && (isHuman() || Gothic::physicsFlags().monsterCharacterVirtual))
    mvAlgo.resetCharacterVirtual();
  if(liveTeleport) {
    // Recreate at the destination instead of sweeping a kinematic body through
    // every item between a scripted teleport's endpoints.
    living.reset();
    arrowImpacts.clear();
    // Large scripted relocations start a new history; normal movement samples
    // must retain the last supported position when the NPC steps off an edge.
    ragdollQuestReturn = {};
    ragdollQuestSpellThrow = false;
    ragdollQuestSampleTick = 0;
    }
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  durtyTranform |= TR_Pos;
  physic.setPosition(Vec3{x,y,z});
  if(movedBody)
    syncRagdoll();
  return true;
  }

bool Npc::setPosition(const Tempest::Vec3& pos) {
  return setPosition(pos.x,pos.y,pos.z);
  }

void Npc::setViewPosition(const Tempest::Vec3& pos) {
  // Routine legacy alignment must not move the frozen physical pose. Explicit
  // teleports use setPosition and translate the dormant state there.
  x = pos.x;
  y = pos.y;
  z = pos.z;
  visual.setTextureOwner(pos,!isPlayer());
  durtyTranform |= TR_Pos;
  }

void Npc::syncCharacterVirtualSupport(const Tempest::Vec3& pos) {
  // Animation already sampled these feet before the physics step. Transport
  // that sample with the accepted support motion so a lift does not become a
  // fictitious stair correction at the same timestamp.
  if(characterVisualHasSample && characterVisualSampleTick==owner.tickCount())
    characterVisualSamplePosition += pos-position();
  setViewPosition(pos);
  updateTransform();
  }

int Npc::aiOutputOrderId() const {
  return aiQueue.aiOutputOrderId();
  }

bool Npc::performOutput(const AiQueue::AiAction &act) {
  if(act.target==nullptr) //FIXME: target is null after loading
    return true;
  const int order = act.target->aiOutputOrderId();
  if(order<act.i0)
    return false;
  if(aiOutputBarrier>owner.tickCount() && act.target==this && !isPlayer())
    return false;
  if(aiPolicy>=NpcProcessPolicy::AiFar)
    return true; // don't waste CPU on far-away svm-talks
  //if(act.act!=AI_OutputSvmOverlay && bodyStateMasked()!=BS_STAND)
  //  return false;
  if(act.act==AI_Output           && outputPipe->output   (*this,act.s0))
    return true;
  auto svm = owner.script().messageFromSvm(act.s0,hnpc->voice);
  if(act.act==AI_OutputSvm        && outputPipe->outputSvm(*this,svm,act.s0,act.target))
    return true;
  if(act.act==AI_OutputSvmOverlay && outputPipe->outputOv(*this,svm,act.s0,act.target))
    return true;
  return false;
  }

void Npc::setDirection(const Tempest::Vec3& pos) {
  float a = angleDir(pos.x, pos.z);
  setDirection(a);
  }

void Npc::setDirection(float rotation) {
  durtyTranform |= TR_Rot;
  angle = rotation;
  physic.setRotation(angle);
  }

void Npc::setDirectionY(float rotation) {
  if(rotation>90)
    rotation = 90;
  if(rotation<-90)
    rotation = -90;
  rotation = std::fmod(rotation,360.f);
  if(!mvAlgo.isDive() && !(interactive()!=nullptr && interactive()->isLadder()))
    return;
  angleY = rotation;
  durtyTranform |= TR_Rot;
  }

void Npc::setRunAngle(float angle) {
  durtyTranform |= TR_Rot;
  runAng = angle;
  }

float Npc::angleDir(float x, float z) {
  float a = 0;
  if(x!=0.f || z!=0.f)
    a = 180.f*std::atan2(z,x)/float(M_PI);
  return a;
  }

bool Npc::resetPositionToTA() {
  LoadProbe::Startup::Detail::Scope reset("reset_npc");
  endMeleeAiSpecial();
  const bool isDragon = (guild()==GIL_DRAGON);
  const bool isDead   = this->isDead();

  if(isDead && !invent.hasMissionItems() && !isDragon)
    return false;

  invent.clearSlot(*this,"",currentInteract!=nullptr);
  if(!isPlayer())
    setInteraction(nullptr,true);

  // return monsters to their way-points
  // if(routines.empty() && !isPlayer())
  //   return currentTaPoint()!=nullptr;

  attachToPoint(nullptr);
  clearAiQueue();

  if(!isDead) {
    visual.stopAnim(*this,"");
    clearState(true);
    }

  if(isPlayer())
    return true;

  LoadProbe::Startup::Detail::Scope routine("current_ta");
  auto at = currentTaPoint();
  routine.stop();
  if(at==nullptr)
    return false;

  if(at->isLocked() && !isDead) {
    LoadProbe::Startup::Detail::Scope next("next_point");
    auto p = owner.findNextPoint(*at);
    if(p!=nullptr)
      at = p;
    }
  setPosition (at->position() );
  setDirection(at->direction());
  owner.script().fixNpcPosition(*this,0,100);

  if(!isDead) {
    attachToPoint(at);
    LoadProbe::Startup::Detail::Scope equip("auto_equip");
    invent.autoEquipWeapons(*this);
    }

  owner.script().invokeRefreshAtInsert(*this);
  return true;
  }

void Npc::stopDlgAnim() {
  visual.stopDlgAnim(*this);
  }

void Npc::clearSpeed() {
  mvAlgo.clearSpeed();
  }

void Npc::resetNavigationEpisode() {
  if(!NavDemos::fullSimulation(*this))
    return;
  setInteraction(nullptr,true);
  clearAiQueue();
  clearState(true);
  hnpc->attribute[ATR_HITPOINTS] = hnpc->attribute[ATR_HITPOINTSMAX];
  releaseRagdoll(false);
  ragdollDormant.reset();
  ragdollLegacyDown = ragdollRecovering = ragdollHasLastPosition = false;
  meleePosture.reset();
  meleeBargeOrigin.reset();
  meleeKnockdown = false;
  meleeBargeKnockdown = false;
  meleeRiposteKnockdown = false;
  meleeGetupForced = false;
  meleeFallTick = meleeRiseTick = 0;
  meleeSlowSince = 0;
  ragdollLastVelocity = {};
  ragdollQuestReturn = {};
  ragdollQuestSpellThrow = false;
  ragdollQuestSampleTick = 0;
  living.reset();
  livingRetryTick = 0;
  livingNeedsRebuild = livingWasFrozen = livingWasInteracting = false;
  bodyImpactWindow.clear();
  bodyImpactLaunchCulprit.reset();
  bodyImpactPendingCulprit.reset();
  bodyImpactSpellFlight=false;
  arrowImpacts.clear();
  meleeRiposte.reset();
  meleeRiposteHoldUntil = 0;
  pendingSpellImpulse = {};
  spellImpulseCulprit.reset();
  visual.clearPhysicalPose();
  visual.pose().stopAllAnim();
  // Called between world ticks: reconstruct transient state (including bounce,
  // cached ground, speed multipliers and controller hysteresis) for a fresh episode.
  mvAlgo.resetCharacterVirtual();
  std::destroy_at(&mvAlgo);
  std::construct_at(&mvAlgo,*this);
  currentTarget = nearestEnemy = currentOther = currentVictim = lastHit = nullptr;
  outWaitTime = aiOutputBarrier = physicsNoiseUntil = 0;
  castLevel = CS_NoCast;
  currentSpellCast = size_t(-1);
  castNextTime = 0;
  lastEventTime = owner.tickCount();
  resetCharacterVisualHeight();
  angleY = runAng = 0;
  stopWalking();
  physic.setEnable(true);
  }

void Npc::setProcessPolicy(NpcProcessPolicy t) {
  if(aiPolicy==t)
    return;
  if(aiPolicy==NpcProcessPolicy::Player)
    runAng = 0;
  aiPolicy=t;
  visual.setTextureOwner(position(),!isPlayer());
  }

void Npc::setSimTier(NpcSimTier t) {
  simTierValue = t;
  simTierAssigned = true;
  if(!loadedFpReferences)
    return;
  loadedFpReferences = false;
  if(t!=NpcSimTier::Simplified || isPlayer() || !simplifiedMovementEnabled())
    return;

  // Names are the existing save representation. Defer their disambiguation
  // until the loaded world has assigned tiers, using the original saved pose.
  const auto resolve=[&](const WayPoint* point,const char* reference) {
    if(point==nullptr || !point->isFreePoint())
      return point;
    const WayPoint* nearest=nullptr;
    size_t matches=0;
    float distance=std::numeric_limits<float>::max();
    owner.forEachWayPoint([&](const WayPoint& candidate) {
      if(!candidate.isFreePoint() || candidate.name!=point->name)
        return;
      ++matches;
      const float d=(candidate.position()-loadedFpPosition).quadLength();
      if(d<distance) {
        distance=d;
        nearest=&candidate;
        }
      });
    if(matches<=1)
      return point;
    owner.noteGraphFpLoadResolution();
    const auto* symbol=owner.script().findSymbol(instanceSymbol());
    Log::i("npc_graph_fp_load_resolution npc=",symbol!=nullptr ? symbol->name() : displayName(),
           " world_id=",owner.npcId(this)," id=",hnpc->id," fp=",point->name," reference=",reference,
           " candidates=",matches," saved=(",loadedFpPosition.x,",",loadedFpPosition.y,",",loadedFpPosition.z,
           ") resolved=(",nearest->position().x,",",nearest->position().y,",",nearest->position().z,
           ") changed=",nearest!=point);
    return nearest;
    };
  currentFp=resolve(currentFp,"currentFp");
  currentFpLock=FpLock(resolve(currentFpLock.point(),"currentFpLock"));
  go2.wp=resolve(go2.wp,"goTo");
  WayPath path;
  while(const auto* point=wayPath.pop())
    path.add(*resolve(point,"path"));
  path.reverse();
  wayPath=std::move(path);
  for(auto* queue:{&aiQueue,&aiQueueOverlay}) {
    std::vector<AiQueue::AiAction> actions;
    actions.reserve(queue->size());
    while(queue->size()>0) {
      auto action=queue->pop();
      action.point=resolve(action.point,"queue");
      actions.push_back(std::move(action));
      }
    for(auto i=actions.rbegin();i!=actions.rend();++i)
      queue->pushFront(std::move(*i));
    }
  }

void Npc::setWalkMode(WalkBit m) {
  if(isUsingItemInHand() && !bool(m&WalkBit::WM_Walk))
    interruptItemUse("run");
  //NOTE: other bits are situational, and should not be set directly
  wlkMode = m & (WalkBit::WM_Run | WalkBit::WM_Walk | WalkBit::WM_Sneak);
  }

WalkBit Npc::walkMode() const {
  auto wlk = wlkMode;
  if(mvAlgo.isDive())
    wlk = WalkBit::WM_Dive;
  else if(mvAlgo.isSwim())
    wlk = WalkBit::WM_Swim;
  else if(mvAlgo.isInWater())
    wlk = WalkBit::WM_Water;
  return wlk;
  }

bool Npc::isPlayer() const {
  return aiPolicy==NpcProcessPolicy::Player;
  }

bool Npc::startClimb(JumpStatus jump) {
  setPosition(physic.position());
  visual.setAnimRotate(*this,0);
  return mvAlgo.startClimb(jump);
  }

bool Npc::checkHealth(bool onChange, bool allowUnconscious) {
  if(isDead()) {
    return false;
    }
  if(isUnconscious() && allowUnconscious) {
    return false;
    }

  const int minHp = isMonster() ? 0 : 1;
  if(hnpc->attribute[ATR_HITPOINTS]<=minHp) {
    if(currentOther==nullptr || !allowUnconscious || !isHuman() ||
       (owner.script().personAttitude(*this,*currentOther)==ATT_HOSTILE && !isNonlethalFight(*currentOther))){
      if(hnpc->attribute[ATR_HITPOINTS]<=0)
        onNoHealth(true,HS_Dead);
      return false;
      }

    if(onChange) {
      onNoHealth(false,HS_Dead);
      return false;
      }
    }
  physic.setEnable(ragdoll==nullptr);
  return true;
  }

void Npc::onNoHealth(bool death, HitSound sndMask) {
  interruptItemUse(death ? "death" : "unconscious");
  Healing::clear(foodPool);
  endMeleeAiSpecial();
  const bool wasDead = isDead();
  meleeChargePhase = ChargePhase::None;
  meleeChargeMult = 1.f;
  meleeChargeReleased = false;
  endMeleeStun("death");
  meleeParadeOrigin.reset();
  meleeKnockdown = false;
  meleeBargeKnockdown = false;
  meleeRiposteKnockdown = false;
  meleeSlowSince = 0;
  meleePosture.reset();
  living.reset();
  ragdollQuestSpellThrow = false;
  if(!isDown()) {
    updateRagdollQuestAnchor();
    const auto hazards = ragdollQuestReturn.armed ? ragdollQuestReturn.hazards : uint8_t(0);
    ragdollQuestReturn.armed = !death && !isPlayer() && isHuman() &&
        Gothic::physicsFlags().questGuard && ragdollQuestReturn.hasAnchor;
    ragdollQuestReturn.hazards = ragdollQuestReturn.armed ? hazards : 0;
    ragdollDormant.reset();
    }
  ragdollLegacyDown = false;
  ragdollRecovering = false;
  invent.switchActiveWeapon(*this,Item::NSLOT);
  visual.dropWeapon(*this);
  visual.dropShield(*this);
  dropTorch();
  visual.setToFightMode(WeaponState::NoWeapon);
  updateWeaponSkeleton();

  setOther(lastHit);
  clearAiQueue();
  attachToPoint(nullptr);

  const char* svm   = death ? "SVM_%d_DEAD" : "SVM_%d_AARGH";
  const char* state = death ? "ZS_Dead"     : "ZS_Unconscious";

  if(!death)
    hnpc->attribute[ATR_HITPOINTS]=1;

  size_t fdead=owner.script().findSymbolIndex(state);
  startState(fdead,"",gtime::endOfTime(),true);
  if(BodyTraces::stampsDeath(death,wasDead,bodyTraceRecord.id,bodyTraceEntered)) {
    // Engine truth of the body trace: kept whatever [GAME] body_traces says, so the
    // age and lifetime are right when the flag is turned on later. A deferred ZS_Dead
    // (BodyController) keeps the first fatal blow's record: see BodyTraces::stampsDeath.
    BodyTrace trace;
    trace.id        = owner.allocBodyTraceId();
    trace.diedAt    = owner.gameMinute();
    trace.clockFrom = trace.diedAt;
    if(lastHit!=nullptr && lastHit!=this) {
      if(lastHit->isPlayer())
        trace.culprit = "PLAYER";
      else if(auto* symbol = owner.script().findSymbol(lastHit->instanceSymbol()))
        trace.culprit = symbol->name();
      }
    bodyTraceRecord  = std::move(trace);
    bodyTraceEntered = isDead();
    // One pool per death: later blows on a body a BodyController still owns don't restart it.
    bloodDeathTick   = owner.tickCount();
    bloodPoolDone    = false;
    }
  // Note: clear perceptions for William in Jarkentar
  for(size_t i=0;i<PERC_Count;++i)
    setPerceptionDisable(PercType(i));
  if(hnpc->voice>0 && sndMask!=HS_NoSound && !isDive()) {
    emitSoundSVM(svm);
    }

  setInteraction(nullptr,true);
  invent.clearSlot(*this,"",false);

  if(death)
    physic.setEnable(false);

  if(death)
    setAnim(lastHitType=='A' ? Anim::DeadA        : Anim::DeadB); else
    setAnim(lastHitType=='A' ? Anim::UnconsciousA : Anim::UnconsciousB);
  BodyObservations::noteEvent(*this,lastHit,death ? "dead" : "unconscious",owner.tickCount());
  if(TestBench::bodyTracing())
    TestBench::bodyEvent(*this,lastHit,"termination",owner.tickCount(),
                        "\"reason\":" + TestBench::bodyJson(death ? "dead" : "unconscious") +
                        ",\"hp\":" + std::to_string(attribute(ATR_HITPOINTS)) +
                        ",\"down\":" + (isDown() ? "true" : "false") + ",\"dead\":" + (isDead() ? "true" : "false"));
  }

void Npc::refreshMeleeFighter() {
  const auto mode = weaponState();
  const bool melee = mode==WeaponState::Fist || mode==WeaponState::W1H || mode==WeaponState::W2H;
  const auto delta = currentTarget!=nullptr ? currentTarget->position()-position() : Vec3{};
  if(MeleeFighter::qualifies(isPlayer(),melee,isAttack(),currentTarget!=nullptr,
                            currentTarget!=nullptr && currentTarget->isDown(),delta.x*delta.x+delta.z*delta.z))
    MeleeFighter::refresh(meleeFighter,currentTarget->meleeFighter,owner.tickCount());
  }

bool Npc::isMeleeFighter() const {
  return MeleeFighter::active(meleeFighter,owner.tickCount(),isPlayer());
  }

bool Npc::hasAutoroll() const {
  auto gl = std::min<uint32_t>(guild(),GIL_MAX);
  return owner.script().guildVal().disable_autoroll[gl]==0;
  }

void Npc::stopWalkAnimation() {
  if(interactive()==nullptr)
    visual.stopWalkAnim(*this);
  setAnimRotate(0);
  }

World& Npc::world() {
  return owner;
  }

const World& Npc::world() const {
  return owner;
  }

Vec3 Npc::position() const {
  return {x,y,z};
  }

Matrix4x4 Npc::transform() const {
  auto matrix = visual.transform();
  // Gameplay users of this matrix retain the exact origin. The presentation
  // correction belongs to the model, its attachments and the foot IK frame only.
  const float visualOffset=characterVisualYOffset+lieVisualYOffset;
  if(visualOffset!=0)
    matrix.set(3,1,matrix.at(3,1)-visualOffset);
  return matrix;
  }

Vec3 Npc::characterVisualPosition() const {
  const auto& matrix = visual.transform();
  return {matrix.at(3,0),matrix.at(3,1),matrix.at(3,2)};
  }

Vec3 Npc::cameraBone(bool isFirstPerson) const {
  syncDeferredAnimation();
  const size_t head = visual.pose().findNode("BIP01 HEAD");

  Vec3 r = {};
  if(isFirstPerson && head!=size_t(-1)) {
    r = visual.mapBone(head);
    } else {
    auto mt = visual.pose().rootBone();
    mt.project(r);
    }

  return r;
  }

Matrix4x4 Npc::cameraMatrix(bool isFirstPerson) const {
  const size_t head = visual.pose().findNode("BIP01 HEAD");
  if(isFirstPerson && head!=size_t(-1)) {
    return visual.pose().bone(head);
    }
  return visual.pose().rootBone();
  }

float Npc::rotation() const {
  return angle;
  }

float Npc::rotationRad() const {
  return angle*float(M_PI)/180.f;
  }

float Npc::rotationY() const {
  return angleY;
  }

float Npc::rotationYRad() const {
  return angleY*float(M_PI)/180.f;
  }

Bounds Npc::bounds() const {
  return visual.bounds();
  }

auto Npc::bBoxCol() const -> const Vec3* {
  if(visual.visualSkeleton()==nullptr)
    return nullptr;
  return visual.visualSkeleton()->bboxCol;
  }

auto Npc::bBox() const -> const Vec3* {
  if(visual.visualSkeleton()==nullptr)
    return nullptr;
  return visual.visualSkeleton()->bbox;
  }

Vec3 Npc::centerPosition() const {
  auto p = position();
  // p.y += 15; // seem to be off by ~15 centimeters, according to comparations vanilla testing
  p.y += visual.pose().translateY();
  return p;
  }

Vec3 Npc::collosionCenter() const {
  auto p = position();
  p += physic.centerAsym();
  return p;
  }

Npc* Npc::lookAtTarget() const {
  return currentLookAtNpc;
  }

std::string_view Npc::portalName() {
  return mvAlgo.portalName();
  }

std::string_view Npc::formerPortalName() {
  return mvAlgo.formerPortalName();
  }

float Npc::qDistTo(const Vec3 pos) const {
  auto dp = pos - centerPosition();
  return dp.quadLength();
  }

float Npc::qDistTo(const WayPoint *f) const {
  if(f==nullptr)
    return 0.f;
  return qDistTo(f->position());
  }

float Npc::qDistTo(const Npc &p) const {
  return qDistTo(p.centerPosition());
  }

float Npc::qDistTo(const Interactive &p) const {
  auto pos = p.nearestPoint(*this);
  return qDistTo(pos);
  }

float Npc::qDistTo(const Item& p) const {
  auto pos = p.midPosition();
  return qDistTo(pos);
  }

Tempest::Vec3 Npc::fightDistanceTo(const Npc& tg) const {
  //NOTE: game script decribe comabt distance as distance between BIP01
  // however, in practice, it's easier and more relieble to use rootTr
  Vec3 cen, tgCen;
  if(auto sk = visual.visualSkeleton()) {
    cen = sk->rootTr;
    transform().project(cen);
    }
  cen += position();

  if(auto sk = tg.visual.visualSkeleton()) {
    tgCen = sk->rootTr;
    transform().project(tgCen);
    }
  tgCen += tg.position();
  return (cen-tgCen);
  }

uint8_t Npc::calcAniComb() const {
  if(manualAim)
    return Pose::calcAniComb(manualPoint-position(),angle);
  if(currentTarget==nullptr)
    return 0;
  auto dpos = currentTarget->position() - position();
  return Pose::calcAniComb(dpos,angle);
  }

std::string_view Npc::displayName() const {
  return hnpc->name[0];
  }

Tempest::Vec3 Npc::displayPosition() const {
  auto p = visual.displayPosition();
  return p+position();
  }

void Npc::setVisual(std::string_view visual) {
  auto skelet = Resources::loadSkeleton(visual);
  setVisual(skelet);
  setPhysic(owner.physic()->addGhostObj(skelet));
  }

bool Npc::hasOverlay(std::string_view sk) const {
  auto skelet = Resources::loadSkeleton(sk);
  return hasOverlay(skelet);
  }

bool Npc::hasOverlay(const Skeleton* sk) const {
  return visual.hasOverlay(sk);
  }

void Npc::addOverlay(std::string_view sk, uint64_t time) {
  auto skelet = Resources::loadSkeleton(sk);
  addOverlay(skelet,time);
  }

void Npc::addOverlay(const Skeleton* sk, uint64_t time) {
  if(time!=0)
    time+=owner.tickCount();
  visual.addOverlay(sk,time);
  }

void Npc::delOverlay(std::string_view sk) {
  visual.delOverlay(sk);
  }

void Npc::delOverlay(const Skeleton *sk) {
  visual.delOverlay(sk);
  }

bool Npc::toggleTorch() {
  string_frm overlay(visual.visualSkeletonScheme(), humansTorchOverlay);
  if(isUsingTorch()) {
    visual.setTorch(false,owner);
    delOverlay(overlay);
    return false;
    }
  visual.setTorch(true,owner);
  addOverlay(overlay,0);
  return true;
  }

void Npc::setTorch(bool use) {
  if(isUsingTorch()==use)
    return;

  string_frm overlay(visual.visualSkeletonScheme(), humansTorchOverlay);
  visual.setTorch(use,owner);
  if(use) {
    addOverlay(overlay,0);
    } else {
    delOverlay(overlay);
    }
  }

bool Npc::isUsingTorch() const {
  return visual.isUsingTorch();
  }

void Npc::dropTorch(bool burnout) {
  syncDeferredAnimation();
  auto sk = visual.visualSkeleton();
  if(sk==nullptr)
    return;

  if(!isUsingTorch())
    return;

  string_frm overlay(visual.visualSkeletonScheme(), humansTorchOverlay);
  visual.setTorch(false,owner);
  delOverlay(overlay);

  size_t torchId = 0;
  if(burnout)
    torchId = owner.script().findSymbolIndex("ItLsTorchburned"); else
    torchId = owner.script().findSymbolIndex("ItLsTorchburning");

  size_t leftHand = sk->findNode("ZS_LEFTHAND");
  if(torchId!=size_t(-1) && leftHand!=size_t(-1)) {

    auto mat = visual.transform();
    if(leftHand<visual.pose().boneCount())
      mat = visual.pose().bone(leftHand);

    owner.addItemDyn(torchId,mat,hnpc->symbol_index());
    }
  }

Tempest::Vec3 Npc::animMoveSpeed(uint64_t dt) const {
  if(!ragdoll && needsRagdollQuestReturn() && !visual.pose().isStanding())
    return {};
  return visual.pose().animMoveSpeed(owner.tickCount(),dt);
  }

void Npc::setVisual(const Skeleton* v) {
  if(visual.visualSkeleton()!=v) {
    living.reset();
    arrowImpacts.clear();
    releaseRagdoll(false);
    ragdollDormant.reset();
    ragdollRecovering = false;
    ragdollLegacyDown = false;
    ragdollHasLastPosition = false;
    ragdollLastVelocity = {};
    }
  visual.setVisual(v);
  invalidateTalentOverlays();
  }

void Npc::setVisualBody(int32_t headTexNr, int32_t teethTexNr, int32_t bodyTexNr, int32_t bodyTexColor,
                        std::string_view ibody, std::string_view ihead) {
  const auto* residency=owner.chunkResidency();
  Resources::TextureLoadScope textureLoadScope(!isPlayer() && residency &&
      residency->textureDistance(ChunkId::fromPosition(x,z))>2);
  body    = ibody;
  head    = ihead;
  vHead   = headTexNr;
  vTeeth  = teethTexNr;
  vColor  = bodyTexNr;
  bdColor = bodyTexColor;

  auto  vhead = head.empty() ? MeshObjects::Mesh() : owner.addView(FileExt::addExt(head,".MMB"),vHead,vTeeth,bdColor);
  auto  vbody = body.empty() ? MeshObjects::Mesh() : owner.addView(FileExt::addExt(body,".ASC"),vColor,0,bdColor);
  visual.setVisualBody(*this,std::move(vhead),std::move(vbody),bdColor);
  updateArmor();

  durtyTranform|=TR_Pos; // update obj matrix
  }

void Npc::updateArmor() {
  const auto* residency=owner.chunkResidency();
  Resources::TextureLoadScope textureLoadScope(!isPlayer() && residency &&
      residency->textureDistance(ChunkId::fromPosition(x,z))>2);
  living.reset();
  auto  ar = invent.currentArmor();
  auto& w  = owner;

  if(ar==nullptr) {
    auto  vbody = body.empty() ? MeshObjects::Mesh() : w.addView(FileExt::addExt(body,".ASC"),vColor,0,bdColor);
    visual.setBody(*this,std::move(vbody),bdColor);
    } else {
    auto& itData = ar->handle();
    auto  flag   = ItmFlags(itData.main_flag);
    if(flag & ITM_CAT_ARMOR){
      auto& asc   = itData.visual_change;
      auto  vbody = asc.empty() ? MeshObjects::Mesh() : w.addView(asc,vColor,0,bdColor);
      visual.setArmor(*this,std::move(vbody));
      }
    }
  }

void Npc::updateQuiver() {
  Quiver::State st;
  const Item*   bow = invent.currentRangedWeapon();
  st.on = Quiver::enabled() && bow!=nullptr && (bow->mainFlag() & ITM_CAT_FF) && !bow->isCrossbow() &&
          visual.bodyProto()!=nullptr && visual.visualSkeleton()!=nullptr;
  if(st.on) {
    st.ammo     = size_t(bow->handle().munition);
    const auto n= Quiver::visibleArrows(invent.itemCount(st.ammo)) - visual.quiver().taken();
    st.visible  = std::max(n,0);
    st.body     = visual.bodyProto();
    st.skeleton = visual.visualSkeleton();
    }
  if(st==visual.quiver().state())
    return;
  if(!st.on) {
    visual.clearQuiver();
    return;
    }
  std::vector<MeshObjects::Mesh>  arrows;
  std::vector<Tempest::Matrix4x4> local;
  if(st.visible>0)
    if(Item* ammo = invent.itemOfClass(st.ammo)) {
      for(int i=0; i<st.visible; ++i) {
        auto view = owner.addView(ammo->handle());
        if(view.isEmpty())
          break;
        arrows.push_back(std::move(view));
        }
      if(!arrows.empty())
        local = Quiver::arrowLayout(arrows[0].protoMesh(),
                                    int(arrows.size()));
      if(local.size()!=arrows.size())
        arrows.clear();
      }
  visual.setQuiver(st,owner.addView(Quiver::Visual),std::move(arrows),std::move(local));
  const auto q   = visual.quiverProbe();
  const auto who = TestBench::who(this);
  char buf[200];
  std::snprintf(buf,sizeof(buf),"quiver npc %.*s arrows %d out %.1f mouth %.2f %.2f %.2f",int(who.size()),who.data(),
                q.arrows,double(q.out),double(q.mouth.x),double(q.mouth.y),double(q.mouth.z));
  TestBench::note(buf);
  }

void Npc::refitQuiver() {
  visual.clearQuiver();
  updateQuiver();
  }

void Npc::setQuiverTaken(int n) {
  visual.setQuiverTaken(n);
  updateQuiver();
  }

void Npc::setSword(MeshObjects::Mesh&& s) {
  visual.setSword(std::move(s));
  updateWeaponSkeleton();
  }

void Npc::setRangedWeapon(MeshObjects::Mesh&& b) {
  visual.setRangedWeapon(std::move(b));
  updateWeaponSkeleton();
  }

void Npc::setShield(MeshObjects::Mesh&& s) {
  visual.setShield(std::move(s));
  updateWeaponSkeleton();
  }

void Npc::setMagicWeapon(Effect&& s) {
  s.setOrigin(this);
  visual.setMagicWeapon(std::move(s),owner);
  updateWeaponSkeleton();
  }

void Npc::setSlotItem(MeshObjects::Mesh&& itm, std::string_view slot) {
  visual.setSlotItem(std::move(itm),slot);
  }

void Npc::setStateItem(MeshObjects::Mesh&& itm, std::string_view slot) {
  visual.setStateItem(std::move(itm),slot);
  }

void Npc::setAmmoItem(MeshObjects::Mesh&& itm, std::string_view slot) {
  visual.setAmmoItem(std::move(itm),slot);
  }

void Npc::clearSlotItem(std::string_view slot) {
  visual.clearSlotItem(slot);
  }

void Npc::updateWeaponSkeleton() {
  visual.updateWeaponSkeleton(invent.currentMeleeWeapon(),invent.currentRangedWeapon());
  }

void Npc::setPhysic(DynamicWorld::NpcItem &&item) {
  physic = std::move(item);
  physic.setUserPointer(this);
  physic.setPosition(Vec3{x,y,z});
  physic.setRotation(angle);
  if(ragdoll)
    physic.setEnable(false);
  }

void Npc::setFatness(float f) {
  bdFatness = f;
  visual.setFatness(f);
  }

void Npc::setScale(float x, float y, float z) {
  if(sz[0]!=x || sz[1]!=y || sz[2]!=z) {
    living.reset();
    arrowImpacts.clear();
    releaseRagdoll(false);
    ragdollDormant.reset();
    ragdollLegacyDown = false;
    ragdollHasLastPosition = false;
    ragdollLastVelocity = {};
    }
  sz[0]=x;
  sz[1]=y;
  sz[2]=z;
  durtyTranform |= TR_Scale;
  physic.setScale(Vec3{x,y,z});
  }

const Animation::Sequence* Npc::playAnimByName(std::string_view name, BodyState bs, bool force) {
  return visual.startAnimAndGet(*this,name,calcAniComb(),bs,force);
  }

bool Npc::setPlayerPose(bool kneel, std::string_view reason) {
  if(!isPlayer())
    return false;
  if(kneel) {
    if(isDown() || isDead() || isInAir() || interactive()!=nullptr || spellFrozen() ||
       !hasAnim("T_STAND_2_PRAY") || !hasAnim("S_PRAY") || !hasAnim("T_PRAY_2_STAND"))
      return false;
    if(playerKneeling)
      return true;
    clearAiQueue();
    aiPush(AiQueue::aiRemoveWeapon());
    aiPush(AiQueue::aiPlayAnim("T_STAND_2_PRAY"));
    playerKneeling = true;
    playerPoseExiting = false;
    }
  else if(playerKneeling) {
    Log::i("[ai] hero pose kneel ends: ",reason);
    playerKneeling = false;
    playerPoseExiting = true;
    clearAiQueue();
    aiPush(AiQueue::aiPlayAnim("T_PRAY_2_STAND"));
    }
  return true;
  }

bool Npc::tickPlayerPose() {
  if(isDown() || isDead()) {
    if(playerKneeling)
      Log::i("[ai] hero pose kneel ends: down");
    playerKneeling = playerPoseExiting = false;
    return false;
    }
  if(playerKneeling)
    return true;
  if(playerPoseExiting && !isAiBusy())
    playerPoseExiting = false;
  return playerPoseExiting;
  }

bool Npc::setAnim(Npc::Anim a) {
  return setAnimAngGet(a)!=nullptr;
  }

const Animation::Sequence* Npc::setAnimAngGet(Anim a) {
  return setAnimAngGet(a,calcAniComb());
  }

const Animation::Sequence* Npc::setAnimAngGet(Anim a, uint8_t comb) {
  if(a==Anim::AttackBlock && (!allowMeleeStunAction("parade") ||
     (Gothic::physicsFlags().meleeParry && !allowStaminaAction("parade",0.f))))
    return nullptr;
  if(a==Anim::MoveBack && !allowMeleeStunAction("jumpback"))
    return nullptr;
  const bool trace = TestBench::bodyTracing() && currentTarget!=nullptr;
  const auto before = trace ? combatPoseIdentity() : std::string();
  auto st  = weaponState();
  auto wlk = walkMode();
  const bool costsStamina = a==Anim::MoveBack && !isUsingItemInHand() && hasStamina() &&
                           (st==WeaponState::Fist || st==WeaponState::W1H || st==WeaponState::W2H) &&
                           !isSwim() && !isDive() && (!isPlayer() || !bool(wlk&WalkBit::WM_Sneak));
  if(costsStamina && !allowStaminaAction("jumpback",MeleeStamina::JumpBackCost,
                                        !isPlayer() && !BodyController::owns(*this)))
    return nullptr;
  const auto staminaBefore = costsStamina ? meleeStaminaIdentity(visual.pose(),owner.tickCount()) : std::string();
  if(isPlayer() && (st==WeaponState::Bow || st==WeaponState::CBow) && !hasManualAim()) {
    const auto bs = bodyStateMasked() & BS_MAX;
    if(bs!=BS_AIMNEAR && bs!=BS_AIMFAR) {
      Tempest::Vec3 dir = {};
      switch(a) {
        case Anim::Move:     dir = { 0,0, 1}; break;
        case Anim::MoveBack: dir = { 0,0,-1}; break;
        case Anim::MoveL:    dir = {-1,0, 0}; break;
        case Anim::MoveR:    dir = { 1,0, 0}; break;
        default: break;
        }
      // Input is processed after movement: prevent it from restarting the carried-weapon walk
      // at the edge every frame. A drawn aim keeps its own animation.
      if(isEdgeMoveBlocked(dir))
        a = Anim::Idle;
      }
    }
  const auto result = visual.startAnimAndGet(*this,a,comb,st,wlk);
  if(result!=nullptr && isUsingItemInHand() && (a==Anim::AttackBlock || a==Anim::Jump || a==Anim::JumpUp ||
     a==Anim::JumpUpLow || a==Anim::JumpUpMid || a==Anim::JumpHang))
    interruptItemUse(a==Anim::AttackBlock ? "parade" : "jump");
  if(result!=nullptr && costsStamina && staminaBefore!=meleeStaminaIdentity(visual.pose(),owner.tickCount()))
    spendStamina("jumpback",MeleeStamina::JumpBackCost);
  if(result!=nullptr && a==Anim::AttackBlock) {
    meleeAiDodgeStreak.reset();
    tickMeleeParry();
    }
  if(trace)
    traceCombatAttempt(a,result!=nullptr,before,"set_anim");
  return result;
  }

void Npc::setAnimRotate(int rot) {
  visual.setAnimRotate(*this,rot);
  }

bool Npc::setAnimItem(std::string_view scheme, int state, bool allowMissing) {
  if(scheme.empty())
    return true;
  if(bodyStateMasked()!=BS_STAND) {
    setAnim(Anim::Idle);
    return false;
    }
  if(allowMissing && !visual.hasAnim(string_frm("T_",scheme,"_STAND_2_S0")))
    return true;
  if(auto sq = visual.startAnimItem(*this,scheme,state)) {
    implAniWait(uint64_t(sq->totalTime()));
    return true;
    }
  return false;
  }

void Npc::stopAnim(std::string_view ani) {
  visual.stopAnim(*this,ani);
  }

void Npc::startFaceAnim(std::string_view anim, float intensity, uint64_t duration) {
  visual.startFaceAnim(*this,anim,intensity,duration);
  }

float Npc::faceWeight(std::string_view anim) {
  return visual.faceWeight(*this,anim);
  }

void Npc::startSpeech(const Tempest::Sound& voice) {
  visual.startSpeech(*this,LipSync::Track::fromSound(voice));
  }

void Npc::startSpeech(std::shared_ptr<LipSync::Track> voice) {
  visual.startSpeech(*this,std::move(voice));
  }

void Npc::setTalkMood(std::string_view mood) {
  visual.setTalkMood(mood);
  }

void Npc::listenTo(uint64_t ms) {
  visual.listen(owner.tickCount()+ms);
  }

void Npc::stopListening() {
  visual.stopListening(owner.tickCount());
  }

void Npc::heardWords() {
  visual.heard(owner.tickCount());
  }

void Npc::thinkOfAnswer(uint64_t ms) {
  const uint64_t now = owner.tickCount();
  visual.think(now,ms==0 ? 0 : now+ms);
  }

void Npc::stopSpeech() {
  visual.stopSpeech();
  }

bool Npc::stopItemStateAnim() {
  return visual.stopItemStateAnim(*this);
  }

bool Npc::hasAnim(std::string_view scheme) const {
  return visual.hasAnim(scheme);
  }

bool Npc::hasAnim(Anim a) const {
  auto st  = weaponState();
  auto wlk = walkMode();
  return visual.hasAnim(a,st,wlk);
  }

bool Npc::hasSwimAnimations() const {
  return hasAnim("S_SWIM") && hasAnim("S_SWIMF");
  }

bool Npc::isFinishingMove() const {
  if(weaponState()==WeaponState::NoWeapon)
    return false;
  return visual.pose().isInAnim("T_1HSFINISH") || visual.pose().isInAnim("T_2HSFINISH");
  }

bool Npc::isStanding() const {
  return visual.isStanding();
  }

bool Npc::isSwim() const {
  return mvAlgo.isSwim();
  }

bool Npc::isInWater() const {
  return mvAlgo.isInWater();
  }

bool Npc::isDive() const {
  return mvAlgo.isDive();
  }

bool Npc::isCasting() const {
  return castLevel!=CS_NoCast;
  }

bool Npc::isJumpAnim() const {
  return visual.pose().isJumpAnim();
  }

bool Npc::isFlyAnim() const {
  return visual.pose().isFlyAnim();
  }

bool Npc::isFalling() const {
  return mvAlgo.state()==MoveAlgo::Falling;
  }

bool Npc::isFallingDeep() const {
  return (mvAlgo.isInAir() || mvAlgo.isFalling()) && (visual.pose().isInAnim("S_FALL") || visual.pose().isInAnim("S_FALLB"));
  }

bool Npc::isSlide() const {
  return mvAlgo.state()==MoveAlgo::Slide;
  }

bool Npc::isInAir() const {
  return mvAlgo.state()==MoveAlgo::InAir;
  }

bool Npc::isJump() const {
  return mvAlgo.state()==MoveAlgo::Jump;
  }

bool Npc::isJumpUp() const {
  return mvAlgo.state()==MoveAlgo::JumpUp;
  }

void Npc::invalidateTalentOverlays() {
  const Talent tl[] = {TALENT_1H, TALENT_2H, TALENT_BOW, TALENT_CROSSBOW, TALENT_ACROBAT};
  for(Talent i:tl) {
    invalidateTalentOverlays(i);
    }
  }

void Npc::invalidateTalentOverlays(Talent t) {
  const auto scheme = visual.visualSkeletonScheme();
  if(scheme.empty())
    return;

  const auto lvl = talentsSk[t];
  if(t==TALENT_1H){
    if(lvl==0){
      delOverlay(string_frm(scheme,"_1HST1.MDS"));
      delOverlay(string_frm(scheme,"_1HST2.MDS"));
      }
    else if(lvl==1){
      addOverlay(string_frm(scheme,"_1HST1.MDS"),0);
      delOverlay(string_frm(scheme,"_1HST2.MDS"));
      }
    else if(lvl==2){
      delOverlay(string_frm(scheme,"_1HST1.MDS"));
      addOverlay(string_frm(scheme,"_1HST2.MDS"),0);
      }
    }
  else if(t==TALENT_2H){
    if(lvl==0){
      delOverlay(string_frm(scheme,"_2HST1.MDS"));
      delOverlay(string_frm(scheme,"_2HST2.MDS"));
      }
    else if(lvl==1){
      addOverlay(string_frm(scheme,"_2HST1.MDS"),0);
      delOverlay(string_frm(scheme,"_2HST2.MDS"));
      }
    else if(lvl==2){
      delOverlay(string_frm(scheme,"_2HST1.MDS"));
      addOverlay(string_frm(scheme,"_2HST2.MDS"),0);
      }
    }
  else if(t==TALENT_BOW){
    if(lvl==0){
      delOverlay(string_frm(scheme,"_BOWT1.MDS"));
      delOverlay(string_frm(scheme,"_BOWT2.MDS"));
      }
    else if(lvl==1){
      addOverlay(string_frm(scheme,"_BOWT1.MDS"),0);
      delOverlay(string_frm(scheme,"_BOWT2.MDS"));
      }
    else if(lvl==2){
      delOverlay(string_frm(scheme,"_BOWT1.MDS"));
      addOverlay(string_frm(scheme,"_BOWT2.MDS"),0);
      }
    }
  else if(t==TALENT_CROSSBOW){
    if(lvl==0){
      delOverlay(string_frm(scheme,"_CBOWT1.MDS"));
      delOverlay(string_frm(scheme,"_CBOWT2.MDS"));
      }
    else if(lvl==1){
      addOverlay(string_frm(scheme,"_CBOWT1.MDS"),0);
      delOverlay(string_frm(scheme,"_CBOWT2.MDS"));
      }
    else if(lvl==2){
      delOverlay(string_frm(scheme,"_CBOWT1.MDS"));
      addOverlay(string_frm(scheme,"_CBOWT2.MDS"),0);
      }
    }
  else if(t==TALENT_ACROBAT){
    if(lvl==0)
      delOverlay(string_frm(scheme,"_ACROBATIC.MDS")); else
      addOverlay(string_frm(scheme,"_ACROBATIC.MDS"),0);
    }
  }

bool Npc::implPointAt(const Tempest::Vec3& to) {
  auto    dpos = to-position();
  uint8_t comb = Pose::calcAniComb(dpos,angle);

  return (setAnimAngGet(Npc::Anim::PointAt,comb)!=nullptr);
  }

bool Npc::implLookAtWp(uint64_t dt) {
  if(currentLookAt==nullptr)
    return false;
  auto dvec = currentLookAt->position();
  return implLookAt(dvec.x,dvec.y,dvec.z,dt);
  }

bool Npc::implLookAtNpc(uint64_t dt) {
  if(currentLookAtNpc==nullptr)
    return false;
  auto selfHead  = visual.mapHeadBone();
  auto otherHead = currentLookAtNpc->visual.mapHeadBone();
  auto dvec = otherHead - selfHead;
  return implLookAt(dvec.x,dvec.y,dvec.z,dt);
  }

void Npc::implHeadBack(uint64_t dt) {
  static const float rotSpeed = 200; // deg per second, as implLookAt
  auto rot = visual.headRotation();
  if(rot.x==0 && rot.y==0)
    return;
  const float step = rotSpeed*float(dt)/1000.f;
  auto back = [step](float v) { return std::abs(v)<=step ? 0.f : v - std::copysign(step,v); };
  visual.setHeadRotation(back(rot.x),back(rot.y));
  }

bool Npc::implLookAt(float dx, float dy, float dz, uint64_t dt) {
  static const float rotSpeed = 200; // deg per second
  static const float maxRot   = 80; // maximum rotation
  Vec2 dst;

  dst.x = visual.viewDirection()-angleDir(dx,dz);
  while(dst.x>180)
    dst.x -= 360;
  while(dst.x<-180)
    dst.x += 360;

  dst.y = std::atan2(dy,std::sqrt(dx*dx+dz*dz));
  dst.y = dst.y*180.f/float(M_PI);

  if(dst.x<-maxRot || dst.x>maxRot) {
    dst.x = 0;
    dst.y = 0;
    }

  if(dst.y<-20)
    dst.y = -20;
  if(dst.y>20)
    dst.y = 20;

  auto rot  = visual.headRotation();
  auto drot = dst-rot;

  drot.x = std::min(std::abs(drot.x),rotSpeed*float(dt)/1000.f);
  drot.y = std::min(std::abs(drot.y),rotSpeed*float(dt)/1000.f);
  if(dst.x<rot.x)
    drot.x = -drot.x;
  if(dst.y<rot.y)
    drot.y = -drot.y;

  rot+=drot;
  visual.setHeadRotation(rot.x,rot.y);

  return false;
  }

bool Npc::implTurnAway(const Npc &oth, uint64_t dt) {
  if(&oth==this)
    return true;

  // turn npc's back to oth, so calculate direction from oth to npc
  auto dx = x-oth.x;
  auto dz = z-oth.z;
  auto  gl   = guild();
  float step = float(owner.script().guildVal().turn_speed[gl]);
  return rotateTo(dx,dz,step,AnimationSolver::TurnType::Std,dt);
  }

bool Npc::implTurnToFai(const Npc& oth, uint64_t dt) {
  if(meleeAiSpecial==MeleeAi::Special::Charge && isMeleeChargeReleasing())
    return false;
  if(&oth==this || oth.isDown())
    return false;

  auto ws = weaponState();
  if(ws==WeaponState::NoWeapon)
    return false;

  auto  gl   = guild();
  auto& gv   = owner.script().guildVal();
  float step = float(gv.turn_speed[gl]);
  //auto  dpos = fghAlgo.distVec(*currentTarget, *this);
  auto dpos = currentTarget->collosionCenter() - collosionCenter();

  // vanilla has a bug(or quirk) apparently, for that
  // also would need to fallthru in FAI code, if no animation is performed
  bool skipAnim = gv.turn_speed[gl] >= 100;
  auto anim = skipAnim ? AnimationSolver::TurnType::None : AnimationSolver::TurnType::Std;
  if(ws==WeaponState::Bow || ws==WeaponState::CBow || ws==WeaponState::Mage) {
    anim = AnimationSolver::TurnType::None;
    }

  auto bs = bodyStateMasked();
  if(bs!=BS_HIT) {
    //NOTE: Troll rotates during the hit, but not very fast - seem to be regulat speed
    step *= 2.f; // faster in combat
    }
  return rotateTo(dpos.x,dpos.z,step,anim,dt) && !skipAnim;
  }

bool Npc::implTurnTo(const Npc &oth, uint64_t dt) {
  if(&oth==this)
    return false;
  auto dx = oth.x-x;
  auto dz = oth.z-z;
  return implTurnTo(dx,dz,AnimationSolver::TurnType::Std,dt);
  }

bool Npc::implTurnTo(const Npc& oth, AnimationSolver::TurnType anim, uint64_t dt) {
  if(&oth==this)
    return false;
  auto dx = oth.x-x;
  auto dz = oth.z-z;
  return implTurnTo(dx,dz,anim,dt);
  }

bool Npc::implTurnTo(const WayPoint* wp, AnimationSolver::TurnType anim, uint64_t dt) {
  if(wp==nullptr)
    return false;
  return implTurnTo(wp->dir.x,wp->dir.z,anim,dt);
  }

bool Npc::implTurnTo(float dx, float dz, AnimationSolver::TurnType anim, uint64_t dt) {
  auto  gl   = guild();
  float step = float(owner.script().guildVal().turn_speed[gl]);
  return rotateTo(dx,dz,step,anim,dt);
  }

bool Npc::implWhirlTo(const Npc &oth, uint64_t dt) {
  return implTurnTo(oth,AnimationSolver::TurnType::Whirl,dt);
  }

bool Npc::implAttack(uint64_t dt, bool bodyOverride, FightAlgo::Action bodyAction) {
  if(currentTarget==nullptr || isPlayer() || isTalk()) {
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    return false;
    }

  if(currentTarget->isDown()){
    traceCombatGate("target_down");
    // NOTE: don't clear internal target, to make scripts happy
    // currentTarget=nullptr;
    fghAlgo.onClearTarget();
    return false;
    }

  if(aiQueue.size()>0 && !(aiQueue.size()==1 && pendingSimpleGoTo()!=nullptr)) {
    traceCombatGate("script_queue");
    // do not messup weapon change animations by MOVE intruction
    return false;
    }

  // A paired victim belongs to its riposter until handover (or the victim clip ends).
  if(currentTarget->meleeRiposteAiHeld && currentTarget->isMeleeRiposteHeld()) {
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    visual.stopWalkAnim(*this);
    if(!visual.pose().isStanding())
      setAnim(Anim::Idle);
    implTurnToFai(*currentTarget,dt);
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return true;
    }

  const auto ws = weaponState();
  const auto bs = bodyStateMasked();
  const auto& flags = Gothic::physicsFlags();
  const bool reactions = isHuman() && ws==WeaponState::W1H;
  const bool dodgeAi = reactions && flags.meleeAiDodge && flags.meleeDodge;
  if(bodyOverride || !dodgeAi || (meleeAiDodgeTiming && meleeAiDodgeTiming->target!=currentTarget))
    meleeAiDodgeTiming.reset();
  const bool parryAi = !bodyOverride && flags.meleeParryAi && flags.meleeParry && isHuman() &&
                       (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H);
  if(!parryAi || (meleeParadeTiming && meleeParadeTiming->target!=currentTarget))
    meleeParadeTiming.reset();
  const bool downTactics = !bodyOverride && flags.meleeDownTactics && flags.meleePosture && flags.meleePhysicsHits &&
                          (ws==WeaponState::W1H || ws==WeaponState::W2H);
  const bool targetKnockdown = downTactics && currentTarget->meleeKnockdown;
  const bool targetStunned = parryAi && currentTarget->isMeleeStunned();
  // Discard the remaining native combo, but let the attack already in flight finish below.
  if(targetKnockdown || targetStunned) {
    fghAlgo.onTargetKnockdown();
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    }
  const bool magnetHeading = visual.pose().isAttackAnim() && meleeShadow && meleeShadow->window && meleeShadow->physicsHits &&
                             meleeShadow->magnet.eligible;

  if((bs==BS_HIT || ((targetKnockdown || targetStunned) && visual.pose().isAttackAnim())) &&
     (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H)) {
    traceCombatGate("melee_hit_lock");
    //NOTE: 'storm' attack has BS_RUN state and not meant to be auto-rotated
    if(!magnetHeading)
      implTurnToFai(*currentTarget,dt);
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }

  if(!bodyOverride && !targetKnockdown && !targetStunned && !fghAlgo.hasInstructions()) {
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    traceCombatGate("no_instructions");
    return false;
    }

  if(bs==BS_LIE) {
    traceCombatGate("lie");
    setAnim(Npc::Anim::Idle);
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }
  if(bs==BS_STUMBLE || bs==BS_FALL || isInAir()) {
    traceCombatGate("stumble_fall_air");
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }

  if(faiWaitTime>=owner.tickCount() || waitTime>=owner.tickCount()) {
    traceCombatGate("fai_or_script_wait");
    if(!magnetHeading)
      implTurnToFai(*currentTarget,dt);
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }

  if(!bodyOverride && tryMeleeAiBarge()) {
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return true;
    }

  if(targetKnockdown || targetStunned) {
    const bool rising = currentTarget->meleeRiseTick!=0;
    // Use the same root-transformed distance as isInFinishRange, bounded by its script limit.
    const float finishRange = std::min(fghAlgo.attackFinishDistance(owner.script()),ws==WeaponState::W1H ? 165.f : 170.f);
    const bool inRange = targetStunned || rising ? fghAlgo.isInWRange(*this,*currentTarget,owner.script()) :
                                 fghAlgo.qDistTo(*this,*currentTarget)<=finishRange*finishRange;
    const bool focus = fghAlgo.isInFocusAngle(*this,*currentTarget,5.f);
    auto action = FightAlgo::resolveDownTactics(downTactics,currentTarget->meleeKnockdown,rising,inRange,focus);
    if(targetStunned) {
      switch(MeleeParry::resolveStunnedTactics(parryAi,true,inRange,focus)) {
        case MeleeParry::StunnedAction::Move:   action = FightAlgo::MV_MOVE; break;
        case MeleeParry::StunnedAction::Turn:   action = FightAlgo::MV_TURN2HIT; break;
        case MeleeParry::StunnedAction::Attack: action = FightAlgo::MV_ATTACK; break;
        case MeleeParry::StunnedAction::None:   break;
        }
      if(TestBench::bodyTracing() && action!=FightAlgo::MV_ATTACK)
        TestBench::bodyEvent(*this,currentTarget,"stunned_target",owner.tickCount(),
                            "\"action\":"+TestBench::bodyJson(action==FightAlgo::MV_MOVE ? "move" : "turn"));
      }
    if(action==FightAlgo::MV_MOVE) {
      implTurnToFai(*currentTarget,dt);
      setAnim(AnimationSolver::Move);
      go2.set(currentTarget,GT_Enemy);
      go2.nativeCombatTarget = currentTarget;
      mvAlgo.tick(dt,MoveAlgo::FaiMove);
      go2.clear();
      wayPath.clear();
      }
    else {
      visual.stopWalkAnim(*this);
      if(!visual.pose().isStanding())
        setAnim(Anim::Idle);
      implTurnToFai(*currentTarget,dt);
      if(action==FightAlgo::MV_ATTACK && visual.pose().isStanding()) {
        if(targetStunned && tryMeleeAiRiposte())
          return true;
        if(!allowStaminaAction("attack",meleeAttackCost(),true)) {
          waitForMeleeStamina(dt);
          return true;
          }
        const auto hit = owner.physic()->ray(collosionCenter(),currentTarget->collosionCenter());
        if(!hit.hasCol && doAttack(Anim::Attack,BS_HIT)) {
          if(targetStunned && TestBench::bodyTracing())
            TestBench::bodyEvent(*this,currentTarget,"stunned_target",owner.tickCount(),"\"action\":\"attack\"");
          implFaiWait(visual.pose().atkTotalTime()+1);
          }
        }
      mvAlgo.tick(dt,MoveAlgo::FaiMove);
      }
    return true;
    }

  const auto act = bodyOverride ? bodyAction : fghAlgo.nextFromQueue(*this,*currentTarget,owner.script());
  if(act!=FightAlgo::MV_BLOCK)
    meleeParadeTiming.reset();
  const auto consume = [&] {
    meleeParadeTiming.reset();
    meleeAiDodgeTiming.reset();
    if(bodyOverride) { BodyController::consumed(*this); return; }
    const auto before = fghAlgo.queuedAction();
    BodyTrajectory::nativeConsumed(*this);
    fghAlgo.consumeAction();
    if(TestBench::bodyTracing())
      TestBench::bodyEvent(*this,currentTarget,"native_consume",owner.tickCount(),
                          "\"choice_id\":" + std::to_string(fghAlgo.traceChoiceId()) +
                          ",\"primitive\":" + std::to_string(int(before)) +
                          ",\"next_primitive\":" + std::to_string(int(fghAlgo.queuedAction())));
    };

  // NOTE: in original-game, this behaviour seem to be hardcoded
  // test case: wolf jump-back quite often when close, but programmed to jump only if attacked
  // so far promoting wait to jump seem to work best
  FightAlgo::ResolverState nativeResolution;
  nativeResolution.inCloseup = fghAlgo.isInCloseupRange(*this,*currentTarget,owner.script());
  nativeResolution.focus30 = nativeResolution.inCloseup && fghAlgo.isInFocusAngle(*this,*currentTarget);
  const bool jmp = FightAlgo::resolveNative(FightAlgo::MV_WAIT,nativeResolution)==FightAlgo::MV_JUMPBACK;
  const bool jumpBack = act==FightAlgo::MV_JUMPBACK || ((act==FightAlgo::MV_WAIT || act==FightAlgo::MV_TURN) && jmp);
  if(!jumpBack)
    meleeAiDodgeTiming.reset();

  // vanilla behavior, required for orcs in G1 orcgraveyard
  if(ws==WeaponState::NoWeapon && isAiQueueEmpty() && canSwitchWeapon()) {
    drawWeaponMelee();
    return true;
    }

  // Recorder labels describe the physical opportunity after all native gates. The pure
  // normalization mirrors the executor below without changing its queue, RNG or decisions.
  auto trajectoryEffective=act;
  if(BodyTrajectory::enabled() && !bodyOverride) {
    // Diagnostic effective primitives follow the native executor even when LOS makes the
    // separate shared policy mask invalid. Hidden geometry never enters capture features.
    auto resolver=nativeResolution;
    resolver.focus5=fghAlgo.isInFocusAngle(*this,*currentTarget,5.f);
    resolver.inWRange=fghAlgo.isInWRange(*this,*currentTarget,owner.script());
    trajectoryEffective=FightAlgo::resolveNative(act,resolver);
    // Native TURN uses the same approach executor as MOVE outside weapon range. Inside
    // range that branch only rotates. Policy TurnToTarget uses TURN2HIT, so labels must
    // describe those physical semantics rather than the native instruction's name.
    if(trajectoryEffective==FightAlgo::MV_MOVE || trajectoryEffective==FightAlgo::MV_TURN) {
      if(!resolver.inWRange) trajectoryEffective=FightAlgo::MV_MOVE;
      else if(!resolver.focus5) trajectoryEffective=FightAlgo::MV_TURN2HIT;
      }
    }
  BodyTrajectory::Attempt trajectory(*this,!bodyOverride,int(act),int(trajectoryEffective),
                                    fghAlgo.traceChoiceId(),fghAlgo.traceParentInstruction(),fghAlgo.traceEntrySerial());

  if(act==FightAlgo::MV_BLOCK) {
    if(parryAi) {
      const auto now = owner.tickCount();
      const auto skip = [&](std::string_view reason) {
        if(TestBench::bodyTracing())
          TestBench::bodyEvent(*this,currentTarget,"parade_skipped",now,"\"reason\":"+TestBench::bodyJson(reason));
        consume();
        };
      const auto hold = [&] {
        visual.stopWalkAnim(*this);
        if(!visual.pose().isStanding())
          setAnim(Anim::Idle);
        implTurnToFai(*currentTarget,dt);
        mvAlgo.tick(dt,MoveAlgo::FaiMove);
        };
      if(!meleeParadeTiming)
        meleeParadeTiming = MeleeParadeTiming{currentTarget,nullptr,0,now};
      const auto held = now-meleeParadeTiming->began;
      if(held>=1000) {
        skip("timeout");
        return true;
        }
      const auto swing = currentTarget->visual.pose().nextMeleeContact(now);
      if(meleeParadeTiming->sequence==nullptr) {
        switch(MeleeParry::awaitSwing(held,bool(swing))) {
          case MeleeParry::SwingAction::Wait:
            hold();
            return true;
          case MeleeParry::SwingAction::GiveUp:
            skip("no_swing");
            return true;
          case MeleeParry::SwingAction::TimeParade:
            break;
          }
        }
      if(!swing) {
        // A late timing error may put the start after the optimal frame. Keep that
        // deadline while the same swing still exists; an interrupted swing is gone.
        bool same = false;
        for(const auto& layer:currentTarget->visual.pose().combatLayers(now))
          same |= layer.sequence==meleeParadeTiming->sequence && layer.origin==meleeParadeTiming->origin;
        if(!same) {
          skip("swing_gone");
          return true;
          }
        }
      const int talent = std::clamp(hitChance(ws==WeaponState::W2H ? TALENT_2H : TALENT_1H),0,100);
      if(swing && (meleeParadeTiming->sequence!=swing->sequence || meleeParadeTiming->origin!=swing->origin)) {
        const auto began = meleeParadeTiming->began;
        const double u = (double(owner.script().rand(1u<<24,this))+.5)/double(1u<<24);
        const double v = (double(owner.script().rand(1u<<24,this))+.5)/double(1u<<24);
        const float error = MeleeParry::timingSigmaMs(talent)*float(std::sqrt(-2.*std::log(u))*std::cos(2.*M_PI*v));
        meleeParadeTiming = MeleeParadeTiming{currentTarget,swing->sequence,swing->origin,began,swing->contact,error};
        if(TestBench::bodyTracing()) {
          const auto start = MeleeParry::paradeStartMs(now,swing->contact,talent,error,meleeParryLeadMs());
          TestBench::bodyEvent(*this,currentTarget,"parade_timed",now,
                              "\"predicted_contact_ms\":"+std::to_string(swing->contact-now)+
                              ",\"held_ms\":"+std::to_string(held)+
                              ",\"wait_ms\":"+std::to_string(start-now)+",\"error_ms\":"+std::to_string(error)+
                              ",\"aim_ms\":"+std::to_string(MeleeParry::windowMs(talent)*.5f+meleeParryLeadMs())+
                              ",\"talent\":"+std::to_string(talent));
          }
        }
      if(swing)
        meleeParadeTiming->contact = swing->contact;
      const auto start = MeleeParry::paradeStartMs(now,meleeParadeTiming->contact,talent,meleeParadeTiming->errorMs,meleeParryLeadMs());
      if(now<start) {
        hold();
        return true;
        }
      }
    if(!fghAlgo.isInFocusAngle(*this, *currentTarget)) {
      consume();
      return true;
      }

    switch(ws) {
      case WeaponState::Fist: {
        if(blockFist())
          consume();
        break;
        }
      case WeaponState::W1H:
      case WeaponState::W2H: {
        if(blockSword())
          consume();
        break;
        }
      default:
        consume();
        break;
      }
    return true;
    }

  if(act==FightAlgo::MV_ATTACK || act==FightAlgo::MV_ATTACKL || act==FightAlgo::MV_ATTACKR) {
    if(!bodyOverride && (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) &&
       !allowStaminaAction("attack",meleeAttackCost(),true)) {
      waitForMeleeStamina(dt);
      return true;
      }
    //NOTE: FIGHT_DIST_CANCEL in scipts is often longer, than senses_range of npc
    const auto sense = fghAlgo.isInFocusAngle(*this,*currentTarget,5.f);
    nativeResolution.focus5=sense;
    if(FightAlgo::resolveNative(act,nativeResolution)==FightAlgo::MV_TURN2HIT) {
      traceCombatGate("attack_turn_to_focus");
      implTurnToFai(*currentTarget,dt);
      mvAlgo.tick(dt,MoveAlgo::FaiMove);
      return true;
      }
#if 0
    consume(); //debug
    return true;
#endif

    static const Anim ani[4] = {Anim::Attack, Anim::AttackL, Anim::AttackR};
    if((act!=FightAlgo::MV_ATTACK && bodyState()!=BS_RUN) &&
       !fghAlgo.isInWRange(*this,*currentTarget,owner.script())) {
      consume();
      return true;
      }

    if(ws==WeaponState::Bow || ws==WeaponState::CBow || ws==WeaponState::Mage) {
      bool obsticle = false;
      if(currentTarget!=nullptr) {
        auto hit = owner.physic()->rayNpc(this->mapWeaponBone(),currentTarget->centerPosition(),this);
        if(hit.hasCol && hit.npcHit!=currentTarget) {
          obsticle = true;
          // if(hit.npcHit!=nullptr && owner.script().personAttitude(*this,*hit.npcHit)==ATT_HOSTILE)
          //   obsticle = false;
          if(hit.npcHit!=nullptr && hit.npcHit!=currentTarget && owner.script().isFriendlyFire(*this,*hit.npcHit))
            obsticle = false;
          }
        }
      if(auto spl = activeWeapon()) {
        if(spl->isSpell() && !spl->isSpellShoot())
          obsticle = false;
        }
      if(obsticle) {
        auto anim = (owner.script().rand(2,this)==0 ? Npc::Anim::MoveL : Npc::Anim::MoveR);
        if(setAnim(anim)){
          visual.setAnimRotate(*this,0);
          implFaiWait(visual.pose().animationTotalTime());
          consume();
          return true;
          }
        }
      }

    if(ws==WeaponState::Mage) {
      const auto cast = beginCastSpell();
      if(cast==BeginCastResult::BC_No)
        return false;
      consume();
      }
    else if(ws==WeaponState::Bow || ws==WeaponState::CBow) {
      if(archAi.isDriving(*this)) {
        // aimed by hand (game/archerai): drawn and let go frame by frame, the attack is over with the arrow;
        // a run (a walk) stops first, as shootBow does: the bow is raised standing
        if(bs==BS_RUN || bs==BS_WALK || bs==BS_SNEAK)
          setAnim(Anim::Idle);
        if(archAi.attack(*this))
          consume();
        }
      else if(shootBow()) {
        consume();
        }
      else if(!implTurnToFai(*currentTarget,dt)) {
        aimBow();
        }
      }
    else if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) {
      const auto hit = owner.physic()->ray(this->collosionCenter(),currentTarget->collosionCenter());
      if(hit.hasCol) {
        // blocked by wall
        consume();
        return true;
        }
      if(bodyOverride && act==FightAlgo::MV_ATTACK && tryMeleeAiRiposte()) {
        consume();
        return true;
        }
      if(!bodyOverride && act==FightAlgo::MV_ATTACK && tryMeleeAiCharge()) {
        consume();
        return true;
        }
      const auto atkType = (ws==WeaponState::Fist) ? Anim::Attack : ani[act-FightAlgo::MV_ATTACK];
      const bool atk     = doAttack(atkType, BS_HIT);

      if(atk || mvAlgo.isSwim() || mvAlgo.isDive()) {
        uint64_t aniTime = visual.pose().atkTotalTime()+1;
        implFaiWait(aniTime);
        if(bs==BS_RUN)
          implAniWait(aniTime);
        consume();
        } else {
        implTurnToFai(*currentTarget,dt);
        }
      }
    else {
      // Attack action without any weapon. Can happend at weapon transition(orc shaman) - skip it.
      consume();
      }
    return true;
    }

  if(act==FightAlgo::MV_TURN2HIT) {
    if(!implTurnTo(*currentTarget,dt))
      consume();
    return true;
    }

  if(act==FightAlgo::MV_STRAFEL) {
    if(setAnim(Npc::Anim::MoveL)) {
      visual.setAnimRotate(*this,0);
      implFaiWait(visual.pose().animationTotalTime());
      consume();
      }
    else if(!hasAnim(Npc::Anim::MoveL)) {
      // avoid soft-locks
      visual.setAnimRotate(*this,0);
      consume();
      }
    return true;
    }

  if(act==FightAlgo::MV_STRAFER) {
    if(setAnim(Npc::Anim::MoveR)) {
      visual.setAnimRotate(*this,0);
      implFaiWait(visual.pose().animationTotalTime());
      consume();
      }
    else if(!hasAnim(Npc::Anim::MoveR)) {
      // avoid soft-locks
      visual.setAnimRotate(*this,0);
      consume();
      }
    return true;
    }

  if(act==FightAlgo::MV_STRAFE_E) {
    // finalize strafe
    if(!setAnim(Npc::Anim::Idle))
      return false;
    consume();
    return true;
    }

  if(jumpBack) {
    if(isSwim()) {
      consume();
      return true;
      }
    if(bodyStateMasked()==BS_PARADE) {
      consume();
      return true;
      }
    if(!fghAlgo.isInFocusAngle(*this, *currentTarget) && !jmp) {
      //NOTE: jump-back is ultimate defence, so better to use it only if npc face player directly
      consume();
      aiState.loopNextTime = owner.tickCount(); // force ZS_MM_Attack_Loop call
      return true;
      }
    if(dodgeAi && (!bodyOverride || act==FightAlgo::MV_JUMPBACK)) {
      const auto now = owner.tickCount();
      const auto skip = [&](std::string_view reason) {
        if(TestBench::isActive())
          TestBench::note(("melee ai dodge "+std::string(TestBench::who(this))+": skipped "+std::string(reason)).c_str());
        if(TestBench::bodyTracing())
          TestBench::bodyEvent(*this,currentTarget,"ai_dodge_skipped",now,"\"reason\":"+TestBench::bodyJson(reason));
        consume();
        };
      const auto hold = [&] {
        visual.stopWalkAnim(*this);
        if(!visual.pose().isStanding())
          setAnim(Anim::Idle);
        implTurnToFai(*currentTarget,dt);
        mvAlgo.tick(dt,MoveAlgo::WaitMove);
        };
      if(!meleeAiDodgeTiming) {
        const auto* armor = invent.currentArmor();
        const auto kind = MeleeDodge::kind(armor ? std::optional<MeleeImpact::ArmorClass>(MeleeImpact::armorClass(armor->handle().protection[PROT_EDGE])) :
                                                 std::optional<MeleeImpact::ArmorClass>());
        const auto choose = [&](MeleeDodge::Direction dir) -> std::optional<MeleeDodge::Move> {
          for(const auto& candidate:MeleeDodge::plan(kind,MeleeDodge::Stance::OneHand,dir).candidates)
            if(visual.hasAnim(candidate.name))
              return MeleeDodge::Move{candidate.kind,MeleeDodge::Stance::OneHand,dir};
          return {};
          };
        const auto back = choose(MeleeDodge::Direction::Back);
        if(back || (!bodyOverride && (choose(MeleeDodge::Direction::Left) || choose(MeleeDodge::Direction::Right)))) {
          const auto direction = bodyOverride ? MeleeDodge::Direction::Back :
                                 MeleeAi::dodgeDirection(float(owner.script().rand(1u<<24,this))/float(1u<<24));
          auto move = choose(direction);
          if(!move)
            move = back;
          if(move)
            meleeAiDodgeTiming = MeleeAiDodgeTiming{currentTarget,nullptr,0,now,0,0,*move};
          }
        }
      // With no usable clip execute the original jump back below, including its price.
      if(meleeAiDodgeTiming) {
        if(!bodyOverride && !meleeAiDodgeStreak.allows(now)) {
          skip("streak");
          return true;
          }
        auto& timing = *meleeAiDodgeTiming;
        if(!allowStaminaAction("dodge",MeleeDodge::cost(timing.move.kind),true)) {
          if(bodyOverride) {
            // P30 falls back immediately; only native AI waits for dodge stamina.
            if(visual.startAnimAndGet(*this,Anim::MoveBack,calcAniComb(),ws,walkMode())) {
              spendStamina("jumpback",MeleeStamina::JumpBackCost);
              implFaiWait(visual.pose().animationTotalTime());
              consume();
              }
            return true;
            }
          timing.began = 0; // Stamina recovery is not time spent waiting for a swing.
          waitForMeleeStamina(dt);
          return true;
          }
        if(timing.began==0)
          timing.began = now;
        if(!bodyOverride) {
          if(timing.sequence!=nullptr && now-timing.began>=1000) {
            skip("timeout");
            return true;
            }
          const auto swing = currentTarget->visual.pose().nextMeleeContact(now);
          if(!swing && timing.sequence!=nullptr) {
            bool same = false;
            for(const auto& layer:currentTarget->visual.pose().combatLayers(now))
              same |= layer.sequence==timing.sequence && layer.origin==timing.origin;
            if(!same) {
              skip("swing_gone");
              return true;
              }
            }
          if((swing || timing.sequence!=nullptr) && !fghAlgo.isInWRange(*currentTarget,*this,owner.script())) {
            skip("out_of_reach");
            return true;
            }
          const int talent = hitChance(TALENT_1H);
          if(swing && (timing.sequence!=swing->sequence || timing.origin!=swing->origin)) {
            const double u = (double(owner.script().rand(1u<<24,this))+.5)/double(1u<<24);
            const double v = (double(owner.script().rand(1u<<24,this))+.5)/double(1u<<24);
            timing.sequence = swing->sequence;
            timing.origin = swing->origin;
            timing.errorMs = MeleeParry::timingSigmaMs(talent)*float(std::sqrt(-2.*std::log(u))*std::cos(2.*M_PI*v));
            }
          if(swing)
            timing.contact = swing->contact;
          const auto window = MeleeDodge::invulnerability(timing.move.kind);
          const auto start = MeleeAi::dodgeStartMs(timing.origin,timing.contact,float(window.beginMs+window.endMs)*.5f,talent,timing.errorMs);
          if(timing.sequence!=nullptr && now<start) {
            hold();
            return true;
            }
          }
        if(meleeDodge(timing.move.direction)) {
          const auto kind = timing.move.kind==MeleeDodge::Kind::HeavyRoll ? "heavy roll" : MeleeDodge::name(timing.move.kind);
          const std::string description = std::string(MeleeDodge::name(timing.move.direction))+" "+std::string(kind);
          const auto swingMs = now-timing.origin;
          const auto contactMs = timing.contact-timing.origin;
          if(TestBench::isActive())
            TestBench::note(("melee ai dodge "+std::string(TestBench::who(this))+": "+description+
                            (timing.sequence ? " at "+std::to_string(swingMs)+" ms of "+std::string(TestBench::who(currentTarget))+"'s swing, contact "+std::to_string(contactMs)+" ms" :
                                               " now, no swing")).c_str());
          if(TestBench::bodyTracing())
            TestBench::bodyEvent(*this,currentTarget,"ai_dodge",now,
                                "\"direction\":"+TestBench::bodyJson(MeleeDodge::name(timing.move.direction))+
                                ",\"dodge_kind\":"+TestBench::bodyJson(MeleeDodge::name(timing.move.kind))+
                                ",\"swing\":"+(timing.sequence ? "true" : "false")+
                                ",\"swing_ms\":"+std::to_string(timing.sequence ? swingMs : 0)+
                                ",\"contact_ms\":"+std::to_string(timing.sequence ? contactMs : 0));
          if(!bodyOverride)
            meleeAiDodgeStreak.dodged(now);
          implFaiWait(visual.pose().animationTotalTime());
          consume();
          }
        return true;
        }
      }
    if(!bodyOverride && (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) &&
       !isDive() && !allowStaminaAction("jumpback",MeleeStamina::JumpBackCost,true)) {
      waitForMeleeStamina(dt);
      return true;
      }
    if(setAnim(Npc::Anim::MoveBack)) {
      implFaiWait(visual.pose().animationTotalTime());
      consume();
      }
    return true;
    }

  if(act==FightAlgo::MV_MOVE || act==FightAlgo::MV_TURN) {
    if(currentTarget->isDown()) {
      if(setAnim(Anim::Idle))
        consume();
      return true;
      }

    const bool prGRange = fghAlgo.isInGRange(*this, *currentTarget, owner.script());
    const bool prWRange = fghAlgo.isInWRange(*this, *currentTarget, owner.script());
    const auto prBs     = bs;

    const float distance = qDistTo(*currentTarget);
    const float range    = float(handle().senses_range);

    if(!prGRange && distance<range*range) {
      // if npc is reasonably far, we can switch to propper pathfinding
      const auto hit = owner.physic()->ray(this->collosionCenter(), currentTarget->collosionCenter());
      if(hit.hasCol) {
        auto near = owner.findWayPoint(currentTarget->position(), [this](const WayPoint &wp) {
          if(!currentTarget->canRayHitPoint(wp.pos))
            return false;
          return true;
          });
        if(near!=nullptr) {
          if(near!=wayPath.last()) {
            wayPath = owner.wayTo(*this,*near);
            go2.set(wayPath.first(), GT_Way);
            go2.nativeCombatTarget = currentTarget;
            }
          return false;
          }
        }
      }

    if(prWRange) {
      //NOTE: bloodfly and other monsters may run to close to player otherwise
      //NOTE2: also for bloodfly we have to use 'hard-stop', to avoid trailing flight
      visual.stopWalkAnim(*this);
      //setAnim(Anim::Idle);
      implTurnToFai(*currentTarget,dt);
      } else {
      if(mvAlgo.checkLastBounce()) {
        if(implTurnToFai(*currentTarget,dt))
          return true;
        }
      setAnim(AnimationSolver::Move);
      go2.set(currentTarget, GT_Enemy);
      go2.nativeCombatTarget = currentTarget;
      mvAlgo.tick(dt, MoveAlgo::FaiMove);
      go2.clear();
      wayPath.clear();
      }

    const bool isGRange = fghAlgo.isInGRange(*this, *currentTarget, owner.script());
    const bool isWRange = fghAlgo.isInWRange(*this, *currentTarget, owner.script());
    const bool isFocus  = fghAlgo.isInFocusAngle(*this, *currentTarget, 5.f);

    if((isWRange || (isGRange!=prGRange) || prBs!=bodyStateMasked()) && isFocus) {
      visual.setAnimRotate(*this, 0);
      consume();
      aiState.loopNextTime = owner.tickCount(); // force ZS_MM_Attack_Loop call
      implAiTick(dt);
      return true;
      }

    implAiTick(dt);
    return true;
    }

  if(act==FightAlgo::MV_WAIT) {
    implFaiWait(200);
    consume();
    stopWalkAnimation();
    return true;
    }

  if(act==FightAlgo::MV_WAITLONG) {
    implFaiWait(300);
    consume();
    stopWalkAnimation();
    return true;
    }

  if(act==FightAlgo::MV_NULL) {
    consume();
    stopWalkAnimation();
    return true;
    }

  return true;
  }

bool Npc::implAiTick(uint64_t dt) {
  if(BodyController::owns(*this))
    return false;
  // Note AI-action queue takes priority, test case: Vatras pray at night
  if(aiQueue.size()==0) {
    if(externalCtrl)
      return false;
    tickRoutine();
    if(aiQueue.size()>0)
      nextAiAction(aiQueue,dt);
    return false;
    }
  nextAiAction(aiQueue,dt);
  return true;
  }

void Npc::implAiWait(uint64_t dt) {
  auto w = owner.tickCount()+dt;
  if(w>waitTime)
    waitTime = w;
  }

void Npc::implAniWait(uint64_t dt) {
  auto w = owner.tickCount()+dt;
  if(w>aniWaitTime)
    aniWaitTime = w;
  }

void Npc::implFaiWait(uint64_t dt) {
  faiWaitTime          = owner.tickCount()+dt;
  aiState.loopNextTime = faiWaitTime;
  }

void Npc::implSetFightMode(const Animation::EvCount& ev) {
  const auto ws = visual.fightMode();
  if(!visual.setFightMode(ev.weaponCh))
    return;

  if(ev.weaponCh==asset::MdsFightMode::NONE && (ws==WeaponState::W1H || ws==WeaponState::W2H)) {
    if(auto melee = invent.currentMeleeWeapon()) {
      auto at = centerPosition();
      if(melee->handle().material==ItemMaterial::MAT_METAL)
        sfxWeapon = ::Sound(owner,::Sound::T_Regular,"UNDRAWSOUND_ME.WAV",at,2500,false); else
        sfxWeapon = ::Sound(owner,::Sound::T_Regular,"UNDRAWSOUND_WO.WAV",at,2500,false);
      sfxWeapon.play();
      }
    }
  else if(ev.weaponCh==asset::MdsFightMode::SINGLE_HANDED || ev.weaponCh==asset::MdsFightMode::DUAL_HANDED) {
    if(auto melee = invent.currentMeleeWeapon()) {
      auto at = centerPosition();
      if(melee->handle().material==ItemMaterial::MAT_METAL)
        sfxWeapon = ::Sound(owner,::Sound::T_Regular,"DRAWSOUND_ME.WAV",at,2500,false); else
        sfxWeapon = ::Sound(owner,::Sound::T_Regular,"DRAWSOUND_WO.WAV",at,2500,false);
      sfxWeapon.play();
      }
    }
  else if(ev.weaponCh==asset::MdsFightMode::BOW || ev.weaponCh==asset::MdsFightMode::CROSSBOW) {
    auto at = centerPosition();
    sfxWeapon = ::Sound(owner,::Sound::T_Regular,"DRAWSOUND_BOW",at,2500,false);
    sfxWeapon.play();
    }
  dropTorch();
  visual.stopDlgAnim(*this);
  updateWeaponSkeleton();
  }

bool Npc::implAiFlee(uint64_t dt) {
  if(currentTarget==nullptr)
    return true;

  if(isFalling())
    return true;

  auto& oth = *currentTarget;

  const WayPoint* wp      = nullptr;
  const float     maxDist = 5*100; // 5 meters

  owner.findWayPoint(position(),[&](const WayPoint& p) {
    if(p.useCounter()>0 || qDistTo(&p)>maxDist*maxDist)
      return false;
    if(p.underWater)
      return false;
    if(!canRayHitPoint(p.position() + Vec3(0,10,0),true))
      return false;
    if(wp==nullptr || oth.qDistTo(&p)>oth.qDistTo(wp))
      wp = &p;
    return false;
    });

  if(go2.flag!=GT_Flee && go2.flag!=GT_No) {
    clearGoTo();
    }

  auto anim = (go2.flag!=GT_No)?AnimationSolver::TurnType::None:AnimationSolver::TurnType::Std;
  if(wp==nullptr || oth.qDistTo(wp)<oth.qDistTo(*this)) {
    auto  dx  = oth.x-x;
    auto  dz  = oth.z-z;
    if(implTurnTo(-dx,-dz,anim,dt))
      return (go2.flag==GT_Flee);
    } else {
    auto  dx  = wp->pos.x-x;
    auto  dz  = wp->pos.z-z;
    if(implTurnTo(dx,dz,anim,dt))
      return (go2.flag==GT_Flee);
    }

  go2.setFlee();
  setAnim(Anim::Move);
  return true;
  }

bool Npc::setGoToLadder() {
  if(go2.wp==nullptr || go2.wp!=wayPath.first())
    return false;
  auto inter = go2.wp->ladder;
  if(inter==nullptr)
    return false;
  auto pos   = inter->nearestPoint(*this);
  if(MoveAlgo::isClose(*this,pos,MAX_AI_USE_DISTANCE)) {
    if(!inter->isAvailable())
      setAnim(AnimationSolver::Idle);
    else if(setInteraction(inter))
      wayPath.pop();
    return true;
    }
  return false;
  }

Npc *Npc::updateNearestEnemy() {
  if(aiPolicy!=NpcProcessPolicy::AiNormal)
    return nullptr;

  Npc*  ret  = nullptr;
  float dist = std::numeric_limits<float>::max();
  if(nearestEnemy!=nullptr &&
     (!nearestEnemy->isDown() && canSenseNpc(*nearestEnemy,true)!=SensesBit::SENSE_NONE)) {
    ret  = nearestEnemy;
    dist = qDistTo(*ret);
    }

  owner.detectNpcNear([this,&ret,&dist](Npc& n){
    if(!isEnemy(n) || n.isDown() || &n==this)
      return;

    float d = qDistTo(n);
    if(d<dist && canSenseNpc(n,true)!=SensesBit::SENSE_NONE) {
      ret  = &n;
      dist = d;
      }
    });
  nearestEnemy = ret;
  return nearestEnemy;
  }

Npc* Npc::updateNearestBody() {
  if(aiPolicy!=NpcProcessPolicy::AiNormal)
    return nullptr;

  Npc*  ret  = nullptr;
  float dist = std::numeric_limits<float>::max();

  owner.detectNpcNear([this,&ret,&dist](Npc& n){
    if(!n.isDead())
      return;

    float d = qDistTo(n);
    if(d<dist && canSenseNpc(n,true)!=SensesBit::SENSE_NONE) {
      ret  = &n;
      dist = d;
      }
    });
  return ret;
  }

void Npc::tickTimedEvt(Animation::EvCount& ev) {
  if(ev.timed.empty())
    return;

  std::sort(ev.timed.begin(),ev.timed.end(),[](const Animation::EvTimed& a,const Animation::EvTimed& b){
    return a.time<b.time;
    });

  // https://auronen.cokoliv.eu/gmc/zengin/anims/events/
  for(auto& i:ev.timed) {
    switch(i.def) {
      case asset::MdsEventType::ITEM_CREATE: {
        if(auto it = invent.addItem(i.item,1,world())) {
          invent.putToSlot(*this,it->clsId(),i.slot[0]);
          }
        break;
        }
      case asset::MdsEventType::ITEM_INSERT: {
        invent.putCurrentToSlot(*this,i.slot[0]);
        break;
        }
      case asset::MdsEventType::ITEM_REMOVE:
      case asset::MdsEventType::ITEM_DESTROY: {
        if(i.def==asset::MdsEventType::ITEM_DESTROY && itemUse.item!=0) {
          finishItemUseEffect("destroy");
          break;
          }
        invent.clearSlot(*this, "", i.def != asset::MdsEventType::ITEM_REMOVE);
        break;
        }
      case asset::MdsEventType::ITEM_PLACE: {
        if(currentInteract!=nullptr)
          Inventory::moveItem(*this, invent, *currentInteract);
        break;
        }
      case asset::MdsEventType::ITEM_EXCHANGE: {
        if(!invent.clearSlot(*this,i.slot[0],true)) {
          // fallback for cooking animations
          invent.putCurrentToSlot(*this,i.slot[0]);
          invent.clearSlot(*this,"",true);
          }
        if(auto it = invent.addItem(i.item,1,world())) {
          invent.putToSlot(*this,it->clsId(),i.slot[0]);
          }
        break;
        }
      case asset::MdsEventType::SET_FIGHT_MODE:
        break;
      case asset::MdsEventType::MUNITION_PLACE: {
        auto active=invent.activeWeapon();
        if(active!=nullptr) {
          const int32_t munition = active->handle().munition;
          invent.putAmmunition(*this,uint32_t(munition),i.slot[0]);
          }
        break;
        }
      case asset::MdsEventType::MUNITION_REMOVE: {
        invent.putAmmunition(*this,0,"");
        break;
        }
      case asset::MdsEventType::TORCH_DRAW:
        setTorch(true);
        break;
      case asset::MdsEventType::TORCH_INVENTORY:
        processDefInvTorch();
        break;
      case asset::MdsEventType::TORCH_DROP:
        dropTorch();
        break;
      case asset::MdsEventType::SOUND_DRAW:
        break;
      case asset::MdsEventType::SOUND_UNDRAW:
        break;
      case asset::MdsEventType::MESH_SWAP:
        break;
      case asset::MdsEventType::HIT_LIMB:
        break;
      case asset::MdsEventType::HIT_DIRECTION:
        break;
      case asset::MdsEventType::DAMAGE_MULTIPLIER:
        break;
      case asset::MdsEventType::PARRY_FRAME:
        break;
      case asset::MdsEventType::OPTIMAL_FRAME:
        break;
      case asset::MdsEventType::HIT_END:
        break;
      case asset::MdsEventType::COMBO_WINDOW:
        break;
      case asset::MdsEventType::UNKNOWN:
        break;
      }
    }
  }

void Npc::tickRegen(int32_t& v, const int32_t max, const int32_t chg, const uint64_t dt) {
  uint64_t tick = owner.tickCount();
  if(tick<dt || chg==0)
    return;
  int32_t time0 = int32_t(tick%1000);
  int32_t time1 = time0+int32_t(dt);

  int32_t val0 = (time0*chg)/1000;
  int32_t val1 = (time1*chg)/1000;

  int32_t nextV = std::max(0,std::min(v+val1-val0,max));
  if(v!=nextV) {
    if(&v==&hnpc->attribute[ATR_HITPOINTS] && nextV<v)
      interruptItemUse("regeneration");
    v = nextV;
    // check health, in case of negative chg
    checkHealth(true,false);
    }
  }

void Npc::tickAnimationTags() {
  if(Gothic::physicsFlags().meleeBladeShadow || Gothic::physicsFlags().meleePhysicsHits || (meleeShadow && meleeShadow->window))
    syncMeleeShadowSwing();
  Animation::EvCount ev;
  if(itemUse.item!=0 && !hasItemUseAnimation())
    interruptItemUse("animation interrupted");
  const bool hasEvents = visual.processEvents(owner,lastEventTime,ev,&logicalBody);
  visual.processLayers(owner,&logicalBody);
  visual.setNpcEffect(owner,*this,hnpc->effect,hnpc->flags);
  if(itemUse.item!=0 && !hasItemUseAnimation()) {
    tickTimedEvt(ev);
    ev.timed.clear();
    if(itemUse.item!=0)
      finishItemUseEffect("animation end");
    }
  if(!hasEvents)
    return;

  if(isPlayer() && TestBench::isActive()) {
    // what the hero's animations do, for the test bench (the reload at another tempo: each event once)
    const char* reloadAnim = weaponState()==WeaponState::CBow ? Archery::CBowReload : Archery::BowReload;
    char        buf[256];
    for(auto& i:ev.timed) {
      std::snprintf(buf,sizeof(buf),"anim event %d %.*s %.*s %.*s (reload %.2f)",int(i.def),int(i.item.size()),i.item.data(),
                    int(i.slot[0].size()),i.slot[0].data(),int(i.slot[1].size()),i.slot[1].data(),double(animProgress(reloadAnim)));
      TestBench::note(buf);
      }
    for(auto& i:ev.morph) {
      std::snprintf(buf,sizeof(buf),"anim morph %.*s %.*s (reload %.2f)",int(i.anim.size()),i.anim.data(),int(i.node.size()),i.node.data(),
                    double(animProgress(reloadAnim)));
      TestBench::note(buf);
      }
    }
  for(auto& i:ev.morph)
    visual.startMMAnim(*this,i.anim,i.node);
  if(ev.groundSounds>0 && isPlayer() && bodyStateMasked()!=BodyState::BS_SNEAK)
    world().sendImmediatePerc(*this,*this,*this,PERC_ASSESSQUIETSOUND);
  if(ev.def_opt_frame>0)
    commitDamage();
  implSetFightMode(ev);
  tickTimedEvt(ev);
  }

void Npc::tick(uint64_t dt) {
  TestBench::beforeNpcTick(*this);
  updateQuiver();
  updateSimpleMovement();
  NpcLogicalState::Tick logicalTick(*this,dt);
  if(!bloodPoolDone && isDead() && owner.tickCount()-bloodDeathTick>=BloodSpawn::SettleMs) {
    bloodPoolDone = true;
    BloodSpawn::onDead(owner,*this);
    }
  tickBodyImpact();
  tickFoodPool();
  tickMeleeParry();
  tickMeleeStamina();
  tickMeleeBarge();
  tickMeleeCharge();
  tickMeleeRiposte();
  tickMeleeAiSpecial();
  // Only NPCs inserted in the live world are ticked. A restored NPC starts
  // with fresh shadow geometry, never a sweep from its former holding point.
  meleeShadowSuspended = false;
  headInput.reset();
  if(tickMeleePosture(dt))
    return;
  if(nonlethalTarget!=size_t(-1) && owner.tickCount()>nonlethalUntil) {
    auto* other = owner.findNpcByInstance(nonlethalTarget);
    if(other==nullptr || (!isAttack() && !isDown() && !other->isAttack() && !other->isDown()))
      nonlethalTarget = size_t(-1);
    }
  static bool dbg = false;
  static int  kId = 432;
  if(dbg && !isPlayer() && hnpc->id!=kId)
    return;

  assert(go2.flag!=GoToHint::GT_Enemy && go2.flag!=GoToHint::GT_EnemyG);

  BodyMovementTrace movementTrace(*this,dt);
  traceCombatSnapshot();
  traceVisibilityDiagnostic("npc_pre_tick");
  // A field can lift an actor before its turn in the NPC list. Admit the queued
  // magical state before ordinary airborne/animation waits, independent of tick order.
  if(isInAir() && hasQueuedPhysicalSpellState(aiQueue))
    nextAiAction(aiQueue,dt);
  if(spellFrozen()) {
    // Script time and the real state callbacks keep running even if the queued freeze
    // animation is waiting in mid-air. B_StopMagicFreeze owns queue cleanup and StandUp.
    lastEventTime = owner.tickCount();
    visual.setNpcEffect(owner,*this,hnpc->effect,hnpc->flags);
    mvAlgo.clearSpeed();
    tickRoutine();
    return;
    }
  if(spellWhirling()) {
    // The vortex may keep the actor airborne for the entire state. Its script
    // loop must still reach B_StopWhirlwind while queued animations wait to land.
    lastEventTime = owner.tickCount();
    tickRoutine();
    mvAlgo.tick(dt);
    return;
    }
  tickAnimationTags();
  tickPeeStream();
  visual.updateMouth(*this,dt);
  visual.updateGestures(*this,dt);
  visual.updateBlink(*this,dt);
  visual.updateFace(*this);

  if(!visual.pose().hasAnim())
    setAnim(AnimationSolver::Idle);

  if(meleeAiSpecial==MeleeAi::Special::Charge || meleeAiSpecial==MeleeAi::Special::Barge) {
    if(meleeAiSpecial==MeleeAi::Special::Charge && isMeleeCharging() && currentTarget!=nullptr)
      implTurnToFai(*currentTarget,dt);
    mvAlgo.clearSpeed();
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return;
    }

  if(isMeleeRiposting() || isMeleeRiposteHeld()) {
    mvAlgo.clearSpeed();
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return;
    }

  if(isMeleeStunned()) {
    tickMeleeStunLook();
    waitForMeleeStamina(dt);
    return;
    }

  if(isDive()) {
    uint32_t gl = guild();
    int32_t  v  = world().script().guildVal().dive_time[gl]*1000;
    int32_t  t  = diveTime();
    if(v>=0 && t>v+int(dt)) {
      int tickSz = world().script().npcDamDiveTime();
      if(tickSz>0) {
        t-=v;
        int dmg = t/tickSz - (t-int(dt))/tickSz;
        if(dmg>0) {
          lastHit = nullptr;
          changeAttribute(ATR_HITPOINTS,-dmg,false);
          }
        }
      }
    }

  nextAiAction(aiQueueOverlay,dt);

  if(tickCast(dt))
    return;

  if(!isDead()) {
    tickRegen(hnpc->attribute[ATR_HITPOINTS],hnpc->attribute[ATR_HITPOINTSMAX],
              hnpc->attribute[ATR_REGENERATEHP],dt);
    tickRegen(hnpc->attribute[ATR_MANA],hnpc->attribute[ATR_MANAMAX],
              hnpc->attribute[ATR_REGENERATEMANA],dt);
    }

  if(visual.hasGaze())
    attention.tick(*this,owner);

  // an archer aiming by hand draws, holds and reloads through the fight AI's waits (game/archerai)
  if(!isPlayer())
    archAi.tick(*this,dt);

  if(BodyController::tick(*this,dt))
    return;
  if(NpcKeys::tick(*this,dt))
    return;

  if(isMeleeCharging()) {
    mvAlgo.clearSpeed();
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return;
    }

  const bool physicalMovement = ragdoll ||
      (Gothic::physicsFlags().ragdoll && isDown() && !ragdollLegacyDown && !ragdollRecovering);

  // A nearby impact briefly interrupts idle work, including an AI_Wait. Combat,
  // conversation and sleep immediately regain control if they begin meanwhile.
  if(physicsNoiseUntil>owner.tickCount()) {
    if(Gothic::physicsFlags().physicsNoise && canHearPhysicsNoise()) {
      if(prepareTurn()) {
        const auto direction = physicsNoisePosition-position();
        implTurnTo(direction.x,direction.z,AnimationSolver::TurnType::Std,dt);
        }
      mvAlgo.tick(dt,MoveAlgo::WaitMove);
      return;
      }
    physicsNoiseUntil = 0;
    }

  if(tickPhysicsInvestigation(dt))
    return;

  if(aniWaitTime<owner.tickCount() && aniWaitTime==logicalBody.nativeWaitEnd)
    logicalBody.waitReleased(owner.tickCount());
  if(waitTime>=owner.tickCount() || aniWaitTime>=owner.tickCount() || outWaitTime>owner.tickCount()) {
    traceCombatGate("tick_wait_or_animation_lock");
    if(physicalMovement)
      return;
    if(!isPlayer() && go2.flag!=GT_Flee && faiWaitTime<owner.tickCount() && currentTarget!=nullptr) {
      const bool magnetHeading = visual.pose().isAttackAnim() && meleeShadow && meleeShadow->window && meleeShadow->physicsHits &&
                                 meleeShadow->magnet.eligible;
      if(!magnetHeading)
        implTurnToFai(*currentTarget,dt);
      }
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return;
    }

  if(physicalMovement) {
    // Keep the ordinary AI barriers above; native bodies alone own gravity/root movement.
    implAiTick(dt);
    return;
    }

  if(!isDown()) {
    if(visual.hasGaze()) {
      // the gaze turns the head and the body (gazeTarget): the old turn of the head goes back
      implHeadBack(dt);
      } else {
      implLookAtNpc(dt);
      implLookAtWp(dt);
      }

    if(implAttack(dt))
      return;

    if(implGoTo(dt)) {
      if(go2.flag==GT_Flee)
        implAiTick(dt);
      return;
      }
    }

  mvAlgo.tick(dt);
  implAiTick(dt);
  }

bool Npc::prepareTurn() {
  if(usesSimplifiedMovement()) {
    // A dialogue can queue a heading before its next line. Turn only the
    // angle; its native body layers retain ownership between voiced lines.
    if(nativeBodyAction())
      return Gothic::inst().isNpcInDialog(*this);
    visual.stopWalkAnim(*this);
    setAnimRotate(0);
    if(!visual.pose().hasAnim())
      setAnim(Anim::Idle);
    if(visual.pose().bodyState()==BS_STAND)
      simpleMovement.body = BS_STAND;
    return true;
    }
  const auto st = bodyStateMasked();
  if(interactive()==nullptr && (st==BS_WALK || st==BS_SNEAK)) {
    visual.stopWalkAnim(*this);
    setAnimRotate(0);
    return false;
    }
  if(interactive()==nullptr) {
    visual.stopWalkAnim(*this);
    visual.stopDlgAnim(*this);
    }
  return true;
  }

bool Npc::hasQueuedPhysicalSpellState(const AiQueue& queue) const {
  if(!Gothic::physicsFlags().spellPhysics)
    return false;
  const auto* action = queue.front();
  if(action==nullptr || action->act!=AI_StartState)
    return false;
  const auto* symbol = owner.script().findSymbol(action->func.ptr);
  return symbol!=nullptr && (symbol->name()=="ZS_MAGICFREEZE" || symbol->name()=="ZS_WHIRLWIND");
  }

void Npc::setTarget(Npc *t) {
  if(!BodyController::owns(*this) && currentTarget!=t)
    BodyController::takeover(*this,"target_changed");
  if(BodyController::owns(*this) && currentTarget!=t) {
    const auto bodyTargetId=t ? BodyObservations::entityId(*t) : 0;
    BodyController::deferNative(*this,[this,bodyTargetId] { setTarget(BodyObservations::findEntity(bodyTargetId)); });
    return;
    }
  if(t!=nullptr)
    stopPhysicsInvestigation(false);
  if(currentTarget==t)
    return;

  currentTarget = t;
  if(!go2.empty() && !isPlayer())
    clearGoTo();
  }

Npc *Npc::target() const {
  return currentTarget;
  }

void Npc::clearNearestEnemy() {
  nearestEnemy = nullptr;
  }

void Npc::setOther(Npc *ot) {
  if(isTalk() && ot && !ot->isPlayer())
    Log::e("unxepected perc acton");
  currentOther = ot;
  }

void Npc::setVictim(Npc* ot) {
  currentVictim = ot;
  }

bool Npc::haveOutput() const {
  if(owner.tickCount()<aiOutputBarrier)
    return true;
  return aiOutputOrderId()!=std::numeric_limits<int>::max();
  }

uint64_t Npc::speechLeft() const {
  const uint64_t now = owner.tickCount();
  return aiOutputBarrier>now ? aiOutputBarrier-now : 0;
  }

bool Npc::hasOutputQueued() const {
  return aiOutputOrderId()!=std::numeric_limits<int>::max();
  }

bool Npc::isPlayingQueuedAnim() const {
  return aniWaitTime>=owner.tickCount();
  }

void Npc::setAiOutputBarrier(uint64_t dt, bool overlay) {
  aiOutputBarrier = owner.tickCount()+dt;
  if(!overlay)
    outWaitTime = aiOutputBarrier;
  }

void Npc::emitSoundEffect(std::string_view sound, float range, bool freeSlot) {
  if(isPlayer() && TestBench::isActive()) {
    char buf[128];
    std::snprintf(buf,sizeof(buf),"sound %.*s",int(sound.size()),sound.data());
    TestBench::note(buf);
    }
  auto sfx = ::Sound(owner,::Sound::T_Regular,sound,centerPosition(),range,freeSlot);
  sfx.play();
  }

void Npc::emitSoundGround(std::string_view sound, float range, bool freeSlot) {
  auto mat = mvAlgo.groundMaterial();
  string_frm buf(sound,"_",MaterialGroupNames[uint8_t(mat)]);
  auto sfx = ::Sound(owner,::Sound::T_Regular,buf,{x,y,z},range,freeSlot);
  sfx.play();
  }

void Npc::emitSoundSVM(std::string_view svm) {
  if(hnpc->voice==0)
    return;
  char frm [32]={};
  std::snprintf(frm,sizeof(frm),"%.*s",int(svm.size()),svm.data());

  char name[32]={};
  std::snprintf(name,sizeof(name),frm,int(hnpc->voice));
  emitSoundEffect(name,2500,true);
  }

void Npc::tickPeeStream() {
  // a man relieving himself in front of the kneeling hero (mercy.py's humiliation): the game's own stream
  // (PEE of S_PEE) is aimed at him. Not bound to a bone, whose axes turn with the pose: the emitter stands
  // at the pelvis and faces where the npc faces
  const auto* hero   = owner.player();
  const bool  peeing = visual.pose().isInAnim("S_PEE") || visual.pose().isInAnim("T_STAND_2_PEE");
  bool        aim    = false;
  if(peeing && hero!=nullptr && hero!=this && hero->playerKneeling && !isDown() && !isDead()) {
    const float dx = hero->x-x, dz = hero->z-z;
    const float d  = std::sqrt(dx*dx+dz*dz);
    const float da = std::fmod(angleDir(dx,dz)-rotation()+540.f,360.f)-180.f;
    aim = d<200.f && std::abs(da)<45.f;
    }
  visual.setMutedPfx(aim ? "PEE" : "");
  const bool on = aim && visual.pose().isInAnim("S_PEE");
  if(on!=bool(peeStream)) {
    peeStream.reset();
    if(on) {
      peeStream = std::make_unique<Effect>(PfxEmitter(owner,"PFX_GAI_PEE"),"");
      peeStream->setActive(true);
      peeStream->setLooped(true);
      }
    }
  if(!peeStream)
    return;
  Matrix4x4  mt = transform();
  const Vec3 at = mapBone("BIP01 PELVIS");
  mt.set(3,0,at.x);
  mt.set(3,1,at.y);
  mt.set(3,2,at.z);
  peeStream->setObjMatrix(mt);
  }

void Npc::startEffect(Npc& to, const VisualFx& vfx) {
  Effect e(vfx,owner,*this,SpellFxKey::Cast);
  e.setActive(true);
  e.setTarget(&to);
  visual.startEffect(owner, std::move(e), 0, true);
  }

void Npc::stopEffect(const VisualFx& vfx) {
  visual.stopEffect(vfx);
  }

void Npc::runEffect(Effect&& e) {
  visual.startEffect(owner, std::move(e), 0, true);
  }

bool Npc::isTargetableBySpell(TargetType t) const {
  if(bool(t&(TARGET_TYPE_ALL|TARGET_TYPE_NPCS)))
    return true;

  const Guild gil = Guild(trueGuild());

  const auto G2_UNDEAD = (gil == GIL_GOBBO_SKELETON ||
    gil == GIL_SUMMONED_GOBBO_SKELETON || gil == GIL_SKELETON      ||
    gil == GIL_SUMMONED_SKELETON       || gil == GIL_SKELETON_MAGE ||
    gil == GIL_SHADOWBEAST_SKELETON    || gil == GIL_ZOMBIE);

  if(bool(t&TARGET_TYPE_HUMANS) && isHuman())
    return true;
  if(bool(t&TARGET_TYPE_ORCS) && gil>GIL_SEPERATOR_ORC)
    return true;
  if(bool(t&TARGET_TYPE_UNDEAD) && G2_UNDEAD)
    return true;

  return false;
  }

void Npc::commitSpell() {
  auto active = invent.getItem(currentSpellCast);
  if(active==nullptr || !active->isSpellOrRune())
    return;

  const int32_t splId = active->spellId();
  const auto&   spl   = owner.script().spellDesc(splId);

  owner.script().invokeSpell(*this,currentTarget,*active);

  if(active->isSpellShoot()) {
    const int lvl = (castLevel-CS_Emit_0)+1;
    DamageCalculator::Damage dmg={};
    for(size_t i=0; i<daedalus::DamageType::NUM; ++i)
      if((spl.damage_type&(1<<i))!=0) {
        dmg[i] = spl.damage_per_level*lvl;
        }

    auto& b = owner.shootSpell(*active, *this, currentTarget);
    b.setDamage(dmg);
    b.setOrigin(this);
    b.setTarget(nullptr);
    visual.setMagicWeaponKey(owner,SpellFxKey::Init);
    } else {
    // NOTE: use pfx_ppsIsLoopingChg ?
    const VisualFx* vfx = owner.script().spellVfx(splId);
    if(vfx!=nullptr) {
      auto e = Effect(*vfx,owner,Vec3(x,y,z),SpellFxKey::Cast);
      e.setOrigin(this);
      e.setTarget((currentTarget==nullptr) ? this : currentTarget);
      e.setSpellId(splId,owner);
      e.setActive(true);
      visual.startEffect(owner,std::move(e),0,true);
      }
    visual.setMagicWeaponKey(owner,SpellFxKey::Init);
    if(currentTarget!=nullptr) {
      currentTarget->lastHitSpell = splId;
      currentTarget->perceptionProcess(*this,nullptr,0,PERC_ASSESSMAGIC);
      }
    if(owner.script().physicalSpell(splId)==GameScript::PhysicalSpell::Whirlwind)
      owner.startSpellField(splId,currentTarget!=nullptr ? currentTarget->position() : position(),this,currentTarget);
    }

  if(active->isSpell()) {
    size_t cnt = active->count();
    invent.delItem(active->clsId(),1,*this);
    if(cnt<=1) {
      Item* spl = nullptr;
      for(uint8_t i=0;i<8;++i) {
        if(auto s = invent.currentSpell(i)) {
          spl = s;
          break;
          }
        }
      if(spl==nullptr) {
        if(spellInfo==0)
          aiPush(AiQueue::aiRemoveWeapon());
        } else {
        drawSpell(spl->spellId());
        }
      }
    }

  if(spellInfo!=0 && transformInto(size_t(spellInfo)))
    spellInfo = 0;
  }

bool Npc::transformInto(size_t instance) {
  if(transformSpl!=nullptr)
    return false;
  transformSpl.reset(new TransformBack(*this));
  invent.updateView(*this);
  visual.clearOverlays();

  owner.script().initializeInstanceNpc(hnpc, instance);
  hnpc->level = transformSpl->hnpc->level;
  return true;
  }

BodyState Npc::bodyState() const {
  if(isDead())
    return BS_DEAD;
  if(isUnconscious())
    return BS_UNCONSCIOUS;
  if(isFalling())
    return BS_FALL;

  uint32_t s = visual.pose().bodyState();
  if(const auto simple = simplifiedBodyState())
    s = uint32_t(*simple);
  if(auto i = interactive())
    s = i->stateMask();
  return BodyState(s);
  }

BodyState Npc::bodyStateMasked() const {
  BodyState bs = bodyState();
  return BodyState(bs & (BS_MAX | BS_FLAG_MASK));
  }

bool Npc::hasState(BodyState s) const {
  if(const auto simple = simplifiedBodyState()) {
    if(s==BS_STAND || s==BS_WALK || s==BS_RUN || s==BS_SNEAK)
      return s==*simple;
    }
  if(visual.pose().hasState(s))
    return true;
  if(auto i = interactive())
    return s==i->stateMask();
  return false;
  }

bool Npc::hasStateFlag(BodyState flg) const {
  if(const auto simple = simplifiedBodyState()) {
    if((*simple&(BS_FLAG_MASK|BS_MOD_MASK))==flg)
      return true;
    for(size_t i=0;i<visual.pose().logicalLayerCount();++i) {
      const auto layer = visual.pose().logicalLayer(i);
      const auto state = BodyState(layer.state&(BS_MAX|BS_FLAG_MASK));
      if(state==BS_STAND || state==BS_WALK || state==BS_RUN || state==BS_SNEAK)
        continue;
      if((layer.state&(BS_FLAG_MASK|BS_MOD_MASK))==flg)
        return true;
      }
    } else if(visual.pose().hasStateFlag(flg)) {
      return true;
      }
  if(auto i = interactive())
    return flg==(i->stateMask() & (BS_FLAG_MASK|BS_MOD_MASK));
  return false;
  }

bool Npc::spellWhirling() const {
  if(!Gothic::physicsFlags().spellPhysics || !aiState.funcIni.isValid() || isDown())
    return false;
  constexpr std::string_view whirlwind="ZS_WHIRLWIND";
  const std::string_view state=aiState.hint;
  if(state.size()!=whirlwind.size())
    return false;
  for(size_t i=0;i<whirlwind.size();++i)
    if(std::toupper(static_cast<unsigned char>(state[i]))!=whirlwind[i])
      return false;
  return true;
  }

bool Npc::spellFrozen() const {
  if(!Gothic::physicsFlags().spellPhysics || !aiState.funcIni.isValid() || isDown())
    return false;
  constexpr std::string_view frozen="ZS_MAGICFREEZE";
  const std::string_view state=aiState.hint;
  if(state.size()!=frozen.size())
    return false;
  for(size_t i=0; i<frozen.size(); ++i)
    if(std::toupper(static_cast<unsigned char>(state[i]))!=frozen[i])
      return false;
  return true;
  }

void Npc::setToFightMode(const size_t item) {
  if(invent.itemCount(item)==0)
    addItem(item,1);

  invent.equip(item,*this,true);
  invent.switchActiveWeapon(*this,1);

  auto w = invent.currentMeleeWeapon();
  if(w==nullptr || w->clsId()!=item)
    return;

  auto weaponSt = WeaponState::W1H;
  if(w->is2H()) {
    weaponSt = WeaponState::W2H;
    } else {
    weaponSt = WeaponState::W1H;
    }

  if(visual.setToFightMode(weaponSt))
    updateWeaponSkeleton();

  auto& weapon = *currentMeleeWeapon();
  auto  st     = weapon.is2H() ? WeaponState::W2H : WeaponState::W1H;
  hnpc->weapon  = (st==WeaponState::W1H ? 3:4);
  }

void Npc::setToFistMode() {
  auto weaponSt=weaponState();
  if(weaponSt==WeaponState::Fist)
    return;
  invent.switchActiveWeaponFist();
  if(visual.setToFightMode(WeaponState::Fist))
    updateWeaponSkeleton();
  hnpc->weapon  = 1;
  }

void Npc::setToBowMode(const size_t item) {
  // The ranged counterpart of setToFightMode: draw without the draw animation.
  if(invent.itemCount(item)==0)
    addItem(item,1);
  invent.equip(item,*this,true);
  invent.switchActiveWeapon(*this,2);
  auto w = invent.currentRangedWeapon();
  if(w==nullptr || w->clsId()!=item)
    return;
  const auto st = w->isCrossbow() ? WeaponState::CBow : WeaponState::Bow;
  if(visual.setToFightMode(st))
    updateWeaponSkeleton();
  hnpc->weapon = (st==WeaponState::Bow ? 5:6);
  }

void Npc::aiPush(AiQueue::AiAction&& a) {
  // Vanilla quiet-sound scripts aim at OTHER. A physical sound is located at the
  // collision, and OTHER may be null or a distant culprit, so native turning owns it.
  if(physicsNoiseCallback && (a.act==AI_TurnToNpc || a.act==AI_LookAtNpc))
    return;
  a.physicsNoiseState = physicsNoiseCallback && a.act==AI_StartState;
  a.nativeCombatTarget=nativeCombatQueueTarget;
  if(BodyController::owns(*this) && a.act!=AI_OutputSvmOverlay)
    BodyController::takeover(*this,"native_script_queue");
  if(a.act==AI_OutputSvmOverlay)
    aiQueueOverlay.pushBack(std::move(a)); else
    aiQueue.pushBack(std::move(a));
  }

Vec3 Npc::mapWeaponBone() const {
  syncDeferredAnimation();
  return visual.mapWeaponBone();
  }

Vec3 Npc::mapHeadBone() const {
  syncDeferredAnimation();
  return visual.mapHeadBone();
  }

Npc::EyePose Npc::navigationEyePose() const {
  syncDeferredAnimation();
  EyePose result;
  const auto& model = visual.transform();
  result.origin = Vec3(model.at(3,0),model.at(3,1)+180.f,model.at(3,2));
  result.forward = Vec3::normalize(Vec3(model.at(2,0),model.at(2,1),model.at(2,2)));
  if(result.forward.length()<0.5f)
    result.forward = Vec3(std::cos(rotationRad()),0,std::sin(rotationRad()));
  const auto* skeleton = visual.visualSkeleton();
  const auto axis = [](const Tempest::Matrix4x4& m, size_t k) {
    const int column = int(k);
    return Vec3::normalize(Vec3(m.at(column,0),m.at(column,1),m.at(column,2)));
    };
  if(skeleton!=nullptr && skeleton->BIP01_HEAD<visual.pose().boneCount() &&
     skeleton->BIP01_HEAD<skeleton->tr.size()) {
    const size_t head = skeleton->BIP01_HEAD;
    const auto& bind = skeleton->tr[head];
    Vec3 bindForward(0,0,1);
    const size_t left = skeleton->findNode("BIP01 L THIGH");
    const size_t right = skeleton->findNode("BIP01 R THIGH");
    if(left<skeleton->tr.size() && right<skeleton->tr.size()) {
      const auto& l = skeleton->tr[left];
      const auto& r = skeleton->tr[right];
      const Vec3 hips = Vec3::crossProduct(Vec3(0,1,0),Vec3(l.at(3,0)-r.at(3,0),0,l.at(3,2)-r.at(3,2)));
      if(hips.length()>1.e-3f)
        bindForward = Vec3::normalize(hips);
      }
    // The same bind-pose face-axis choice used by Gaze; apply it to the finished head.
    size_t forwardAxis = 0, upAxis = 0;
    float forwardDot = 0, upDot = 0;
    for(size_t k=0; k<3; ++k) {
      const auto a = axis(bind,k);
      const float f = Vec3::dotProduct(a,bindForward);
      if(std::abs(f)>std::abs(forwardDot)) {
        forwardAxis = k;
        forwardDot = f;
        }
      if(std::abs(a.y)>std::abs(upDot)) {
        upAxis = k;
        upDot = a.y;
        }
      }
    const auto& shown = visual.renderedBone(head);
    result.origin = Vec3(shown.at(3,0),shown.at(3,1),shown.at(3,2));
    result.forward = axis(shown,forwardAxis)*(forwardDot<0.f ? -1.f : 1.f);
    result.up = axis(shown,upAxis)*(upDot<0.f ? -1.f : 1.f);
    result.animatedHead = true;
    }
  result.right = Vec3::crossProduct(result.forward,result.up);
  if(result.right.length()<1.e-3f)
    result.right = Vec3::crossProduct(result.forward,Vec3(0,0,1));
  result.right = Vec3::normalize(result.right);
  result.up = Vec3::normalize(Vec3::crossProduct(result.right,result.forward));
  return result;
  }

void Npc::setBloodHit(int64_t gameMs, const Vec3& wound) {
  syncDeferredAnimation();
  bloodHit  = gameMs;
  bloodBone = size_t(-1);
  auto sk = visual.visualSkeleton();
  if(sk==nullptr)
    return;
  float best = 0;
  for(size_t i=0; i<sk->nodes.size(); ++i) {
    const float d = (visual.mapBone(i)-wound).quadLength();
    if(bloodBone==size_t(-1) || d<best) {
      best      = d;
      bloodBone = i;
      }
    }
  }

Vec3 Npc::bloodWound() const {
  syncDeferredAnimation();
  auto sk = visual.visualSkeleton();
  if(sk==nullptr || bloodBone>=sk->nodes.size())
    return centerPosition();
  return visual.mapBone(bloodBone);
  }

Vec3 Npc::mapBone(std::string_view bone) const {
  syncDeferredAnimation();
  if(auto sk = visual.visualSkeleton()) {
    size_t id = sk->findNode(bone);
    if(id!=size_t(-1))
      return visual.mapBone(id);
    }

  Vec3 ret = {};
  ret.y = physic.centerY()-y;
  return ret+position();
  }

bool Npc::turnTo(float dx, float dz, bool noAnim, uint64_t dt) {
  return implTurnTo(dx,dz,noAnim?AnimationSolver::TurnType::None:AnimationSolver::TurnType::Std,dt);
  }

bool Npc::rotateTo(float dx, float dz, float step, AnimationSolver::TurnType anim, uint64_t dt) {
  //step *= (float(dt)/1000.f)*60.f/100.f;
  step *= (float(dt)/1000.f);

  if(dx==0.f && dz==0.f) {
    if(!usesSimplifiedMovement() || !nativeBodyAction())
      setAnimRotate(0);
    return false;
    }

  if(!isRotationAllowed())
    return false;

  if(usesSimplifiedMovement()) {
    const float target = angleDir(dx,dz);
    const float delta = std::remainder(target-angle,360.f);
    if(!nativeBodyAction())
      setAnimRotate(0);
    if(std::abs(delta)<=step*2.f) {
      setDirection(target);
      return false;
      }
    setDirection(angle+std::clamp(delta,-step,step));
    return true;
    }

  float a  = angleDir(dx,dz);
  float da = a-angle;

  if(anim == AnimationSolver::TurnType::None || std::cos(double(da)*M_PI/180.0)>0) {
    if(float(std::abs(int(da)%360))<=(step*2.f)) {
      setAnimRotate(0);
      setDirection(a);
      return false;
      }
    } else {
    visual.stopWalkAnim(*this);
    }

  const auto sgn = std::sin(double(da)*M_PI/180.0);
  if(sgn==0) {
    setAnimRotate(0);
    } else {
    const int rot = (sgn<0) ? +1 : -1;
    switch(anim) {
      case AnimationSolver::TurnType::Std:
        setAnimRotate(rot);
        break;
      case AnimationSolver::TurnType::None:
        setAnimRotate(0);
        break;
      case AnimationSolver::TurnType::Whirl:
        visual.setAnimWhirl(*this, rot);
        break;
      }
    setDirection(angle - float(rot)*step);
    }
  return true;
  }

bool Npc::isRotationAllowed() const {
  auto bs  = bodyStateMasked();
  bool air = (!isPlayer() && isInAir()) || isFallingDeep();
  return currentInteract==nullptr && !isFinishingMove() && bs!=BS_CLIMB && bs!=BS_LIE && !air;
  }

bool Npc::checkGoToNpcdistance(const Npc &other) {
  return fghAlgo.isInAttackRange(*this,other,owner.script());
  }

bool Npc::canSwitchWeapon() const {
  if(isUnconscious())
    return false;
  if(isMeleeCharging())
    return true;
  auto bs = bodyStateMasked();
  if(bs==BS_STAND || bs==BS_WALK || bs==BS_RUN || bs==BS_SNEAK || bs==BS_NONE)
    return true;
  return false;
  // return !(mvAlgo.isFalling() || mvAlgo.isInAir() || mvAlgo.isSlide() || mvAlgo.isSwim());
  }

bool Npc::closeWeapon(bool noAnim) {
  cancelMeleeCharge("weapon sheathed");
  auto weaponSt=weaponState();
  if(weaponSt==WeaponState::NoWeapon)
    return true;
  if(!noAnim && !visual.startAnim(*this,WeaponState::NoWeapon))
    return false;
  if(isUsingItemInHand())
    interruptItemUse("weapon sheathed");
  visual.setAnimRotate(*this,0);
  if(isPlayer())
    setTarget(nullptr);
  invent.switchActiveWeapon(*this,Item::NSLOT);
  invent.putAmmunition(*this,0,"");
  if(noAnim) {
    visual.setToFightMode(WeaponState::NoWeapon);
    updateWeaponSkeleton();
    }
  hnpc->weapon      = 0;
  // clear spell-cast state
  castLevel        = CS_NoCast;
  currentSpellCast = size_t(-1);
  castNextTime     = 0;
  if(isPlayer())
    owner.sendPassivePerc(*this,*this,PERC_ASSESSREMOVEWEAPON);
  return true;
  }

bool Npc::drawWeaponFist() {
  if(!canSwitchWeapon())
    return false;
  auto weaponSt=weaponState();
  if(weaponSt==WeaponState::Fist)
    return true;
  if(weaponSt!=WeaponState::NoWeapon) {
    closeWeapon(false);
    return false;
    }

  if(isMonster()) {
    if(!visual.startAnim(*this,WeaponState::Fist))
      visual.setToFightMode(WeaponState::Fist);
    } else {
    if(!visual.startAnim(*this,WeaponState::Fist))
      return false;
    }

  invent.switchActiveWeaponFist();
  hnpc->weapon = 1;
  emitDrawWeapon();
  return true;
  }

bool Npc::drawWeaponMelee() {
  if(!canSwitchWeapon())
    return false;
  auto weaponSt=weaponState();
  if(weaponSt==WeaponState::Fist || weaponSt==WeaponState::W1H || weaponSt==WeaponState::W2H)
    return true;
  if(invent.currentMeleeWeapon()==nullptr)
    return drawWeaponFist();
  if(weaponSt!=WeaponState::NoWeapon) {
    closeWeapon(false);
    return false;
    }

  if(!setInteraction(nullptr,true))
    return false;

  auto& weapon = *invent.currentMeleeWeapon();
  auto  st     = weapon.is2H() ? WeaponState::W2H : WeaponState::W1H;
  if(!visual.startAnim(*this,st))
    return false;

  invent.switchActiveWeapon(*this,1);
  hnpc->weapon = (st==WeaponState::W1H ? 3:4);
  emitDrawWeapon();
  return true;
  }

bool Npc::drawWeaponBow() {
  if(!canSwitchWeapon())
    return false;
  auto weaponSt=weaponState();
  if(weaponSt==WeaponState::Bow || weaponSt==WeaponState::CBow || invent.currentRangedWeapon()==nullptr)
    return true;
  if(weaponSt!=WeaponState::NoWeapon) {
    closeWeapon(false);
    return false;
    }

  if(!setInteraction(nullptr,true))
    return false;

  auto& weapon = *invent.currentRangedWeapon();
  auto  st     = weapon.isCrossbow() ? WeaponState::CBow : WeaponState::Bow;
  if(!visual.startAnim(*this,st))
    return false;
  invent.switchActiveWeapon(*this,2);
  hnpc->weapon = (st==WeaponState::Bow ? 5:6);
  emitDrawWeapon();
  return true;
  }

bool Npc::drawMage(uint8_t slot) {
  if(!canSwitchWeapon())
    return false;
  Item* it = invent.currentSpell(uint8_t(slot-3));
  if(it==nullptr) {
    closeWeapon(false);
    return true;
    }
  return drawSpell(it->spellId());
  }

bool Npc::drawSpell(int32_t spell) {
  if(mvAlgo.isFalling() || mvAlgo.isSwim() || bodyStateMasked()==BS_CASTING)
    return false;
  auto weaponSt=weaponState();
  if(weaponSt!=WeaponState::NoWeapon && weaponSt!=WeaponState::Mage) {
    closeWeapon(false);
    return false;
    }

  if(!setInteraction(nullptr,true))
    return false;

  if(!visual.startAnim(*this,WeaponState::Mage))
    return false;

  invent.switchActiveSpell(spell,*this);
  hnpc->weapon = 7;

  updateWeaponSkeleton();
  if(weaponSt==WeaponState::NoWeapon)
    emitDrawWeapon();
  return true;
  }

void Npc::emitDrawWeapon() {
  // as in the original game: npc's around react to the player drawing a weapon (B_AssessDrawWeapon)
  if(isPlayer())
    owner.sendPassivePerc(*this,*this,PERC_DRAWWEAPON);
  }

WeaponState Npc::weaponState() const {
  return visual.fightMode();
  }

bool Npc::canFinish(Npc& oth) {
  if(isNonlethalFight(oth))
    return false;
  auto ws = weaponState();
  if(ws!=WeaponState::W1H && ws!=WeaponState::W2H)
    return false;

  if(!oth.isUnconscious())
    return false;

  if(!fghAlgo.isInFinishRange(*this,oth,owner.script()))
    return false;
  return true;
  }

bool Npc::doAttack(Anim anim, BodyState bs) {
  if(isMeleeCharging())
    return false;
  if(meleeKnockdown)
    return false;
  if(!allowMeleeStunAction(bs==BS_PARADE ? "parade" : "attack"))
    return false;
  if(bs==BS_PARADE && Gothic::physicsFlags().meleeParry && !allowStaminaAction("parade",0.f))
    return false;
  const auto weaponSt = weaponState();
  const bool costsStamina = hasStamina() && bs==BS_HIT &&
                           (weaponSt==WeaponState::Fist || weaponSt==WeaponState::W1H || weaponSt==WeaponState::W2H) &&
                           !(anim==Anim::AttackFinish && currentTarget!=nullptr && canFinish(*currentTarget));
  if(costsStamina && !allowStaminaAction("attack",meleeAttackCost()))
    return false;
  // Pose accepts a repeated request for an already playing layer too. Only a new
  // layer or combo segment pays, never a repeated input within the same swing.
  const auto staminaBefore = costsStamina ? meleeStaminaIdentity(visual.pose(),owner.tickCount()) : std::string();
  const bool trace = TestBench::bodyTracing() && currentTarget!=nullptr;
  const auto before = trace ? combatPoseIdentity() : std::string();
  const auto result = [&](bool accepted) {
    if(accepted && isUsingItemInHand())
      interruptItemUse(bs==BS_PARADE ? "parade" : "attack");
    if(accepted && (bs==BS_HIT || bs==BS_PARADE))
      meleeAiDodgeStreak.reset();
    if(accepted && costsStamina && staminaBefore!=meleeStaminaIdentity(visual.pose(),owner.tickCount()))
      spendStamina("attack",meleeAttackCost());
    if(accepted && bs==BS_PARADE)
      tickMeleeParry();
    BodyObservations::noteEvent(*this,currentTarget,accepted ? "animation_accepted" : "animation_rejected",owner.tickCount());
    if(trace)
      traceCombatAttempt(anim,accepted,before,"do_attack");
    return accepted;
    };
  if(weaponSt==WeaponState::NoWeapon || weaponSt==WeaponState::Mage)
    return result(false);

  if(mvAlgo.isSwim())
    return result(false);

  if(bs==BS_PARADE && hasState(BS_PARADE))
    return result(false);

  const auto wlk = walkMode();
  visual.setAnimRotate(*this,0);
  // A blow at one lying in a knockdown is the original's chop from above (T_1HSFINISH,
  // T_2HSFINISH): the swings at standing height pass over a body on the ground. The blade
  // decides the hit and the damage as for any blow; only the unconscious are finished off.
  if(isMeleeDownStrike(anim,weaponSt))
    if(visual.continueCombo(*this,Anim::AttackFinish,bs,weaponSt,wlk)!=nullptr) {
      meleeDownStrike = true;
      return result(true);
      }
  if(auto sq = visual.continueCombo(*this,anim,bs,weaponSt,wlk)) {
    (void)sq;
    // implAniWait(uint64_t(sq->atkTotalTime(visual.comboLength())+1));
    return result(true);
    }
  return result(false);
  }

void Npc::fistShoot() {
  doAttack(Anim::Attack,BS_HIT);
  }

bool Npc::blockFist() {
  if(meleeKnockdown)
    return false;
  auto weaponSt=weaponState();
  if(weaponSt!=WeaponState::Fist)
    return false;
  visual.setAnimRotate(*this,0);
  return setAnim(Anim::AttackBlock);
  }

bool Npc::isFinishSequence(const Animation::Sequence& seq) {
  return seq.name=="T_1HSFINISH" || seq.name=="T_2HSFINISH";
  }

bool Npc::isMeleeDownStrike(Anim anim, WeaponState ws) const {
  // lying, not yet rising: one who gets up is met by the ordinary blows at his height
  if(!Gothic::physicsFlags().meleePhysicsHits || currentTarget==nullptr || !currentTarget->meleeKnockdown ||
     currentTarget->meleeRiseTick!=0)
    return false;
  if(ws!=WeaponState::W1H && ws!=WeaponState::W2H)
    return false;
  if(anim!=Anim::Attack && anim!=Anim::AttackL && anim!=Anim::AttackR)
    return false;
  return fghAlgo.isInFinishRange(*this,*currentTarget,owner.script());
  }

bool Npc::finishingMove() {
  if(currentTarget==nullptr || !canFinish(*currentTarget))
    return false;

  if(doAttack(Anim::AttackFinish,BS_HIT)) {
    currentTarget->hnpc->attribute[ATR_HITPOINTS] = 0;
    currentTarget->checkHealth(true,false);
    owner.sendPassivePerc(*this,*this,*currentTarget,PERC_ASSESSMURDER);
    return true;
    }
  return false;
  }

void Npc::swingSword() {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return;
  doAttack(Anim::Attack,BS_HIT);
  }

bool Npc::swingSwordL() {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return false;
  return doAttack(Anim::AttackL,BS_HIT);
  }

bool Npc::swingSwordR() {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return false;
  return doAttack(Anim::AttackR,BS_HIT);
  }

bool Npc::blockSword() {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return false;
  return doAttack(Anim::AttackBlock,BS_PARADE);
  // return setAnimAngGet(Anim::AttackBlock,calcAniComb())!=nullptr;
  }

Npc::BeginCastResult Npc::beginCastSpell() {
  if(castLevel!=CS_NoCast)
    return BeginCastResult::BC_No;

  auto bs = bodyStateMasked();
  if(bs!=BS_STAND)
    return BeginCastResult::BC_No;

  auto active=invent.activeWeapon();
  if(active==nullptr)
    return BeginCastResult::BC_No;

  setAnimRotate(0);
  if(attribute(ATR_MANA)<=0) {
    setAnim(Anim::MagNoMana);
    return BeginCastResult::BC_NoMana;
    }

  // castLevel        = CS_Invest_0;
  currentSpellCast = active->clsId();
  castNextTime     = owner.tickCount();
  hnpc->aivar[88]  = 0; // HACK: clear AIV_SpellLevel
  manaInvested     = 0;

  const SpellCode code = SpellCode(owner.script().invokeMana(*this,currentTarget,manaInvested));
  switch(code) {
    case SPL_SENDSTOP:
    case SPL_DONTINVEST:
      setAnim(Anim::MagNoMana);
      castLevel        = CS_NoCast;
      currentSpellCast = size_t(-1);
      castNextTime     = 0;
      return BeginCastResult::BC_NoMana;
    case SPL_STATUS_CANINVEST_NO_MANADEC:
    case SPL_RECEIVEINVEST:
    case SPL_NEXTLEVEL: {
      ++manaInvested;
      auto ani = owner.script().spellCastAnim(*this,*active);
      if(!visual.startAnimSpell(*this,ani,true))
        Log::d("Couldn't start animation for spell '",currentSpellCast,"'");
      castLevel = CS_Invest_0;
      return BeginCastResult::BC_Invest;
      }
    case SPL_SENDCAST: {
      castLevel = CS_Cast_0;
      return BeginCastResult::BC_Cast;
      }
    default:
      Log::d("unexpected Spell_ProcessMana result: '",int(code),"' for spell '",currentSpellCast,"'");
      endCastSpell();
      return BeginCastResult::BC_No;
    }

  return BeginCastResult::BC_No;
  }

bool Npc::tickCast(uint64_t dt) {
  if(castLevel==CS_NoCast)
    return false;

  auto active = currentSpellCast!=size_t(-1) ? invent.getItem(currentSpellCast) : nullptr;

  if(currentSpellCast!=size_t(-1)) {
    if(active==nullptr || !active->isSpellOrRune() || isDown()) {
      // canot cast spell
      castLevel        = CS_NoCast;
      currentSpellCast = size_t(-1);
      castNextTime     = 0;
      return true;
      }

    if(!isPlayer() && currentTarget!=nullptr) {
      implTurnTo(*currentTarget,AnimationSolver::TurnType::None,dt);
      }
    }

  if(CS_Cast_0<=castLevel && castLevel<=CS_Cast_Last) {
    // cast anim
    if(active!=nullptr) {
      auto ani = owner.script().spellCastAnim(*this,*active);
      if(!visual.startAnimSpell(*this,ani,false))
        return true;
      }
    castLevel    = CastState(int(castLevel) + int(CS_Emit_0) - int(CS_Cast_0));
    castNextTime = 0;
    return true;
    }

  if((CS_Emit_0<=castLevel && castLevel<=CS_Emit_Last) || castLevel==CS_Finalize) {
    // final commit
    if(!setAnim(Npc::Anim::Idle))
      return true;
    if(castLevel!=CS_Finalize)
      commitSpell();
    castLevel        = CS_NoCast;
    currentSpellCast = size_t(-1);
    castNextTime     = 0;
    spellInfo        = 0;
    return false;
    }

  if(active==nullptr)
    return false;

  if(bodyStateMasked()!=BS_CASTING)
    return true;

  if(owner.tickCount()<castNextTime)
    return true;

  const SpellCode code = SpellCode(owner.script().invokeMana(*this,currentTarget,manaInvested));

  if(!isPlayer() && aiExpectedInvest<=manaInvested) {
    endCastSpell();
    return true;
    }

  switch(code) {
    case SpellCode::SPL_NEXTLEVEL:
    case SpellCode::SPL_RECEIVEINVEST:
    case SpellCode::SPL_STATUS_CANINVEST_NO_MANADEC: {
      if(code==SPL_NEXTLEVEL) {
        int32_t castLvl = int(castLevel)-int(CS_Invest_0);
        if(castLvl<15)
          castLevel = CastState(castLevel+1);
        visual.setMagicWeaponKey(owner,SpellFxKey::Invest,castLvl+1);
        }
      auto& spl = owner.script().spellDesc(active->spellId());
      castNextTime += uint64_t(spl.time_per_mana);
      ++manaInvested;
      return true;
      }
    case SpellCode::SPL_DONTINVEST:
    case SpellCode::SPL_SENDCAST:
    case SpellCode::SPL_SENDSTOP: {
      if(code==SPL_DONTINVEST && isPlayer())
        return true;
      endCastSpell();
      return true;
      }
    default:
      Log::d("unexpected Spell_ProcessMana result: '",int(code),"' for spell '",currentSpellCast,"'");
      return false;
    }
  return true;
  }

void Npc::endCastSpell(bool playerCtrl) {
  if(castLevel<CS_Invest_0 || castLevel>CS_Invest_Last)
    return;
  int32_t castLvl = int(castLevel)-int(CS_Invest_0);
  if(!playerCtrl) {
    castLevel = CastState(castLvl+CS_Cast_0);
    return;
    }
  SpellCode code = SpellCode(owner.script().invokeManaRelease(*this,currentTarget,manaInvested));
  if(code==SpellCode::SPL_SENDCAST)
    castLevel = CastState(castLvl+CS_Cast_0); else
    castLevel = CS_Finalize;
  }

void Npc::setActiveSpellInfo(int32_t info) {
  spellInfo = info;
  }

int32_t Npc::activeSpellLevel() const {
  if(CS_Cast_0<=castLevel && castLevel<=CS_Cast_Last)
    return int(castLevel)-int(CS_Cast_0)+1;
  if(CS_Invest_0<=castLevel && castLevel<=CS_Invest_Last)
    return int(castLevel)-int(CS_Invest_0)+1;
  return 0;
  }

bool Npc::aimBow() {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return false;
  auto bs = bodyStateMasked();
  if(bs!=BS_STAND && bs!=BS_AIMNEAR && bs!=BS_AIMFAR && bs!=BS_HIT) {
    setAnim(Anim::Idle);
    return false;
    }
  // aimed with no arrow (bolt) put in: lowered before the reload put the next one, it is put in now
  const char* reload = weaponState()==WeaponState::CBow ? Archery::CBowReload : Archery::BowReload;
  const bool  empty  = (bs==BS_AIMNEAR || bs==BS_AIMFAR) && !visual.hasAmmoShown() && hasAmmunition() &&
                       animProgress(reload)<0.f;
  if(!setAnim(empty ? Anim::ReloadBow : Anim::AimBow))
    return false;
  visual.setAnimRotate(*this,0);
  return true;
  }

bool Npc::isBowLoaded() const {
  return visual.hasAmmoShown();
  }

bool Npc::shootBow(Interactive* focOverride) {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return false;

  auto bs = bodyStateMasked();
  if(bs!=BS_STAND && bs!=BS_AIMNEAR && bs!=BS_AIMFAR && bs!=BS_HIT) {
    setAnim(Anim::Idle);
    return true;
    }

  const int32_t munition = active->handle().munition;
  if(!hasAmmunition())
    return false;

  if(!setAnim(Anim::Attack))
    return false;

  auto itm = invent.getItem(size_t(munition));
  if(itm==nullptr)
    return false;

  auto& b = owner.shootBullet(*itm,*this,currentTarget,focOverride);
  // the arrow left the hand: the reload places the next one
  visual.dropAmmo();
  if(TestBench::isActive()) {
    const auto by = TestBench::who(this);
    const auto at = TestBench::who(currentTarget);
    char buf[200];
    std::snprintf(buf,sizeof(buf),"dice shot: by %.*s at %.*s",int(by.size()),by.data(),int(at.size()),at.data());
    TestBench::note(buf);
    }

  invent.delItem(size_t(munition),1,*this);
  b.setOrigin(this);
  b.setDamage(DamageCalculator::rangeDamageValue(*this));

  auto rgn = currentRangedWeapon();
  if(rgn!=nullptr && rgn->isCrossbow())
    b.setHitChance(float(hnpc->hitchance[TALENT_CROSSBOW])/100.f); else
    b.setHitChance(float(hnpc->hitchance[TALENT_BOW]     )/100.f);
  return true;
  }

float Npc::animProgress(std::string_view name) const {
  return visual.pose().animProgress(name,owner.tickCount());
  }

void Npc::setManualAim(bool on, const Tempest::Vec3& point, const Tempest::Vec2& sway, const Tempest::Vec2& tremor,
                       const Tempest::Vec3& shake) {
  manualAim    = on;
  manualPoint  = point;
  manualSway   = on ? sway   : Tempest::Vec2();
  manualTremor = on ? tremor : Tempest::Vec2();
  manualShake  = on ? shake  : Tempest::Vec3();
  }

bool Npc::isAimSteady() const {
  const auto bs = bodyStateMasked();
  return (bs==BS_AIMNEAR || bs==BS_AIMFAR) && visual.isAimSteady();
  }

bool Npc::arrowRay(Tempest::Vec3& pos, Tempest::Vec3& dir) const {
  return visual.arrowRay(pos,dir);
  }

bool Npc::tickManualAim(const Tempest::Vec3& at, Archer::Input in, const Archery::Stats& st) {
  // how far the aim turned since the last frame, from the eyes: a turn shakes it (the angle by atan2: acos
  // of a dot near 1 turns the floats' noise into a turn of 0.03 deg a frame)
  const Tempest::Vec3 eye = position() + Tempest::Vec3(0,165,0);
  const Tempest::Vec3 dir = Tempest::Vec3::normalize(at-eye);
  in.turn    = arch.isRaised() ? std::atan2(Tempest::Vec3::crossProduct(dir,aimDirPrev).length(),Tempest::Vec3::dotProduct(dir,aimDirPrev)) : 0.f;
  aimDirPrev = dir;
  // the next arrow as fast as the hands are (dexterity; a crossbow: strength); set when it changes (the
  // test bench may set its own)
  if(st.reload!=reloadTempo) {
    setAnimSpeed(Archery::reloadAnim(st.crossbow),st.reload);
    reloadTempo = st.reload;
    }
  in.raised = true;
  in.ready  = isAimSteady();
  in.reload = animProgress(Archery::reloadAnim(st.crossbow))>=0.f;
  in.loaded = isBowLoaded();
  bool shot = false;
  if(arch.tick(in,st)) {
    const float f = arch.draw();
    shot = releaseArrow(f,st);
    arch.onShot(shot);
    // how hard the bow kicks as it is seen (graphics/mesh/bowdraw): drawn this far, this heavy
    if(shot)
      arch.setRelease(st.crossbow ? 0.f : Archery::releaseKick(f,st.need),Archery::releaseSettle(st.strength));
    }
  setManualAim(true,at,arch.sway(),arch.tremor(),arch.shake());
  return shot;
  }

void Npc::endManualAim(bool idle) {
  setAnimSpeed(Archery::BowReload,1);
  setAnimSpeed(Archery::CBowReload,1);
  reloadTempo = 1;
  if(arch.isRaised()) {
    Archer::Input in;
    in.raised = false;
    arch.tick(in,Archery::stats(*this,currentRangedWeapon()));
    const auto ws = weaponState();
    if(idle && !isDown() && !meleeKnockdown && (ws==WeaponState::Bow || ws==WeaponState::CBow))
      setAnim(Anim::Idle);
    }
  setManualAim(false);
  setAimStance(0,0,0,0);
  setAimLook({});
  }

bool Npc::releaseArrow(float draw, const Archery::Stats& st) {
  auto active = invent.activeWeapon();
  if(active==nullptr || !hasAmmunition())
    return false;
  const auto bs = bodyStateMasked();
  if(bs!=BS_AIMNEAR && bs!=BS_AIMFAR)
    return false;

  // along the arrow as it lies, swaying: read before the shot moves the arms
  Tempest::Vec3 pos, dir;
  if(!visual.arrowRay(pos,dir))
    return false;
  const int32_t munition = active->handle().munition;
  auto          itm      = invent.getItem(size_t(munition));
  if(itm==nullptr || !setAnim(Anim::Attack))
    return false;

  // the release throws it off, up to the cone
  if(st.cone>0.f) {
    auto&               script = owner.script();
    const float         r      = st.cone*std::sqrt(float(script.rand(10000,this))/10000.f);
    const float         a      = 2.f*float(M_PI)*float(script.rand(10000,this))/10000.f;
    Tempest::Vec3       side   = Tempest::Vec3::crossProduct(dir,Tempest::Vec3(0,1,0));
    if(side.length()<1e-3f)
      side = Tempest::Vec3(1,0,0);
    side = Tempest::Vec3::normalize(side);
    const Tempest::Vec3 up     = Tempest::Vec3::crossProduct(side,dir);
    dir = Tempest::Vec3::normalize(dir*std::cos(r) + (side*std::cos(a) + up*std::sin(a))*std::sin(r));
    }

  const float speed = Archery::speedAt(st,draw);
  auto&       b     = owner.shootArrow(*itm,pos,dir,speed);
  visual.dropAmmo();
  if(TestBench::isActive()) {
    const auto by = TestBench::who(this);
    char buf[256];
    std::snprintf(buf,sizeof(buf),"arrow shot: from %.1f %.1f %.1f dir %.5f %.5f %.5f speed %.3f draw %.2f, by %.*s",
                  pos.x,pos.y,pos.z,dir.x,dir.y,dir.z,double(speed),double(draw),int(by.size()),by.data());
    TestBench::note(buf);
    }
  invent.delItem(size_t(munition),1,*this);
  b.setOrigin(this);
  b.setDamage(DamageCalculator::rangeDamageValue(*this));
  b.setPrecise(draw);
  return true;
  }

HitZones::Zone Npc::rayHitZone(const Tempest::Vec3& from, const Tempest::Vec3& to, float& t, size_t* bone) const {
  return visual.rayHitZone(from,to,t,bone);
  }

bool Npc::hitZoneCenter(HitZones::Zone z, Tempest::Vec3& at) const {
  return visual.hitZoneCenter(z,at);
  }

bool Npc::hasAmmunition() const {
  auto active=invent.activeWeapon();
  if(active==nullptr)
    return false;
  const int32_t munition = active->handle().munition;
  if(munition<0 || invent.itemCount(size_t(munition))<=0)
    return false;
  return true;
  }

bool Npc::isEnemy(const Npc &other) const {
  return owner.script().personAttitude(*this,other)==ATT_HOSTILE;
  }

bool Npc::isDead() const {
  return owner.script().isDead(*this);
  }

bool Npc::isLie() const {
  return bodyStateMasked()==BS_LIE;
  }

bool Npc::isUnconscious() const {
  return owner.script().isUnconscious(*this);
  }

bool Npc::isDown() const {
  return isUnconscious() || isDead();
  }

bool Npc::isAttack() const {
  return owner.script().isAttack(*this);
  }

bool Npc::isTalk() const {
  return owner.script().isTalk(*this);
  }

void Npc::setTalkPartner(Npc* other, uint64_t ms) {
  if(other==this)
    return;
  talkWith  = other;
  talkUntil = owner.tickCount() + ms;
  }

Npc* Npc::talkPartner() const {
  static const float maxDist = 2000; // further away it is not a conversation any more
  if(talkWith==nullptr || talkUntil<=owner.tickCount() || talkWith->isDead())
    return nullptr;
  if((talkWith->position()-position()).quadLength()>maxDist*maxDist)
    return nullptr;
  return talkWith;
  }

Gaze::Source Npc::gazeTarget(Tempest::Vec3& at) const {
  if(headInput) {
    const float yaw = rotationRad()+headInput->x*float(M_PI)/180.f;
    const float pitch = headInput->y*float(M_PI)/180.f;
    at = mapHeadBone() + Vec3(std::cos(yaw)*std::cos(pitch),std::sin(pitch),std::sin(yaw)*std::cos(pitch))*1000.f;
    return Gaze::Source::Input;
    }
  static const float focusDist = 800;
  const Vec3 eyes = {0,6,0}; // the eyes are a bit above the head bone
  if(auto other = talkPartner()) {
    at = other->mapHeadBone() + eyes;
    return Gaze::Source::Talk;
    }
  if(physicsNoiseUntil>owner.tickCount() && Gothic::physicsFlags().physicsNoise && canHearPhysicsNoise()) {
    at = physicsNoisePosition;
    return Gaze::Source::Script;
    }
  if(currentLookAtNpc!=nullptr && currentLookAtNpc!=this) {
    at = currentLookAtNpc->mapHeadBone() + eyes;
    return Gaze::Source::Script;
    }
  if(currentLookAt!=nullptr) {
    // a waypoint is on the floor: where the eyes of one standing there would be
    at    = currentLookAt->position();
    at.y += mapHeadBone().y - position().y;
    return Gaze::Source::Script;
    }
  if(isPlayer()) {
    // the hero looks at the npc in focus; with a weapon the body turns to it anyway
    auto t = currentTarget;
    if(t==nullptr || t==this || weaponState()!=WeaponState::NoWeapon || qDistTo(*t)>focusDist*focusDist)
      return Gaze::Source::None;
    at = t->mapHeadBone() + eyes;
    return Gaze::Source::Focus;
    }
  if(auto t = attention.target()) {
    at = t->mapHeadBone() + eyes;
    return Gaze::Source::Attention;
    }
  return Gaze::Source::None;
  }

void Npc::flinch(const Flinch::Hit& hit) {
  visual.flinch(*this,hit);
  }

std::string Npc::animNames() const {
  return visual.pose().animNames();
  }

bool Npc::isAttackAnim() const {
  return visual.pose().isAttackAnim();
  }

bool Npc::isPrehit() const {
  return visual.pose().isPrehit(owner.tickCount());
  }

bool Npc::isImmortal() const {
  return hnpc->flags & daedalus::NpcFlag::IMMORTAL;
  }

bool Npc::setInteraction(Interactive *id, bool quick) {
  if(currentInteract==id)
    return true;

  if(currentInteract!=nullptr) {
    return currentInteract->detach(*this,quick);
    }

  if(id==nullptr)
    return (currentInteract==nullptr);

  if(id->attach(*this)) {
    currentInteract = id;
    if(!isPlayer() && (isHuman() || Gothic::physicsFlags().monsterCharacterVirtual)) {
      mvAlgo.resetCharacterVirtual(true);
      resetCharacterVisualHeight();
      }
    // Remove the contact rig in the sequential physics handoff, including
    // when an immediate detach leaves only its transition animation behind.
    livingWasInteracting = true;
    attachToPoint(nullptr); //NOTE: Fajeth campfire
    if(!quick) {
      visual.stopAnim(*this,"");
      setAnimRotate(0);
      }
    return true;
    }

  return false;
  }

void Npc::quitInteraction() {
  if(currentInteract==nullptr)
    return;
  if(!isPlayer() && (isHuman() || Gothic::physicsFlags().monsterCharacterVirtual)) {
    mvAlgo.resetCharacterVirtual(true);
    resetCharacterVisualHeight();
    }
  if(invTorch)
    processDefInvTorch();
  setDirectionY(0);
  currentInteract=nullptr;
  }

void Npc::processDefInvTorch() {
  if(invTorch || isUsingTorch()) {
    visual.setTorch(invTorch,owner);
    invTorch = !invTorch;
    }
  }

void Npc::setDetectedMob(Interactive* id) {
  moveMob         = id;
  moveMobCacheKey = position();
  }

Interactive* Npc::detectedMob() const {
  if(currentInteract!=nullptr)
    return currentInteract;
  if((moveMobCacheKey-position()).quadLength()<10.f*10.f)
    return moveMob;
  return nullptr;
  }

void Npc::multSpeed(float s) {
  mvAlgo.multSpeed(s);
  }

bool Npc::testMove(const Vec3& pos) {
  DynamicWorld::CollisionTest out;
  return physic.testMove(pos,out);
  }

bool Npc::tryMove(const Vec3& dp) {
  DynamicWorld::CollisionTest out;
  return tryMove(dp, out);
  }

bool Npc::tryMove(const Vec3& dp, DynamicWorld::CollisionTest& out) {
  return tryTranslate(Vec3(x,y,z) + dp, out);
  }

bool Npc::tryTranslate(const Vec3& to) {
  DynamicWorld::CollisionTest out;
  return tryTranslate(to,out);
  }

bool Npc::tryTranslate(const Vec3& to, DynamicWorld::CollisionTest& out) {
  switch(physic.tryMove(to, out)) {
    case DynamicWorld::MoveCode::MC_Fail:
      return false;
    case DynamicWorld::MoveCode::MC_Partial:
      setViewPosition(out.partial);
      return true;
    case DynamicWorld::MoveCode::MC_Skip:
    case DynamicWorld::MoveCode::MC_OK:
      setViewPosition(to);
      return true;
    }
  return false;
  }

Npc::JumpStatus Npc::tryJump() {
  float len = MoveAlgo::climbMove;
  float rot = rotationRad();
  float s   = std::sin(rot), c = std::cos(rot);
  Vec3  dp  = Vec3{len*c, 0, len*s};

  auto& g  = owner.script().guildVal();
  auto  gl = guild();

  if(isSlide() || isSwim() || isDive()) {
    JumpStatus ret;
    ret.anim   = Anim::Idle;
    return ret;
    }

  const float jumpLow = float(g.jumplow_height[gl]);
  const float jumpMid = float(g.jumpmid_height[gl]);
  const float jumpUp  = float(g.jumpup_height[gl]);

  auto pos0 = physic.position();

  JumpStatus ret;
  DynamicWorld::CollisionTest info;
  if(!mvAlgo.isJumpUp() && physic.testMove(pos0+dp,info)) {
    // jump forward
    ret.anim   = Anim::Jump;
    ret.noClimb = true;
    return ret;
    }

  auto  lnd   = owner.physic()->landRay(pos0 + dp + Vec3(0, jumpUp + jumpLow, 0));
  float jumpY = lnd.v.y;
  auto  pos1  = Vec3(pos0.x,jumpY,pos0.z);
  auto  pos2  = pos1 + dp;

  float dY    = jumpY - y;

  if(dY<=0.f ||
     !physic.testMove(pos2,pos1,info)) {
    ret.anim    = Anim::JumpUp;
    ret.height  = y + jumpUp;
    ret.noClimb = true;
    return ret;
    }

  if(!physic.testMove(pos1,pos0,info) ||
     !physic.testMove(pos2,pos1,info)) {
    // check approximate path of climb failed
    ret.anim    = Anim::JumpUp;
    ret.noClimb = true;
    return ret;
    }

  if(dY>=jumpUp || dY>=jumpMid) {
    // Jump to the edge, and then pull up. Height: 200-350cm
    ret.anim   = Anim::JumpUp;
    ret.height = y + jumpUp;
    return ret;
    }

  DynamicWorld::CollisionTest out;
  if(mvAlgo.testSlide(Vec3{pos0.x,jumpY,pos0.z}+dp,out)) {
    // cannot climb to non angled surface
    ret.anim    = Anim::Jump;
    ret.noClimb = true;
    return ret;
    }

  if(mvAlgo.isJumpUp() && dY<=jumpLow + visual.pose().translateY()) {
    // jumpup -> climb
    ret.anim   = Anim::JumpHang;
    ret.height = jumpY;
    return ret;
    }

  if(mvAlgo.isJumpUp()) {
    ret.anim    = Anim::Idle;
    ret.noClimb = true;
    return ret;
    }

  if(dY<=jumpLow) {
    // Without using the hands, just big footstep. Height: 50-100cm
    ret.anim   = Anim::JumpUpLow;
    ret.height = jumpY;
    return ret;
    }

  if(dY<=jumpMid) {
    // Supported on the hands in one sentence. Height: 100-200cm
    ret.anim   = Anim::JumpUpMid;
    ret.height = jumpY;
    return ret;
    }

  return JumpStatus(); // error
  }

void Npc::startDive() {
  mvAlgo.startDive();
  }

void Npc::transformBack() {
  if(transformSpl==nullptr)
    return;
  transformSpl->undo(*this);
  setVisual(transformSpl->skeleton);
  setVisualBody(vHead,vTeeth,vColor,bdColor,body,head);
  closeWeapon(true);

  // invalidate tallent overlays
  for(size_t i=0; i<TALENT_MAX_G2; ++i)
    setTalentSkill(Talent(i),talentsSk[i]);

  invent.updateView(*this);
  transformSpl.reset();
  }

std::vector<GameScript::DlgChoice> Npc::dialogChoices(Npc& player,const std::vector<uint32_t> &except,bool includeImp) {
  return owner.script().dialogChoices(player.hnpc,this->hnpc,except,includeImp);
  }

bool Npc::isAiQueueEmpty() const {
  return !isPhysicsInvestigating() && aiQueue.size()==0 &&
         go2.empty() &&
         waitTime<owner.tickCount();
  }

void Npc::drawVobBox(DbgPainter& p) const {
  physic.debugDraw(p);

  p.setBrush(Tempest::Color(0,1,0));
  p.drawPoint(position());

  const auto cen = centerPosition();
  p.setBrush(Tempest::Color(0,0,1));
  p.drawPoint(cen);

  p.setPen(Tempest::Color(1,1,1));
  p.drawLine(cen, cen+Tempest::Vec3(0,25,0));
  p.setPen(Tempest::Color(1,1,0));
  p.drawLine(cen, cen+Tempest::Vec3(25,0,0));
  p.setPen(Tempest::Color(1,0.5f,0));
  p.drawLine(cen, cen+Tempest::Vec3(0,0,25));

  if(auto sk = visual.visualSkeleton()) {
    auto bbox = sk->bboxCol;

    auto tr = transform();
    tr.translate(0,visual.pose().translateY(),0);

    p.setPen(Color(1,0,0));
    p.drawObb(tr, bbox);
    }
  }

void Npc::drawVobRay(DbgPainter& p, const Npc& oth) const {
  const bool freeLos = true;
  const auto mid     = oth.physic.center();
  p.setPen(Color(0,1,0));

  if(canRayHitPoint(mid,freeLos)) {
    // mid of dead npc may endedup inside a wall; extra check for physical center
    p.drawLine(mapHeadBone(), mid);
    return;
    }
  if(oth.visual.visualSkeleton()==nullptr)
    return;
  if(oth.visual.visualSkeleton()->BIP01_HEAD==size_t(-1))
    return;
  auto head = oth.visual.mapHeadBone();
  if(canRayHitPoint(head,freeLos)) {
    p.drawLine(mapHeadBone(), head);
    return;
    }
  p.setPen(Color(1,0,0));
  p.drawLine(mapHeadBone(), head);
  }

Vec3 Npc::aimDirection(const Vec3& target) const {
  const float a = rotationRad();
  const Vec3 f(std::cos(a),0,std::sin(a));
  float shoulder = 145.f;
  if(const auto sk = visual.visualSkeleton()) {
    const size_t sh = sk->findNode("BIP01 R UPPERARM");
    if(sh<sk->tr.size())
      shoulder = sk->tr[sh].at(3,1);
    }
  Vec3 from = position()+Vec3(0,shoulder,0)+f*35.f;
  if(hasManualAim()) {
    Vec3 point,dir;
    if(arrowRay(point,dir) && (target-point).length()>150.f)
      from = point;
    return target-from;
    }
  if(weaponState()==WeaponState::Bow || weaponState()==WeaponState::CBow)
    return World::bulletVelocity(from,target);
  return target-from;
  }

Vec3 Npc::aimLookAt(const Vec3& target) const {
  const float a = rotationRad();
  const Vec3 facing(std::cos(a),0,std::sin(a));
  // Follow the hero's physical shoulder as the original aim camera did, without the
  // camera's extra forward displacement. Read the last applied pose, not the hand.
  const Vec3 shift = aimLeanShift();
  const Vec3 offset = facing*std::max(Vec3::dotProduct(shift,facing),0.f) + Vec3(0,std::min(shift.y,0.f),0);
  // A crosshair on the nearby roof edge still looks down from this elevated origin;
  // the directional clearance rays continue past the hit to find a pose over the edge.
  const Vec3 direction = target-(Camera::aimShoulder(position(),facing)+offset);
  return direction.length()>1e-3f ? Vec3::normalize(direction) : facing;
  }

Vec3 Npc::aimLeanDirection(const Vec3& target) const {
  if(hasManualAim())
    return aimLookAt(target);
  return target-position();
  }

Vec3 Npc::aimHead(const Vec3& target, float angle) const {
  const float a = rotationRad();
  const Vec3 f = {std::cos(a),0,std::sin(a)};
  return visual.pose().aimIk().headAt(position(),f,aimLeanDirection(target),angle);
  }

Vec3 Npc::aimCurrentHead() const {
  const auto& mt = visual.transform();
  const auto& aim = visual.pose().aimIk();
  return aim.headAt(Vec3(mt.at(3,0),mt.at(3,1),mt.at(3,2)),
                    Vec3(mt.at(2,0),0,mt.at(2,2)),aim.appliedDirection(),aim.appliedLean());
  }

Vec3 Npc::aimLeanShift() const {
  const auto& mt = visual.transform();
  const auto& aim = visual.pose().aimIk();
  const Vec3 origin(mt.at(3,0),mt.at(3,1),mt.at(3,2));
  const Vec3 facing(mt.at(2,0),0,mt.at(2,2));
  return aim.headAt(origin,facing,aim.appliedDirection(),aim.appliedLean())-
         aim.headAt(origin,facing,aim.appliedDirection(),0.f);
  }

Vec3 Npc::aimArrow(const Vec3& target, float angle) const {
  const float a = rotationRad();
  const Vec3 f(std::cos(a),0,std::sin(a));
  return visual.pose().aimIk().arrowAt(position(),f,aimLeanDirection(target),angle);
  }

Aim::LeanEdge Npc::aimEdge(const Vec3& direction, float range) const {
  const auto edge = mvAlgo.edgeAhead(direction,range);
  return {edge.found,edge.distance,edge.drop,edge.supportY};
  }

Aim::LeanResult Npc::aimLean(const Vec3& target, bool manual) const {
  if(!manual) {
    const auto d = aimDirection(target);
    Aim::LeanResult result;
    result.angle = Aim::leanForNpc(-std::atan2(d.y,std::sqrt(d.x*d.x+d.z*d.z)));
    return result;
    }
  const auto look = aimLookAt(target);
  const float down = -std::atan2(look.y,std::sqrt(look.x*look.x+look.z*look.z));
  return visual.pose().aimIk().leanNeeded(*this,look,look,down,aimEdge(look));
  }

bool Npc::isAlignedToGround() const {
  if(ragdoll)
    return false;
  auto gl = guild();
  // four-legged creatures stand on the ground with foot IK instead of tilting as a whole
  const bool align = owner.script().guildVal().surface_align[gl]!=0 && !visual.hasQuadrupedIk();
  return align || isDead() || isLie();
  }

Vec3 Npc::groundNormal() const {
  auto ground = mvAlgo.groundNormal();
  const bool align = isAlignedToGround();

  if(!align || mvAlgo.isInAir() || mvAlgo.isSwim())
    ground = {0,1,0};
  if(ground==Vec3())
    ground = {0,1,0};
  return ground;
  }

Matrix4x4 Npc::mkPositionMatrix() const {
  if(ragdoll)
    return ragdollObjectMatrix;
  const auto ground = groundNormal();
  const bool align  = isAlignedToGround();

  float angY = mvAlgo.isDive() ? angleY : 0;
  if(align) {
    float rot  = rotationRad();
    float s    = std::sin(rot), c = std::cos(rot);
    auto  dir  = Vec3(c,0,s);
    auto  norm = Vec3::normalize(ground);

    float cx = Vec3::dotProduct(norm,dir);
    angY = -std::asin(cx)*180.f/float(M_PI);
    }

  Matrix4x4 mt = Matrix4x4();
  mt.identity();
  mt.translate(x,y,z);
  mt.rotateOY(90-angle);
  if(angY!=0)
    mt.rotateOX(-angY);
  if(isPlayer() && !align) {
    mt.rotateOZ(runAng);
    }
  mt.scale(sz[0],sz[1],sz[2]);
  return mt;
  }

void Npc::updateTransform() {
  updateAnimation(0, true);
  }

void Npc::syncDeferredAnimation() const {
  if(animLodPendingDt==0 || !AnimationLod::canSyncOnRead())
    return;
  AnimationLod::noteSync();
  // An Npc is never created const: the solved pose is a cache of the world clock.
  const_cast<Npc*>(this)->updateAnimation(0);
  }

bool Npc::animationLodPinned() const {
  // Melee shadow bodies and blades, living hit bodies, ragdolls and mobsi read
  // this NPC's bones every frame from world logic. A drawn weapon alone does not
  // pin: monsters keep Fist mode at rest. A fighter has a target, swings, or has
  // an open shadow-blade window.
  const bool fighting = weaponState()!=WeaponState::NoWeapon &&
                        (currentTarget!=nullptr || isAttackAnim() || (meleeShadow && meleeShadow->window));
  return animLodTargeted || isPlayer() || fighting ||
         ragdoll!=nullptr || living!=nullptr || currentInteract!=nullptr || spellFrozen();
  }

void Npc::resetCharacterVisualHeight() {
  if(characterVisualYOffset!=0)
    durtyTranform |= TR_Pos;
  characterVisualSteps.clear();
  characterVisualYOffset = 0;
  characterVisualHasSample = false;
  characterVisualGroundSample = false;
  }

void Npc::updateCharacterVisualHeight() {
  if(usesSimplifiedMovement())
    return;
  const bool characterFlag = isPlayer() ? Gothic::physicsFlags().characterVirtual :
                             isHuman() ? Gothic::physicsFlags().npcCharacterVirtual :
                                         Gothic::physicsFlags().monsterCharacterVirtual;
  if(!isPlayer() && !characterFlag && (!isHuman() || characterVisualSteps.empty())) {
    if(characterVisualHasSample)
      resetCharacterVisualHeight();
    return;
    }
  constexpr uint64_t blendMilliseconds = 100;
  constexpr float heightNoise = 0.1f; // cm
  constexpr float maxVisualLag = 10.f; // cm
  const auto now = owner.tickCount();
  const auto at = position();
  const auto normal = mvAlgo.groundNormal();
  const bool groundedState = !isDown() && !spellFrozen() && interactive()==nullptr &&
                             mvAlgo.state()==MoveAlgo::Run &&
                             !(bodyStateMasked()==BS_JUMP && isFlyAnim());
  const bool monsterAttack = !isPlayer() && !isHuman() && bodyStateMasked()==BS_HIT;
  const bool groundState = groundedState && !monsterAttack;
  const bool enabled = characterFlag && mvAlgo.characterVirtualActive() && groundState;
  // A radius handoff (or a human NPC flag handoff) keeps the remaining stair blend. Removing
  // that visual offset immediately would pop the model even though its feet agree.
  // An attack does not create stair corrections, but must finish a pending one.
  const bool finishNpcBlend = !isPlayer() && groundedState && !characterVisualSteps.empty();
  const float previousOffset = characterVisualYOffset;

  if((!enabled && !finishNpcBlend) || (characterVisualHasSample && now<characterVisualSampleTick)) {
    characterVisualSteps.clear();
    characterVisualYOffset = 0;
    }
  else {
    auto alive = std::find_if(characterVisualSteps.begin(),characterVisualSteps.end(),[now](const CharacterVisualStep& step) {
      return now-step.tick<blendMilliseconds;
      });
    characterVisualSteps.erase(characterVisualSteps.begin(),alive);
    if(enabled && characterVisualHasSample && characterVisualGroundSample) {
      const auto delta = at-characterVisualSamplePosition;
      const auto support = mvAlgo.characterVirtualGroundVelocity()*(float(now-characterVisualSampleTick)*0.001f);
      const auto relative = delta-support;
      float predictedMin = support.y, predictedMax = support.y;
      // Relative travel along the support plane explains an ordinary slope.
      // Either endpoint normal may explain a transition between two surfaces.
      // Include support-only travel: a blended normal must not turn continued
      // ascent relative to that support into a fictitious downward step.
      for(const auto& n:{characterVisualSampleNormal,normal}) {
        if(n.y<=1.e-3f)
          continue;
        const float predicted = support.y-(n.x*relative.x+n.z*relative.z)/n.y;
        if(!std::isfinite(predicted))
          continue;
        predictedMin = std::min(predictedMin,predicted);
        predictedMax = std::max(predictedMax,predicted);
        }
      const float explained = std::clamp(delta.y,predictedMin,predictedMax);
      const float residual = delta.y-explained;
      const float correction = std::copysign(std::max(0.f,std::abs(residual)-heightNoise),residual);
      if(correction!=0)
        characterVisualSteps.push_back({now,-correction});
      }
    characterVisualYOffset = 0;
    for(const auto& step:characterVisualSteps)
      characterVisualYOffset += step.offset*(1.f-float(now-step.tick)/float(blendMilliseconds));
    if(std::abs(characterVisualYOffset)>maxVisualLag) {
      const float limited = std::clamp(characterVisualYOffset,-maxVisualLag,maxVisualLag);
      const float scale = limited/characterVisualYOffset;
      // Keep each correction's remaining lifetime while discarding excess lag.
      // Clamping only the shown offset would retain a hidden backlog; clearing
      // the history would instead snap the model to its physical feet.
      for(auto& step:characterVisualSteps)
        step.offset *= scale;
      characterVisualYOffset = limited;
      }
    }

  if(!isPlayer() && !isHuman() && (enabled || finishNpcBlend) &&
     characterVisualHasSample && characterVisualGroundSample && now>=characterVisualSampleTick) {
    // Releasing an earlier stair blend can add to a small physical rise.
    // Keep that presentation remainder in the blend. A high guild-sized
    // ledge may still snap: the human ten-centimetre lag limit takes priority.
    constexpr float maxVisualStep = 12.f; // cm per movement update
    const float previousY = characterVisualSamplePosition.y+previousOffset;
    const float desiredY = at.y+characterVisualYOffset;
    const float blendedY = std::clamp(desiredY,previousY-maxVisualStep,previousY+maxVisualStep);
    const float limitedY = std::clamp(blendedY,at.y-maxVisualLag,at.y+maxVisualLag);
    const float correction = limitedY-desiredY;
    if(correction!=0) {
      characterVisualSteps.push_back({now,correction});
      characterVisualYOffset += correction;
      }
    }

  // Keep a baseline even while the freshly loaded controller is inactive. Its first
  // floor correction can then blend from the saved height, while airborne handoffs snap.
  characterVisualHasSample = true;
  characterVisualGroundSample = groundedState;
  characterVisualSamplePosition = at;
  characterVisualSampleNormal = normal;
  characterVisualSampleTick = now;
  if(characterVisualYOffset!=previousOffset)
    durtyTranform |= TR_Pos;
  }

void Npc::updateAnimation(uint64_t dt, bool force) {
  FrameProfile::Scope timing(FrameProfile::Zone::ActorAnimationCpu,FrameProfile::watchedActor(this));
  // Frames the animation LOD skipped: the pose follows the world clock, the
  // smoothing filters and effects get the whole interval at once.
  dt += animLodPendingDt;
  animLodPendingDt = 0;
  if(lieVisualYOffset!=0) {
    auto matrix=visual.transform();
    matrix.set(3,1,matrix.at(3,1)-lieVisualYOffset);
    lieVisualYOffset=0;
    // Rebuild even when another animation update happens at the same timestamp.
    visual.setObjMatrix(matrix,true);
    }
  const bool frozen = spellFrozen();
  const bool clearedPhysicalPose = visual.clearPhysicalPose();
  const auto camera = Gothic::inst().camera();
  if(isPlayer() && camera!=nullptr && camera->isFree())
    dt = 0;

  updateCharacterVisualHeight();

  if(durtyTranform) {
    const auto ground = groundNormal();
    if(lastGroundNormal!=ground) {
      durtyTranform |= TR_Rot;
      lastGroundNormal = ground;
      }

    sfxWeapon.setPosition(x,y,z);
    Matrix4x4 pos;
    if(durtyTranform==TR_Pos) {
      pos = visual.transform();
      pos.set(3,0,x);
      pos.set(3,1,y+characterVisualYOffset);
      pos.set(3,2,z);
      } else {
      pos = mkPositionMatrix();
      pos.set(3,1,pos.at(3,1)+characterVisualYOffset);
      }

    visual.setObjMatrix(pos,false);
    durtyTranform = 0;
    }

  bool syncAtt = visual.updateAnimation(this,nullptr,owner,dt,force,frozen) || clearedPhysicalPose;
  if(frozen) {
    if(living)
      livingNeedsRebuild = !living->moveKinematic(visual.pose().transform(),visual.pose().boneCount(),visual.transform(),
                                                float(std::max<uint64_t>(dt,1))*0.001f);
    if(syncAtt || force)
      visual.syncAttaches();
    return;
    }
  if(ragdoll) {
    // the quiver of a body down or dead lies on the ground too (graphics/mesh/quiver)
    syncAtt |= visual.updateQuiver(*this,dt);
    if(syncAtt || force)
      visual.syncAttaches();
    return;
    }
  syncAtt |= visual.updateLean(*this,dt);
  // Headless body training can keep foot IK on the watched arena actors only: nobody
  // looks at the rest of the world there, and their foot IK is ~30% of a serial tick.
  static const bool footIkWatchedOnly = [] {
    const char* v = std::getenv("OPENGOTHIC_FOOTIK_WATCHED_ONLY");
    return v!=nullptr && v[0]=='1';
    }();
  if(!footIkWatchedOnly || BodyObservations::watched(*this))
    syncAtt |= visual.updateFootIk(*this,owner,dt,isAlignedToGround());
  syncAtt |= visual.updateAim(*this,dt);
  syncAtt |= visual.updateGaze(*this,dt);
  syncAtt |= visual.updateFlinch(*this,dt);
  syncAtt |= updateLyingVisualHeight();
  if(living)
    livingNeedsRebuild = !living->moveKinematic(visual.pose().transform(),visual.pose().boneCount(),visual.transform(),
                                              float(std::max<uint64_t>(dt,1))*0.001f);
  updateMeleeBenchBlade();
  if(living && living->reactionWeight()>0.f && living->getPose(arrowPhysicalPose))
    syncAtt |= visual.setPhysicalPose(arrowPhysicalPose,living->reactionBones(),living->reactionWeight());
  syncAtt |= visual.updateQuiver(*this,dt);
  if(syncAtt)
    visual.syncAttaches();
  }

bool Npc::updateLyingVisualHeight() {
  if(usesSimplifiedMovement())
    return false;
  if(!isHuman() || ragdoll || spellFrozen() || !isLie() || mvAlgo.state()!=MoveAlgo::Run || interactive()!=nullptr)
    return false;
  Vec3 lowest;
  if(!visual.minSkinnedVertex(fatness(),lowest))
    return false;
  // Look only around the current root and posed surface. A hand over a ledge
  // must not pull the whole animation down toward a distant lower floor.
  constexpr float rayMargin=20.f;
  const Vec3 from(lowest.x,std::max(y,lowest.y)+rayMargin,lowest.z);
  const float distance=from.y-(std::min(y,lowest.y)-rayMargin);
  const auto floor=owner.physic()->landRay(from,distance);
  if(!floor.hasCol || floor.n.y<=.5f)
    return false;
  const float offset=floor.v.y-lowest.y;
  if(!std::isfinite(offset) || offset==0)
    return false;
  auto matrix=visual.transform();
  matrix.set(3,1,matrix.at(3,1)+offset);
  lieVisualYOffset=offset;
  visual.setObjMatrix(matrix,true);
  return true;
  }
