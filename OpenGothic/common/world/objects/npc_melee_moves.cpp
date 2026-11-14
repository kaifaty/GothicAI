#include "npc.h"
#include "npc_melee.h"
#include "npc_melee_shadow.h"

#include <Tempest/Log>

#include "game/bodyobservations.h"
#include "game/bodycontroller.h"
#include "game/meleeparry.h"
#include "game/meleecharge.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "world/benchhooks.h"
#include "gothic.h"
#include "resources.h"

#include <mutex>
#include <unordered_set>

using namespace Tempest;

// Melee moves of Npc: stamina, stun, parry, dodges, charge and AI special moves.
// Moved from npc.cpp unchanged.

namespace {

// One line in the log per missing dodge animation name, not one per press
void reportMissingDodgeAnimation(const std::string& name) {
  static std::mutex                      sync;
  static std::unordered_set<std::string> reported;
  std::lock_guard<std::mutex> guard(sync);
  if(reported.insert(name).second)
    Log::i("[melee] dodge animation ",name," is not there: its fallback plays");
  }

}

namespace NpcMelee {

std::optional<Pose::CombatLayer> meleeParadeLayer(const Pose& pose, uint64_t now) {
  // A dodge plays in BS_PARADE, as the jump back does, but it is no parade even with a defence window
  for(const auto& layer:pose.combatLayers(now))
    if(layer.state==BS_PARADE && !layer.sequence->data->defWindow.empty() &&
       !MeleeDodge::parseAnimation(layer.sequence->name))
      return layer;
  return std::nullopt;
  }

std::string meleeStaminaIdentity(const Pose& pose, uint64_t now) {
  auto result = std::to_string(pose.comboLength());
  for(const auto& layer:pose.combatLayers(now))
    result += "|" + layer.sequence->name + ":" + std::to_string(layer.origin) + ":" +
              std::to_string(layer.combination);
  return result;
  }

}

bool Npc::hasStamina() const {
  return isHuman() && Gothic::physicsFlags().meleeStamina;
  }

float Npc::staminaMax() const {
  return hasStamina() ? MeleeStamina::maximum(attribute(ATR_STRENGTH)) : 0.f;
  }

float Npc::stamina() const {
  return meleeStamina.initialized ? std::min(meleeStamina.current,staminaMax()) : staminaMax();
  }

float Npc::staminaFraction() const {
  const float maximum = staminaMax();
  return maximum>0.f ? std::clamp(stamina()/maximum,0.f,1.f) : 1.f;
  }

bool Npc::isStaminaExhausted() const {
  return hasStamina() && meleeStamina.exhausted;
  }

float Npc::meleeStunBonus(const Npc& attacker) const {
  const auto ws = attacker.weaponState();
  const bool humanAttack = attacker.isHuman() && (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H);
  const bool beastAttack = isHuman() && attacker.hasMeleeBeastPoise();
  return Gothic::physicsFlags().meleeStagger && (isHuman() || hasMeleeBeastPoise()) && isMeleeStunned() &&
         (humanAttack || beastAttack) ? 1.5f : 1.f;
  }

MeleeImpact::BackstabKind Npc::meleeBackstabKind(const Npc& attacker) const {
  const auto ws = attacker.weaponState();
  if(!Gothic::physicsFlags().meleeBackstab || !attacker.isHuman() ||
     (ws!=WeaponState::Fist && ws!=WeaponState::W1H && ws!=WeaponState::W2H) ||
     isDown() || meleeKnockdown || (!isHuman() && !(isMonster() && invent.activeWeapon()==nullptr)))
    return MeleeImpact::BackstabKind::None;
  const auto* state = owner.script().findSymbol(aiState.funcIni.ptr);
  const bool sleeping = state!=nullptr && (state->name()=="ZS_SLEEP" || state->name()=="ZS_MAGICSLEEP" ||
                                          state->name()=="ZS_MM_RTN_SLEEP");
  const bool recentHit = std::any_of(meleeRecentHits.begin(),meleeRecentHits.end(),[&](const auto& hit) {
    return hit.first==&attacker && MeleeImpact::backstabRecentHit(owner.tickCount(),hit.second);
    });
  const bool combat = currentTarget==&attacker && (weaponState()!=WeaponState::NoWeapon || isAttack());
  return MeleeImpact::backstabKind(MeleeImpact::backstabAngle(angle,angleDir(attacker.x-x,attacker.z-z)),
                                  attacker.talentSkill(TALENT_SNEAK),sleeping,isMeleeStunned(),
                                  recentHit || (combat && !sleeping));
  }

float Npc::meleeBackstabMultiplier(MeleeImpact::BackstabKind kind) const {
  return MeleeImpact::backstabMultiplier(kind,isMonster() && MeleeImpact::beastSize(physicsMass())==MeleeImpact::BeastSize::Huge,1.f);
  }

bool Npc::hasMeleeBeastPoise() const {
  return Gothic::physicsFlags().meleeBeastPoise && isMonster() && invent.activeWeapon()==nullptr;
  }

