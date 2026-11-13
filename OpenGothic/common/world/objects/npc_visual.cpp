#include "npc.h"
#include "npc_melee.h"
#include "npc_melee_shadow.h"

#include "graphics/mesh/skeleton.h"
#include "graphics/effect.h"
#include "game/bodycontroller.h"
#include "game/bodyobservations.h"
#include "game/gamescript.h"
#include "world/animationlod.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "utils/fileext.h"
#include "utils/string_frm.h"
#include "camera.h"
#include "frameprofile.h"
#include "gothic.h"
#include "resources.h"
#include "world/benchhooks.h"

#include <Tempest/Log>
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace Tempest;

static std::string_view humansTorchOverlay = "_TORCH.MDS";

std::string_view Npc::cameraTargetScheme() const {
  auto inter = interactive();
  return inter!=nullptr ? inter->schemeName() : std::string_view();
  }

std::string_view Npc::cameraTargetPosScheme() const {
  auto inter = interactive();
  return inter!=nullptr ? inter->posSchemeName() : std::string_view();
  }

void Npc::stopDlgAnim(bool atEase) {
  visual.stopDlgAnim(*this,atEase);
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
  auto  vbody = body.empty() ? MeshObjects::Mesh() : owner.addView(bodyVisual(),vColor,0,bdColor);
  visual.setVisualBody(*this,std::move(vhead),std::move(vbody),bdColor);
  updateArmor();

  durtyTranform|=TR_Pos; // update obj matrix
  }

// The body without armour. Our content may draw one texture variant of a shared body as its own mesh
// ("bodies" of models/render-rigs.json): the hero's clothes are a variant of the naked body all men have.
std::string Npc::bodyVisual() const {
  const std::string own = Resources::bodyOfVariant(body,vColor);
  return FileExt::addExt(own.empty() ? body : own,".ASC");
  }

