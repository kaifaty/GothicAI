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
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
#include "world/benchhooks.h"
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

// Npc::tick may intentionally return without calling MoveAlgo (for example a native attack gate).
// Observe that branch explicitly instead of advancing movement just to make telemetry contiguous.
struct Npc::BodyMovementTrace final {
  BodyMovementTrace(Npc& npc, uint64_t dt)
    :npc(npc),dt(dt),serial(npc.mvAlgo.traceTickSerial()),previous(npc.bodyTraceMovementActive) {
    active = Bench::bodyTracing() && npc.currentTarget!=nullptr;
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
    Bench::bodyEvent(npc,npc.currentTarget,"physics_step",npc.owner.tickCount(),fields);
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
  static std::atomic<uint32_t> creationCounter{0};
  creationSeqValue = ++creationCounter;
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
  if(startupFloorPendingValue)
    owner.script().startupFloor().forget(*this);
  endMeleeAiSpecial();
  clearMeleeShadow();
  if(std::getenv("OPENGOTHIC_MELEE_BENCH")!=nullptr && owner.physic()!=nullptr)
    owner.physic()->removeMeleeBlade(this);
  NpcKeys::forget(this);
  BodyObservations::forget(this);
  Bench::bodyForget(this);
  if(currentInteract)
    currentInteract->detach(*this,true);
  if(hnpc && hnpc->user_ptr==this)
    hnpc->user_ptr=nullptr;
  }

std::string Npc::startupFloorDigest() const {
  // Exact bits, so off/on runs compare byte for byte.
  std::string ret;
  auto bits = [&ret](float v) {
    uint32_t u = 0;
    std::memcpy(&u,&v,sizeof(u));
    char buf[12];
    std::snprintf(buf,sizeof(buf),"%08x",unsigned(u));
    ret += buf;
    ret += ' ';
    };
  bits(x); bits(y); bits(z);
  bits(angle); bits(angleY);
  ret += "wp=";
  ret += currentFp!=nullptr ? currentFp->name : std::string("-");
  ret += " ta=";
  ret += hnpc->wp;
  ret += " body=";
  if(physic.hasBody()) {
    const auto p = physic.position();
    bits(p.x); bits(p.y); bits(p.z);
    }
  ret += "mat=";
  const auto m = visual.transform();
  for(int i=0;i<4;++i)
    for(int j=0;j<4;++j)
      bits(m.at(i,j));
  return ret;
  }

void Npc::syncLogicalBodyState(bool rebuild) {
  logicalBody.sync(*this,rebuild);
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
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,lastHit,"termination",owner.tickCount(),
                        "\"reason\":" + Bench::bodyJson(death ? "dead" : "unconscious") +
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
          // the reload from the quiver took it out of there: one less shows till the shot takes it from the
          // inventory
          if(visual.pose().isOwnReload() && visual.hasAmmoShown())
            setQuiverTaken(1);
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

  if(isPlayer() && Bench::isActive()) {
    // what the hero's animations do, for the test bench (the reload at another tempo: each event once)
    const char* reloadAnim = weaponState()==WeaponState::CBow ? Archery::CBowReload : Archery::BowReload;
    char        buf[256];
    for(auto& i:ev.timed) {
      std::snprintf(buf,sizeof(buf),"anim event %d %.*s %.*s %.*s (reload %.2f)",int(i.def),int(i.item.size()),i.item.data(),
                    int(i.slot[0].size()),i.slot[0].data(),int(i.slot[1].size()),i.slot[1].data(),double(animProgress(reloadAnim)));
      Bench::note(buf);
      }
    for(auto& i:ev.morph) {
      std::snprintf(buf,sizeof(buf),"anim morph %.*s %.*s (reload %.2f)",int(i.anim.size()),i.anim.data(),int(i.node.size()),i.node.data(),
                    double(animProgress(reloadAnim)));
      Bench::note(buf);
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
  Bench::beforeNpcTick(*this);
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
  if(isPlayer() && Bench::isActive()) {
    char buf[128];
    std::snprintf(buf,sizeof(buf),"sound %.*s",int(sound.size()),sound.data());
    Bench::note(buf);
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

Vec3 Npc::mapEyes() const {
  syncDeferredAnimation();
  return visual.mapEyes();
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

bool Npc::checkGoToNpcdistance(const Npc &other) {
  return fghAlgo.isInAttackRange(*this,other,owner.script());
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