bool Npc::hasMeleeSuperArmor() const {
  if(isMeleeCharging())
    return MeleeCharge::superArmor(true,false,0,0);
  if(isMeleeChargeReleasing()) {
    for(const auto& layer:visual.pose().combatLayers(owner.tickCount()))
      if(layer.sequence->name=="T_1HCHARGEHIT" && !layer.sequence->data->defHitEnd.empty())
        return MeleeCharge::superArmor(false,true,layer.time-layer.start,layer.sequence->data->defHitEnd.back());
    }
  if(hasMeleeBeastPoise())
    return !isDown() && !meleeKnockdown && !isMeleeStunned() &&
           MeleeImpact::beastSize(physicsMass())!=MeleeImpact::BeastSize::Small && visual.pose().isAttackAnim();
  if(!Gothic::physicsFlags().meleeStagger || !isHuman() || isDown() || meleeKnockdown || isMeleeStunned() ||
     (weaponState()!=WeaponState::W2H && !meleeDownStrike))
    return false;
  const auto* item = invent.activeWeapon();
  if(item==nullptr)
    return false;
  const auto* mesh = Resources::loadMesh(item->handle().visual);
  if(mesh==nullptr || mesh->bbox()[0].x>=-4.f)
    throw std::runtime_error("Melee super armor requires a weapon mesh extending along local -X");
  const float length = -mesh->bbox()[0].x;
  const auto extent = mesh->bbox()[1]-mesh->bbox()[0];
  const bool normalY = extent.y<=extent.z;
  const auto now = owner.tickCount();
  for(const auto& layer:visual.pose().combatLayers(now)) {
    if(!layer.sequence->isAttackAnim() && !(meleeDownStrike && isFinishSequence(*layer.sequence)))
      continue;
    cacheMeleeShadowWindows(layer.sequence,length,normalY);
    const MeleeShadow::Window* selected = nullptr;
    const auto combo = uint32_t(visual.pose().comboLength());
    for(const auto& window:meleeShadow->windows)
      if(window.sequence==layer.sequence && window.length==length && window.normalY==normalY &&
         (selected==nullptr || (window.index<=combo && window.index>selected->index)))
        selected = &window;
    if(selected && double(layer.time-layer.start)<=selected->endMs)
      return true;
    }
  return false;
  }

bool Npc::isMeleeStunned() const {
  const bool enabled = meleeStunReason=="posture" ? Gothic::physicsFlags().meleeStagger && (isHuman() || hasMeleeBeastPoise()) : Gothic::physicsFlags().meleeParry;
  return enabled && owner.tickCount()<meleeStunUntil;
  }

bool Npc::allowMeleeStunAction(std::string_view action) {
  if(!isMeleeStunned())
    return true;
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,currentTarget,"stun_refused",owner.tickCount(),
                        "\"action\":"+Bench::bodyJson(action));
  return false;
  }

void Npc::startMeleeStun(Npc& by, std::string_view reason) {
  const bool enabled = reason=="posture" ? Gothic::physicsFlags().meleeStagger : Gothic::physicsFlags().meleeParry;
  const bool beast = reason=="posture" && hasMeleeBeastPoise() && MeleeImpact::beastCanAccumulatePosture(physicsMass());
  if(!enabled || (!isHuman() && !beast) || isDown() || meleeKnockdown || isMeleeStunned())
    return;
  interruptItemUse("stagger");
  endMeleeStun("timeout");
  meleeStunReason = reason;
  meleeStunUntil = owner.tickCount()+MeleeImpact::stunDuration(reason);
  if(beast)
    meleePosture.value = 0;
  stopMeleeSwing();
  clearGoTo();
  mvAlgo.clearSpeed();
  meleeStunLook(by);
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,&by,"stun_start",owner.tickCount(),
                        "\"reason\":"+Bench::bodyJson(reason)+
                        (beast ? meleeBeastFields(physicsMass()) : std::string())+
                        ",\"duration_ms\":"+std::to_string(MeleeImpact::stunDuration(reason)));
  }

static float beastStumbleSpeed(const Animation::Sequence& seq, float targetMs) {
  // A beast's own stumble (400–800 ms) stretched toward its beat of the stun, never below half speed.
  return std::clamp(seq.totalTime()/targetMs,0.5f,1.f);
  }

void Npc::meleeStunLook(const Npc& by) {
  syncDeferredAnimation();
  Vec3 away = position()-by.position();
  away.y = 0;
  away = away.length()>1e-3f ? Vec3::normalize(away) : Vec3(-std::cos(rotationRad()),0,-std::sin(rotationRad()));
  if(!isHuman()) {
    // A beast off balance: its own stagger back, stretched to about 700 ms, trunk and head giving
    // way from the blow; the second beat (tickMeleeStunLook) lurches it forward to the end of the stun.
    meleeStunBeat = false;
    if(auto seq = setAnimAngGet(Anim::StumbleB))
      visual.setLayerSpeed(seq,beastStumbleSpeed(*seq,700.f),owner.tickCount());
    Flinch::Hit hit;
    hit.dir      = away;
    hit.height   = 0.6f;
    hit.twist    = 0.2f;
    hit.strength = 0.8f;
    flinch(hit);
    return;
    }
  // The blow was stopped and thrown back: the original stagger back, slowed to carry most of the
  // stun, the upper body giving way from the one who stopped it and the weapon arm thrown back
  // on the living rig, its motors bringing it into the stagger. T_STUMBLEB lasts 360 ms.
  // Balance lost (posture) is the heavier one over 1.5 s: the trunk gives way lower and the chest
  // is thrown back with the arm, then a second beat (tickMeleeStunLook) catches it forward.
  const bool  posture      = meleeStunReason=="posture";
  constexpr float staggerSpeed = 0.5f;
  meleeStunBeat = !posture;
  if(auto seq = setAnimAngGet(Anim::StumbleB))
    visual.setLayerSpeed(seq,staggerSpeed,owner.tickCount());
  Flinch::Hit hit;
  hit.dir      = away;
  hit.height   = posture ? 0.55f : 0.85f;
  hit.twist    = posture ? 0.25f : 0.4f; // the weapon hand goes back first and turns the trunk after it
  hit.strength = posture ? 0.85f : 0.6f;
  flinch(hit);
  if(!Gothic::physicsFlags().meleeReactions || !living)
    return;
  const auto  ws       = weaponState();
  const float momentum = ws==WeaponState::W2H ? 30.f : (ws==WeaponState::W1H ? 22.f : 12.f);
  const Vec3  impulse  = Vec3::normalize(away+Vec3(0,0.3f,0))*(momentum*0.25f);
  const size_t arm = visual.pose().findNode("BIP01 R FOREARM");
  if(arm<visual.pose().boneCount())
    arrowImpacts.push_back({arm,visual.mapBone(arm),impulse,0.55f,by.handlePtr(),true,2.f});
  const size_t chest = posture ? visual.pose().findNode("BIP01 SPINE2") : size_t(-1);
  if(chest<visual.pose().boneCount())
    arrowImpacts.push_back({chest,visual.mapBone(chest),Vec3::normalize(away)*(momentum*0.3f),0.5f,by.handlePtr(),true,1.5f});
  }

