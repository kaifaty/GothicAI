#include "npc.h"
#include "graphics/mesh/protomesh.h"

#include <Tempest/Matrix4x4>
#include <Tempest/Log>
#include <cctype>
#include <cstdlib>

#include "graphics/mesh/skeleton.h"
#include "graphics/visualfx.h"
#include "game/damagecalculator.h"
#include "game/herodeed.h"
#include "game/deedwitness.h"
#include "game/bodyobservations.h"
#include "game/bodycontroller.h"
#include "game/bodytrajectory.h"
#include "game/npckeys.h"
#include "game/serialize.h"
#include "physics/physicsnoise.h"
#include "game/ragdollsave.h"
#include "game/gamescript.h"
#include "utils/string_frm.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "utils/versioninfo.h"
#include "utils/fileext.h"
#include "utils/dbgpainter.h"
#include "camera.h"
#include "commandline.h"
#include "testbench.h"
#include "gothic.h"
#include "resources.h"
#include "frameprofile.h"
#include "navdemos.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>

using namespace Tempest;

static std::string_view humansTorchOverlay = "_TORCH.MDS";

namespace {

std::string meleeShadowPoint(const Vec3& p) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << '[' << p.x << ',' << p.y << ',' << p.z << ']';
  return out.str();
  }

std::string meleeShadowWho(const Npc* npc) {
  if(npc==nullptr)
    return "null";
  const auto* symbol = npc->world().script().findSymbol(npc->instanceSymbol());
  return "{\"id\":" + std::to_string(npc->world().npcId(npc)) +
         ",\"instance\":" + std::to_string(npc->instanceSymbol()) + ",\"name\":" +
         TestBench::bodyJson(symbol!=nullptr ? std::string_view(symbol->name()) : std::string_view("npc")) +
         ",\"position_cm\":" + meleeShadowPoint(npc->position()) + "}";
  }

std::string meleeShadowBone(const MeleeBladeContact& hit) {
  const auto* skeleton = hit.npc!=nullptr ? hit.npc->visualSkeleton() : nullptr;
  return skeleton!=nullptr && hit.bone<skeleton->nodes.size() ? skeleton->nodes[hit.bone].name : "";
  }

void writeMeleeShadow(const std::string& row) {
  // NPC animation updates run on workers. Keep complete rows and rotation under one lock.
  static std::mutex mutex;
  std::lock_guard<std::mutex> guard(mutex);
  static std::ofstream out;
  static uintmax_t bytes = 0;
  constexpr uintmax_t limit = 8*1024*1024;
  const char* path = "melee-blade-shadow.jsonl";
  if(!out.is_open()) {
    std::error_code error;
    bytes = std::filesystem::file_size(path,error);
    if(error)
      bytes = 0;
    out.open(path,std::ios::app);
    }
  if(bytes+row.size()+1>limit) {
    out.close();
    std::error_code error;
    std::filesystem::remove("melee-blade-shadow.previous.jsonl",error);
    std::filesystem::rename(path,"melee-blade-shadow.previous.jsonl",error);
    out.open(path,std::ios::trunc);
    bytes = 0;
    }
  if(out) {
    out << row << '\n';
    out.flush();
    bytes += row.size()+1;
    }
  }

}

struct Npc::MeleeShadow final {
  struct Window final {
    const Animation::Sequence* sequence = nullptr;
    float length = 0;
    uint32_t index = 0, first = 0, optimal = 0, last = 0;
    float beginMs = 0, optimalMs = 0, endMs = 0, peakCmS = 0, thresholdCmS = 0;
    bool left = false, normalY = true;
    float eventBoundaryMs = 0;
    };
  struct Contact final {
    MeleeBladeContact hit;
    std::string target, bone;
    };
  std::vector<Window> windows;
  std::optional<Window> window;
  uint64_t id = 0, start = 0, tick = 0;
  float speed = 1;
  std::string attacker, weapon, animation, legacyTarget = "null", legacyResult = "no_optimal_frame";
  const Npc* legacyNpc = nullptr;
  bool legacyHit = false, legacyBlock = false, legacyDodge = false;
  int32_t damage = 0;
  std::vector<Contact> contacts;
  };

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
    return self.fghAlgo.isInWRange(self, *npc, self.owner.script()); //need to be consistent with implAttack
  if(npc!=nullptr)
    return MoveAlgo::isClose(self, *npc, dist);
  if(wp!=nullptr)
    return MoveAlgo::isClose(self, *wp, dist);
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