void Npc::updateArmor() {
  const auto* residency=owner.chunkResidency();
  Resources::TextureLoadScope textureLoadScope(!isPlayer() && residency &&
      residency->textureDistance(ChunkId::fromPosition(x,z))>2);
  living.reset();
  auto  ar = invent.currentArmor();
  auto& w  = owner;

  if(ar==nullptr) {
    auto  vbody = body.empty() ? MeshObjects::Mesh() : w.addView(bodyVisual(),vColor,0,bdColor);
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
  const auto who = Bench::who(this);
  char buf[200];
  std::snprintf(buf,sizeof(buf),"quiver npc %.*s arrows %d out %.1f mouth %.2f %.2f %.2f",int(who.size()),who.data(),
                q.arrows,double(q.out),double(q.mouth.x),double(q.mouth.y),double(q.mouth.z));
  Bench::note(buf);
  }

void Npc::refitQuiver() {
  visual.clearQuiver();
  updateQuiver();
  }

void Npc::setQuiverTaken(int n) {
  if(visual.quiver().taken()==n)
    return;
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
  // the arrow put away (the bow lowered): back in the quiver
  if(itm.isEmpty())
    setQuiverTaken(0);
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
  const bool trace = Bench::bodyTracing() && currentTarget!=nullptr;
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

void Npc::startCues(std::shared_ptr<const SpeechCues::Plan> plan, bool voiced) {
  visual.startCues(*this,std::move(plan),voiced);
  }

bool Npc::startCue(std::string_view name, char side) {
  return visual.startCue(*this,name,side);
  }

float Npc::cueWeight(std::string_view name) {
  return visual.cueWeight(*this,name);
  }

bool Npc::startLeak(std::string_view felt, bool lower) {
  return visual.startLeak(*this,felt,lower);
  }

float Npc::leakWeight() {
  return visual.leakWeight(*this);
  }

void Npc::startSpeech(const Tempest::Sound& voice, std::string_view name) {
  visual.startSpeech(*this,LipSync::Track::fromSound(voice,name));
  }

void Npc::startSpeech(std::shared_ptr<LipSync::Track> voice) {
  visual.startSpeech(*this,std::move(voice));
  }

void Npc::setFaceTrack(std::shared_ptr<const SpeechFace::Track> track, std::string_view line) {
  visual.setFaceTrack(std::move(track),line);
  }

void Npc::setTalkMood(std::string_view mood, std::string_view line, uint64_t holdMs) {
  visual.setTalkMood(mood,line,owner.tickCount()+holdMs);
  }

void Npc::listenTo(uint64_t ms, const Npc* speaker) {
  auto toward = TalkGestures::Mood::Neutral;
  if(speaker!=nullptr)
    switch(owner.script().personAttitude(*this,*speaker)) {  // as Npc_GetAttitude
      case ATT_FRIENDLY: toward = TalkGestures::Mood::Friendly; break;
      case ATT_ANGRY:    toward = TalkGestures::Mood::Angry;    break;
      case ATT_HOSTILE:  toward = TalkGestures::Mood::Hostile;  break;
      default: break;
      }
  visual.listen(owner.tickCount()+ms,toward);
  }

int32_t Npc::voiceAccents(std::string_view line, const Npc& listener) {
  return visual.voiceAccents(line,listener.visual.faceProfile(),owner.tickCount());
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
  visual.stopSpeech(owner.tickCount());
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

FootIk::Input mkFootIkInput(const FootIk& ik, const Npc& npc, World& world, const Matrix4x4& obj,
                              bool alignedToGround, uint64_t dt, const AimWalk* walk) {
  FootIk::Input in;
  in.dt     = dt;
  in.floorY = obj.at(3,1);
  in.crouch = npc.aimCrouch()*FootIk::aimCrouchDepth()*ik.legScale();
  // the test bench measures the ground with IK off too, and under a body tilted to the ground as
  // a whole (surface_align): that is the baseline
  if(!ik.isValid() || (!FootIk::isEnabled() && !FootIk::isProbing()) || (alignedToGround && !FootIk::isProbing()))
    return in;
  if(npc.isDead() || npc.isUnconscious() || npc.interactive()!=nullptr)
    return in;
  if(npc.isInAir() || npc.isFalling() || npc.isSlide() || npc.isSwim()) {
    in.hardOff = true;
    return in;
    }

  const uint32_t bs = npc.bodyStateMasked() & BS_MAX;
  if(bs==(BS_JUMP & BS_MAX) || bs==(BS_FALL & BS_MAX) || bs==(BS_SWIM & BS_MAX) ||
     bs==(BS_DIVE & BS_MAX) || bs==(BS_CLIMB & BS_MAX)) {
    in.hardOff = true;
    return in;
    }
  // A melee roll goes over the ground on the body, off as in a jump; a dash fades out as the jump back does
  if(bs==(BS_PARADE & BS_MAX) && npc.isMeleeDodgeRoll()) {
    in.hardOff = true;
    return in;
    }
  // Two legs: standing, locomotion, aiming a bow (a shot is a frame of BS_HIT) and casting a spell, the
  // feet stand still there; other animations (attacks, items, ...) fade out.
  // Four legs keep it: a beast attacks standing on the ground, and is not tilted to it otherwise
  const auto ws    = npc.weaponState();
  const bool bow   = ws==WeaponState::Bow || ws==WeaponState::CBow;
  const bool stand = bs==BS_NONE || bs==BS_AIMNEAR || bs==BS_AIMFAR || bs==BS_CASTING || (bow && bs==(BS_HIT & BS_MAX));
  if(!ik.isQuadruped() && !stand && bs!=(BS_WALK & BS_MAX) && bs!=(BS_SNEAK & BS_MAX) && bs!=BS_RUN && bs!=BS_SPRINT)
    return in;

  const Vec3 pos(obj.at(3,0),obj.at(3,1),obj.at(3,2));
  if(!world.isInPfxRange(pos))
    return in;
  auto physic = world.physic();
  if(physic==nullptr)
    return in;

  return ik.mkGroundInput(in,*physic,obj,alignedToGround,walk);
  }