void Npc::tickMeleeStunLook() {
  // Balance lost, second beat: the stagger back runs out (720 ms) and the body lurches forward
  // to catch itself (the original stagger from behind, slowed), instead of standing in the run
  // pose for the rest of the stun. A beast's own stumble fills what is left.
  if(meleeStunBeat || meleeStunReason!="posture")
    return;
  const auto now   = owner.tickCount();
  const auto start = meleeStunUntil-MeleeImpact::stunDuration("posture");
  if(now<start+700)
    return;
  constexpr float recoverSpeed = 0.6f;
  auto seq = setAnimAngGet(Anim::StumbleA);
  if(seq==nullptr)
    return; // the stumble back cannot be cut: lurch forward the tick it runs out
  meleeStunBeat = true;
  visual.setLayerSpeed(seq,isHuman() ? recoverSpeed : beastStumbleSpeed(*seq,float(meleeStunUntil-now)),now);
  }

void Npc::endMeleeStun(std::string_view reason) {
  if(meleeStunUntil==0)
    return;
  if(meleeStunReason=="posture")
    meleePosture.recover(owner.tickCount());
  meleeStunReason.clear();
  meleeStunUntil = 0;
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,currentTarget,"stun_end",owner.tickCount(),
                        "\"reason\":"+Bench::bodyJson(reason));
  }

void Npc::tickMeleeParry() {
  const auto now = owner.tickCount();
  if(!Gothic::physicsFlags().meleeParryAi || !Gothic::physicsFlags().meleeParry ||
     currentTarget==nullptr || isDown() || meleeKnockdown || isMeleeStunned())
    meleeParadeTiming.reset();
  const bool stunEnabled = meleeStunReason=="posture" ? Gothic::physicsFlags().meleeStagger && (isHuman() || hasMeleeBeastPoise()) : Gothic::physicsFlags().meleeParry;
  if(meleeStunUntil!=0 && (!stunEnabled || now>=meleeStunUntil))
    endMeleeStun(stunEnabled ? "timeout" : "reset");
  if(!Gothic::physicsFlags().meleeParry) {
    meleeParadeOrigin.reset();
    return;
    }
  if(!isHuman() || isDown() || meleeKnockdown)
    return;
  const auto layer = meleeParadeLayer(visual.pose(),now);
  const bool same = layer && meleeParadeOrigin && layer->origin==*meleeParadeOrigin;
  if(meleeParadeOrigin && (same ? layer->time-layer->start>=meleeParadeEnd : now>=*meleeParadeOrigin+meleeParadeEnd)) {
    if(!meleeParadeContact) {
      tickMeleeStamina();
      const float cost = hasStamina() ? MeleeParry::WhiffCost : 0.f;
      spendStamina("parade_whiff",cost);
      if(Bench::bodyTracing())
        Bench::bodyEvent(*this,currentTarget,"parade_whiff",now,"\"cost\":"+std::to_string(cost));
      }
    meleeParadeOrigin.reset();
    }
  // Interrupted parades do not pay a whiff; only a completed defence window does.
  if(!same)
    meleeParadeOrigin.reset();
  if(layer && !meleeParadeOrigin && layer->time-layer->start<layer->sequence->data->defWindow.back()) {
    meleeParadeOrigin = layer->origin;
    meleeParadeEnd = layer->sequence->data->defWindow.back();
    meleeParadeContact = false;
    }
  }

void Npc::applyMeleeParry(Npc& attacker, const MeleeBladeContact* contact) {
  tickMeleeParry();
  const auto now = owner.tickCount();
  const auto layer = meleeParadeLayer(visual.pose(),now);
  if(!layer)
    return;
  meleeParadeContact = true;
  const auto elapsed = now-layer->origin;
  const auto talent = hitChance(weaponState()==WeaponState::W2H ? TALENT_2H : TALENT_1H);
  const auto result = MeleeParry::classify(elapsed,talent,layer->sequence->data->defWindow.back());
  const bool parry = result==MeleeParry::Result::Parry;
  tickMeleeStamina();
  const float cost = !hasStamina() ? 0.f : parry ? MeleeParry::ParryCost :
                     MeleeParry::guardCost(MeleeImpact::postureIncrease(meleeImpulse(attacker,contact).length(),
                                           physicsMass(),meleePostureSkillScale(attacker),MeleeImpact::postureThresholdScale()));
  const float before = stamina();
  const bool broken = !parry && MeleeParry::guardBroken(before,cost,hasStamina());
  spendStamina(parry ? "parry" : "guard",cost);
  if(Bench::bodyTracing()) {
    auto fields = "\"parade_ms\":"+std::to_string(elapsed)+
                  ",\"window_ms\":"+std::to_string(MeleeParry::windowMs(talent))+
                  ",\"cost\":"+std::to_string(cost);
    if(!parry)
      fields += ",\"before\":"+std::to_string(before)+",\"after\":"+std::to_string(stamina());
    Bench::bodyEvent(*this,&attacker,parry ? "parry" : "guard",now,fields);
    if(broken)
      Bench::bodyEvent(*this,&attacker,"guard_break",now,
                          "\"cost\":"+std::to_string(cost)+",\"before\":"+std::to_string(before));
    }
  if(parry && attacker.isHuman())
    attacker.startMeleeStun(*this,"parried");
  if(broken)
    startMeleeStun(attacker,"guard_break");
  // Physical contacts also stop the attacker in consumeMeleeContacts; the legacy
  // range/focus path must cut its accepted swing back to the same stance.
  attacker.stopMeleeSwing();
  }

void Npc::tickMeleeStamina() {
  if(!hasStamina()) {
    meleeStamina = {};
    return;
    }
  const bool exhausted = meleeStamina.exhausted;
  MeleeStamina::recover(meleeStamina,staminaMax(),owner.tickCount(),hasState(BS_PARADE) || isMeleeCharging());
  if(exhausted && !meleeStamina.exhausted && Bench::bodyTracing())
    Bench::bodyEvent(*this,currentTarget,"stamina_recovered",owner.tickCount(),
                        "\"after\":" + std::to_string(stamina()));
  }