struct Npc::TransformBack {
  TransformBack(Npc& self) {
    hnpc        = std::make_shared<zenkit::INpc>(*self.hnpc);
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

  TransformBack(Npc& owner, zenkit::DaedalusVm& vm, Serialize& fin) {
    hnpc           = std::make_shared<zenkit::INpc>();
    hnpc->user_ptr = nullptr;
    fin.readNpc(vm, hnpc);
    invent.load(fin,owner);
    fin.read(talentsSk,talentsVl);
    fin.read(body,head,vHead,vTeeth,vColor,bdColor);

    std::string sk;
    fin.read(sk);
    skeleton = Resources::loadSkeleton(sk);
    }

  void undo(Npc& self) {
    int32_t aivar[zenkit::INpc::aivar_count]={};

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

  void save(Serialize& fout) {
    fout.write(*hnpc);
    invent.save(fout);
    fout.write(talentsSk,talentsVl);
    fout.write(body,head,vHead,vTeeth,vColor,bdColor);
    fout.write(skeleton!=nullptr ? skeleton->name() : "");
    }

  std::shared_ptr<zenkit::INpc>   hnpc={};
  Inventory                       invent;
  int32_t                         talentsSk[TALENT_MAX_G2]={};
  int32_t                         talentsVl[TALENT_MAX_G2]={};

  std::string                     body,head;
  int32_t                         vHead=0, vTeeth=0, vColor=0;
  int32_t                         bdColor=0;

  const Skeleton*                 skeleton = nullptr;
  };


Npc::Npc(World &owner, size_t instance, std::string_view waypoint, NpcProcessPolicy aiPolicy)
  :owner(owner),aiPolicy(aiPolicy),mvAlgo(*this) {
  outputPipe     = owner.script().openAiOuput();

  hnpc           = std::make_shared<zenkit::INpc>();
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

void Npc::save(Serialize &fout, size_t id, std::string_view directory) {
  fout.setEntry("worlds/",fout.worldName(),directory,id,"/data");
  fout.write(*hnpc);
  fout.write(body,head,vHead,vTeeth,bdColor,vColor,bdFatness);
  fout.write(x,y,z,angle,sz);
  fout.write(wlkMode,trGuild,talentsSk,talentsVl,refuseTalkMilis);
  fout.write(permAttitude,tmpAttitude);
  fout.write(perceptionTime,perceptionNextTime);
  for(auto& i:perception)
    fout.write(i.func);

  // extra state
  fout.write(lastHitType,lastHitSpell);
  if(currentSpellCast<uint32_t(-1))
    fout.write(uint32_t(currentSpellCast)); else
    fout.write(uint32_t(-1));
  fout.write(uint8_t(castLevel),castNextTime,manaInvested,aiExpectedInvest);
  fout.write(spellInfo);

  saveTrState(fout);
  saveAiState(fout);

  fout.write(currentInteract,currentOther,currentVictim);
  fout.write(currentLookAt,currentLookAtNpc,currentTarget,nearestEnemy);

  go2.save(fout);
  fout.write(currentFp,currentFpLock);
  wayPath.save(fout);

  mvAlgo.save(fout);
  fghAlgo.save(fout);
  fout.write(lastEventTime,angleY,runAng);
  fout.write(invTorch);
  fout.write(isUsingTorch());

  Vec3 phyPos = physic.position();
  fout.write(phyPos);

  fout.setEntry("worlds/",fout.worldName(),directory,id,"/visual");
  visual.save(fout,*this);

  fout.setEntry("worlds/",fout.worldName(),directory,id,"/inventory");
  if(!invent.isEmpty() || id==size_t(-1))
    invent.save(fout);

  // Optional independent entry: the old NPC/visual byte layouts remain readable.
  fout.setEntry("worlds/",fout.worldName(),directory,id,"/ragdoll");
  RagdollSave::Snapshot state;
  state.legacyDown = ragdollLegacyDown || ragdollRecovering;
  state.body = ragdoll ? std::optional<Ragdoll::State>(ragdoll->state()) : ragdollDormant;
  RagdollSave::save(fout,state);
  fout.setEntry("worlds/",fout.worldName(),directory,id,"/ragdoll-recovery");
  visual.pose().saveRagdollRecovery(fout);
  fout.setEntry("worlds/",fout.worldName(),directory,id,"/ragdoll-quest-return");
  RagdollSave::saveQuestReturn(fout,ragdollQuestReturn);
  if(visual.stuckArrowCount()>0) {
    fout.setEntry("worlds/",fout.worldName(),directory,id,"/stuck-arrows");
    visual.saveStuckArrows(fout);
    }
  }

void Npc::load(Serialize &fin, size_t id, std::string_view directory) {
  releaseRagdoll(false);
  ragdollDormant.reset();
  ragdollRecovering = false;
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  ragdollQuestReturn = {};
  ragdollQuestSpellThrow = false;
  ragdollQuestSampleTick = 0;
  ragdollQuestReturns = 0;
  ragdollQuestFrom = {};
  fin.setEntry("worlds/",fin.worldName(),directory,id,"/data");

  hnpc = std::make_shared<zenkit::INpc>();
  hnpc->user_ptr        = this;
  fin.readNpc(owner.script().getVm(), hnpc);
  fin.read(body,head,vHead,vTeeth,bdColor,vColor,bdFatness);

  auto* sym = owner.script().findSymbol(hnpc->symbol_index());
  if (sym != nullptr)
    sym->set_instance(hnpc);

  fin.read(x,y,z,angle,sz);
  resetCharacterVisualHeight();
  lieVisualYOffset=0;
  fin.read(wlkMode,trGuild,talentsSk,talentsVl,refuseTalkMilis);
  durtyTranform = TR_Pos|TR_Rot|TR_Scale;
  if(fin.version()<55)
    angle -= 90;

  fin.read(permAttitude,tmpAttitude);
  fin.read(perceptionTime,perceptionNextTime);
  for(auto& i:perception)
    fin.read(i.func);

  // extra state
  fin.read(lastHitType,lastHitSpell);
  {
  uint32_t currentSpellCastU32 = uint32_t(-1);
  fin.read(currentSpellCastU32);
  currentSpellCast = (currentSpellCastU32==uint32_t(-1) ? size_t(-1) : currentSpellCastU32);
  }
  fin.read(reinterpret_cast<uint8_t&>(castLevel),castNextTime);
  if(fin.version()>44)
    fin.read(manaInvested,aiExpectedInvest);
  fin.read(spellInfo);
  loadTrState(fin);
  loadAiState(fin);

  fin.read(currentInteract,currentOther,currentVictim);
  if(fin.version()>=42)
    fin.read(currentLookAt);
  fin.read(currentLookAtNpc,currentTarget,nearestEnemy);

  go2.load(fin);
  fin.read(currentFp,currentFpLock);
  wayPath.load(fin);

  mvAlgo.load(fin);
  fghAlgo.load(fin);
  fin.read(lastEventTime,angleY,runAng);

  bool isUsingTorch = false;
  if(fin.version()>36) {
    fin.read(invTorch);
    fin.read(isUsingTorch);
    }

  Vec3 phyPos = {};
  fin.read(phyPos);

  fin.setEntry("worlds/",fin.worldName(),directory,id,"/visual");
  visual.load(fin,*this);
  physic.setPosition(phyPos);

  setVisualBody(vHead,vTeeth,vColor,bdColor,body,head);

  if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/inventory"))
    invent.load(fin,*this);

  // post-alignment
  updateTransform();
  if(isUsingTorch)
    visual.setTorch(true,owner);
  if(isDead())
    physic.setEnable(false);

  if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/ragdoll-quest-return") &&
     !RagdollSave::loadQuestReturn(fin,ragdollQuestReturn))
    Log::e("ragdoll: invalid quest NPC return point");

  // Absence denotes an old save: existing dead/KO poses stay animated until a new down event.
  ragdollLegacyDown = isDown();
  if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/ragdoll")) {
    RagdollSave::Snapshot state;
    if(RagdollSave::load(fin,state)) {
      ragdollLegacyDown = state.legacyDown;
      if(state.body && isDown())
        ragdollDormant = std::move(state.body);
      if(fin.setEntry("worlds/",fin.worldName(),directory,id,"/ragdoll-recovery")) {
        try {
          visual.pose().loadRagdollRecovery(fin);
          ragdollRecovering = !ragdollDormant && visual.pose().isRagdollBlending();
          } catch(const std::exception& e) {
          visual.pose().clearRagdollPose();
          Log::e("ragdoll recovery load: ",e.what());
          }
        }
      if(ragdollDormant && Gothic::physicsFlags().ragdoll)
        prepareRagdoll();
      else if(visual.pose().isRagdollBlending())
        updateAnimation(0,true); // Optional recovery must be visible before the first loaded frame.
      } else {
      Log::e("ragdoll: invalid saved body, keeping the animated pose");
      }
    }
  // Existing save entries distinguish a live airborne throw from KO recovery.
  const auto movement = mvAlgo.state();
  ragdollQuestSpellThrow = ragdollQuestReturn.armed && !isDown() &&
      !ragdollLegacyDown && !ragdollRecovering &&
      (movement==MoveAlgo::InAir || movement==MoveAlgo::Falling ||
       movement==MoveAlgo::Swim || movement==MoveAlgo::Dive);
  if(Gothic::physicsFlags().arrowSticking &&
     fin.setEntry("worlds/",fin.worldName(),directory,id,"/stuck-arrows") &&
     !visual.loadStuckArrows(fin,owner))
    Log::e("arrows: invalid saved NPC attachments, keeping the body without arrows");
  }

void Npc::postValidate() {
  if(currentInteract!=nullptr && !currentInteract->isAttached(*this))
    currentInteract = nullptr;
  }

void Npc::saveAiState(Serialize& fout) const {
  fout.write(aniWaitTime,waitTime,faiWaitTime,outWaitTime);
  fout.write(uint8_t(aiPolicy));
  fout.write(aiState.funcIni,aiState.funcLoop,aiState.funcEnd,aiState.sTime,aiState.eTime,aiState.started,aiState.loopNextTime);
  fout.write(aiPrevState);

  aiQueue.save(fout);
  aiQueueOverlay.save(fout);

  fout.write(uint32_t(routines.size()));
  for(auto& i:routines) {
    fout.write(i.start,i.end,i.callback,i.point,i.fallbackName);
    }
  }

void Npc::loadAiState(Serialize& fin) {
  fin.read(aniWaitTime);
  fin.read(waitTime,faiWaitTime);
  fin.read(outWaitTime);
  fin.read(reinterpret_cast<uint8_t&>(aiPolicy));
  fin.read(aiState.funcIni,aiState.funcLoop,aiState.funcEnd,aiState.sTime,aiState.eTime,aiState.started,aiState.loopNextTime);
  fin.read(aiPrevState);

  aiState.hint = "";
  if(auto s = owner.script().findSymbol(aiState.funcIni.ptr)) {
    aiState.hint = s->name().c_str();
    }

  aiQueue.load(fin);
  aiQueueOverlay.load(fin);

  uint32_t size=0;
  fin.read(size);
  routines.resize(size);
  for(auto& i:routines) {
    fin.read(i.start,i.end,i.callback,i.point);
    if(fin.version()>51)
      fin.read(i.fallbackName);
    }
  }

void Npc::saveTrState(Serialize& fout) const {
  if(transformSpl!=nullptr) {
    fout.write(true);
    transformSpl->save(fout);
    } else {
    fout.write(false);
    }
  }

void Npc::loadTrState(Serialize& fin) {
  bool hasTr = false;
  fin.read(hasTr);
  if(hasTr)
    transformSpl.reset(new TransformBack(*this, owner.script().getVm(), fin));
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
  const bool g2       = owner.version().game==2;
  const bool isDragon = (g2 && guild()==GIL_DRAGON);
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

  auto at = currentTaPoint();
  if(at==nullptr)
    return false;

  if(at->isLocked() && !isDead) {
    auto p = owner.findNextPoint(*at);
    if(p!=nullptr)
      at = p;
    }
  setPosition (at->position() );
  setDirection(at->direction());
  owner.script().fixNpcPosition(*this,0,100);

  if(!isDead) {
    attachToPoint(at);
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
  ragdollLastVelocity = {};
  ragdollQuestReturn = {};
  ragdollQuestSpellThrow = false;
  ragdollQuestSampleTick = 0;
  living.reset();
  livingRetryTick = 0;
  livingNeedsRebuild = livingWasFrozen = livingWasInteracting = false;
  arrowImpacts.clear();
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
  }

void Npc::setWalkMode(WalkBit m) {
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

bool Npc::setRagdollDown(bool death) {
  hnpc->attribute[ATR_HITPOINTS] = death ? 0 : 1;
  onNoHealth(death,HS_NoSound);
  return isDown();
  }

void Npc::addRagdollImpulse(const Vec3& impulse) {
  if(ragdoll)
    ragdoll->addImpulse(impulse);
  }

float Npc::physicsMass() const {
  if(ragdoll)
    return ragdoll->mass();
  if(living)
    return living->mass();
  const auto* skeleton = visual.visualSkeleton();
  const float base = skeleton!=nullptr ? Ragdoll::speciesMass(skeleton->name()) : 75.f;
  return std::max(0.01f,base*std::abs(sz[0]*sz[1]*sz[2]));
  }

bool Npc::addSpellImpulse(const Vec3& impulseNs, bool reaction, Npc* culprit) {
  if(!Gothic::physicsFlags().spellPhysics || spellFrozen() ||
     !std::isfinite(impulseNs.x) || !std::isfinite(impulseNs.y) || !std::isfinite(impulseNs.z) ||
     impulseNs.quadLength()<=0.f)
    return false;
  if(ragdoll) {
    ragdoll->addImpulse(impulseNs,culprit);
    return true;
    }
  if(isDown()) {
    pendingSpellImpulse += impulseNs;
    spellImpulseCulprit = culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<zenkit::INpc>();
    return true;
    }
  updateRagdollQuestAnchor();
  if(!mvAlgo.addSpellImpulse(impulseNs,physicsMass(),reaction))
    return false;
  spellImpulseCulprit = culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<zenkit::INpc>();
  if(living)
    living->setNoiseCulprit(culprit);
  if(!isPlayer() && isHuman() && ragdollQuestReturn.hasAnchor &&
     Gothic::physicsFlags().questGuard) {
    if(!ragdollQuestReturn.armed)
      ragdollQuestReturn.hazards = 0;
    ragdollQuestReturn.armed = true;
    ragdollQuestSpellThrow = true;
    }
  return true;
  }

void Npc::suspendRagdoll() {
  // Removed/invalid NPCs can survive until world unload and still receive a
  // transform update at deadPoint. Keep that update out of shadow physics.
  meleeShadowSuspended = true;
  clearMeleeShadow();
  pendingSpellImpulse = {};
  spellImpulseCulprit.reset();
  living.reset();
  arrowImpacts.clear();
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  const bool hadBody = ragdoll!=nullptr;
  if(hadBody)
    ragdollDormant = ragdoll->state();
  releaseRagdoll(false);
  if(hadBody)
    physic.setEnable(false);
  }

bool Npc::createRagdoll() {
  const auto* skeleton = visual.visualSkeleton();
  if(skeleton==nullptr || !visual.hitZoneShape().isValid())
    return false;
  updateAnimation(0,true);
  Vec3 velocity;
  const auto now = owner.tickCount();
  if(ragdollHasLastPosition && now>=ragdollLastTick && now-ragdollLastTick<=250) {
    // A hit before this NPC's tick, or immediately after the world tick, leaves its root at
    // the last live sample. Carry that sample's velocity instead of treating it as a stop.
    velocity = ragdollLastVelocity;
    const auto delta = position()-ragdollLastPosition;
    if(now>ragdollLastTick && delta.length()>0.f)
      velocity = delta*(1000.f/float(now-ragdollLastTick));
    const float speed = velocity.length();
    if(!std::isfinite(speed))
      velocity = {}; else if(speed>3000.f)
      velocity *= 3000.f/speed;
    }
  auto body = owner.physic()->createRagdoll(*skeleton,visual.hitZoneShape(),visual.pose().transform(),
                                          visual.pose().boneCount(),visual.transform(),velocity);
  if(!body || !body->valid())
    return false;
  if(ragdollDormant && !body->restore(*ragdollDormant)) {
    ragdollDormant.reset();
    return false;
    }
  ragdollDormant.reset();
  ragdoll = std::move(body);
  ragdoll->setNpc(this);
  if(const auto culprit = spellImpulseCulprit.lock())
    ragdoll->setNoiseCulprit(static_cast<Npc*>(culprit->user_ptr));
  dispatchArrowPhysics(*ragdoll);
  if(Gothic::physicsFlags().spellPhysics && pendingSpellImpulse.quadLength()>0.f) {
    const auto culprit = spellImpulseCulprit.lock();
    ragdoll->addImpulse(pendingSpellImpulse,culprit ? reinterpret_cast<Npc*>(culprit->user_ptr) : nullptr);
    }
  pendingSpellImpulse = {};
  spellImpulseCulprit.reset();
  ragdollStartTick = owner.tickCount();
  physic.setEnable(false);
  mvAlgo.takeRagdollControl();
  syncRagdoll();
  if(const auto* trace=std::getenv("OPENGOTHIC_RAGDOLL_TRACE");trace!=nullptr && trace[0]=='1')
    Log::i("[ragdoll] created actor=",displayName()," hero=",isPlayer()," parts=",ragdoll->partCount()," dead=",isDead());
  return true;
  }

void Npc::releaseRagdoll(bool blend) {
  const bool hadBody = ragdoll!=nullptr;
  if(hadBody)
    if(const auto* trace=std::getenv("OPENGOTHIC_RAGDOLL_TRACE");trace!=nullptr && trace[0]=='1')
      Log::i("[ragdoll] released actor=",displayName()," hero=",isPlayer()," parts=",ragdoll->partCount()," dead=",isDead());
  visual.pose().clearRagdollPose(blend,owner.tickCount());
  ragdoll.reset();
  ragdollPose.clear();
  if(hadBody && hnpc)
    physic.setEnable(!isDead());
  }

bool Npc::recoverRagdoll() {
  if(!ragdoll)
    return true;
  if(isDead())
    return false;
  checkRagdollQuestHazard();
  const bool returnToAnchor = needsRagdollQuestReturn();
  if(!returnToAnchor && !ragdoll->isSleeping() && (owner.tickCount()-ragdollStartTick<500 || !ragdoll->settled()))
    return false;

  const bool onBack = ragdoll->onBack();
  const auto pelvis = ragdoll->pelvisTransform();
  Vec3 at(pelvis.at(3,0),pelvis.at(3,1),pelvis.at(3,2));
  const auto floor = owner.physic()->landRay(at+Vec3(0,80,0),380.f);
  if(floor.hasCol && floor.n.y>0.25f)
    at.y = floor.v.y;

  if(returnToAnchor) {
    ragdollQuestFrom = position();
    at = ragdollQuestReturn.anchor;
    ++ragdollQuestReturns;
    }

  // Rebase the animated frame while the physical overlay still holds the shown pose.
  const auto physicalRoot = ragdoll->rootTransform();
  const float fx = physicalRoot.at(2,0), fz = physicalRoot.at(2,2);
  if(returnToAnchor)
    angle = ragdollQuestReturn.angle;
  else if(fx*fx+fz*fz>1e-6f)
    angle = 90.f-std::atan2(fx,fz)*180.f/float(M_PI);
  x = at.x; y = at.y; z = at.z;
  physic.setPosition(at);
  ragdollObjectMatrix.identity();
  ragdollObjectMatrix.translate(x,y,z);
  ragdollObjectMatrix.rotateOY(90-angle);
  ragdollObjectMatrix.scale(sz[0],sz[1],sz[2]);
  visual.setObjMatrix(ragdollObjectMatrix,false);
  visual.pose().stopAllAnim();
  const char* wounded = onBack ? "S_WOUNDEDB" : "S_WOUNDED";
  if(visual.startAnimAndGet(*this,wounded,calcAniComb(),BS_UNCONSCIOUS)==nullptr) {
    // Creatures without wounded poses retain the standard idle fallback.
    visual.startAnimAndGet("S_RUN",owner.tickCount(),true);
    }
  // Do not interpolate a world-space corpse pose across the whole ravine.
  releaseRagdoll(!returnToAnchor);
  ragdollDormant.reset();
  if(!returnToAnchor)
    ragdollQuestReturn.armed = false;
  ragdollRecovering = true;
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  mvAlgo.clearSpeed();
  durtyTranform = TR_Pos|TR_Rot|TR_Scale;
  return true;
  }

void Npc::updateRagdollQuestAnchor() {
  if(isPlayer() || !isHuman() || isDown() || ragdoll || interactive()!=nullptr ||
     ragdollQuestReturn.armed ||
     !Gothic::physicsFlags().questGuard ||
     !Gothic::physicsFlags().ragdoll || mvAlgo.state()!=MoveAlgo::Run)
    return;
  const auto now = owner.tickCount();
  if(ragdollQuestReturn.hasAnchor && now>=ragdollQuestSampleTick && now-ragdollQuestSampleTick<250)
    return;
  ragdollQuestSampleTick = now;
  const auto at = position();
  const auto floor = owner.physic()->landRay(at+Vec3(0,15,0),45.f);
  if(!floor.hasCol || floor.n.y<0.7f || std::abs(floor.v.y-at.y)>30.f)
    return;
  const auto water = owner.physic()->waterRay(at,15.f);
  if(water.hasCol)
    return;
  ragdollQuestReturn.anchor = {at.x,floor.v.y,at.z};
  ragdollQuestReturn.angle = angle;
  ragdollQuestReturn.hasAnchor = true;
  }

bool Npc::needsRagdollQuestReturn() const {
  return ragdollQuestReturn.armed && ragdollQuestReturn.hasAnchor && ragdollQuestReturn.hazards!=0 &&
         !isPlayer() && isHuman() && !isDead() && Gothic::physicsFlags().questGuard;
  }

void Npc::checkRagdollQuestHazard() {
  if(!ragdollQuestReturn.armed || !Gothic::physicsFlags().questGuard)
    return;
  auto at = position();
  if(ragdoll) {
    const auto pelvis = ragdoll->pelvisTransform();
    at = Vec3(pelvis.at(3,0),pelvis.at(3,1),pelvis.at(3,2));
    }
  if(at.y<ragdollQuestReturn.anchor.y-RagdollSave::QuestReturn::maxFall)
    ragdollQuestReturn.hazards |= RagdollSave::QuestReturn::fell;
  if((ragdollQuestReturn.hazards & RagdollSave::QuestReturn::water)==0) {
    const auto water = owner.physic()->waterRay(at,60.f);
    if(water.hasCol) {
      // The padded water ray can start below a dry bank or bridge. A solid
      // support between the pelvis and that lower surface keeps the body dry.
      const auto support = water.wdepth<at.y ? owner.physic()->landRay(at,at.y-water.wdepth) : DynamicWorld::RayLandResult();
      if(!support.hasCol || support.v.y<=water.wdepth)
        ragdollQuestReturn.hazards |= RagdollSave::QuestReturn::water;
      }
    }
  }

bool Npc::recoverSpellThrow() {
  if(!ragdollQuestSpellThrow || ragdoll || isDown() || !ragdollQuestReturn.armed)
    return false;
  checkRagdollQuestHazard();
  const bool returnToAnchor = needsRagdollQuestReturn();
  ragdollQuestReturn.armed = false;
  ragdollQuestSpellThrow = false;
  if(!returnToAnchor)
    return false;
  ragdollQuestFrom = position();
  ++ragdollQuestReturns;
  angle = ragdollQuestReturn.angle;
  const auto at = ragdollQuestReturn.anchor;
  x = at.x; y = at.y; z = at.z;
  physic.setPosition(at);
  living.reset();
  arrowImpacts.clear();
  mvAlgo.takeRagdollControl();
  ragdollHasLastPosition = false;
  ragdollLastVelocity = {};
  setAnim(Anim::Idle);
  durtyTranform = TR_Pos|TR_Rot|TR_Scale;
  return true;
  }

void Npc::prepareRagdoll() {
  if(!isDown() || !Gothic::physicsFlags().spellPhysics)
    pendingSpellImpulse = {};
  if(!Gothic::physicsFlags().spellPhysics || (!isDown() && !isInAir()))
    spellImpulseCulprit.reset();
  if(!Gothic::physicsFlags().arrowSticking)
    visual.clearStuckArrows();
  if(!Gothic::physicsFlags().arrowPhysics)
    arrowImpacts.clear();
  if(!ragdollQuestSpellThrow && !ragdoll && !isDown() &&
     ragdollQuestReturn.armed && ragdollQuestReturn.hazards!=0) {
    if(!needsRagdollQuestReturn()) {
      ragdollQuestReturn.armed = false;
      }
    else if(visual.pose().isStanding()) {
      // Finish at the dry point too: get-up root motion must not carry a quest
      // NPC straight over the same edge again. 'armed' also survives a mid-rise save.
      const auto at = ragdollQuestReturn.anchor;
      x = at.x; y = at.y; z = at.z;
      physic.setPosition(at);
      mvAlgo.clearSpeed();
      durtyTranform |= TR_Pos;
      ragdollQuestReturn.armed = false;
      }
    }
  updateRagdollQuestAnchor();
  const bool enabled = Gothic::physicsFlags().ragdoll;
  if(!enabled) {
    if(isDown())
      arrowImpacts.clear();
    if(ragdollHasLastPosition) {
      ragdollHasLastPosition = false;
      ragdollLastVelocity = {};
      }
    if(ragdoll) {
      ragdollDormant = ragdoll->state();
      // Legacy fall alignment expects the actor origin at the floor. Rebase while
      // the overlay still holds the shown world pose, keeping the dormant body fixed.
      const auto pelvis = ragdoll->pelvisTransform();
      const Vec3 at(x,pelvis.at(3,1),z);
      const auto floor = owner.physic()->landRay(at+Vec3(0,80,0),380.f);
      if(floor.hasCol && floor.n.y>0.25f)
        y = floor.v.y;
      ragdoll.reset();
      physic.setPosition(position());
      visual.setObjMatrix(mkPositionMatrix(),false);
      releaseRagdoll(true);
      physic.setEnable(!isDead());
      mvAlgo.clearSpeed();
      durtyTranform = TR_Pos|TR_Rot|TR_Scale;
      }
    if(!isDown()) {
      if(ragdollDormant)
        physic.setEnable(true);
      ragdollDormant.reset();
      ragdollLegacyDown = false;
      ragdollRecovering = false;
      }
    return;
    }
  if(!isDown()) {
    ragdollLegacyDown = false;
    ragdollDormant.reset();
    if(ragdoll)
      recoverRagdoll();
    else {
      ragdollRecovering = false;
      physic.setEnable(true);
      }
    const auto now = owner.tickCount();
    if(ragdollHasLastPosition && now>ragdollLastTick && now-ragdollLastTick<=250) {
      ragdollLastVelocity = (position()-ragdollLastPosition)*(1000.f/float(now-ragdollLastTick));
      const float speed = ragdollLastVelocity.length();
      if(!std::isfinite(speed))
        ragdollLastVelocity = {}; else if(speed>3000.f)
        ragdollLastVelocity *= 3000.f/speed;
      } else if(!ragdollHasLastPosition || now!=ragdollLastTick) {
      ragdollLastVelocity = {};
      }
    ragdollLastPosition = position();
    ragdollLastTick = now;
    ragdollHasLastPosition = true;
    return;
    }
  if(ragdoll || ragdollLegacyDown || (ragdollRecovering && !ragdollDormant)) {
    if(ragdoll)
      dispatchArrowPhysics(*ragdoll);
    else
      arrowImpacts.clear();
    return;
    }
  if(!createRagdoll()) {
    ragdollLegacyDown = true;
    arrowImpacts.clear();
    Log::e("ragdoll: unsupported skeleton or body state for ",displayName());
    }
  }

Npc::MeleeBenchMode Npc::meleeBenchMode() const {
  if(meleeBench!=MeleeBenchMode::Off)
    return meleeBench;
  static const MeleeBenchMode runnerMode = [] {
    const auto* value = std::getenv("OPENGOTHIC_MELEE_BENCH");
    if(value!=nullptr && std::string_view(value)=="blade-motors")
      return MeleeBenchMode::Full;
    if(value!=nullptr && std::string_view(value)=="blade")
      return MeleeBenchMode::Blade;
    return MeleeBenchMode::Off;
    }();
  if(runnerMode==MeleeBenchMode::Off)
    return MeleeBenchMode::Off;
  return BodyObservations::watched(*this) ? runnerMode : MeleeBenchMode::Off;
  }

void Npc::configureMeleeBench(MeleeBenchMode mode, bool dynamicPelvis) {
  const auto* value = std::getenv("OPENGOTHIC_MELEE_BENCH");
  if(value==nullptr || std::string_view(value)!="fixture")
    return;
  meleeBench = mode;
  meleeBenchDynamicPelvis = dynamicPelvis;
  living.reset();
  livingRetryTick = 0;
  livingNeedsRebuild = false;
  prepareLivingBody();
  }

void Npc::updateMeleeBenchBlade() {
  const auto mode = meleeBenchMode();
  if(mode==MeleeBenchMode::Off || mode==MeleeBenchMode::Base || mode==MeleeBenchMode::Chain)
    return;
  // Full fixture takes isolate motors. Runner full takes also include their blade.
  if(mode==MeleeBenchMode::Full && meleeBench!=MeleeBenchMode::Off)
    return;
  const auto* weapon = invent.activeWeapon();
  const auto* skeleton = visual.visualSkeleton();
  if(weapon==nullptr || skeleton==nullptr || weapon->swordLength()<=0)
    return;
  const auto hand = skeleton->findNode("ZS_RIGHTHAND");
  if(hand<visual.pose().boneCount()) {
    const auto* mesh=Resources::loadMesh(weapon->handle().visual);
    if(mesh==nullptr || mesh->bbox()[0].x>=-4.f)
      throw std::runtime_error("Melee benchmark requires a weapon mesh extending along local -X");
    owner.physic()->updateMeleeBlade(this,visual.pose().bone(hand),-mesh->bbox()[0].x);
    }
  }

void Npc::syncMeleeShadowSwing() {
  const auto mode = weaponState();
  if(meleeShadowSuspended || !Gothic::physicsFlags().meleeBladeShadow || !isHuman() || isDown() ||
     (mode!=WeaponState::Fist && mode!=WeaponState::W1H && mode!=WeaponState::W2H)) {
    finishMeleeShadowSwing();
    return;
    }
  const auto& pose = visual.pose();
  std::optional<Pose::CombatLayer> attack;
  for(const auto& layer:pose.combatLayers(owner.tickCount()))
    if(layer.sequence->isAttackAnim())
      attack = layer;
  if(!attack) {
    finishMeleeShadowSwing();
    return;
    }
  const auto* skeleton = visual.visualSkeleton();
  if(skeleton==nullptr)
    return;
  const auto* item = mode==WeaponState::Fist ? nullptr : invent.activeWeapon();
  float length = 12.f;
  bool normalY = true;
  if(item!=nullptr) {
    const auto* mesh = Resources::loadMesh(item->handle().visual);
    if(mesh==nullptr || mesh->bbox()[0].x>=-4.f)
      throw std::runtime_error("Melee shadow requires a weapon mesh extending along local -X");
    length = -mesh->bbox()[0].x;
    const auto extent = mesh->bbox()[1]-mesh->bbox()[0];
    normalY = extent.y<=extent.z;
    }
  if(mode!=WeaponState::Fist && item==nullptr)
    return;
  if(!meleeShadow)
    meleeShadow = std::make_unique<MeleeShadow>();
  auto& shadow = *meleeShadow;
  const auto* seq = attack->sequence;
  const auto& data = *seq->data;
  const auto combo = uint32_t(pose.comboLength());
  const auto cached = [&](const MeleeShadow::Window& w) {
    return w.sequence==seq && w.length==length && w.normalY==normalY;
    };
  if(std::none_of(shadow.windows.begin(),shadow.windows.end(),cached)) {
    // Sample the actual MAN hand path in the actor's frame. Locomotion cannot
    // open a stationary blade's window. Adjacent optimal tags partition combos.
    const auto root = skeleton->findRootNode();
    const auto right = skeleton->findNode(mode==WeaponState::Fist ? "BIP01 R HAND" : "ZS_RIGHTHAND");
    const auto left = skeleton->findNode("BIP01 L HAND");
    if(right>=pose.boneCount() || root>=pose.boneCount() || data.numFrames<2 || data.fpsRate<=0)
      throw std::runtime_error("Melee shadow requires a sampled humanoid attack and hand bone");
    std::vector<uint32_t> optimal;
    for(const auto& event:data.events)
      if(event.type==zenkit::MdsEventType::OPTIMAL_FRAME)
        for(const auto frame:event.frames) {
          // The same clamp as Sequence::processEvents, including truncated MAN ranges.
          const auto clamped = frame<int32_t(data.firstFrame) ? 0u :
                               (int64_t(frame)>=int64_t(data.firstFrame)+data.numFrames ? data.numFrames-1 :
                                (frame>int32_t(data.lastFrame) ? data.lastFrame-data.firstFrame : uint32_t(frame)-data.firstFrame));
          optimal.push_back(seq->reverse ? data.numFrames-1-clamped : clamped);
          }
    std::sort(optimal.begin(),optimal.end());
    optimal.erase(std::unique(optimal.begin(),optimal.end()),optimal.end());
    std::vector<float> rightSpeed(data.numFrames,0), leftSpeed(data.numFrames,0);
    const auto sampleSpeeds = [&](size_t hand,std::vector<float>& speeds) {
      Vec3 previous;
      for(uint32_t frame=0;frame<data.numFrames;++frame) {
        const float progress = float(frame)/float(data.numFrames-1);
        Matrix4x4 matrix;
        if(!pose.animRelative(seq->name,seq->reverse ? 1.f-progress : progress,root,hand,matrix))
          throw std::runtime_error("Cannot sample melee shadow attack hand");
        Vec3 point(mode==WeaponState::Fist ? 0.f : -length,0,0);
        matrix.project(point);
        if(frame>0)
          speeds[frame] = (point-previous).length()*data.fpsRate;
        previous = point;
        }
      speeds[0] = speeds[1];
      };
    sampleSpeeds(right,rightSpeed);
    if(mode==WeaponState::Fist && left<pose.boneCount())
      sampleSpeeds(left,leftSpeed);
    for(uint32_t index=0;index<optimal.size();++index) {
      const auto opt = optimal[index];
      const uint32_t lo = index==0 ? 0 : (optimal[index-1]+opt)/2+1;
      const uint32_t hi = index+1==optimal.size() ? data.numFrames-1 : (opt+optimal[index+1])/2;
      const float rpeak = *std::max_element(rightSpeed.begin()+lo,rightSpeed.begin()+hi+1);
      const float lpeak = *std::max_element(leftSpeed.begin()+lo,leftSpeed.begin()+hi+1);
      const bool useLeft = mode==WeaponState::Fist && lpeak>rpeak;
      const auto& speeds = useLeft ? leftSpeed : rightSpeed;
      const float peak = std::max(rpeak,lpeak);
      const float threshold = std::max(100.f,peak*.25f);
      uint32_t first = opt, last = opt;
      while(first>lo && speeds[first]>=threshold)
        --first;
      while(last<hi && speeds[last+1]>=threshold)
        ++last;
      // A low-speed optimal frame still has its own adjacent sample interval.
      if(first==last) {
        first = first>lo ? first-1 : first;
        last = last<hi ? last+1 : last;
        }
      const uint32_t end = seq->reverse ? std::max(last+1,opt+2) : last+1;
      shadow.windows.push_back({seq,length,index,first,opt,last,float(first)*1000.f/data.fpsRate,
                                float(opt)*1000.f/data.fpsRate,float(end)*1000.f/data.fpsRate,
                                peak,threshold,useLeft,normalY,float(opt+(seq->reverse ? 1u : 0u))*1000.f/data.fpsRate});
      }
    }
  const MeleeShadow::Window* window = nullptr;
  for(const auto& w:shadow.windows)
    if(cached(w) && (window==nullptr || (w.index<=combo && w.index>window->index)))
      window = &w;
  if(window==nullptr)
    return;
  if(shadow.window && shadow.window->sequence==seq && shadow.window->index==window->index &&
     shadow.start==attack->start && shadow.window->length==length && shadow.window->normalY==normalY)
    return;
  const auto selected = *window;
  finishMeleeShadowSwing(false);
  shadow.window = selected;
  ++shadow.id;
  shadow.start = attack->start;
  shadow.tick = owner.tickCount();
  shadow.speed = attack->speed;
  shadow.attacker = meleeShadowWho(this);
  shadow.animation = seq->name;
  const auto* weaponSymbol = item!=nullptr ? owner.script().findSymbol(item->clsId()) : nullptr;
  shadow.weapon = "{\"mode\":" + TestBench::bodyJson(mode==WeaponState::Fist ? "fist" :
                                                   (mode==WeaponState::W1H ? "1h" : "2h")) +
                  ",\"name\":" + TestBench::bodyJson(weaponSymbol!=nullptr ? std::string_view(weaponSymbol->name()) : std::string_view("unarmed")) +
                  ",\"visual\":" + TestBench::bodyJson(item==nullptr ? "" : item->handle().visual) +
                  ",\"length_cm\":" + std::to_string(length) + "}";
  shadow.legacyTarget = "null";
  shadow.legacyNpc = nullptr;
  shadow.legacyResult = "no_optimal_frame";
  shadow.legacyHit = shadow.legacyBlock = shadow.legacyDodge = false;
  shadow.damage = 0;
  }

void Npc::updateMeleeShadow() {
  if(meleeShadowSuspended || !Gothic::physicsFlags().meleeBladeShadow) {
    clearMeleeShadow();
    return;
    }
  std::vector<HitZones::Posed> zones;
  visual.hitZonesPosed(zones);
  std::vector<MeleeBodyCapsule> body;
  body.reserve(zones.size());
  for(const auto& capsule:zones)
    body.push_back({capsule.a,capsule.b,capsule.r,capsule.bone,uint8_t(capsule.zone)});
  owner.physic()->updateShadowBody(this,body);
  meleeShadowBody = true;
  syncMeleeShadowSwing();
  if(!meleeShadow || !meleeShadow->window)
    return;
  auto& shadow = *meleeShadow;
  const auto& pose = visual.pose();
  const auto& window = *shadow.window;
  const auto bonePoint = [&](size_t bone,Vec3 point) {
    pose.bone(bone).project(point);
    return point;
    };
  std::vector<MeleeBladeSegment> blade;
  if(weaponState()==WeaponState::Fist) {
    const auto hand = pose.findNode(window.left ? "BIP01 L HAND" : "BIP01 R HAND");
    const auto forearm = pose.findNode(window.left ? "BIP01 L FOREARM" : "BIP01 R FOREARM");
    if(hand<pose.boneCount() && forearm<pose.boneCount()) {
      const auto fist = bonePoint(hand,{});
      const auto elbow = bonePoint(forearm,{});
      auto normal = bonePoint(hand,{0,1,0})-fist;
      const auto finger = pose.findNode(window.left ? "BIP01 L FINGER0" : "BIP01 R FINGER0");
      const auto knuckle = finger<pose.boneCount() ? bonePoint(finger,{}) : fist;
      blade.push_back({fist,knuckle,normal,6.f});
      blade.push_back({elbow,fist,normal,4.f});
      }
    }
  else {
    const auto hand = pose.findNode("ZS_RIGHTHAND");
    if(hand<pose.boneCount()) {
      const auto grip = bonePoint(hand,{});
      const Vec3 normal = window.normalY ? Vec3(0,1,0) : Vec3(0,0,1);
      blade.push_back({grip,bonePoint(hand,{-window.length,0,0}),bonePoint(hand,normal)-grip,2.f});
      }
    }
  // Publish the whole pose interval. Physics clips it to the authored window,
  // including frames whose endpoint is already past the end of that window.
  const double activeBegin = double(shadow.start)+double(window.beginMs)/shadow.speed;
  const double activeEnd   = double(shadow.start)+double(window.endMs)/shadow.speed;
  owner.physic()->updateShadowBlade(this,shadow.id,blade,owner.tickCount(),activeBegin,activeEnd);
  if(Gothic::inst().doMeleeBladeDraw()) {
    const double now = double(owner.tickCount());
    meleeShadowDrawPose   = blade;
    meleeShadowDrawActive = now>=activeBegin && now<activeEnd;
    }
  auto contacts = owner.physic()->takeShadowBladeContacts(this);
  noteMeleeShadowMarks(contacts);
  for(const auto& hit:contacts)
    shadow.contacts.push_back({hit,meleeShadowWho(hit.npc),meleeShadowBone(hit)});
  }

void Npc::noteMeleeShadowMarks(const std::vector<MeleeBladeContact>& contacts) {
  if(!Gothic::inst().doMeleeBladeDraw())
    return;
  const uint64_t now = owner.tickCount();
  for(const auto& hit:contacts) {
    std::string label = hit.world ? std::string("world") : std::string(HitZones::name(HitZones::Zone(hit.zone)));
    if(!hit.world)
      label += " " + std::to_string(int(hit.speedCmS)) + "cm/s";
    meleeShadowMarks.push_back({hit.point,now,hit.world,std::move(label)});
    }
  constexpr size_t maxMarks = 16;
  if(meleeShadowMarks.size()>maxMarks)
    meleeShadowMarks.erase(meleeShadowMarks.begin(),meleeShadowMarks.end()-maxMarks);
  }

void Npc::drawMeleeShadow(DbgPainter& p, uint64_t now) const {
  constexpr uint64_t markLifetime = 3000;
  if(meleeShadow && meleeShadow->window) {
    // Yellow while the blade can hit, grey before and after the active window.
    p.setPen(meleeShadowDrawActive ? Tempest::Color(1,1,0) : Tempest::Color(0.6f,0.6f,0.6f));
    for(const auto& segment:meleeShadowDrawPose) {
      p.drawLine(segment.from,segment.to);
      p.drawPoint(segment.to,3);
      }
    }
  for(const auto& mark:meleeShadowMarks) {
    if(now>mark.time+markLifetime)
      continue;
    // Red: a body part was hit; blue: the world stopped the swing.
    p.setBrush(mark.world ? Tempest::Color(0.2f,0.4f,1) : Tempest::Color(1,0,0));
    p.setPen(mark.world ? Tempest::Color(0.2f,0.4f,1) : Tempest::Color(1,0,0));
    p.drawPoint(mark.point,6);
    p.drawText(mark.point,mark.label);
    }
  }

void Npc::clearMeleeShadow() {
  finishMeleeShadowSwing();
  if(meleeShadowBody && owner.physic()!=nullptr) {
    owner.physic()->removeShadowBody(this);
    meleeShadowBody = false;
    }
  }

void Npc::finishMeleeShadowSwing(bool remove) {
  if(!meleeShadow || !meleeShadow->window || owner.physic()==nullptr)
    return;
  auto& shadow = *meleeShadow;
  auto contacts = owner.physic()->takeShadowBladeContacts(this);
  noteMeleeShadowMarks(contacts);
  meleeShadowDrawPose.clear();
  meleeShadowDrawActive = false;
  for(const auto& hit:contacts)
    shadow.contacts.push_back({hit,meleeShadowWho(hit.npc),meleeShadowBone(hit)});
  if(remove)
    owner.physic()->removeShadowBlade(this);
  const auto& window = *shadow.window;
  bool physicalHit = false, sameTarget = false, extraTarget = false, worldHit = false;
  std::string physical = "[", world = "null";
  for(const auto& entry:shadow.contacts) {
    const auto& contact = entry.hit;
    const auto point = meleeShadowPoint(contact.point);
    if(contact.world) {
      worldHit = true;
      world = "{\"point_cm\":" + point + ",\"step\":" + std::to_string(contact.step) + "}";
      continue;
      }
    physicalHit = true;
    const bool same = contact.npc!=nullptr && contact.npc==shadow.legacyNpc;
    sameTarget |= same;
    extraTarget |= !same;
    if(physical.size()>1)
      physical += ',';
    physical += "{\"target\":" + entry.target +
                ",\"part\":" + TestBench::bodyJson(HitZones::name(HitZones::Zone(contact.zone))) +
                ",\"bone\":" + std::to_string(contact.bone) + ",\"bone_name\":" + TestBench::bodyJson(entry.bone) +
                ",\"point_cm\":" + point +
                ",\"blade_fraction\":" + std::to_string(contact.bladeFraction) +
                ",\"speed_cm_s\":" + std::to_string(contact.speedCmS) +
                ",\"surface_angle_deg\":" + std::to_string(contact.surfaceAngleDeg) +
                ",\"step\":" + std::to_string(contact.step) + "}";
    }
  physical += ']';
  std::string comparison = "null";
  if(worldHit && shadow.legacyHit && !sameTarget)
    comparison = "\"world_before_target\"";
  else if(physicalHit && (extraTarget || !sameTarget) && shadow.legacyHit)
    comparison = "\"different_or_extra_target\"";
  else if(physicalHit && shadow.legacyHit)
    comparison = "\"same_target\"";
  else if(physicalHit)
    comparison = "\"physics_only\"";
  else if(shadow.legacyHit)
    comparison = "\"legacy_only\"";
  const auto number = [](auto value) { return std::to_string(value); };
  writeMeleeShadow("{\"swing\":" + number(shadow.id) + ",\"world\":" + TestBench::bodyJson(owner.name()) +
                  ",\"game_time_ms\":" + number(shadow.tick) + ",\"end_time_ms\":" + number(owner.tickCount()) +
                  ",\"attacker\":" + shadow.attacker + ",\"weapon\":" + shadow.weapon +
                  ",\"animation\":" + TestBench::bodyJson(shadow.animation) +
                  ",\"window\":{\"combo_index\":" + number(window.index) +
                  ",\"first_frame\":" + number(window.first) + ",\"optimal_frame\":" + number(window.optimal) +
                  ",\"last_frame\":" + number(window.last) + ",\"begin_ms\":" + number(window.beginMs) +
                  ",\"optimal_ms\":" + number(window.optimalMs) + ",\"end_ms\":" + number(window.endMs) +
                  ",\"event_boundary_ms\":" + number(window.eventBoundaryMs) +
                  ",\"reverse\":" + (window.sequence->reverse ? "true" : "false") +
                  ",\"animation_speed\":" + number(shadow.speed) + ",\"peak_cm_s\":" + number(window.peakCmS) +
                  ",\"threshold_cm_s\":" + number(window.thresholdCmS) + "},\"legacy\":{\"target\":" +
                  shadow.legacyTarget + ",\"hit\":" + (shadow.legacyHit ? "true" : "false") +
                  ",\"damage\":" + number(shadow.damage) + ",\"blocked\":" + (shadow.legacyBlock ? "true" : "false") +
                  ",\"dodged\":" + (shadow.legacyDodge ? "true" : "false") +
                  ",\"result\":" + TestBench::bodyJson(shadow.legacyResult) +
                  "},\"physical\":{\"targets\":" + physical + ",\"world\":" + world +
                  "},\"comparison\":" + comparison + "}");
  shadow.contacts.clear();
  shadow.window.reset();
  }

void Npc::prepareLivingBody() {
  constexpr float enterRadius = 2000.f, leaveRadius = 2200.f;
  const auto benchMode = meleeBenchMode();
  const bool benchRig = benchMode==MeleeBenchMode::Full || benchMode==MeleeBenchMode::Chain || benchMode==MeleeBenchMode::Blade;
  if(currentInteract!=nullptr)
    livingWasInteracting = true;
  else if(visual.pose().isStanding())
    livingWasInteracting = false;
  else if(visual.pose().hasState(BS_MOBINTERACT))
    livingWasInteracting = true;
  else if(!visual.pose().hasState(BS_CLIMB))
    livingWasInteracting = false;
  // Mob clips align their bones independently of the walking root. Their short
  // pose changes are real animation, but are not contact velocities. Detach can
  // clear currentInteract before ToStand finishes; retain the suspension until
  // that clip ends or ordinary movement interrupts it. Idle successors can keep
  // the interaction body-state tag, so recognize their standing pose explicitly.
  const bool interacting = currentInteract!=nullptr || livingWasInteracting;
  const bool frozen = spellFrozen();
  if(frozen!=livingWasFrozen) {
    // Enter with a completely kinematic rig, discarding any active hit reaction.
    // On thaw the ordinary living-body policy decides whether this rig is still needed.
    living.reset();
    livingRetryTick = 0;
    livingNeedsRebuild = false;
    livingWasFrozen = frozen;
    arrowImpacts.clear();
    }
  const auto* hero = owner.player();
  // Population actors retain the same contact rig as actors near the hero.
  const bool outside = !NavDemos::fullSimulation(*this) && hero!=nullptr &&
                       (living ? qDistTo(*hero)>leaveRadius*leaveRadius
                               : qDistTo(*hero)>=enterRadius*enterRadius);
  if(benchMode==MeleeBenchMode::Base || isDown() || ragdoll || interacting || (!benchRig && !frozen && (isPlayer() || hero==nullptr ||
     !Gothic::physicsFlags().livingBodies || outside))) {
    living.reset();
    livingRetryTick = 0;
    livingNeedsRebuild = false;
    arrowImpacts.clear();
    return;
    }
  if(frozen)
    arrowImpacts.clear();
  if(livingNeedsRebuild || (!benchRig && living && !living->isKinematic() && !Gothic::physicsFlags().arrowPhysics)) {
    living.reset();
    livingNeedsRebuild = false;
    }
  if(living) {
    if(!frozen)
      dispatchArrowPhysics(*living);
    return;
    }
  if(owner.tickCount()<livingRetryTick) {
    arrowImpacts.clear();
    return;
    }
  const auto* skeleton = visual.visualSkeleton();
  if(skeleton==nullptr || !visual.hitZoneShape().isValid()) {
    if(benchRig) throw std::runtime_error("Melee benchmark requires a skeleton with fitted HitZones");
    arrowImpacts.clear();
    return;
    }
  // Only birth needs a forced solve. Subsequent targets come from the normal
  // animation update, including IK/aim/flinch, without solving the pose twice.
  updateAnimation(0,true);
  living = owner.physic()->createKinematicBody(*skeleton,visual.hitZoneShape(),visual.pose().transform(),
                                             visual.pose().boneCount(),visual.transform());
  if(benchRig && !living)
    throw std::runtime_error("melee benchmark requires a valid 24-part living body");
  if(living) {
    living->setNpc(this);
    if(benchMode==MeleeBenchMode::Full || benchMode==MeleeBenchMode::Chain)
      if(!living->configureBench(benchMode==MeleeBenchMode::Full ? Ragdoll::BenchMode::Full : Ragdoll::BenchMode::Chain,
                                meleeBenchDynamicPelvis ? Ragdoll::BenchPelvis::Dynamic : Ragdoll::BenchPelvis::Kinematic))
        throw std::runtime_error("melee benchmark motor configuration failed");
    if(const auto culprit = spellImpulseCulprit.lock())
      living->setNoiseCulprit(static_cast<Npc*>(culprit->user_ptr));
    }
  else
    livingRetryTick = owner.tickCount()+1000;
  if(living && !frozen)
    dispatchArrowPhysics(*living);
  else
    arrowImpacts.clear();
  }

bool Npc::stickArrow(MeshObjects::Mesh&& arrow, size_t bone, const Matrix4x4& worldMatrix,
                     std::string_view visualName) {
  // A corpse hit is selected after the native step, before the usual pose export.
  // Bind against that same current pose so the first sync cannot move the pin twice.
  if(ragdoll)
    syncRagdoll();
  if(bone>=visual.pose().boneCount()) {
    const auto& transform = visual.transform();
    const float scale = Vec3(transform.at(0,0),transform.at(0,1),transform.at(0,2)).length();
    const Vec3 at(worldMatrix.at(3,0),worldMatrix.at(3,1),worldMatrix.at(3,2));
    bone = visual.hitZoneShape().closestBone(visual.pose().transform(),visual.pose().boneCount(),scale,at);
    }
  return visual.stickArrow(std::move(arrow),bone,worldMatrix,visualName);
  }

void Npc::addCorpseArrowImpulse(const Bullet& bullet) {
  if(ragdoll && isDown())
    queueArrowPhysics(bullet);
  }

void Npc::queueArrowPhysics(const Bullet& bullet) {
  if(bullet.isSpell() || !Gothic::physicsFlags().arrowPhysics)
    return;
  const auto direction = bullet.direction(); // cm/ms, as used by the projectile integrator
  const float speed = direction.length();
  const auto point = bullet.impactPoint();
  if(!std::isfinite(speed) || speed<1.e-3f ||
     !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
    return;
  size_t bone = bullet.impactBone();
  if(bone>=visual.pose().boneCount()) {
    const auto& transform = visual.transform();
    const float scale = Vec3(transform.at(0,0),transform.at(0,1),transform.at(0,2)).length();
    bone = visual.hitZoneShape().closestBone(visual.pose().transform(),visual.pose().boneCount(),scale,point);
    }
  if(bone>=visual.pose().boneCount())
    return;
  // An 80 g projectile: cm/ms -> m/s gives momentum in N*s. Draw and weapon
  // strength already determine the actual projectile speed; damage dice do not.
  const float momentum = std::clamp(0.08f*speed*10.f,0.2f,12.f);
  const float strength = std::clamp(momentum/8.f,0.1f,1.f);
  const Vec3 impulse = direction*(momentum/speed);
  arrowImpacts.push_back({bone,point,impulse,strength,
                           bullet.origin()!=nullptr ? bullet.origin()->handlePtr() : std::weak_ptr<zenkit::INpc>()});
  arrowPhysics.bone = bone;
  arrowPhysics.point = point;
  arrowPhysics.impulseNs = impulse;
  arrowPhysics.strength = strength;
  ++arrowPhysics.hits;
  }

void Npc::dispatchArrowPhysics(Ragdoll& body) {
  for(const auto& hit:arrowImpacts) {
    const auto culprit = hit.culprit.lock();
    if(body.queueHit(hit.bone,hit.point,hit.impulseNs,hit.strength,culprit ? reinterpret_cast<Npc*>(culprit->user_ptr) : nullptr)) {
      ++arrowPhysics.applied;
      if(&body==ragdoll.get())
        ++arrowPhysics.corpseImpulses;
      }
    }
  arrowImpacts.clear();
  }

void Npc::syncRagdoll() {
  const bool hasPose=[&] { FrameProfile::Scope timing(FrameProfile::Zone::NativeExport,FrameProfile::watchedActor(this));
    return ragdoll && ragdoll->getPose(ragdollPose); }();
  if(!hasPose)
    return;
  ragdollObjectMatrix = ragdoll->rootTransform();
  x = ragdollObjectMatrix.at(3,0);
  y = ragdollObjectMatrix.at(3,1);
  z = ragdollObjectMatrix.at(3,2);
  checkRagdollQuestHazard();
  physic.setPosition(position());
  visual.pose().setRagdollPose(visual.visualSkeleton(),ragdollPose,ragdoll->poseIndices());
  durtyTranform = TR_Pos|TR_Rot|TR_Scale;
  { FrameProfile::Scope timing(FrameProfile::Zone::ForcedSyncAnimation,FrameProfile::watchedActor(this)); updateAnimation(0,true); }
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

const Animation::Sequence* Npc::playAnimByName(std::string_view name, BodyState bs) {
  return visual.startAnimAndGet(*this,name,calcAniComb(),bs);
  }

bool Npc::setAnim(Npc::Anim a) {
  return setAnimAngGet(a)!=nullptr;
  }

const Animation::Sequence* Npc::setAnimAngGet(Anim a) {
  return setAnimAngGet(a,calcAniComb());
  }

const Animation::Sequence* Npc::setAnimAngGet(Anim a, uint8_t comb) {
  const bool trace = TestBench::bodyTracing() && currentTarget!=nullptr;
  const auto before = trace ? combatPoseIdentity() : std::string();
  auto st  = weaponState();
  auto wlk = walkMode();
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
  if(trace)
    traceCombatAttempt(a,result!=nullptr,before,"set_anim");
  return result;
  }

void Npc::setAnimRotate(int rot) {
  visual.setAnimRotate(*this,rot);
  }

bool Npc::setAnimItem(std::string_view scheme, int state) {
  if(scheme.empty())
    return true;
  if(bodyStateMasked()!=BS_STAND) {
    setAnim(Anim::Idle);
    return false;
    }
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
  return visual.faceWeight(anim);
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

void Npc::setTalentSkill(Talent t, int32_t lvl) {
  if(t>=TALENT_MAX_G2)
    return;
  talentsSk[t] = lvl;
  invalidateTalentOverlays(t);
  }

int32_t Npc::talentSkill(Talent t) const {
  if(t<TALENT_MAX_G2)
    return talentsSk[t];
  return 0;
  }

void Npc::setTalentValue(Talent t, int32_t lvl) {
  if(t<TALENT_MAX_G2)
    talentsVl[t] = lvl;
  }

int32_t Npc::talentValue(Talent t) const {
  if(t<TALENT_MAX_G2)
    return talentsVl[t];
  return 0;
  }

int32_t Npc::hitChance(Talent t) const {
  if(t<zenkit::INpc::hitchance_count)
    return hnpc->hitchance[t];
  return 0;
  }

bool Npc::isRefuseTalk() const {
  return refuseTalkMilis>=owner.tickCount();
  }

int32_t Npc::mageCycle() const {
  return talentSkill(TALENT_MAGE);
  }

bool Npc::canSneak() const {
  return talentSkill(TALENT_SNEAK)!=0;
  }

void Npc::setRefuseTalk(uint64_t milis) {
  refuseTalkMilis = owner.tickCount()+milis;
  }

int32_t Npc::attribute(Attribute a) const {
  if(a<ATR_MAX)
    return hnpc->attribute[a];
  return 0;
  }

void Npc::changeAttribute(Attribute a, int32_t val, bool allowUnconscious) {
  if(a>=ATR_MAX || val==0)
    return;

  if(val<0 && a==ATR_HITPOINTS) {
    if(isPlayer() && Gothic::inst().isGodMode())
      return;
    if(isPlayer() && owner.currentCs()!=nullptr)
      return;
    if(isImmortal())
      return;
    }

  hnpc->attribute[a]+=val;
  if(hnpc->attribute[a]<0)
    hnpc->attribute[a]=0;
  if(a==ATR_HITPOINTS && hnpc->attribute[a]>hnpc->attribute[ATR_HITPOINTSMAX])
    hnpc->attribute[a] = hnpc->attribute[ATR_HITPOINTSMAX];
  if(a==ATR_MANA && hnpc->attribute[a]>hnpc->attribute[ATR_MANAMAX])
    hnpc->attribute[a] = hnpc->attribute[ATR_MANAMAX];

  if(val<0)
    invent.invalidateCond(*this);

  if(a==ATR_HITPOINTS) {
    checkHealth(true,allowUnconscious);
    if(aiPolicy==NpcProcessPolicy::AiFar || aiPolicy==NpcProcessPolicy::AiFar2)
      aiState.started = true;
    }
  }

int32_t Npc::protection(Protection p) const {
  if(p<PROT_MAX)
    return hnpc->protection[p];
  return 0;
  }

void Npc::changeProtection(Protection p, int32_t val) {
  if(p<PROT_MAX)
    hnpc->protection[p]=val;
  }

uint32_t Npc::instanceSymbol() const {
  return uint32_t(hnpc->symbol_index());
  }

uint32_t Npc::guild() const {
  return std::min(uint32_t(hnpc->guild), uint32_t(GIL_MAX-1));
  }

bool Npc::isMonster() const {
  const bool g2 = owner.version().game==2;
  const auto SEPERATOR_ORC = g2 ? GIL_SEPERATOR_ORC : GIL_G1_SEPERATOR_ORC;
  const auto SEPERATOR_HUM = g2 ? GIL_SEPERATOR_HUM : GIL_G1_SEPERATOR_HUM;
  return SEPERATOR_HUM<guild() && guild()<SEPERATOR_ORC;
  }

bool Npc::isHuman() const {
  const bool g2 = owner.version().game==2;
  const auto SEPERATOR_HUM = g2 ? GIL_SEPERATOR_HUM : GIL_G1_SEPERATOR_HUM;
  return guild() < SEPERATOR_HUM;
  }

void Npc::setTrueGuild(int32_t g) {
  trGuild = g;
  }

int32_t Npc::trueGuild() const {
  if(trGuild==GIL_NONE)
    return hnpc->guild;
  return trGuild;
  }

int32_t Npc::magicCyrcle() const {
  return talentSkill(TALENT_RUNES);
  }

int32_t Npc::level() const {
  return hnpc->level;
  }

int32_t Npc::experience() const {
  return hnpc->exp;
  }

int32_t Npc::experienceNext() const {
  return hnpc->exp_next;
  }

int32_t Npc::learningPoints() const {
  return hnpc->lp;
  }

int32_t Npc::diveTime() const {
  return mvAlgo.diveTime();
  }

void Npc::setAttitude(Attitude att) {
  permAttitude = att;
  }

bool Npc::isFriend() const {
  bool g2 = owner.version().game==2;
  return ( g2 && hnpc->type==zenkit::NpcType::G2_FRIEND) ||
         (!g2 && hnpc->type==zenkit::NpcType::G1_FRIEND);
  }

void Npc::setTempAttitude(Attitude att) {
  tmpAttitude = att;
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

bool Npc::implGoTo(uint64_t dt) {
  float dist = 0;
  if(go2.npc) {
    dist = fghAlgo.prefferedAttackDistance(*this,*go2.npc,owner.script());
    } else {
    // use smaller threshold, to avoid edge-looping in script
    dist = MoveAlgo::closeToPointThreshold*0.5f;
    if(!mvAlgo.checkLastBounce())
      dist = MoveAlgo::closeToPointThreshold*1.5f;
    if(go2.wp!=nullptr && go2.wp->useCounter()>1)
      dist = float(MAX_AI_USE_DISTANCE);
    }
  if(go2.flag==GT_Point && go2.pointDistance>0.f)
    dist = go2.pointDistance;
  return implGoTo(dt,dist);
  }

bool Npc::implGoTo(uint64_t dt, float destDist) {
  if(go2.flag==GT_No)
    return false;

  if(isInAir() || interactive()!=nullptr) {
    mvAlgo.tick(dt);
    return true;
    }

  auto target = go2.target();
  auto dpos   = target - position();

  if(go2.flag==GT_Flee) {
    // nop
    }
  else if(go2.isClose(*this, destDist)) {
    bool finished = true;
    if(go2.flag==GT_Way) {
      go2.wp = go2.wp->hasLadderConn(wayPath.first()) ? wayPath.first() : wayPath.pop();
      if(go2.wp!=nullptr) {
        attachToPoint(go2.wp);
        if(setGoToLadder()) {
          mvAlgo.tick(dt);
          return true;
          }
        finished = false;
        }
      }
    if(finished) {
      if(go2.flag==Npc::GT_NextFp && implTurnTo(go2.wp,AnimationSolver::TurnType::Std,dt))
        return true;
      clearGoTo();
      }
    }
  else {
    if(setGoToLadder()) {
      mvAlgo.tick(dt);
      return true;
      }
    if(mvAlgo.checkLastBounce() && implTurnTo(dpos.x,dpos.z,AnimationSolver::TurnType::Std,dt)) {
      mvAlgo.tick(dt);
      return true;
      }
    }

  if(!go2.empty()) {
    setAnim(AnimationSolver::Move);
    mvAlgo.tick(dt);
    return true;
    }
  return false;
  }

bool Npc::implAttack(uint64_t dt, bool bodyOverride, FightAlgo::Action bodyAction) {
  if(currentTarget==nullptr || isPlayer() || isTalk())
    return false;

  if(currentTarget->isDown()){
    traceCombatGate("target_down");
    // NOTE: don't clear internal target, to make scripts happy
    // currentTarget=nullptr;
    fghAlgo.onClearTarget();
    return false;
    }

  if(aiQueue.size()>0) {
    traceCombatGate("script_queue");
    // do not messup weapon change animations by MOVE intruction
    return false;
    }

  const auto ws = weaponState();
  const auto bs = bodyStateMasked();

  if(bs==BS_HIT && (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H)) {
    traceCombatGate("melee_hit_lock");
    //NOTE: 'storm' attack has BS_RUN state and not meant to be auto-rotated
    implTurnToFai(*currentTarget,dt);
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }

  if(!bodyOverride && !fghAlgo.hasInstructions()) {
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
    implTurnToFai(*currentTarget,dt);
    mvAlgo.tick(dt,MoveAlgo::FaiMove);
    return true;
    }

  const auto act = bodyOverride ? bodyAction : fghAlgo.nextFromQueue(*this,*currentTarget,owner.script());
  const auto consume = [&] {
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

  if(act==FightAlgo::MV_JUMPBACK || (act==FightAlgo::MV_WAIT && jmp) || (act==FightAlgo::MV_TURN && jmp)) {
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

std::string Npc::combatPoseIdentity() const {
  std::string result = std::to_string(visual.pose().comboLength());
  for(const auto& layer:visual.pose().combatLayers(owner.tickCount()))
    result += "|" + layer.sequence->name + ":" + std::to_string(layer.start) + ":" +
              std::to_string(layer.speed) + ":" + std::to_string(layer.combination) + ":" +
              std::to_string(unsigned(layer.state));
  return result;
  }

std::string Npc::combatPoseLayers() const {
  std::string result = "[";
  const auto times = [](const auto& values) {
    std::string json = "[";
    for(const auto value:values) {
      if(json.size()>1)
        json += ",";
      json += std::to_string(value);
      }
    return json + "]";
    };
  for(const auto& layer:visual.pose().combatLayers(owner.tickCount())) {
    const auto& seq = *layer.sequence;
    const auto& data = *seq.data;
    std::vector<int32_t> optimal;
    for(const auto& ev:data.events)
      if(ev.type==zenkit::MdsEventType::OPTIMAL_FRAME)
        for(auto frame:ev.frames)
          optimal.push_back(frame);
    if(result.size()>1)
      result += ",";
    result += "{\"name\":" + TestBench::bodyJson(seq.name) +
              ",\"start\":" + std::to_string(layer.start) + ",\"clock\":" + std::to_string(layer.time) +
              ",\"speed\":" + std::to_string(layer.speed) + ",\"combination\":" + std::to_string(layer.combination) +
              ",\"body_state\":" + std::to_string(unsigned(layer.state)) +
              ",\"combo_length\":" + std::to_string(visual.pose().comboLength()) +
              ",\"fps\":" + std::to_string(data.fpsRate) + ",\"duration_ms\":" + std::to_string(seq.totalTime()) +
              ",\"def_window_ms\":" + times(data.defWindow) + ",\"def_par_ms\":" + times(data.defParFrame) +
              ",\"hit_end_ms\":" + times(data.defHitEnd) + ",\"optimal_frames\":" + times(optimal) + "}";
    }
  return result + "]";
  }

void Npc::traceCombatSnapshot() {
  if(!TestBench::bodyTracing() || currentTarget==nullptr)
    return;
  const auto now = owner.tickCount();
  const auto& pose = visual.pose();
  const bool prehit = pose.isPrehit(now), defence = pose.isDefence(now), jumpback = pose.isJumpBack(now);
  const bool attack = pose.isAttackAnim(), defWindow = pose.isDefWindow(now), parWindow = pose.isDefParWindow(now);
  const std::string state = combatPoseIdentity() + ":" + std::to_string(unsigned(bodyState())) + ":" +
                           std::to_string(int(weaponState())) + ":" + std::to_string(prehit) + ":" +
                           std::to_string(defence) + ":" + std::to_string(jumpback) + ":" +
                           std::to_string(attack) + ":" + std::to_string(defWindow) + ":" +
                           std::to_string(parWindow) + ":" + std::to_string(isDown()) + ":" +
                           std::to_string(attribute(ATR_HITPOINTS)) + ":" +
                           std::to_string(int(fghAlgo.queuedAction())) + ":" +
                           std::to_string(int(processPolicy())) + ":" +
                           std::to_string(faiWaitTime>=now) + ":" + std::to_string(aniWaitTime>=now) + ":" +
                           std::to_string(waitTime>=now) + ":" + std::to_string(outWaitTime>now);
  const bool changed = state!=bodyTraceState;
  // At 30 Hz this samples adjacent 50 ms buckets (33/67 ms spacing); window/layer edges are always logged.
  if(!changed && now/50==bodyTraceSampleTime/50)
    return;
  bodyTraceState = state;
  bodyTraceSampleTime = now;
  char values[1400];
  std::snprintf(values,sizeof(values),
                "\"sample_phase\":\"npc_pre_tick_animation_tags\",\"edge\":%s,\"hp\":%d,\"body_state\":%u,"
                "\"weapon_state\":%d,\"prehit\":%s,\"defence\":%s,\"jumpback\":%s,\"attack_anim\":%s,"
                "\"def_window\":%s,\"def_par_window\":%s,\"down\":%s,\"dead\":%s,\"x\":%.6f,\"y\":%.6f,"
                "\"z\":%.6f,\"yaw\":%.6f,\"fai_wait_until\":%llu,\"ani_wait_until\":%llu,"
                "\"wait_until\":%llu,\"output_wait_until\":%llu,\"script_queue_size\":%zu,\"primitive\":%d,\"choice_id\":%llu",
                changed ? "true" : "false",int(attribute(ATR_HITPOINTS)),unsigned(bodyState()),int(weaponState()),
                prehit ? "true" : "false",defence ? "true" : "false",jumpback ? "true" : "false",attack ? "true" : "false",
                defWindow ? "true" : "false",parWindow ? "true" : "false",isDown() ? "true" : "false",isDead() ? "true" : "false",
                double(x),double(y),double(z),double(angle),(unsigned long long)faiWaitTime,(unsigned long long)aniWaitTime,
                (unsigned long long)waitTime,(unsigned long long)outWaitTime,size_t(aiQueue.size()),int(fghAlgo.queuedAction()),
                (unsigned long long)fghAlgo.traceChoiceId());
  TestBench::bodyEvent(*this,currentTarget,"phase_snapshot",now,std::string(values) +
                       ",\"process_policy\":" + std::to_string(int(processPolicy())) +
                       ",\"native_ai_normal\":" + (processPolicy()==NpcProcessPolicy::AiNormal ? "true" : "false") +
                       ",\"layers\":" + combatPoseLayers(),pose.animNames());
  }

void Npc::traceVisibilityDiagnostic(std::string_view sourcePhase, bool force) noexcept {
  if(!BodyController::visibilityDiagnosticsEnabled() || !TestBench::bodyTracing() ||
     !BodyObservations::watched(*this) || currentTarget==nullptr)
    return;
  try {
    constexpr uint64_t PeriodMs=50, MaxRecords=8192;
    const auto generation=BodyController::visibilityDiagnosticsGeneration();
    if(visibilityDiagnosticGeneration!=generation) {
      visibilityDiagnosticGeneration=generation; visibilityDiagnosticCount=0;
      visibilityDiagnosticTime={}; visibilityDiagnosticKey={};
      }
    const auto now=owner.tickCount();
    const size_t slot=sourcePhase=="post_observation" ? 1 : 0;
    if(visibilityDiagnosticCount>=MaxRecords) {
      if(force || visibilityDiagnosticKey[slot].empty() || now/PeriodMs!=visibilityDiagnosticTime[slot]/PeriodMs) {
        BodyController::visibilityDiagnosticRecorded(true); visibilityDiagnosticTime[slot]=now;
        }
      return;
      }
    using Clock=std::chrono::steady_clock;
    const auto snapshotTime=Clock::now();
    const auto eye=visual.mapHeadBone();
    const auto& other=*currentTarget;
    const auto view=visual.viewDirection();
    const auto range=float(hnpc->senses_range);
    const auto threshold=std::cos(100*M_PI/180.0);
    struct Ray final {
      Vec3 point={}, hitPoint={}, hitNormal={};
      float distance=0, angle=0, fraction=0;
      double coneCos=0, bodyConeCos=0;
      bool rangePass=false, fovPass=false, bodyFovPass=false, hit=false, vob=false, passes=false;
      };
    const auto inspect=[&](const Vec3& point) {
      Ray result; result.point=point;
      result.distance=std::sqrt(qDistTo(point)); result.rangePass=qDistTo(point)<=range*range;
      const auto bearing=angleDir(eye.x-point.x,eye.z-point.z);
      result.angle=view-bearing;
      // Match the legacy float-angle/cos comparison exactly. Unconditional static rays
      // explain a cone failure too, but never feed back into the actual query or model.
      result.coneCos=double(std::cos(float(M_PI)*(view-bearing)/180.f));
      result.bodyConeCos=double(std::cos(float(M_PI)*(rotation()-bearing)/180.f));
      result.fovPass=result.coneCos<=threshold; result.bodyFovPass=result.bodyConeCos<=threshold;
      const auto hit=owner.physic()->ray(eye,point);
      result.hit=hit.hasCol; result.vob=hit.hasCol && hit.vob!=nullptr;
      if(hit.hasCol) { result.hitPoint=hit.v; result.hitNormal=hit.n; result.fraction=hit.hitFraction; }
      result.passes=result.rangePass && result.fovPass && !result.hit;
      return result;
      };
    const auto center=inspect(other.physic.center());
    const auto skeleton=other.visual.visualSkeleton();
    const bool hasHead=skeleton!=nullptr && skeleton->BIP01_HEAD!=size_t(-1);
    const auto head=hasHead ? inspect(other.visual.mapHeadBone()) : Ray{};
    const bool actual=canSeeNpc(other,false);
    const bool sense=(hnpc->senses & int32_t(SensesBit::SENSE_SEE))!=0;
    const bool targetTerminal=other.isDown() || other.attribute(ATR_HITPOINTS)<=0;
    const auto control=BodyController::snapshot(*this);
    const auto animation=animNames();
    const auto key=animation+":"+std::to_string(actual)+":"+std::to_string(center.fovPass)+":"+
                   std::to_string(head.fovPass)+":"+std::to_string(center.hit)+":"+std::to_string(head.hit);
    if(!force && visibilityDiagnosticKey[slot]==key && now/PeriodMs==visibilityDiagnosticTime[slot]/PeriodMs)
      return;
    const char* reason="mixed_ray_rejection";
    if(actual) reason="visible";
    else if(!center.rangePass && (!hasHead || !head.rangePass)) reason="out_of_senses_range";
    else if(!center.fovPass && (!hasHead || !head.fovPass)) reason="outside_pose_cone";
    else if(center.hit && (!hasHead || head.hit)) reason="static_occlusion";
    else if(!hasHead) reason="target_head_missing";
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::boolalpha << std::setprecision(9);
    const auto vec=[&](const Vec3& point) {
      if(!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
        throw std::runtime_error("nonfinite visibility diagnostic position");
      out << '[' << point.x << ',' << point.y << ',' << point.z << ']';
      };
    const auto ray=[&](const Ray& value) {
      if(!std::isfinite(value.distance) || !std::isfinite(value.angle) || !std::isfinite(value.coneCos) ||
         !std::isfinite(value.bodyConeCos) || !std::isfinite(value.fraction))
        throw std::runtime_error("nonfinite visibility diagnostic ray");
      out << "{\"range_pass\":" << value.rangePass << ",\"distance_cm\":" << value.distance
          << ",\"fov_pass\":" << value.fovPass << ",\"legacy_angle_deg\":" << value.angle
          << ",\"cone_cos\":" << value.coneCos << ",\"body_cone_cos\":" << value.bodyConeCos
          << ",\"body_fov_pass\":" << value.bodyFovPass << ",\"ray_hit\":" << value.hit
          << ",\"hit_vob\":" << value.vob << ",\"hit_fraction\":" << value.fraction
          << ",\"query_pass\":" << value.passes << ",\"target_point_cm\":";
      vec(value.point); out << ",\"hit_point_cm\":";
      if(value.hit) vec(value.hitPoint); else out << "null";
      out << ",\"hit_normal\":"; if(value.hit) vec(value.hitNormal); else out << "null";
      out << '}';
      };
    if(!std::isfinite(view) || !std::isfinite(rotation())) throw std::runtime_error("nonfinite visibility diagnostic yaw");
    const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(snapshotTime.time_since_epoch()).count());
    out << "\"diagnostic_schema_version\":1,\"source_phase\":" << TestBench::bodyJson(sourcePhase)
        << ",\"diagnostic_generation\":" << generation << ",\"diagnostic_seq\":" << visibilityDiagnosticCount+1
        << ",\"oracle_debug_only\":true,\"sample_period_ms\":" << PeriodMs << ",\"forced_edge\":" << force
        << ",\"snapshot_wall_monotonic_ns\":" << ns << ",\"wall_monotonic_ns\":" << ns
#ifdef __linux__
        << ",\"clock_domain\":\"linux-clock-monotonic\""
#else
        << ",\"clock_domain\":\"cxx-steady-clock\""
#endif
        << ",\"sim_step\":" << BodyObservations::currentStep() << ",\"session_epoch\":" << BodyObservations::sessionEpoch()
        << ",\"world_epoch\":" << BodyObservations::worldEpoch() << ",\"entity_id\":" << BodyObservations::entityId(*this)
        << ",\"target_entity_id\":" << BodyObservations::entityId(other)
        << ",\"owner\":" << TestBench::bodyJson(control.owned ? "scripted" : "native")
        << ",\"provider\":" << TestBench::bodyJson(control.provider) << ",\"control_generation\":" << control.controlGeneration
        << ",\"command_id\":" << control.commandId << ",\"execution_id\":" << control.executionId
        << ",\"release_pending\":" << control.releasePending << ",\"control_allowed\":" << control.controlAllowed
        << ",\"body_state_base\":" << unsigned(bodyStateMasked()) << ",\"native_fai_wait\":" << (faiWaitTime>=now)
        << ",\"native_script_wait\":" << (waitTime>=now)
        << ",\"native_fai_wait_remaining_ms\":" << (faiWaitTime>now ? faiWaitTime-now : 0)
        << ",\"native_script_wait_remaining_ms\":" << (waitTime>now ? waitTime-now : 0)
        << ",\"body_yaw_deg\":" << rotation() << ",\"pose_view_deg\":" << view
        << ",\"fov_half_angle_deg\":100,\"legacy_cos_threshold\":" << threshold
        << ",\"senses_range_cm\":" << range << ",\"senses_see\":" << sense << ",\"target_terminal\":" << targetTerminal
        << ",\"actual_can_see\":" << actual << ",\"controller_visible\":" << (sense && actual && !targetTerminal)
        << ",\"query_matches_diagnostic\":" << (actual==(center.passes || (hasHead && head.passes)))
        << ",\"reason\":" << TestBench::bodyJson(reason) << ",\"eye_cm\":";
    vec(eye); out << ",\"self_position_cm\":"; vec(position()); out << ",\"target_position_cm\":"; vec(other.position());
    out << ",\"target_has_head\":" << hasHead << ",\"center_ray\":"; ray(center);
    out << ",\"head_ray\":"; if(hasHead) ray(head); else out << "null";
    TestBench::bodyEvent(*this,&other,"visibility_diagnostic",now,out.str(),animation);
    ++visibilityDiagnosticCount; visibilityDiagnosticTime[slot]=now; visibilityDiagnosticKey[slot]=key;
    BodyController::visibilityDiagnosticRecorded();
    }
  catch(...) { BodyController::visibilityDiagnosticFailed(); }
  }

void Npc::traceCombatGate(std::string_view reason) {
  if(!TestBench::bodyTracing() || currentTarget==nullptr)
    return;
  const auto key = std::string(reason) + ":" + std::to_string(fghAlgo.traceChoiceId()) + ":" +
                   std::to_string(int(fghAlgo.queuedAction()));
  if(key==bodyTraceGate)
    return;
  bodyTraceGate = key;
  TestBench::bodyEvent(*this,currentTarget,"native_gate",owner.tickCount(),
                      "\"reason\":" + TestBench::bodyJson(reason) + ",\"choice_id\":" + std::to_string(fghAlgo.traceChoiceId()) +
                      ",\"primitive\":" + std::to_string(int(fghAlgo.queuedAction())));
  }

Npc::BodyObservationState Npc::bodyObservationState() const {
  BodyObservationState s;
  const uint64_t now = owner.tickCount();
  const auto& pose = visual.pose();
  const auto ws = weaponState();
  s.bodyState = uint32_t(bodyState());
  s.weaponState = uint8_t(ws);
  if(const auto weapon = invent.activeWeapon()) {
    s.weaponSymbol = uint32_t(weapon->clsId());
    s.weaponLength = weapon->swordLength();
    }
  switch(ws) {
    case WeaponState::W1H: s.skillTalent = TALENT_1H; break;
    case WeaponState::W2H: s.skillTalent = TALENT_2H; break;
    case WeaponState::Bow: s.skillTalent = TALENT_BOW; break;
    case WeaponState::CBow: s.skillTalent = TALENT_CROSSBOW; break;
    default: break;
    }
  s.skill = s.skillTalent==TALENT_UNKNOWN ? 0 : hitChance(Talent(s.skillTalent));
  s.fightTactic = hnpc->fight_tactic;
  s.primitive = int32_t(fghAlgo.queuedAction());
  s.choiceId = fghAlgo.traceChoiceId();
  const auto remaining = [now](uint64_t deadline) { return deadline>now ? deadline-now : 0; };
  s.faiWaitMs = remaining(faiWaitTime);
  s.aniWaitMs = remaining(aniWaitTime);
  s.aiWaitMs = remaining(waitTime);
  s.outputWaitMs = remaining(outWaitTime);
  s.faiGate = faiWaitTime>=now;
  s.aniGate = aniWaitTime>=now;
  s.aiGate = waitTime>=now;
  s.outputGate = outWaitTime>now;
  s.attackAnim = pose.isAttackAnim();
  s.prehit = pose.isPrehit(now);
  s.defence = pose.isDefence(now);
  s.jumpBack = pose.isJumpBack(now);
  s.defWindow = pose.isDefWindow(now);
  s.parWindow = pose.isDefParWindow(now);
  s.interruptible = true;
  for(const auto& layer:pose.combatLayers(now)) {
    if(layer.sequence==nullptr)
      continue;
    const auto& seq = *layer.sequence;
    if(seq.isAttackAnim() || layer.state==BS_PARADE) {
      const auto duration=seq.isAttackAnim() ? seq.atkTotalTime(pose.comboLength()) : seq.totalTime();
      const auto elapsed=layer.time>=layer.start ? double(layer.time-layer.start) : 0.0;
      if(double(duration)>=elapsed && layer.speed>0)
        s.segmentRemainingMs=std::max(s.segmentRemainingMs,uint64_t(std::ceil((double(duration)-elapsed)/double(layer.speed)))+1);
      }
    const bool finished = seq.isFinished(layer.time,layer.start,pose.comboLength()) &&
                          seq.next.empty() && seq.animCls!=Animation::Loop;
    if(layer.state==BS_ITEMINTERACT || (!finished && !seq.canInterrupt(layer.time,layer.start,pose.comboLength())))
      s.interruptible = false;
    }
  // AttackBlock's generic solver draws std::rand() for G2 sword variants. Only inspect the
  // actual named clips here; every other listed primitive lookup is free of RNG.
  s.hasMove = hasAnim(Anim::Move);
  s.hasLeft = hasAnim(Anim::MoveL);
  s.hasRight = hasAnim(Anim::MoveR);
  s.hasBack = hasAnim(Anim::MoveBack);
  s.hasAttack = hasAnim(Anim::Attack);
  s.hasAttackLeft = hasAnim(Anim::AttackL);
  s.hasAttackRight = hasAnim(Anim::AttackR);
  if(ws==WeaponState::W1H || ws==WeaponState::W2H) {
    const std::string prefix = ws==WeaponState::W1H ? "T_1HPARADE_" : "T_2HPARADE_";
    s.hasBlock = Gothic::inst().version().game==2 ? hasAnim(prefix+"0") : hasAnim(prefix+"O");
    }
  const auto bs = bodyStateMasked();
  const bool melee = ws==WeaponState::W1H || ws==WeaponState::W2H;
  s.nativeEligible = melee && !isPlayer() && !isTalk() && !isDown() && !isInAir() &&
                     !isSwim() && !isDive() && !externalCtrl && aiQueue.size()==0 &&
                     bs!=BS_HIT && bs!=BS_LIE && bs!=BS_STUMBLE && bs!=BS_FALL &&
                     !s.faiGate && !s.aniGate && !s.aiGate && !s.outputGate && s.interruptible;
  return s;
  }

Npc::BodyObservationTarget Npc::bodyObservationTarget(const Npc& tg) const {
  BodyObservationTarget s;
  auto& script = owner.script();
  s.fightDistanceCm = fightDistanceTo(tg).length();
  s.attackRangeCm = fghAlgo.prefferedAttackDistance(*this,tg,script);
  s.gRangeCm = fghAlgo.prefferedGDistance(*this,tg,script);
  s.closeupRangeCm = 0.75f*fghAlgo.baseDistance(*this,tg,script);
  s.inWRange = fghAlgo.isInWRange(*this,tg,script);
  s.inGRange = fghAlgo.isInGRange(*this,tg,script);
  s.inCloseup = fghAlgo.isInCloseupRange(*this,tg,script);
  s.focus30 = fghAlgo.isInFocusAngle(*this,tg);
  s.focus5 = fghAlgo.isInFocusAngle(*this,tg,5.f);
  s.meleeLineClear = !owner.physic()->ray(collosionCenter(),tg.collosionCenter()).hasCol;
  return s;
  }

void Npc::traceCombatAttempt(Anim requested, bool accepted, const std::string& before, std::string_view hook) {
  if(!TestBench::bodyTracing())
    return;
  const auto after = combatPoseIdentity();
  const bool changed = before!=after;
  TestBench::bodyEvent(*this,currentTarget,"native_attempt",owner.tickCount(),
                      "\"hook\":" + TestBench::bodyJson(hook) + ",\"choice_id\":" + std::to_string(fghAlgo.traceChoiceId()) + ",\"primitive\":" +
                      std::to_string(int(fghAlgo.queuedAction())) + ",\"requested_anim\":" + std::to_string(int(requested)) +
                      ",\"accepted\":" + (accepted ? "true" : "false") + ",\"pose_changed\":" + (changed ? "true" : "false") +
                      ",\"pose_before\":" + TestBench::bodyJson(before) + ",\"pose_after\":" + TestBench::bodyJson(after) +
                      ",\"layers_after\":" + combatPoseLayers(),visual.pose().animNames());
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

  if(ev.weaponCh==zenkit::MdsFightMode::NONE && (ws==WeaponState::W1H || ws==WeaponState::W2H)) {
    if(auto melee = invent.currentMeleeWeapon()) {
      auto at = centerPosition();
      if(melee->handle().material==ItemMaterial::MAT_METAL)
        sfxWeapon = ::Sound(owner,::Sound::T_Regular,"UNDRAWSOUND_ME.WAV",at,2500,false); else
        sfxWeapon = ::Sound(owner,::Sound::T_Regular,"UNDRAWSOUND_WO.WAV",at,2500,false);
      sfxWeapon.play();
      }
    }
  else if(ev.weaponCh==zenkit::MdsFightMode::SINGLE_HANDED || ev.weaponCh==zenkit::MdsFightMode::DUAL_HANDED) {
    if(auto melee = invent.currentMeleeWeapon()) {
      auto at = centerPosition();
      if(melee->handle().material==ItemMaterial::MAT_METAL)
        sfxWeapon = ::Sound(owner,::Sound::T_Regular,"DRAWSOUND_ME.WAV",at,2500,false); else
        sfxWeapon = ::Sound(owner,::Sound::T_Regular,"DRAWSOUND_WO.WAV",at,2500,false);
      sfxWeapon.play();
      }
    }
  else if(ev.weaponCh==zenkit::MdsFightMode::BOW || ev.weaponCh==zenkit::MdsFightMode::CROSSBOW) {
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

void Npc::commitDamage() {
  const auto mode=weaponState();
  if(owner.physic()->destructionEnabled() &&
     (mode==WeaponState::Fist || mode==WeaponState::W1H || mode==WeaponState::W2H)) {
    const auto& guild=owner.script().guildVal();
    const auto gl=this->guild();
    auto kind=FractureRule::Melee::Fist;
    float range=float(guild.fight_range_base[gl]+guild.fight_range_fist[gl]);
    float weaponDamage=0.f;
    if(mode!=WeaponState::Fist) {
      const auto* weapon=invent.activeWeapon();
      if(weapon!=nullptr) {
        const bool twoHand=mode==WeaponState::W2H;
        kind=twoHand ? FractureRule::Melee::TwoHand : FractureRule::Melee::OneHand;
        range=float(guild.fight_range_base[gl]+(twoHand ? guild.fight_range_2ha[gl] : guild.fight_range_1ha[gl])+
                    weapon->swordLength());
        weaponDamage=float(weapon->handle().damage_total);
        }
      else range=0.f;
      }
    const float strength=float(attribute(ATR_STRENGTH));
    const float angle=rotationRad();
    const size_t affected=owner.physic()->meleeMovableVobs(collosionCenter(),{std::cos(angle),0,std::sin(angle)},
                                                         range,weaponDamage,strength,kind,this);
    if(TestBench::isActive())
      TestBench::destructionMeleeHit(*this,affected,weaponDamage+strength);
    }
  const auto hitTrace = [&](const char* result) {
    if(Gothic::physicsFlags().meleeBladeShadow && meleeShadow && meleeShadow->window) {
      meleeShadow->legacyTarget = meleeShadowWho(currentTarget);
      meleeShadow->legacyNpc = currentTarget;
      meleeShadow->legacyResult = result;
      }
    BodyObservations::noteEvent(*this,currentTarget,result,owner.tickCount());
    if(TestBench::bodyTracing())
      TestBench::bodyEvent(*this,currentTarget,"hit_attempt",owner.tickCount(),
                          "\"result\":" + TestBench::bodyJson(result),visual.pose().animNames());
    };
  if(currentTarget==nullptr) {
    hitTrace("no_target");
    return;
    }
  if(!fghAlgo.isInAttackRange(*this,*currentTarget,owner.script())) {
    hitTrace("out_of_range");
    return;
    }
  if(!fghAlgo.isInFocusAngle(*this,*currentTarget)) {
    hitTrace("out_of_focus");
    return;
    }
  hitTrace("applied");
  auto* shadow = Gothic::physicsFlags().meleeBladeShadow && meleeShadow && meleeShadow->window ? meleeShadow.get() : nullptr;
  auto* shadowTarget = shadow!=nullptr ? currentTarget : nullptr;
  int32_t hpBefore = 0;
  if(shadow!=nullptr) {
    // Observe the same pre-call defence predicates; damage and its RNG still run once below.
    const auto& targetPose = currentTarget->visual.pose();
    const bool down = currentTarget->isDown();
    shadow->legacyDodge = !down && targetPose.isJumpBack(owner.tickCount()) &&
                         currentTarget->fghAlgo.isInJumpBackAngle(*currentTarget,*this);
    shadow->legacyBlock = !down && (!isMonster() || inventory().activeWeapon()!=nullptr) &&
                         currentTarget->fghAlgo.isInFocusAngle(*currentTarget,*this) &&
                         targetPose.isDefence(owner.tickCount());
    shadow->legacyHit = !down && !shadow->legacyDodge && !shadow->legacyBlock;
    shadow->legacyResult = down ? "target_down" : (shadow->legacyBlock ? "blocked" :
                                                 (shadow->legacyDodge ? "evaded" : "applied"));
    hpBefore = currentTarget->attribute(ATR_HITPOINTS);
    }
  currentTarget->takeDamage(*this,nullptr);
  if(shadow!=nullptr)
    shadow->damage = std::max(0,hpBefore-shadowTarget->attribute(ATR_HITPOINTS));
  }

void Npc::takeDamage(Npc &other, const Bullet* b, bool ignoreDefence) {
  if(isDown())
    return;

  stopPhysicsInvestigation(false);

  assert(b==nullptr || !b->isSpell());
  const auto& pose    = visual.pose();
  const bool  isJumpb = !ignoreDefence && pose.isJumpBack(owner.tickCount()) && fghAlgo.isInJumpBackAngle(*this,other);
  const bool  isBlock = !ignoreDefence && (!other.isMonster() || other.inventory().activeWeapon()!=nullptr) &&
                         fghAlgo.isInFocusAngle(*this,other) &&
                         pose.isDefence(owner.tickCount());
  if((isBlock || isJumpb) && b==nullptr)
    BodyObservations::noteEvent(*this,&other,isBlock ? "blocked" : "evaded",owner.tickCount());
  if(TestBench::bodyTracing())
    TestBench::bodyEvent(*this,&other,"defence_result",owner.tickCount(),
                        std::string("\"damage_kind\":") + TestBench::bodyJson(b==nullptr ? "melee" : "ranged") +
                        ",\"blocked\":" + (isBlock && b==nullptr ? "true" : "false") +
                        ",\"jumped_back\":" + (isJumpb && b==nullptr ? "true" : "false"),pose.animNames());

  lastHit = &other;
  if(!isPlayer())
    setOther(&other);
  owner.sendPassivePerc(*this,other,*this,PERC_ASSESSFIGHTSOUND);

  if(!(isBlock || isJumpb) || b!=nullptr) {
    takeDamage(other,b,COLL_DOEVERYTHING,0,false);
    } else {
    if(invent.activeWeapon()!=nullptr)
      visual.emitBlockEffect(*this,other);
    }
  }

void Npc::takeDamage(Npc& other, const Bullet* b, const VisualFx* vfx, int32_t splId) {
  if(isDown())
    return;

  lastHitSpell = splId;
  lastHit      = &other;
  if(!isPlayer())
    setOther(&other);

  CollideMask bMask = owner.script().canNpcCollideWithSpell(*this,&other,splId);
  if(bMask!=COLL_DONOTHING)
    Effect::onCollide(owner,vfx,position(),this,&other,splId);
  takeDamage(other,b,bMask,splId,true);
  }

void Npc::takeDamage(Npc& other, const Bullet* b, const CollideMask bMask, int32_t splId, bool isSpell) {
  const int32_t traceHpBefore = attribute(ATR_HITPOINTS);
  float a  = angleDir(other.x-x,other.z-z);
  float da = a-angle;
  if(std::cos(da*M_PI/180.0)<0)
    lastHitType='A'; else
    lastHitType='B';

  DamageCalculator::Damage dmg={};
  DamageCalculator::Val    hitResult;
  SpellCategory            splCat     = SpellCategory::SPELL_BAD;
  const bool               dontKill   = isNonlethalFight(other) ||
                                       (((b==nullptr && splId==0) || (bMask & COLL_DONTKILL)) && (!isSwim()));
  int32_t                  damageType = DamageCalculator::damageTypeMask(other);

  if(isSpell) {
    auto& spl  = owner.script().spellDesc(splId);
    splCat     = SpellCategory(spl.spell_type);
    damageType = spl.damage_type;
    for(size_t i=0; i<zenkit::DamageType::NUM; ++i)
      if((damageType&(1<<i))!=0)
        dmg[i] = spl.damage_per_level;
    }

  // Capture the victim's state before its damage script changes attack reason/state.
  auto heroAttack = HeroDeed::capture(other,"attack",this);
  if(heroAttack!=nullptr && !other.isPlayer())
    heroAttack->collectWitnesses(other,this);
  if(!isSpell || splCat==SpellCategory::SPELL_BAD) {
    stopPhysicsInvestigation(false);
    perceptionProcess(other,this,0,PERC_ASSESSDAMAGE,other.isPlayer() ? heroAttack : nullptr);
    const auto tracePrimitiveBefore = fghAlgo.queuedAction();
    fghAlgo.onTakeHit();
    BodyController::takeHit(*this);
    if(TestBench::bodyTracing())
      TestBench::bodyEvent(*this,&other,"native_queue_clear",owner.tickCount(),
                          "\"reason\":\"take_hit\",\"primitive_before\":" + std::to_string(int(tracePrimitiveBefore)));
    implFaiWait(0);
    }

  hitResult = DamageCalculator::damageValue(other,*this,b,isSpell,dmg,bMask);
  if(hitResult.hasHit && !isSpell && b!=nullptr)
    queueArrowPhysics(*b);
  const auto traceDamage = [&] {
    BodyObservations::noteEvent(*this,&other,hitResult.value>0 ? "damage_received" : "damage_no_loss",owner.tickCount());
    const int32_t lost = traceHpBefore-attribute(ATR_HITPOINTS);
    if(lost>0)
      Gothic::inst().onCombatHit(*this,other,lost,isSpell ? "spell" : (b==nullptr ? "melee" : "ranged"));
    if(TestBench::bodyTracing())
      TestBench::bodyEvent(*this,&other,"damage",owner.tickCount(),
                          "\"damage_kind\":" + TestBench::bodyJson(isSpell ? "spell" : (b==nullptr ? "melee" : "ranged")) +
                          ",\"hp_before\":" + std::to_string(traceHpBefore) + ",\"hp_after\":" +
                          std::to_string(attribute(ATR_HITPOINTS)) + ",\"net\":" + std::to_string(hitResult.value) +
                          ",\"has_hit\":" + (hitResult.hasHit ? "true" : "false") +
                          ",\"down\":" + (isDown() ? "true" : "false") + ",\"dead\":" + (isDead() ? "true" : "false"));
    };
  if(TestBench::isActive()) {
    // what it did: the weapon's damage before the draw and the armour, k the draw times the zone (the zone
    // scales what the armour let through); the arrow's kind
    int32_t raw = 0;
    if(b!=nullptr)
      for(auto v:b->damage().val)
        raw += v;
    const char* kind = isSpell ? "spell" : (b==nullptr ? "melee" : (b->isPrecise() ? "precise" : "dice"));
    const float k    = b!=nullptr && b->isPrecise() ? Archery::damageAt(b->draw())*Archery::zoneDamage(b->hitZone()) : 1.f;
    const auto  zone = b!=nullptr && b->isPrecise() ? HitZones::name(b->hitZone()) : std::string_view("-");
    const auto  who  = TestBench::who(this);
    const auto  by   = TestBench::who(&other);
    char buf[256];
    std::snprintf(buf,sizeof(buf),"damage %.*s: raw %d zone %.*s k %.2f prot %d net %d hit %d, hp %d, %s, by %.*s",
                  int(who.size()),who.data(),int(raw),int(zone.size()),zone.data(),double(k),int(protection(PROT_POINT)),
                  int(hitResult.value),int(hitResult.hasHit),int(attribute(ATR_HITPOINTS)),kind,int(by.size()),by.data());
    TestBench::note(buf);
    }
  if(!isSpell && !isDown() && hitResult.hasHit)
    owner.addWeaponHitEffect(other,b,*this).play();

  if(isDown()) {
    onNoHealth(dontKill,HS_NoSound);
    traceDamage();
    return;
    }

  if(hitResult.hasHit && !benchStill) {
    auto state   = bodyStateMasked();
    bool stumble = false;
    if(interactive()==nullptr && ((state&BS_FLAG_INTERRUPTABLE)!=BS_NONE || state==BS_RUN || state==BS_NONE)) {
      //NONE/RUN requires for monsters like waran
      const bool noInter = (hnpc->bodystate_interruptable_override!=0);
      if(!noInter) {
        //NOTE: kepp rotation animation: this results in more accurate fight with trolls
        // visual.setAnimRotate(*this,0);
        visual.interrupt(); // TODO: put down in pipeline, at Pose and merge with setAnimAngGet
        }

      if((damageType & (1<<zenkit::DamageType::FLY))==0)
        stumble = setAnimAngGet(lastHitType=='A' ? Anim::StumbleA  : Anim::StumbleB)!=nullptr;
      }
    if((damageType & (1<<zenkit::DamageType::FLY))==0)
      hitReaction(other,b,hitResult.value,stumble);
    }

  // throw enemy
  const bool physicalWind = isSpell && Gothic::physicsFlags().spellPhysics &&
                            owner.script().physicalSpell(splId)==GameScript::PhysicalSpell::WindFist;
  if(hitResult.hasHit && (damageType & (1<<zenkit::DamageType::FLY)) && !physicalWind) {
    mvAlgo.accessDamFly(x-other.x, z-other.z, lastHitType);
    }

  if(hitResult.value>0) {
    if(heroAttack!=nullptr && !other.isPlayer()) {
      other.consumeServerAttack();
      Gothic::inst().onHeroDeed(*heroAttack);
      heroAttack.reset(); // native ASSESSOTHERSDAMAGE still runs, without changing our snapshot
      }
    currentOther = &other;
    changeAttribute(ATR_HITPOINTS,-hitResult.value,dontKill);

    if(bMask&(COLL_APPLYVICTIMSTATE|COLL_DOEVERYTHING)) {
      owner.sendPassivePerc(*this,other,*this,PERC_ASSESSOTHERSDAMAGE,std::move(heroAttack));
      if(isUnconscious()){
        owner.sendPassivePerc(*this,other,*this,PERC_ASSESSDEFEAT);
        }
      else if(isDead()) {
        owner.sendPassivePerc(*this,other,*this,PERC_ASSESSMURDER);
        }
      else {
        if(owner.script().rand(2,this)==0) {
          emitSoundSVM("SVM_%d_AARGH");
          }
        }
      }
    }
  traceDamage();
  }

void Npc::hitReaction(const Npc& other, const Bullet* b, int32_t damage, bool stumble) {
  // the upper body gives way to the blow (graphics/mesh/flinch), on top of the stumble if one plays
  const Vec3  feet   = position();
  const Vec3  center = collosionCenter();
  const float top    = std::max(mapHeadBone().y - feet.y, 50.f);
  Flinch::Hit hit;
  Vec3        at; // where it lands
  if(b!=nullptr && Vec3(b->direction().x,0,b->direction().z).length()>1e-3f) {
    hit.dir = b->direction();
    at      = b->position();
    } else {
    // a weapon swung sideways pushes along the swing too and lands on the side it comes from
    Vec3 to = feet - other.position();
    to.y = 0;
    to   = to.length()>1e-3f ? Vec3::normalize(to) : Vec3(-std::cos(rotationRad()),0,-std::sin(rotationRad()));
    const auto  anim  = other.animNames();
    const float swing = anim.find("ATTACKL")!=std::string::npos ? 1.f : (anim.find("ATTACKR")!=std::string::npos ? -1.f : 0.f);
    const float oa    = other.rotationRad();
    const Vec3  left  = Vec3::crossProduct(Vec3(std::cos(oa),0,std::sin(oa)),Vec3(0,1,0)); // of the one who strikes
    hit.dir = to + left*(0.6f*swing);
    at      = center - left*(20.f*swing) - to*20.f;
    at.y    = feet.y + top*(other.isMonster() ? 0.6f : 0.85f);
    }
  // 0 the hips (half the height to the head), 1 the head
  hit.height = std::clamp((at.y - feet.y)/top*2.f - 1.f, 0.f, 1.f);
  // off the middle the blow turns the trunk: about up, by the lever
  Vec3 r = at - center;
  r.y = 0;
  Vec3 d = Vec3(hit.dir.x,0,hit.dir.z);
  if(d.length()>1e-3f) {
    d = Vec3::normalize(d);
    hit.twist = std::clamp(Vec3::crossProduct(r,d).y/25.f, -1.f, 1.f);
    }
  const int32_t hpMax = std::max(attribute(ATR_HITPOINTSMAX),1);
  hit.strength = std::clamp(0.35f + 2.5f*float(std::max(damage,0))/float(hpMax), 0.35f, 1.f);
  if(stumble)
    hit.strength *= 0.6f; // the stumble throws the body already
  flinch(hit);
  }

void Npc::takeFallDamage(const Vec3& fallSpeed) {
  if(bodyStateMasked()==BS_FALL) {
    if(!isFallingDeep()) {
      // small fall
      setAnim(Anim::Idle);
      } else {
      const float a  = angleDir(-fallSpeed.x,-fallSpeed.z);
      const float da = a-angle;
      if(std::cos(da*M_PI/180.0)<0 || Vec2(fallSpeed.x,fallSpeed.z).length()<0.1f)
        lastHitType='A'; else
        lastHitType='B';
      // Match the incoming fall so its authored transition refreshes every
      // bone before the sparse lying loop inherits the pose.
      setAnim(Anim::Fallen);
      }
    }
  auto dmg = DamageCalculator::damageFall(*this,fallSpeed.length());
  if(!dmg.hasHit)
    return;
  int32_t hp = attribute(ATR_HITPOINTS);
  if(hp>dmg.value) {
    emitSoundSVM("SVM_%d_AARGH");
    clearState(true);
    }
  changeAttribute(ATR_HITPOINTS,-dmg.value,false);
  }

void Npc::takeDrownDamage() {
  changeAttribute(Attribute::ATR_HITPOINTS, -attribute(Attribute::ATR_HITPOINTSMAX), false);
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
      case zenkit::MdsEventType::ITEM_CREATE: {
        if(auto it = invent.addItem(i.item,1,world())) {
          invent.putToSlot(*this,it->clsId(),i.slot[0]);
          }
        break;
        }
      case zenkit::MdsEventType::ITEM_INSERT: {
        invent.putCurrentToSlot(*this,i.slot[0]);
        break;
        }
      case zenkit::MdsEventType::ITEM_REMOVE:
      case zenkit::MdsEventType::ITEM_DESTROY: {
        invent.clearSlot(*this, "", i.def != zenkit::MdsEventType::ITEM_REMOVE);
        break;
        }
      case zenkit::MdsEventType::ITEM_PLACE: {
        if(currentInteract!=nullptr)
          Inventory::moveItem(*this, invent, *currentInteract);
        break;
        }
      case zenkit::MdsEventType::ITEM_EXCHANGE: {
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
      case zenkit::MdsEventType::SET_FIGHT_MODE:
        break;
      case zenkit::MdsEventType::MUNITION_PLACE: {
        auto active=invent.activeWeapon();
        if(active!=nullptr) {
          const int32_t munition = active->handle().munition;
          invent.putAmmunition(*this,uint32_t(munition),i.slot[0]);
          }
        break;
        }
      case zenkit::MdsEventType::MUNITION_REMOVE: {
        invent.putAmmunition(*this,0,"");
        break;
        }
      case zenkit::MdsEventType::TORCH_DRAW:
        setTorch(true);
        break;
      case zenkit::MdsEventType::TORCH_INVENTORY:
        processDefInvTorch();
        break;
      case zenkit::MdsEventType::TORCH_DROP:
        dropTorch();
        break;
      case zenkit::MdsEventType::SOUND_DRAW:
        break;
      case zenkit::MdsEventType::SOUND_UNDRAW:
        break;
      case zenkit::MdsEventType::MESH_SWAP:
        break;
      case zenkit::MdsEventType::HIT_LIMB:
        break;
      case zenkit::MdsEventType::HIT_DIRECTION:
        break;
      case zenkit::MdsEventType::DAMAGE_MULTIPLIER:
        break;
      case zenkit::MdsEventType::PARRY_FRAME:
        break;
      case zenkit::MdsEventType::OPTIMAL_FRAME:
        break;
      case zenkit::MdsEventType::HIT_END:
        break;
      case zenkit::MdsEventType::COMBO_WINDOW:
        break;
      case zenkit::MdsEventType::UNKNOWN:
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
    v = nextV;
    // check health, in case of negative chg
    checkHealth(true,false);
    }
  }

void Npc::tickAnimationTags() {
  if(Gothic::physicsFlags().meleeBladeShadow || (meleeShadow && meleeShadow->window))
    syncMeleeShadowSwing();
  Animation::EvCount ev;
  const bool hasEvents = visual.processEvents(owner,lastEventTime,ev);
  visual.processLayers(owner);
  visual.setNpcEffect(owner,*this,hnpc->effect,hnpc->flags);
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
  // Only NPCs inserted in the live world are ticked. A restored NPC starts
  // with fresh shadow geometry, never a sweep from its former holding point.
  meleeShadowSuspended = false;
  headInput.reset();
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
  visual.updateMouth(*this,dt);
  visual.updateGestures(*this,dt);
  visual.updateBlink(*this,dt);

  if(!visual.pose().hasAnim())
    setAnim(AnimationSolver::Idle);

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

  if(waitTime>=owner.tickCount() || aniWaitTime>=owner.tickCount() || outWaitTime>owner.tickCount()) {
    traceCombatGate("tick_wait_or_animation_lock");
    if(physicalMovement)
      return;
    if(!isPlayer() && go2.flag!=GT_Flee && faiWaitTime<owner.tickCount() && currentTarget!=nullptr) {
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

void Npc::nextAiAction(AiQueue& queue, uint64_t dt) {
  if(isInAir() && !hasQueuedPhysicalSpellState(queue))
    return;
  if(queue.size()==0)
    return;
  auto act = queue.pop();
  switch(act.act) {
    case AI_None: break;
    case AI_LookAtNpc:{
      currentLookAt=nullptr;
      currentLookAtNpc=act.target;
      break;
      }
    case AI_LookAt:{
      currentLookAtNpc=nullptr;
      currentLookAt=act.point;
      break;
      }
    case AI_TurnAway: {
      if(!prepareTurn()) {
        queue.pushFront(std::move(act));
        break;
        }
      if(act.target!=nullptr && implTurnAway(*act.target,dt)) {
        queue.pushFront(std::move(act));
        break;
        }
      break;
      }
    case AI_TurnToNpc: {
      if(!prepareTurn()) {
        queue.pushFront(std::move(act));
        break;
        }
      if(act.target!=nullptr && implTurnTo(*act.target,dt)) {
        queue.pushFront(std::move(act));
        break;
        }
      // Not looking quite correct in dialogs, when npc turns around
      // Example: Esteban dialog
      // currentLookAt    = nullptr;
      // currentLookAtNpc = nullptr;
      break;
      }
    case AI_WhirlToNpc: {
      if(!prepareTurn()) {
        queue.pushFront(std::move(act));
        break;
        }
      if(act.target!=nullptr && implWhirlTo(*act.target,dt)) {
        queue.pushFront(std::move(act));
        break;
        }
      break;
      }
    case AI_GoToNpc:
      if(!setInteraction(nullptr)) {
        queue.pushFront(std::move(act));
        break;
        }
      attachToPoint(nullptr);
      go2.set(act.target);
      go2.nativeCombatTarget = act.nativeCombatTarget==act.target ? act.nativeCombatTarget : nullptr;
      wayPath.clear();
      break;
    case AI_GoToNextFp: {
      if(!setInteraction(nullptr)) {
        queue.pushFront(std::move(act));
        break;
        }
      auto fp = owner.findNextFreePoint(*this,act.s0);
      if(fp!=nullptr) {
        attachToPoint(fp);
        go2.set(fp,GoToHint::GT_NextFp);
        wayPath.clear();
        }
      break;
      }
    case AI_GoToPoint: {
      if(isInAir() || !setInteraction(nullptr)) {
        queue.pushFront(std::move(act));
        break;
        }
      if(wayPath.last()!=act.point) {
        wayPath     = owner.wayTo(*this,*act.point);
        auto wpoint = wayPath.pop();

        if(wpoint!=nullptr) {
          go2.set(wpoint);
          attachToPoint(wpoint);
          } else {
          attachToPoint(act.point);
          clearGoTo();
          }
        }
      break;
      }
    case AI_StopLookAt:
      currentLookAtNpc=nullptr;
      currentLookAt=nullptr;
      visual.setHeadRotation(0,0);
      break;
    case AI_RemoveWeapon:
      if(!isDead()) {
        if(closeWeapon(false)) {
          stopWalkAnimation();
          }
        auto ws = weaponState();
        if(ws!=WeaponState::NoWeapon){
          queue.pushFront(std::move(act));
          }
        }
      break;
    case AI_StartState:
      startQueuedState(act);
      break;
    case AI_PlayAnim:{
      if(auto sq = playAnimByName(act.s0,BS_NONE)) {
        implAniWait(uint64_t(sq->totalTime()));
        implFaiWait(uint64_t(sq->totalTime()));
        } else {
        if(visual.hasAnim(act.s0))
          queue.pushFront(std::move(act));
        }
      break;
      }
    case AI_PlayAnimBs:{
      BodyState bs = BodyState(act.i0);
      if(auto sq = playAnimByName(act.s0,bs)) {
        implAniWait(uint64_t(sq->totalTime()));
        implFaiWait(uint64_t(sq->totalTime()));
        } else {
        if(visual.hasAnim(act.s0)) {
          queue.pushFront(std::move(act));
          } else {
          /* ZS_MM_Rtn_Sleep will set NPC_WALK mode and run T_STAND_2_SLEEP animation.
           * The problem is: T_STAND_2_SLEEP may not exists, in that case only NPC_WALK should be applied,
           * we will do so by playing Idle anim.
           */
          setAnim(Anim::Idle);
          }
        }
      break;
      }
    case AI_Wait:
      if(isExternalControl() && CommandLine::inst().isDevMode()) {
        const auto* symbol = owner.script().findSymbol(instanceSymbol());
        Log::i("[ai] queue_wait ",symbol!=nullptr ? symbol->name() : ""," game_ms=",owner.tickCount(),
               " duration_ms=",act.i0);
        }
      implAiWait(uint64_t(act.i0));
      break;
    case AI_StandUp:
    case AI_StandUpQuick: {
      if(ragdoll) {
        if(isDead())
          break;
        if(!recoverRagdoll()) {
          queue.pushFront(std::move(act));
          break;
          }
        }
      const auto bs = bodyStateMasked();
      // NOTE: B_ASSESSTALK calls AI_StandUp, to make npc stand, if it's not on a chair or something
      if(interactive()!=nullptr) {
        if((interactive()->isLadder() && !isPlayer()) || !setInteraction(nullptr,false)) {
          queue.pushFront(std::move(act));
          }
        break;
        }
      else if(bs==BS_UNCONSCIOUS || bs==BS_LIE) {
        if(!setAnim(Anim::Idle))
          queue.pushFront(std::move(act)); else
          implAniWait(visual.pose().animationTotalTime());
        }
      else if(bs!=BS_DEAD) {
        visual.stopAnim(*this,"");
        setStateItem(MeshObjects::Mesh(),"");
        setAnim(Anim::Idle);
        }
      break;
      }
    case AI_EquipArmor:
      invent.equipArmor(act.i0,*this);
      break;
    case AI_EquipBestArmor:
      invent.equipBestArmor(*this);
      break;
    case AI_EquipMelee:
      invent.equipBestMeleeWeapon(*this);
      break;
    case AI_EquipRange:
      invent.equipBestRangedWeapon(*this);
      break;
    case AI_UseMob: {
      if(act.i0<0) {
        if(!setInteraction(nullptr))
          queue.pushFront(std::move(act));
        break;
        }
      /*
       * Rhademes doesn't quit talk properly
      if(owner.script().isTalk(*this)) {
        queue.pushFront(std::move(act));
        break;
        }*/

      auto inter = owner.availableMob(*this,act.s0);
      if(inter==nullptr) {
        /* in L`Hiver, version 1.3 there is a typo: "COOL" instead of "BSCOOL"
         * maybe 'scheme' need to be checked loosely, or maybe ignored.
         *
         * For now, if no mob found - discard command, to avoid npc soft-lock.
         */
        // queue.pushFront(std::move(act));
        break;
        }

      if(currentInteract!=nullptr && inter!=currentInteract) {
        setInteraction(nullptr);
        queue.pushFront(std::move(act));
        break;
        }

      if(inter!=nullptr) {
        auto pos = inter->nearestPoint(*this);
        if(currentInteract==nullptr && !MoveAlgo::isClose(*this, pos, MAX_AI_USE_DISTANCE)) { // too far
          go2.set(pos);
          // go to MOBSI and then complete AI_UseMob
          queue.pushFront(std::move(act));
          return;
          }
        if(!setInteraction(inter)) {
          // queue.pushFront(std::move(act));
          }
        }

      if(currentInteract==nullptr || currentInteract->stateId()!=act.i0) {
        queue.pushFront(std::move(act));
        return;
        }

      clearGoTo();
      break;
      }
    case AI_UseItem: {
      if(!isStanding()) {
        setAnim(Npc::Anim::Idle);
        queue.pushFront(std::move(act));
        break;
        }
      if(act.i0!=0)
        useItem(uint32_t(act.i0));
      break;
      }
    case AI_UseItemToState:
      if(act.i0!=0) {
        uint32_t itm   = uint32_t(act.i0);
        int      state = act.i1;
        if(state>0)
          visual.stopDlgAnim(*this);
        if(!invent.putState(*this,state>=0 ? itm : 0,state))
          queue.pushFront(std::move(act));
        }
      break;
    case AI_Teleport: {
      setPosition (act.point->position() );
      setDirection(act.point->direction());
      if(isPlayer()) {
        updateTransform();
        Gothic::inst().camera()->reset(this);
        }
      }
      break;
    case AI_DrawWeapon:
      if(canSwitchWeapon()) {
        if(!drawWeaponMelee() &&
           !drawWeaponBow())
          queue.pushFront(std::move(act));
        }
      break;
    case AI_DrawWeaponMelee:
      if(canSwitchWeapon()) {
        if(!drawWeaponMelee())
          queue.pushFront(std::move(act));
        }
      break;
    case AI_DrawWeaponRange:
      if(canSwitchWeapon()) {
        if(!drawWeaponBow())
          queue.pushFront(std::move(act));
        }
      break;
    case AI_DrawSpell: {
      if(canSwitchWeapon()) {
        const int32_t spell = act.i0;
        if(!invent.hasSpell(spell))
          break;
        if(drawSpell(spell))
          aiExpectedInvest = act.i1; else
          queue.pushFront(std::move(act));
        }
      break;
      }
    case AI_Attack:
      if(!BodyController::owns(*this) && currentTarget!=nullptr) {
        if(!fghAlgo.fetchInstructions(*this,*currentTarget,owner.script()))
          queue.pushFront(std::move(act));
        }
      break;
    case AI_Flee:
      if(!implAiFlee(dt))
        queue.pushFront(std::move(act));
      break;
    case AI_Dodge:
      if(auto sq = setAnimAngGet(Anim::MoveBack)) {
        visual.setAnimRotate(*this,0);
        implAniWait(uint64_t(sq->totalTime()));
        } else {
        queue.pushFront(std::move(act));
        }
      break;
    case AI_UnEquipWeapons:
      invent.unequipWeapons(owner.script(),*this);
      break;
    case AI_UnEquipArmor:
      invent.unequipArmor(owner.script(),*this);
      break;
    case AI_Output:
    case AI_OutputSvm:
    case AI_OutputSvmOverlay:{
      if(performOutput(act)) {
        uint64_t msgTime = 0;
        if(aiPolicy!=NpcProcessPolicy::AiFar2) {
          if(act.act==AI_Output) {
            msgTime = owner.script().messageTime(act.s0);
            } else {
            auto svm  = owner.script().messageFromSvm(act.s0,hnpc->voice);
            msgTime   = owner.script().messageTime(svm);
            }
          visual.startFaceAnim(*this,"VISEME",1,msgTime);
          // the speaker and the one it speaks to look at each other while the line lasts
          if(act.target!=nullptr && act.target!=this) {
            static const uint64_t tail = 1000;
            setTalkPartner(act.target,msgTime+tail);
            act.target->setTalkPartner(this,msgTime+tail);
            }
          }
        if(TestBench::isActive())
          TestBench::npcOutput(*this,act.s0,act.target,msgTime);
        if(act.act!=AI_OutputSvmOverlay) {
          visual.startAnimDialog(*this);
          visual.setAnimRotate(*this,0);
          }
        } else {
        queue.pushFront(std::move(act));
        }
      break;
      }
    case AI_ProcessInfo: {
      const int PERC_DIST_DIALOG = 2000;

      if(act.target==nullptr)
        break;

      if(owner.isInDialog()) {
        queue.pushFront(std::move(act));
        break;
        }

      if(this!=act.target && act.target->isPlayer() && act.target->currentInteract!=nullptr) {
        //queue.pushFront(std::move(act));
        break;
        }

      if(act.target->qDistTo(*this)>PERC_DIST_DIALOG*PERC_DIST_DIALOG) {
        break;
        }

      if(act.target->interactive()==nullptr && !act.target->isAiBusy())
        act.target->stopWalkAnimation();
      if(interactive()==nullptr && !isAiBusy())
        stopWalkAnimation();

      if(auto p = owner.script().openDlgOuput(*this,*act.target)) {
        outputPipe = p;
        setOther(act.target);
        act.target->setOther(this);
        act.target->outputPipe = p;
        } else {
        queue.pushFront(std::move(act));
        }
      }
      break;
    case AI_StopProcessInfo:
      if(outputPipe->close()) {
        outputPipe = owner.script().openAiOuput();
        if(currentOther!=nullptr)
          currentOther->outputPipe = owner.script().openAiOuput();
        } else {
        queue.pushFront(std::move(act));
        }
      break;
    case AI_ContinueRoutine:
      resumeAiRoutine();
      break;
    case AI_AlignToWp:
    case AI_AlignToFp:{
      if(auto fp = currentFp){
        if(fp->dir.x!=0.f || fp->dir.z!=0.f){
          if(implTurnTo(fp->dir.x,fp->dir.z,AnimationSolver::TurnType::Std,dt))
            queue.pushFront(std::move(act));
          }
        }
      break;
      }
    case AI_SetNpcsToState:{
      const int32_t r = act.i0*act.i0;
      owner.detectNpc(position(),float(hnpc->senses_range),[&act,this,r](Npc& other) {
        if(&other==this)
          return;
        if(other.isDead())
          return;
        if(qDistTo(other)>float(r))
          return;
        other.aiPush(AiQueue::aiStartState(act.func,1,other.currentOther,other.currentVictim,other.hnpc->wp));
        });
      break;
      }
    case AI_SetWalkMode:{
      setWalkMode(WalkBit(act.i0));
      break;
      }
    case AI_FinishingMove:{
      if(act.target==nullptr || !act.target->isUnconscious())
        break;

      if(!fghAlgo.isInFinishRange(*this,*act.target,owner.script())){
        queue.pushFront(std::move(act));
        go2.set(act.target);
        setAnim(Npc::Anim::Move);
        implGoTo(dt,fghAlgo.attackFinishDistance(owner.script()));
        }
      else if(!isStanding()) {
        clearGoTo();
        queue.pushFront(std::move(act));
        }
      else if(!implTurnTo(*act.target,dt)) {
        queue.pushFront(std::move(act));
        }
      else if(canFinish(*act.target)){
        setTarget(act.target);
        if(!finishingMove())
          queue.pushFront(std::move(act));
        }
      break;
      }
    case AI_TakeItem:{
      if(act.item==nullptr)
        break;
      if(takeItem(*act.item)==nullptr)
        queue.pushFront(std::move(act));
      break;
      }
    case AI_GotoItem:{
      go2.set(act.item);
      break;
      }
    case AI_PointAt:{
      if(act.point==nullptr)
        break;
      if(!implPointAt(act.point->position()))
        queue.pushFront(std::move(act));
      break;
      }
    case AI_PointAtNpc:{
      if(act.target==nullptr)
        break;
      if(!implPointAt(act.target->position()))
        queue.pushFront(std::move(act));
      break;
      }
    case AI_StopPointAt:{
      visual.stopAnim(*this,"T_POINT");
      break;
      }
    case AI_PrintScreen:{
      auto  msg     = act.s0;
      auto  posx    = act.i0;
      auto  posy    = act.i1;
      int   timesec = act.i2;
      auto  font    = act.s1;

      bool complete = false;
      if(aiOutputBarrier<=owner.tickCount()) {
        if(outputPipe->printScr(*this,timesec,msg,posx,posy,font))
          complete = true;
        }

      if(!complete)
        queue.pushFront(std::move(act));
      break;
      }
    }
  }

bool Npc::startQueuedState(const AiQueue::AiAction& act) {
  // Preserve the routine end time for transitions such as ZS_GotoBed -> ZS_Sleep.
  if(!startState(act.func,act.s0,aiState.eTime,act.i0==0))
    return false;
  setOther(act.target);
  setVictim(act.victim);
  return true;
  }

bool Npc::startState(ScriptFn id, std::string_view wp) {
  return startState(id,wp,gtime::endOfTime(),false);
  }

bool Npc::startState(ScriptFn id, std::string_view wp, gtime endTime, bool noFinalize) {
  if(!id.isValid())
    return false;
  if(BodyController::owns(*this)) {
    const std::string bodyDeferredWp(wp);
    BodyController::deferNative(*this,[this,id,bodyDeferredWp,endTime,noFinalize] {
      startState(id,bodyDeferredWp,endTime,noFinalize);
      });
    return true;
    }

  if(aiState.funcIni==id) {
    if(!noFinalize) {
      // NOTE: B_AssessQuietSound can cause soft-lock on npc without this
      aiState.started = false;
      }
    if(!wp.empty())
      hnpc->wp = wp;
    return true;
    }

  clearAiQueue();
  clearState(noFinalize);
  if(!wp.empty())
    hnpc->wp = wp;

  {
    // ZS_GotoBed -> ZS_Sleep relie on clean state
    for(size_t i=0; i<PERC_Count; ++i)
      setPerceptionDisable(PercType(i));
  }

  if(wp=="TOT" && aiPolicy!=NpcProcessPolicy::Player && aiPolicy!=NpcProcessPolicy::AiNormal) {
    // workaround for Pedro removal script
    auto& point = owner.deadPoint();
    attachToPoint(nullptr);
    setPosition(point.position());
    }

  auto& st = owner.script().aiState(id);
  if(isPlayer() && !isPlayerEnabledState(st)) {
    // disable for now, as it causes infinite lock in freeze state
    // https://github.com/Try/OpenGothic/issues/906
    // extra 'aiStandup' to avoid issue with B_StopMagicFreeze
    aiPush(AiQueue::aiStandup());
    return false;
    }

  aiState.started      = false;
  aiState.funcIni      = st.funcIni;
  aiState.funcLoop     = st.funcLoop;
  aiState.funcEnd      = st.funcEnd;
  aiState.sTime        = owner.tickCount();
  aiState.eTime        = endTime;
  aiState.loopNextTime = owner.tickCount();
  aiState.hint         = st.name();
  return true;
  }

void Npc::clearState(bool noFinalize) {
  if(BodyController::deferNative(*this,[this,noFinalize] { clearState(noFinalize); })) return;
  if(aiState.funcIni.isValid() && aiState.started) {
    if(!noFinalize)
      owner.script().invokeState(this,currentOther,currentVictim,aiState.funcEnd);  // cleanup
    aiPrevState = aiState.funcIni;
    invent.putState(*this,0,0);
    visual.stopItemStateAnim(*this);
    }
  aiState = AiState();
  }

bool Npc::isPlayerEnabledState(const ::AiState& st) const {
  // allowed player states are hard-coded
  // https://forum.worldofplayers.de/forum/threads/1533803-G1-AI_StartState-hardcoded-ZS-states-for-Player?p=26034737&viewfull=1#post26034737

  static const std::array playerEnabledStatesG1 = {
    "ZS_DEAD",   "ZS_UNCONSCIOUS", "ZS_MAGICFREEZE",
    "ZS_PYRO",   "ZS_ASSESSMAGIC", "ZS_ASSESSSTOPMAGIC",
    "ZS_ZAPPED", "ZS_SHORTZAPPED", "ZS_MAGICSLEEP",
    "ZS_MAGICFEAR"
    };
  static const std::array playerEnabledStatesG2 = {
    "ZS_DEAD",   "ZS_UNCONSCIOUS", "ZS_MAGICFREEZE",
    "ZS_PYRO",   "ZS_ASSESSMAGIC", "ZS_ASSESSSTOPMAGIC",
    "ZS_ZAPPED", "ZS_SHORTZAPPED", "ZS_MAGICSLEEP",
    "ZS_WHIRLWIND"
    };

  const auto* sym = owner.script().findSymbol(st.funcIni);
  if(sym==nullptr)
    return false;

  const auto& playerEnabledStates = (owner.version().game==2 ? playerEnabledStatesG2 : playerEnabledStatesG1);
  for(auto* pState:playerEnabledStates)
    if(sym->name()==pState)
      return true;
  return false;
  }

int Npc::invokeRoutine(ScriptFn function) {
  // A state alone is not provenance: a perception or a bridge command can append
  // story actions while ZS_ATTACK is active. Mark only this state's own callbacks.
  struct Restore final {
    Npc*& slot;
    Npc* previous;
    ~Restore() { slot=previous; }
    } restore{nativeCombatQueueTarget,nativeCombatQueueTarget};
  nativeCombatQueueTarget=std::string_view(aiState.hint)=="ZS_ATTACK" &&
                          (function==aiState.funcIni || function==aiState.funcLoop) ? currentTarget : nullptr;
  return owner.script().invokeState(this,currentOther,currentVictim,function);
  }

void Npc::tickRoutine() {
  if(!aiState.funcIni.isValid() && !isPlayer()) {
    auto r = currentRoutine();
    if(r.callback.isValid()) {
      auto t = endTime(r);
      startState(r.callback, r.wayPointName(), t, false);
      }
    else if(hnpc->start_aistate!=0) {
      auto endTime = owner.time();
      endTime.addMilis(uint64_t(gtime(4, 0).toInt()));
      startState(uint32_t(hnpc->start_aistate), "", endTime, false);
      }
    }

  if(!aiState.funcIni.isValid())
    return;

  if(!aiState.started) {
    aiState.started      = true;
    aiState.loopNextTime = owner.tickCount();
    // WA: for gothic1 dialogs
    perceptionNextTime   = owner.tickCount();
    invokeRoutine(aiState.funcIni);
    return;
    }

  const bool fastPath = (aiPolicy==NpcProcessPolicy::AiFar2 && routines.empty()); //HACK: don't process far away Npc
  if(aiState.loopNextTime<=owner.tickCount()) {
    aiState.loopNextTime = owner.tickCount() + perceptionTimeClampt();
    int loop = LOOP_CONTINUE;
    if(aiState.funcLoop.isValid()) {
      static const float MAX_DIST = 300;
      if(fastPath && currentFp!=nullptr && qDistTo(currentFp) < MAX_DIST*MAX_DIST) {
        loop = LOOP_CONTINUE;
        }
      else if(fastPath && currentFp!=nullptr) {
        // for debugging
        loop = invokeRoutine(aiState.funcLoop);
        }
      else {
        loop = invokeRoutine(aiState.funcLoop);
        }
      } else {
      // ZS_DEATH   have no loop-function, in G1, G2-classic
      // ZS_GETMEAT have no loop-function, in G2-notr
      loop = owner.version().hasZSStateLoop() ? 1 : 0;
      }

    if(aiState.eTime<=owner.time()) {
      // Avoid interruption of ZS_TALK/ZS_ATTACK
      if(currentTarget==nullptr && outputPipe->isFinished())
        loop = LOOP_END;
      }

    if(loop!=LOOP_CONTINUE) {
      clearState(false);
      currentOther  = nullptr;
      currentVictim = nullptr;
      }
    }
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

  const bool g2 = owner.version().game==2;
  const auto SEPERATOR_ORC = g2 ? GIL_SEPERATOR_ORC : GIL_G1_SEPERATOR_ORC;
  const auto G1_UNDEAD = (gil == GIL_G1_ZOMBIE || gil == GIL_G1_UNDEADORC || gil == GIL_G1_SKELETON);
  const auto G2_UNDEAD = (gil == GIL_GOBBO_SKELETON ||
    gil == GIL_SUMMONED_GOBBO_SKELETON || gil == GIL_SKELETON      ||
    gil == GIL_SUMMONED_SKELETON       || gil == GIL_SKELETON_MAGE ||
    gil == GIL_SHADOWBEAST_SKELETON    || gil == GIL_ZOMBIE);

  if(bool(t&TARGET_TYPE_HUMANS) && isHuman())
    return true;
  if(bool(t&TARGET_TYPE_ORCS) && gil>SEPERATOR_ORC)
    return true;
  if(bool(t&TARGET_TYPE_UNDEAD) && g2 && G2_UNDEAD)
    return true;
  if(bool(t&TARGET_TYPE_UNDEAD) && !g2 && G1_UNDEAD)
    return true;

  return false;
  }

void Npc::commitSpell() {
  auto active = invent.getItem(currentSpellCast);
  if(active==nullptr || !active->isSpellOrRune())
    return;

  const int32_t splId = active->spellId();
  const auto&   spl   = owner.script().spellDesc(splId);

  if(owner.version().game==2)
    owner.script().invokeSpell(*this,currentTarget,*active);

  if(active->isSpellShoot()) {
    const int lvl = (castLevel-CS_Emit_0)+1;
    DamageCalculator::Damage dmg={};
    for(size_t i=0; i<zenkit::DamageType::NUM; ++i)
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

void Npc::forEachRoutine(const std::function<void(const RoutineInfo&)>& f) const {
  for(auto& r:routines)
    f(RoutineInfo{r.start,r.end,r.callback,r.wayPointName()});
  }

const Npc::Routine& Npc::currentRoutine(bool assertWp) const {
  // find routine for current time
  // if there is no such routine search counter clock-wise until one is found
  auto time = owner.time().timeInDay();
  for(auto& i:routines) {
    if(assertWp && i.point==nullptr)
      continue;
    if(i.end<i.start && (time<i.end || i.start<=time))
      return i;
    if(i.start<=time && time<i.end)
      return i;
    }

  const auto     day   = gtime(24,0).toInt();
  const Routine* rtn   = nullptr;
  int64_t        delta = std::numeric_limits<int64_t>::max();
  for(auto& i:routines) {
    if(assertWp && i.point==nullptr)
      continue;
    int64_t d = time.toInt() - i.end.toInt();
    if(d<0)
      d += day;
    // take the last one if multiple with same end time exist
    if(d<=delta) {
      rtn   = &i;
      delta = d;
      }
    }

  if(rtn!=nullptr)
    return *rtn;
  static Routine r;
  return r;
  }

const WayPoint* Npc::currentTaPoint() const {
  if(routines.empty())
    return owner.findPoint(hnpc->wp,false);
  return currentRoutine(true).point;
  }

gtime Npc::endTime(const Npc::Routine &r) const {
  auto wtime = owner.time();
  auto time  = wtime.timeInDay();

  //NOTE: should we consider time extension for invalid routine sequences?
  if(r.end<r.start) {
    if(time<r.end)
      return gtime(wtime.day(),r.end.hour(),r.end.minute());
    return gtime(wtime.day()+1,r.end.hour(),r.end.minute());
    }
  if(r.start<r.end) {
    if(r.end.hour()==0 || r.end<time)
      return gtime(wtime.day()+1,r.end.hour(),r.end.minute()); else
      return gtime(wtime.day(),r.end.hour(),r.end.minute());
    }
  if(r.start==r.end && r.end.toInt()==0) {
    // for example Rtn_Start_1081 in NTR is filled with zeros
    return gtime(wtime.day()+1,r.end.hour(),r.end.minute());
    }
  // error - routine is not active now
  return wtime;
  }

BodyState Npc::bodyState() const {
  if(isDead())
    return BS_DEAD;
  if(isUnconscious())
    return BS_UNCONSCIOUS;
  if(isFalling())
    return BS_FALL;

  uint32_t s = visual.pose().bodyState();
  if(auto i = interactive())
    s = i->stateMask();
  return BodyState(s);
  }

BodyState Npc::bodyStateMasked() const {
  BodyState bs = bodyState();
  return BodyState(bs & (BS_MAX | BS_FLAG_MASK));
  }

bool Npc::hasState(BodyState s) const {
  if(visual.pose().hasState(s))
    return true;
  if(auto i = interactive())
    return s==i->stateMask();
  return false;
  }

bool Npc::hasStateFlag(BodyState flg) const {
  if(visual.pose().hasStateFlag(flg))
    return true;
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

void Npc::resumeAiRoutine() {
  clearState(false);
  auto& r = currentRoutine();
  if(r.callback.isValid()) {
    auto t = endTime(r);
    startState(r.callback,r.wayPointName(),t,false);
    }
  }

Item* Npc::addItem(const size_t item, size_t count) {
  return invent.addItem(item,count,owner);
  }

Item* Npc::addItem(std::unique_ptr<Item>&& i) {
  return invent.addItem(std::move(i));
  }

Item* Npc::takeItem(Item& item) {
  const bool npcTheft = !isPlayer() && isServerDeed("take_item");
  if(npcTheft && (serverDeedSimulationError()!=nullptr || item.isMission()))
    return nullptr;
  if(npcTheft) {
    const auto current = HeroDeed::capture(*this,"take_item",nullptr,&item);
    const auto& actualOwner = current->owner.npc.empty() ? current->roomOwner.npc : current->owner.npc;
    if(actualOwner!=serverDeedOwner)
      return nullptr;
    }
  if(interactive()!=nullptr)
    return nullptr;
  if(item.isTorchBurn() && (isUsingTorch() || weaponState()!=WeaponState::NoWeapon))
    return nullptr;

  auto state = bodyStateMasked();
  if(state!=BS_STAND && state!=BS_SNEAK && state!=BS_SWIM && state!=BS_DIVE) {
    return nullptr;
    }

  const auto  dpos = item.midPosition()-centerPosition();
  const auto* sq   = setAnimAngGet(Npc::Anim::ItmGet, Pose::calcAniCombVert(dpos));
  if(sq==nullptr)
    return nullptr;

  if(npcTheft && serverDeedCount>0 && serverDeedCount<item.count()) {
    auto deed = HeroDeed::capture(*this,"take_item",nullptr,&item,nullptr,serverDeedCount);
    deed->collectWitnesses(*this,nullptr);
    auto* taken = addItem(item.clsId(),serverDeedCount);
    if(taken==nullptr)
      return nullptr;
    item.setCount(item.count()-serverDeedCount);
    Gothic::inst().onHeroDeed(*deed);
    implAniWait(uint64_t(sq->totalTime()));
    return taken;
    }
  std::unique_ptr<Item> ptr = owner.takeItem(item);
  if(ptr!=nullptr && ptr->isTorchBurn()) {
   if(!toggleTorch())
     return nullptr;
    size_t torchId = owner.script().findSymbolIndex("ItLsTorch");
    if(torchId!=size_t(-1))
      return nullptr;
    ptr.reset(new Item(owner,torchId,Item::T_Inventory));
    }

  auto it = ptr.get();
  if(it==nullptr)
    return nullptr;

  auto deed = HeroDeed::capture(*this,"take_item",nullptr,it);
  if(deed!=nullptr && !isPlayer())
    deed->collectWitnesses(*this,nullptr);
  it = addItem(std::move(ptr));
  if(isPlayer() && it!=nullptr)
    owner.sendPassivePerc(*this,*this,*it,PERC_ASSESSTHEFT,std::move(deed));
  else if(it!=nullptr && deed!=nullptr)
    Gothic::inst().onHeroDeed(*deed);

  implAniWait(uint64_t(sq->totalTime()));
  return it;
  }

void Npc::onWldItemRemoved(const Item& itm) {
  aiQueue.onWldItemRemoved(itm);
  aiQueueOverlay.onWldItemRemoved(itm);
  }

void Npc::addItem(size_t id, Interactive &chest, size_t count) {
  auto* item = chest.inventory().getItem(id);
  if(!isPlayer() && isServerDeed("take_item") &&
     (serverDeedSimulationError()!=nullptr || (item!=nullptr && item->isMission())))
    return;
  auto deed = HeroDeed::capture(*this,"take_item",nullptr,item,&chest,
                                std::min(count,chest.inventory().itemCount(id)));
  if(deed!=nullptr && !isPlayer())
    deed->collectWitnesses(*this,nullptr);
  const auto before = invent.itemCount(id);
  Inventory::transfer(invent,chest.inventory(),nullptr,id,count,owner);
  if(deed!=nullptr && invent.itemCount(id)>before) {
    deed->count = invent.itemCount(id)-before;
    if(isPlayer())
      if(auto use = chest.heroUseDeed())
        deed->witnesses = use->witnesses;
    Gothic::inst().onHeroDeed(*deed);
    }
  }

void Npc::addItem(size_t id, Npc &from, size_t count) {
  Inventory::transfer(invent,from.invent,&from,id,count,owner);
  }

void Npc::moveItem(size_t id, Interactive &to, size_t count) {
  Inventory::transfer(to.inventory(),invent,this,id,count,owner);
  }

void Npc::sellItem(size_t id, Npc &to, size_t count) {
  if(id==owner.script().goldId()->index())
    return;
  int32_t price = invent.sellPriceOf(id);
  Inventory::transfer(to.invent,invent,this,id,count,owner);
  invent.addItem(owner.script().goldId()->index(),size_t(price)*count,owner);
  }

void Npc::buyItem(size_t id, Npc &from, size_t count) {
  if(id==owner.script().goldId()->index())
    return;

  int32_t price = from.invent.priceOf(id);
  if(price>0 && size_t(price)*count>invent.goldCount()) {
    count = invent.goldCount()/size_t(price);
    }
  if(count==0) {
    owner.script().printCannotBuyError(*this);
    return;
    }

  Inventory::transfer(invent,from.invent,nullptr,id,count,owner);
  if(price>=0)
    invent.delItem(owner.script().goldId()->index(),size_t( price)*count,*this); else
    invent.addItem(owner.script().goldId()->index(),size_t(-price)*count,owner);
  }

void Npc::dropItem(size_t id, size_t count) {
  if(id==size_t(-1))
    return;
  size_t cnt = invent.itemCount(id);
  if(count>cnt)
    count = cnt;
  if(count<1)
    return;

  auto sk = visual.visualSkeleton();
  if(sk==nullptr)
    return;

  size_t rightHand = sk->findNode("ZS_RIGHTHAND");
  if(rightHand==size_t(-1))
    return;

  if(!setAnim(Anim::ItmDrop))
    return;

  auto mat = visual.transform();
  if(rightHand<visual.pose().boneCount())
    mat = visual.pose().bone(rightHand);

  auto it = owner.addItemDyn(id,mat,hnpc->symbol_index());
  it->setCount(count);
  invent.delItem(id,count,*this);
  }

void Npc::clearInventory() {
  invent.clear(owner.script(),*this);
  }

Item* Npc::currentArmor() {
  return invent.currentArmor();
  }

Item* Npc::currentMeleeWeapon() {
  return invent.currentMeleeWeapon();
  }

Item* Npc::currentRangedWeapon() {
  return invent.currentRangedWeapon();
  }

Item* Npc::currentShield() {
  return invent.currentShield();
  }

Vec3 Npc::mapWeaponBone() const {
  return visual.mapWeaponBone();
  }

Vec3 Npc::mapHeadBone() const {
  return visual.mapHeadBone();
  }

Npc::EyePose Npc::navigationEyePose() const {
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

Vec3 Npc::mapBone(std::string_view bone) const {
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
    setAnimRotate(0);
    return false;
    }

  if(!isRotationAllowed())
    return false;

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

size_t Npc::itemCount(size_t id) const {
  return invent.itemCount(id);
  }

Item* Npc::activeWeapon() {
  return invent.activeWeapon();
  }

Item *Npc::getItem(size_t id) {
  return invent.getItem(id);
  }

void Npc::delItem(size_t item, uint32_t amount) {
  invent.delItem(item,amount,*this);
  }

void Npc::useItem(size_t item) {
  useItem(item,Item::NSLOT,false);
  }

void Npc::useItem(size_t item, uint8_t slotHint, bool force) {
  invent.use(item,*this,slotHint,force);
  }

void Npc::setCurrentItem(size_t item) {
  invent.setCurrentItem(item);
  }

void Npc::unequipItem(size_t item) {
  invent.unequip(item,*this);
  }

bool Npc::canSwitchWeapon() const {
  if(isUnconscious())
    return false;
  auto bs = bodyStateMasked();
  if(bs==BS_STAND || bs==BS_WALK || bs==BS_RUN || bs==BS_SNEAK || bs==BS_NONE)
    return true;
  return false;
  // return !(mvAlgo.isFalling() || mvAlgo.isInAir() || mvAlgo.isSlide() || mvAlgo.isSwim());
  }

bool Npc::closeWeapon(bool noAnim) {
  auto weaponSt=weaponState();
  if(weaponSt==WeaponState::NoWeapon)
    return true;
  if(!noAnim && !visual.startAnim(*this,WeaponState::NoWeapon))
    return false;
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
  const bool trace = TestBench::bodyTracing() && currentTarget!=nullptr;
  const auto before = trace ? combatPoseIdentity() : std::string();
  const auto result = [&](bool accepted) {
    BodyObservations::noteEvent(*this,currentTarget,accepted ? "animation_accepted" : "animation_rejected",owner.tickCount());
    if(trace)
      traceCombatAttempt(anim,accepted,before,"do_attack");
    return accepted;
    };
  auto weaponSt = weaponState();
  if(weaponSt==WeaponState::NoWeapon || weaponSt==WeaponState::Mage)
    return result(false);

  if(mvAlgo.isSwim())
    return result(false);

  if(bs==BS_PARADE && hasState(BS_PARADE))
    return result(false);

  const auto wlk = walkMode();
  visual.setAnimRotate(*this,0);
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
  auto weaponSt=weaponState();
  if(weaponSt!=WeaponState::Fist)
    return false;
  visual.setAnimRotate(*this,0);
  return setAnim(Anim::AttackBlock);
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
      bool g2  = owner.version().game==2;
      if(g2 || visual.hasAnim(string_frm("T_MAGRUN_2_",ani,"CAST")))
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

  if(owner.version().game==1) {
    changeAttribute(ATR_MANA,-1,false);
    if(!isPlayer() && code!=SpellCode::SPL_SENDCAST)
      assert(attribute(ATR_MANA)>0);
    }

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
  if(Gothic::inst().version().game==1) {
    b.setHitChance(float(hnpc->attribute[ATR_DEXTERITY])/100.f);
    if(rgn!=nullptr && rgn->isCrossbow())
      b.setCritChance(float(talentsVl[TALENT_CROSSBOW])/100.f); else
      b.setCritChance(float(talentsVl[TALENT_BOW]     )/100.f);
    }
  else {
    if(rgn!=nullptr && rgn->isCrossbow())
      b.setHitChance(float(hnpc->hitchance[TALENT_CROSSBOW])/100.f); else
      b.setHitChance(float(hnpc->hitchance[TALENT_BOW]     )/100.f);
    }
  return true;
  }

float Npc::animProgress(std::string_view name) const {
  return visual.pose().animProgress(name,owner.tickCount());
  }

void Npc::setManualAim(bool on, const Tempest::Vec3& point, const Tempest::Vec2& sway, const Tempest::Vec2& tremor) {
  manualAim    = on;
  manualPoint  = point;
  manualSway   = on ? sway   : Tempest::Vec2();
  manualTremor = on ? tremor : Tempest::Vec2();
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
    shot = releaseArrow(arch.draw(),st);
    arch.onShot(shot);
    }
  setManualAim(true,at,arch.sway(),arch.tremor());
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
    if(idle && !isDown() && (ws==WeaponState::Bow || ws==WeaponState::CBow))
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
  return hnpc->flags & zenkit::NpcFlag::IMMORTAL;
  }

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
    if(!TestBench::bodyTracing() || currentTarget==nullptr)
      return;
    TestBench::bodyEvent(*this,&pl,"native_perception",owner.tickCount(),
                         "\"perception_kind\":\"periodic\",\"gate\":"+TestBench::bodyJson(gate)+
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
    if(!TestBench::bodyTracing() || currentTarget==nullptr)
      return;
    TestBench::bodyEvent(*this,&pl,"native_perception",owner.tickCount(),
                         "\"perception_kind\":\"dispatch\",\"gate\":"+TestBench::bodyJson(gate)+
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
  TestBench::physicsNoiseHeard(*this,noise,callbackDispatched);
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

bool Npc::isInState(ScriptFn stateFn) const {
  return aiState.funcIni==stateFn;
  }

bool Npc::isInRoutine(ScriptFn stateFn) const {
  auto& rout = currentRoutine();
  return rout.callback==stateFn && aiState.funcIni==stateFn;
  }

bool Npc::wasInState(ScriptFn stateFn) const {
  return aiPrevState==stateFn;
  }

uint64_t Npc::stateTime() const {
  return owner.tickCount()-aiState.sTime;
  }

void Npc::setStateTime(int64_t time) {
  aiState.sTime = owner.tickCount()-uint64_t(time);
  }

void Npc::addRoutine(gtime s, gtime e, uint32_t callback, std::string_view point) {
  auto wp = world().findPoint(point,false);

  Routine r;
  r.start    = s;
  r.end      = e;
  r.callback = callback;
  r.point    = wp;
  if(wp==nullptr)
    r.fallbackName = point;
  routines.push_back(r);

  std::stable_sort(routines.begin(), routines.end(), [](const Routine& l, const Npc::Routine& r) {
    return l.start < r.start;
    });
  }

void Npc::clearRoutine() {
  routines.clear();
  }

void Npc::excRoutine(size_t callback) {
  routines.clear();
  owner.script().invokeState(this,currentOther,currentVictim,callback);
  // aiState.eTime = gtime();
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

auto Npc::navigationGoal() const -> NavigationGoal {
  NavigationGoal ret;
  if(!go2.empty()) {
    ret.active = true;
    ret.npc = go2.npc;
    ret.point = go2.wp;
    // Preserve the final goal as intermediate waypoints change along the same walk.
    if(go2.flag==GT_Way && go2.npc==nullptr && wayPath.last()!=nullptr)
      ret.point = wayPath.last();
    ret.position = ret.point!=nullptr ? ret.point->position() : go2.target();
    ret.kind = ret.npc!=nullptr ? "npc" : (ret.point!=nullptr ? (ret.point->isFreePoint() ? "freepoint" : "waypoint") : "point");
    if(ret.point!=nullptr)
      ret.name = ret.point->name;
    else if(go2.flag==GT_Item)
      ret.kind = "item";
    return ret;
    }

  // A future walk behind AI_Wait/occupation is not a moving NPC. Only the ready head counts.
  const auto now = owner.tickCount();
  if(waitTime>=now || aniWaitTime>=now || outWaitTime>now)
    return ret;
  const auto* action = aiQueue.front();
  if(action==nullptr)
    return ret;
  if(action->act==AI_GoToPoint && action->point!=nullptr) {
    ret.active = true;
    ret.point = action->point;
    ret.position = action->point->position();
    ret.kind = ret.point->isFreePoint() ? "freepoint" : "waypoint";
    ret.name = ret.point->name;
    }
  else if(action->act==AI_GoToNpc && action->target!=nullptr) {
    ret.active = true;
    ret.npc = action->target;
    ret.position = action->target->position();
    ret.kind = "npc";
    }
  else if(action->act==AI_GoToNextFp) {
    // Resolving the freepoint would acquire runtime state. Keep the queued name until scripts do it.
    ret.active = true;
    ret.kind = "freepoint_query";
    ret.name = action->s0;
    }
  else if(action->act==AI_GotoItem && action->item!=nullptr) {
    ret.active = true;
    ret.kind = "item";
    ret.position = action->item->position();
    }
  return ret;
  }

bool Npc::isAiBusy() const {
  return !isAiQueueEmpty() ||
         aniWaitTime>=owner.tickCount() ||
         outWaitTime>=owner.tickCount();
  }

void Npc::clearAiQueue() {
  if(BodyController::deferNative(*this,[this] { clearAiQueue(); })) return;
  stopPhysicsInvestigation(false);
  currentLookAt    = nullptr;
  currentLookAtNpc = nullptr;
  visual.setHeadRotation(0,0);

  aiQueue.clear();
  aiQueueOverlay.clear();
  aniWaitTime = 0;
  waitTime    = 0;
  faiWaitTime = 0;
  fghAlgo.onClearTarget();
  wayPath.clear();
  clearGoTo();
  }

void Npc::attachToPoint(const WayPoint *p) {
  currentFp     = p;
  currentFpLock = FpLock(currentFp);
  }

bool Npc::goToPosition(const Vec3& position) {
  if(isInAir() || !setInteraction(nullptr))
    return false;
  attachToPoint(nullptr);
  wayPath.clear();
  go2.clear();
  // GoTo::isClose(Vec3) measures from the actor's center, whereas navmesh points
  // (like WayPoint::groundPos) lie on the walkable surface.
  go2.set(position+(centerPosition()-this->position()));
  go2.pointDistance = 20.f;
  return true;
  }

void Npc::clearGoTo() {
  if(!go2.empty()) {
    stopWalking();
    go2.clear();
    }
  }

void Npc::stopWalking() {
  if(setAnim(Anim::Idle))
    return;
  // hard stop
  stopWalkAnimation();
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
  auto head = oth.visual.mapHeadBone();
  if(canRayHitPoint(head,freeLos))
    return true;
  return false;
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

bool Npc::canSeeNpcOverEdge(const Npc& oth) const {
  const auto ws = weaponState();
  if(!Aim::isEnabled() || (ws!=WeaponState::Bow && ws!=WeaponState::CBow && ws!=WeaponState::Mage))
    return false;
  const Vec3  head = visual.mapHeadBone();
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
  return canRayHitPoint(eye,tg,180.f) || canRayHitPoint(eye,oth.visual.mapHeadBone(),180.f);
  }

bool Npc::canSeeSource() const {
  const auto head = visual.mapHeadBone();
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
  return canRayHitPoint(visual.mapHeadBone(), pos, angOverride, extRange);
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
  const auto  cen   = visual.mapHeadBone();
  const auto  dir   = itMid - cen;
  const float range = float(hnpc->senses_range);

  if(dir.quadLength()>range*range)
    return false;

  if(!freeLos) {
    float dx  = dir.x, dz = dir.z;
    float dir = angleDir(dx,dz);
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

void Npc::resetCharacterVisualHeight() {
  if(characterVisualYOffset!=0)
    durtyTranform |= TR_Pos;
  characterVisualSteps.clear();
  characterVisualYOffset = 0;
  characterVisualHasSample = false;
  characterVisualGroundSample = false;
  }

void Npc::updateCharacterVisualHeight() {
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
    updateMeleeShadow();
    return;
    }
  if(ragdoll) {
    if(syncAtt || force)
      visual.syncAttaches();
    updateMeleeShadow();
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
  if(syncAtt)
    visual.syncAttaches();
  updateMeleeShadow();
  }

bool Npc::updateLyingVisualHeight() {
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