float Npc::meleeAttackCost() const {
  const auto ws = weaponState();
  if(ws==WeaponState::Fist)
    return MeleeStamina::attackCost(0,0,true);
  int requirement = 0;
  if(const auto* weapon = invent.activeWeapon()) {
    const auto& item = weapon->handle();
    for(size_t i=0;i<daedalus::IItem::condition_count;++i)
      if(item.cond_atr[i]==ATR_STRENGTH)
        requirement = std::max(requirement,int(item.cond_value[i]));
    }
  return MeleeStamina::attackCost(requirement,hitChance(ws==WeaponState::W2H ? TALENT_2H : TALENT_1H));
  }

bool Npc::allowStaminaAction(std::string_view action, float cost, bool fullCost) {
  tickMeleeStamina();
  const bool allowed = fullCost ? MeleeStamina::canAfford(meleeStamina,cost,hasStamina()) :
                                  MeleeStamina::canAct(meleeStamina,hasStamina());
  if(!allowed && Bench::bodyTracing())
    Bench::bodyEvent(*this,currentTarget,"stamina_refused",owner.tickCount(),
                        "\"action\":" + Bench::bodyJson(action) + ",\"after\":" + std::to_string(stamina()));
  return allowed;
  }

void Npc::spendStamina(std::string_view action, float cost) {
  if(!hasStamina())
    return;
  const float before = stamina();
  const bool exhausted = meleeStamina.exhausted;
  MeleeStamina::drain(meleeStamina,cost,owner.tickCount());
  if(!Bench::bodyTracing())
    return;
  const auto fields = "\"action\":" + Bench::bodyJson(action) + ",\"cost\":" + std::to_string(cost) +
                      ",\"before\":" + std::to_string(before) + ",\"after\":" + std::to_string(stamina());
  Bench::bodyEvent(*this,currentTarget,"stamina_spend",owner.tickCount(),fields);
  if(!exhausted && meleeStamina.exhausted)
    Bench::bodyEvent(*this,currentTarget,"stamina_exhausted",owner.tickCount(),fields);
  }

void Npc::waitForMeleeStamina(uint64_t dt) {
  if(isMeleeStunned()) {
    mvAlgo.clearSpeed();
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    return;
    }
  stopWalkAnimation();
  implTurnToFai(*currentTarget,dt);
  if(fghAlgo.isInWRange(*currentTarget,*this,owner.script())) {
    if(currentTarget->isPrehit()) {
      if(weaponState()==WeaponState::Fist ? blockFist() : blockSword()) {
        implFaiWait(visual.pose().animationTotalTime());
        return;
        }
      } else {
      const auto step = owner.script().rand(2,this)==0 ? FightAlgo::MV_STRAFEL : FightAlgo::MV_STRAFER;
      if(setAnim(step==FightAlgo::MV_STRAFEL ? Anim::MoveL : Anim::MoveR)) {
        visual.setAnimRotate(*this,0);
        implFaiWait(visual.pose().animationTotalTime());
        return;
        }
      }
    }
  setAnim(Anim::Idle);
  implFaiWait(200);
  }

std::optional<Npc::MeleeDodgeLayer> Npc::meleeDodgeLayer() const {
  const auto& pose = visual.pose();
  if(!Gothic::physicsFlags().meleeDodge || !pose.hasState(BS_PARADE))
    return std::nullopt;
  for(const auto& layer:pose.combatLayers(owner.tickCount()))
    if(layer.state==BS_PARADE)
      if(auto move = MeleeDodge::parseAnimation(layer.sequence->name))
        return MeleeDodgeLayer{*move,layer.origin};
  return std::nullopt;
  }

bool Npc::isMeleeDodging() const {
  return meleeDodgeLayer().has_value();
  }

bool Npc::isMeleeDodgeRoll() const {
  const auto layer = meleeDodgeLayer();
  return layer && (layer->move.kind==MeleeDodge::Kind::Roll || layer->move.kind==MeleeDodge::Kind::HeavyRoll);
  }

bool Npc::isMeleeDodgeInvulnerable() const {
  const auto layer = meleeDodgeLayer();
  const auto now   = owner.tickCount();
  return layer && now>=layer->origin && MeleeDodge::invulnerable(layer->move.kind,now-layer->origin);
  }

MeleeDodge::Guard Npc::meleeGuard(const Npc& attacker) const {
  const auto& pose = visual.pose();
  const auto  now  = owner.tickCount();
  MeleeDodge::GuardSituation s;
  s.dodging     = isMeleeDodging();
  s.dodgeWindow = isMeleeDodgeInvulnerable();
  s.knockedDown = meleeKnockdown;
  s.stunned     = isMeleeStunned();
  s.jumpBack    = pose.isJumpBack(now) && fghAlgo.isInJumpBackAngle(*this,attacker);
  s.parade      = pose.isDefence(now) && fghAlgo.isInFocusAngle(*this,attacker);
  s.unblockable = attacker.isMonster() && attacker.inventory().activeWeapon()==nullptr;
  return MeleeDodge::guard(s);
  }

std::optional<uint64_t> Npc::nextMeleeContactMs() const {
  const auto now = owner.tickCount();
  if(const auto contact = visual.pose().nextMeleeContact(now))
    return contact->contact>now ? contact->contact-now : 0;
  return std::nullopt;
  }

void Npc::meleeDodgeNote(const std::string& text, std::string_view phase, const std::string& fields) {
  if(Bench::isActive()) {
    const auto self = Bench::who(this);
    Bench::note(("melee dodge "+std::string(self)+": "+text).c_str());
    }
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,currentTarget,phase,owner.tickCount(),fields,visual.pose().animNames());
  }

bool Npc::meleeDodgeEvades(Npc& other, std::string_view kind) {
  const auto layer = meleeDodgeLayer();
  if(!layer || !meleeGuard(other).dodged)
    return false;
  const auto now     = owner.tickCount();
  const auto elapsed = now>=layer->origin ? now-layer->origin : 0;
  const auto window = MeleeDodge::invulnerability(layer->move.kind);
  if(kind=="melee")
    BodyObservations::noteEvent(*this,&other,"evaded",now);
  const auto by = Bench::who(&other);
  meleeDodgeNote(std::string(kind)+" hit by "+std::string(by)+" at "+std::to_string(elapsed)+" ms of "+
                 std::string(MeleeDodge::name(layer->move.kind))+" "+std::to_string(window.beginMs)+"-"+
                 std::to_string(window.endMs)+" ms: did not pass (no damage, balance or reaction), hp "+
                 std::to_string(attribute(ATR_HITPOINTS)),
                 "dodge_invulnerable",
                 "\"damage_kind\":"+Bench::bodyJson(kind)+",\"by\":"+Bench::bodyJson(by)+
                 ",\"kind\":"+Bench::bodyJson(MeleeDodge::name(layer->move.kind))+
                 ",\"direction\":"+Bench::bodyJson(MeleeDodge::name(layer->move.direction))+
                 ",\"elapsed_ms\":"+std::to_string(elapsed)+
                 ",\"window_ms\":["+std::to_string(window.beginMs)+","+std::to_string(window.endMs)+"]"+
                 ",\"hp\":"+std::to_string(attribute(ATR_HITPOINTS)));
  return true;
  }

bool Npc::meleeAiSpecialBusy(std::string_view move) {
  if(currentTarget==nullptr || !currentTarget->meleeAiSpecialSlot.busy())
    return false;
  if(Bench::isActive())
    Bench::note(("melee ai "+std::string(move)+" "+std::string(Bench::who(this))+": "+
                    std::string(Bench::who(currentTarget))+" skipped busy").c_str());
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,currentTarget,"ai_special_busy",owner.tickCount(),
                        "\"special\":"+Bench::bodyJson(move));
  return true;
  }

void Npc::beginMeleeAiSpecial(MeleeAi::Special move) {
  if(currentTarget==nullptr || meleeAiSpecial!=MeleeAi::Special::None ||
     !currentTarget->meleeAiSpecialSlot.acquire(this,move))
    throw std::runtime_error("Melee AI special target is already occupied");
  meleeAiSpecialTarget = currentTarget->handlePtr();
  meleeAiSpecial = move;
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,currentTarget,"ai_special_start",owner.tickCount(),
                        "\"special\":"+Bench::bodyJson("charge"));
  }

void Npc::endMeleeAiSpecial() {
  if(meleeAiSpecial==MeleeAi::Special::None)
    return;
  const auto handle = meleeAiSpecialTarget.lock();
  auto* target = handle ? reinterpret_cast<Npc*>(handle->user_ptr) : nullptr;
  if(target!=nullptr)
    target->meleeAiSpecialSlot.release(this);
  const auto move = meleeAiSpecial;
  meleeAiSpecialEnded = owner.tickCount();
  if(move==MeleeAi::Special::Charge && Bench::isActive())
    Bench::note(("melee ai charge "+std::string(Bench::who(this))+": release turn "+
                    std::to_string(meleeAiChargeTurn)+" deg").c_str());
  if(Bench::bodyTracing()) {
    if(move==MeleeAi::Special::Charge)
      Bench::bodyEvent(*this,target,"ai_charge_end",owner.tickCount(),
                          "\"turn_deg\":"+std::to_string(meleeAiChargeTurn));
    Bench::bodyEvent(*this,target,"ai_special_end",owner.tickCount(),
                        "\"special\":"+Bench::bodyJson("charge"));
    }
  meleeAiSpecialTarget.reset();
  meleeAiSpecial = MeleeAi::Special::None;
  }

void Npc::tickMeleeAiSpecial() {
  bool active = false;
  switch(meleeAiSpecial) {
    case MeleeAi::Special::None:
      return;
    case MeleeAi::Special::Charge:
      active = isMeleeCharging();
      if(isMeleeChargeReleasing()) {
        meleeAiChargeTurn += std::abs(std::remainder(angle-meleeAiChargeYaw,360.f));
        meleeAiChargeYaw = angle;
        for(const auto& layer:visual.pose().combatLayers(owner.tickCount()))
          if(layer.sequence->name=="T_1HCHARGEHIT" && !layer.sequence->data->defHitEnd.empty())
            active |= layer.time-layer.start<=layer.sequence->data->defHitEnd.back();
        }
      break;
    }
  if(!active || isDown())
    endMeleeAiSpecial();
  }

bool Npc::tryMeleeAiCharge() {
  const auto& flags = Gothic::physicsFlags();
  if(!flags.meleeAiCharge || !flags.meleeCharge || !isHuman() || weaponState()!=WeaponState::W1H ||
     currentTarget==nullptr || !visual.pose().isStanding())
    return false;
  const auto skip = [&](std::string_view reason) {
    if(Bench::isActive())
      Bench::note(("melee ai charge "+std::string(Bench::who(this))+": "+std::string(Bench::who(currentTarget))+
                      " skipped "+std::string(reason)).c_str());
    };
  if(!fghAlgo.isInWRange(*this,*currentTarget,owner.script()) || !fghAlgo.isInFocusAngle(*this,*currentTarget,5.f)) {
    skip("range");
    return false;
    }
  if(currentTarget->isMeleeStunned() || currentTarget->visual.pose().nextMeleeContact(owner.tickCount())) {
    skip("target_attacking_or_stunned");
    return false;
    }
  if(!canMeleeCharge()) {
    skip("stance_or_clips");
    return false;
    }
  if(!MeleeAi::specialCooldownReady(owner.tickCount(),meleeAiSpecialEnded)) {
    skip("cooldown");
    return false;
    }
  if(meleeAiSpecialBusy("charge"))
    return false;
  if(!allowStaminaAction("charge",MeleeCharge::cost(meleeAttackCost(),hasStamina()),true)) {
    skip("stamina");
    return false;
    }
  const int talent = hitChance(TALENT_1H);
  const float chance = MeleeAi::chargeChance(talent);
  const float uniform = float(owner.script().rand(1u<<24,this))/float(1u<<24);
  const bool charge = MeleeAi::chargeRoll(talent,uniform) && beginMeleeCharge();
  if(charge) {
    meleeAiChargeHold = MeleeAi::chargeHoldMs(float(owner.script().rand(1u<<24,this))/float(1u<<24));
    meleeAiChargeTurn = 0.f;
    meleeAiDodgeStreak.reset();
    }
  if(Bench::isActive())
    Bench::note(("melee ai charge "+std::string(Bench::who(this))+": "+std::string(Bench::who(currentTarget))+
                    " chance "+std::to_string(chance)+" → "+(charge ? "charge" : "attack")).c_str());
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,currentTarget,"ai_charge_roll",owner.tickCount(),
                        "\"chance\":"+std::to_string(chance)+",\"roll\":"+std::to_string(uniform)+
                        ",\"hold_ms\":"+std::to_string(charge ? meleeAiChargeHold : 0)+
                        ",\"outcome\":"+Bench::bodyJson(charge ? "charge" : "attack"));
  return charge;
  }

bool Npc::canMeleeCharge() const {
  const auto bs = bodyStateMasked();
  MeleeCharge::Situation s;
  s.enabled          = Gothic::physicsFlags().meleeCharge;
  s.player           = isPlayer() && isHuman();
  s.aiEnabled        = isHuman() && Gothic::physicsFlags().meleeAiCharge;
  s.oneHand          = weaponState()==WeaponState::W1H;
  s.windupAnimation  = visual.hasAnim("T_1HCHARGE");
  s.holdAnimation    = visual.hasAnim("S_1HCHARGE");
  s.releaseAnimation = visual.hasAnim("T_1HCHARGEHIT");
  s.standing         = (bs==BS_STAND || bs==BS_NONE) && isStanding() && interactive()==nullptr && !isMeleeCharging();
  s.attacking        = isAttackAnim();
  s.parading         = hasState(BS_PARADE);
  s.stunned          = isMeleeStunned();
  s.knockedDown      = meleeKnockdown || isDown();
  s.inAir            = isInAir() || isFalling() || isSlide() || isJump() || isJumpUp();
  s.inWater          = isInWater() || isSwim() || isDive();
  return MeleeCharge::available(s);
  }

bool Npc::isMeleeCharging() const {
  return meleeChargePhase==ChargePhase::Windup || meleeChargePhase==ChargePhase::Hold;
  }

bool Npc::isMeleeChargeReleasing() const {
  return meleeChargePhase==ChargePhase::Release && visual.pose().isInAnim("T_1HCHARGEHIT");
  }

float Npc::meleeChargeMultiplier() const {
  return isMeleeChargeReleasing() ? meleeChargeMult : 1.f;
  }

void Npc::meleeChargeNote(const std::string& text, std::string_view phase, const std::string& fields) {
  if(Bench::isActive()) {
    const auto self = Bench::who(this);
    Bench::note(("melee charge "+std::string(self)+": "+text).c_str());
    }
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,currentTarget,phase,owner.tickCount(),fields,visual.pose().animNames());
  }

void Npc::meleeChargeTap(uint64_t heldMs) {
  meleeChargeNote("tap at "+std::to_string(heldMs)+" ms","charge_tap","\"held_ms\":"+std::to_string(heldMs));
  }

bool Npc::beginMeleeCharge() {
  if(!canMeleeCharge() || (!isPlayer() && (currentTarget==nullptr || meleeAiSpecialBusy("charge"))))
    return false;
  const float cost = MeleeCharge::cost(meleeAttackCost(),hasStamina());
  if(!allowStaminaAction("charge",cost,true))
    return false;
  if(visual.startAnimAndGet(*this,"T_1HCHARGE",0,BS_HIT)==nullptr)
    return false;
  if(isUsingItemInHand())
    interruptItemUse("attack");
  if(!isPlayer())
    beginMeleeAiSpecial(MeleeAi::Special::Charge);
  spendStamina("charge",cost);
  meleeChargePhase = ChargePhase::Windup;
  meleeChargeStart = owner.tickCount();
  meleeChargeMult = 1.f;
  meleeChargeReleased = false;
  clearGoTo();
  mvAlgo.clearSpeed();
  meleeChargeNote("windup","charge_windup","\"cost\":"+std::to_string(cost));
  return true;
  }

void Npc::releaseMeleeCharge() {
  if(!isMeleeCharging())
    return;
  meleeChargeReleased = true;
  tickMeleeCharge();
  }

void Npc::cancelMeleeCharge(std::string_view reason) {
  if(!isMeleeCharging())
    return;
  meleeChargePhase = ChargePhase::None;
  meleeChargeMult = 1.f;
  meleeChargeReleased = false;
  // A successful dodge has replaced the charge layer already. Sheathing cuts it before its transition.
  if(reason!="dodge") {
    visual.interrupt();
    setAnim(Anim::Idle);
    }
  meleeChargeNote("cancelled by "+std::string(reason),"charge_cancelled","\"reason\":"+Bench::bodyJson(reason));
  }

void Npc::tickMeleeCharge() {
  if(meleeChargePhase==ChargePhase::None)
    return;
  if(!isPlayer() && isMeleeCharging()) {
    const auto handle = meleeAiSpecialTarget.lock();
    auto* target = handle ? reinterpret_cast<Npc*>(handle->user_ptr) : nullptr;
    if(!Gothic::physicsFlags().meleeAiCharge || target==nullptr || target!=currentTarget || target->isDown() ||
       target->isLie() || target->meleeKnockdown) {
      cancelMeleeCharge("AI target unavailable");
      return;
      }
    if(MeleeAi::chargeRelease(owner.tickCount()-meleeChargeStart,meleeAiChargeHold,
                             fghAlgo.isInWRange(*this,*target,owner.script())))
      meleeChargeReleased = true;
    }
  if(!Gothic::physicsFlags().meleeCharge || weaponState()!=WeaponState::W1H || isDown()) {
    cancelMeleeCharge(!Gothic::physicsFlags().meleeCharge ? "flag disabled" : isDown() ? "down" : "weapon sheathed");
    meleeChargePhase = ChargePhase::None;
    return;
    }
  if(meleeChargePhase==ChargePhase::Release) {
    if(!isMeleeChargeReleasing()) {
      meleeChargePhase = ChargePhase::None;
      meleeChargeMult = 1.f;
      }
    return;
    }
  const auto elapsed = owner.tickCount()-meleeChargeStart;
  if(MeleeCharge::releaseReady(elapsed,meleeChargeReleased)) {
    const auto* sequence = visual.startAnimAndGet(*this,"T_1HCHARGEHIT",0,BS_HIT,true);
    if(sequence==nullptr)
      throw std::runtime_error("Melee charge release animation disappeared");
    meleeChargePhase = ChargePhase::Release;
    meleeChargeMult = MeleeCharge::multiplier(elapsed);
    if(meleeAiSpecial==MeleeAi::Special::Charge) {
      meleeAiChargeYaw = angle;
      meleeAiChargeTurn = 0.f;
      if(Bench::isActive())
        Bench::note(("melee ai charge "+std::string(Bench::who(this))+": "+std::string(Bench::who(currentTarget))+
                        " hold "+std::to_string(meleeAiChargeHold)+" ms, release at "+std::to_string(elapsed)+" ms").c_str());
      if(Bench::bodyTracing())
        Bench::bodyEvent(*this,currentTarget,"ai_charge_release",owner.tickCount(),
                            "\"hold_ms\":"+std::to_string(meleeAiChargeHold)+",\"elapsed_ms\":"+std::to_string(elapsed));
      }
    if(MeleeCharge::autoRelease(elapsed) && !meleeChargeReleased)
      meleeChargeNote("auto release","charge_auto_release","\"elapsed_ms\":"+std::to_string(elapsed));
    char text[128];
    std::snprintf(text,sizeof(text),"release c=%.3f mult=%.3f",double(MeleeCharge::level(elapsed)),double(meleeChargeMult));
    meleeChargeNote(text,"charge_release","\"elapsed_ms\":"+std::to_string(elapsed)+
                    ",\"c\":"+std::to_string(MeleeCharge::level(elapsed))+",\"charge_mult\":"+std::to_string(meleeChargeMult));
    return;
    }
  if(elapsed>=MeleeCharge::WindupMs && meleeChargePhase==ChargePhase::Windup) {
    if(visual.startAnimAndGet(*this,"S_1HCHARGE",0,BS_HIT,true)==nullptr)
      throw std::runtime_error("Melee charge hold animation disappeared");
    meleeChargePhase = ChargePhase::Hold;
    meleeChargeNote("hold","charge_hold","\"elapsed_ms\":"+std::to_string(elapsed));
    }
  }

bool Npc::meleeDodge(MeleeDodge::Direction direction) {
  const auto ws    = weaponState();
  const auto bs    = bodyStateMasked();
  auto*      armor = invent.currentArmor();
  const auto armorClass = armor!=nullptr ? std::optional<MeleeImpact::ArmorClass>(MeleeImpact::armorClass(armor->handle().protection[PROT_EDGE])) :
                                           std::optional<MeleeImpact::ArmorClass>();
  const auto wanted = MeleeDodge::kind(armorClass);
  const std::string dir    = std::string(MeleeDodge::name(direction));
  const std::string armour = std::string(MeleeDodge::name(armorClass));
  const std::string head   = "\"direction\":"+Bench::bodyJson(dir)+",\"armour\":"+Bench::bodyJson(armour)+
                             ",\"kind_wanted\":"+Bench::bodyJson(MeleeDodge::name(wanted));

  MeleeDodge::Situation s;
  s.enabled     = Gothic::physicsFlags().meleeDodge;
  // a human with a melee weapon or the fists drawn: a beast (the hero transformed too) keeps its own moves
  s.meleeWeapon = isHuman() && (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H);
  s.attacking   = isAttackAnim();
  s.stunned     = isMeleeStunned();
  s.knockedDown = meleeKnockdown || isDown();
  s.inAir       = isInAir() || isFalling() || isSlide() || isJump() || isJumpUp();
  s.inWater     = isInWater() || isSwim() || isDive();
  s.dodging     = isMeleeDodging();
  s.activity    = (isMeleeCharging() || bs==BS_STAND || bs==BS_NONE) ? MeleeDodge::Activity::Stand :
                  bs==BS_WALK   ? MeleeDodge::Activity::Walk   :
                  bs==BS_RUN    ? MeleeDodge::Activity::Run    :
                  bs==BS_PARADE ? MeleeDodge::Activity::Parade : MeleeDodge::Activity::Other;
  if(interactive()!=nullptr)
    s.activity = MeleeDodge::Activity::Other;
  const auto refusal = MeleeDodge::refusal(s);
  if(refusal!=MeleeDodge::Refusal::None) {
    if(refusal==MeleeDodge::Refusal::Stunned)
      allowMeleeStunAction("dodge"); // the stun_refused event, as a parade's and a jump back's
    meleeDodgeNote(dir+", armour "+armour+": refused, "+std::string(MeleeDodge::name(refusal)),"dodge_refused",
                   head+",\"reason\":"+Bench::bodyJson(MeleeDodge::name(refusal)));
    return false;
    }

  const auto stance = ws==WeaponState::W2H ? MeleeDodge::Stance::TwoHand :
                      ws==WeaponState::W1H ? MeleeDodge::Stance::OneHand : MeleeDodge::Stance::Fist;
  const auto plan = MeleeDodge::plan(wanted,stance,direction);
  const MeleeDodge::Candidate* chosen = nullptr;
  std::string missing;
  for(const auto& c:plan.candidates) {
    if(visual.hasAnim(c.name)) {
      chosen = &c;
      break;
      }
    reportMissingDodgeAnimation(c.name);
    missing += (missing.empty() ? "" : ", ")+c.name;
    }

  if(chosen==nullptr) {
    if(!plan.jumpBack) {
      meleeDodgeNote(dir+", armour "+armour+", "+std::string(MeleeDodge::name(wanted))+": no dodge this way (no "+missing+")",
                     "dodge_none",head+",\"missing\":"+Bench::bodyJson(missing));
      return false;
      }
    // Backwards the original jump back stays: its own cost and its old evasion, no dodge window
    tickMeleeStamina();
    const float before = stamina();
    const bool charging = isMeleeCharging();
    if(charging && !MeleeStamina::canAct(meleeStamina,hasStamina()))
      return false;
    if(charging) {
      visual.interrupt();
      cancelMeleeCharge("dodge");
      }
    bool ok = false;
    if(isUsingItemInHand()) {
      const bool costsStamina = hasStamina() && (!isPlayer() || !bool(walkMode()&WalkBit::WM_Sneak));
      if(!allowMeleeStunAction("jumpback") ||
         (costsStamina && !allowStaminaAction("jumpback",MeleeStamina::JumpBackCost,
                                             !isPlayer() && !BodyController::owns(*this))))
        return false;
      const char* clip = ws==WeaponState::Fist ? "T_FISTPARADEJUMPB" :
                         ws==WeaponState::W1H ? "T_1HJUMPB" : "T_2HJUMPB";
      ok = visual.startAnimAndGet(*this,clip,calcAniComb(),BS_PARADE)!=nullptr;
      if(ok) {
        interruptItemUse("dodge");
        if(costsStamina)
          spendStamina("jumpback",MeleeStamina::JumpBackCost);
        }
      } else {
      ok = setAnim(Anim::MoveBack);
      }
    const auto  cost   = before-stamina();
    meleeDodgeNote(dir+", armour "+armour+", "+std::string(MeleeDodge::name(wanted))+": the original jump back "+
                   visual.pose().animNames()+" (no "+missing+"), "+(ok ? "started" : "refused")+", cost "+
                   std::to_string(int(std::lround(cost)))+", window none",
                   "dodge_jumpback",head+",\"missing\":"+Bench::bodyJson(missing)+",\"started\":"+(ok ? "true" : "false")+
                   ",\"cost\":"+std::to_string(cost));
    return ok;
    }

  const auto kind      = chosen->kind;
  const bool fallback  = kind!=wanted;
  const auto window    = MeleeDodge::invulnerability(kind);
  const bool fullCost  = !isPlayer();
  const auto moveText  = std::string(MeleeDodge::name(wanted))+(fallback ? " -> "+std::string(MeleeDodge::name(kind)) : "")+" "+
                         chosen->name+(fallback ? " (no "+missing+")" : "");
  tickMeleeStamina();
  if(!MeleeDodge::staminaAllows(meleeStamina,kind,fullCost,hasStamina())) {
    if(Bench::bodyTracing())
      Bench::bodyEvent(*this,currentTarget,"stamina_refused",owner.tickCount(),
                          "\"action\":\"dodge\",\"after\":"+std::to_string(stamina()));
    meleeDodgeNote(dir+", armour "+armour+", "+moveText+": refused, stamina "+std::to_string(int(std::lround(stamina())))+
                   (isStaminaExhausted() ? " (spent)" : "")+" for "+std::to_string(int(MeleeDodge::cost(kind))),
                   "dodge_refused",head+",\"reason\":\"stamina\",\"stamina\":"+std::to_string(stamina()));
    return false;
    }

  const auto now     = owner.tickCount();
  const auto before  = meleeStaminaIdentity(visual.pose(),now);
  // Out of a block parade the dodge cuts it: a parade cannot be cut for its whole length otherwise. Gothic's jump
  // backs carry a defence window too (T_1HPARADEJUMPB, frames 1-12), but they are not cut (MeleeDodge::cutsParade):
  // a dodge pressed in a jump back, a blow's swing or a stumble is refused as by any transition that holds.
  bool fromParade = false;
  for(const auto& layer:visual.pose().combatLayers(now))
    if(layer.state==BS_PARADE && MeleeDodge::cutsParade(layer.sequence->name,!layer.sequence->data->defWindow.empty()))
      fromParade = true;
  const auto* sq     = visual.startAnimAndGet(*this,chosen->name,0,BS_PARADE,fromParade || isMeleeCharging());
  if(sq==nullptr || before==meleeStaminaIdentity(visual.pose(),now)) {
    meleeDodgeNote(dir+", armour "+armour+", "+moveText+": not taken, playing "+visual.pose().animNames(),
                   "dodge_refused",head+",\"reason\":\"pose\"");
    return false;
    }
  if(isUsingItemInHand())
    interruptItemUse("dodge");
  cancelMeleeCharge("dodge");
  const float staminaBefore = stamina();
  const float cost          = MeleeDodge::charged(kind,hasStamina());
  spendStamina("dodge",cost);
  meleeDodgeNote(dir+", armour "+armour+", "+moveText+", cost "+std::to_string(int(cost))+
                 (hasStamina() ? "" : " (melee_stamina off)")+", window "+
                 std::to_string(window.beginMs)+"-"+std::to_string(window.endMs)+" ms",
                 "dodge",head+",\"kind\":"+Bench::bodyJson(MeleeDodge::name(kind))+
                 ",\"animation_name\":"+Bench::bodyJson(chosen->name)+",\"fallback\":"+(fallback ? "true" : "false")+
                 ",\"missing\":"+Bench::bodyJson(missing)+",\"cost\":"+std::to_string(cost)+
                 ",\"stamina_before\":"+std::to_string(staminaBefore)+",\"stamina_after\":"+std::to_string(stamina())+
                 ",\"window_ms\":["+std::to_string(window.beginMs)+","+std::to_string(window.endMs)+"]");
  return true;
  }
