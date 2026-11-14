#include "npc.h"
#include "npc_melee.h"
#include "npc_melee_shadow.h"
#include "world/bloodspawn.h"

#include "graphics/visualfx.h"
#include "graphics/effect.h"
#include "game/damagecalculator.h"
#include "game/bodyimpact.h"
#include "game/herodeed.h"
#include "game/bodyobservations.h"
#include "game/bodyarena.h"
#include "game/bodycontroller.h"
#include "game/meleeparry.h"
#include "game/meleecharge.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "world/benchhooks.h"
#include "gothic.h"

#include <filesystem>
#include <fstream>
#include <mutex>

using namespace Tempest;

// Damage, hit reactions, melee posture and body impacts of Npc. Moved from npc.cpp unchanged.

namespace {

const char* meleeBeastSizeClass(float mass) {
  const auto size = MeleeImpact::beastSize(mass);
  return size==MeleeImpact::BeastSize::Small ? "small" :
         size==MeleeImpact::BeastSize::Medium ? "medium" : "huge";
  }

void writeMeleePosture(const std::string& row) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> guard(mutex);
  static std::ofstream out;
  static uintmax_t bytes = 0;
  constexpr uintmax_t limit = 8*1024*1024;
  const char* path = "melee-posture.jsonl";
  const auto entry = "{\"arena_seed\":" + (BodyArena::enabled() ? std::to_string(BodyArena::seed()) : "null") + ',' + row.substr(1);
  if(!out.is_open()) {
    std::error_code error;
    bytes = std::filesystem::file_size(path,error);
    if(error)
      bytes = 0;
    out.open(path,std::ios::app);
    }
  if(bytes+entry.size()+1>limit) {
    out.close();
    std::error_code error;
    std::filesystem::remove("melee-posture.previous.jsonl",error);
    std::filesystem::rename(path,"melee-posture.previous.jsonl",error);
    out.open(path,std::ios::trunc);
    bytes = 0;
    }
  if(out) {
    out << entry << '\n';
    out.flush();
    bytes += entry.size()+1;
    }
  }

}

namespace NpcMelee {

std::string meleeBeastFields(float mass) {
  return std::string(",\"target_kind\":\"beast\",\"size_class\":\"")+meleeBeastSizeClass(mass)+'"';
  }

}

bool Npc::meleeDestructionBlow(MeleeDestructionBlow& blow) const {
  const auto mode=weaponState();
  if(mode!=WeaponState::Fist && mode!=WeaponState::W1H && mode!=WeaponState::W2H)
    return false;
  const auto& guild=owner.script().guildVal();
  const auto gl=this->guild();
  blow={};
  blow.rangeCm=float(guild.fight_range_base[gl]+guild.fight_range_fist[gl]);
  if(mode!=WeaponState::Fist) {
    const auto* weapon=invent.activeWeapon();
    if(weapon!=nullptr) {
      const bool twoHand=mode==WeaponState::W2H;
      blow.kind=twoHand ? FractureRule::Melee::TwoHand : FractureRule::Melee::OneHand;
      blow.rangeCm=float(guild.fight_range_base[gl]+(twoHand ? guild.fight_range_2ha[gl] : guild.fight_range_1ha[gl])+
                         weapon->swordLength());
      blow.weaponDamage=float(weapon->handle().damage_total);
      }
    else blow.rangeCm=0.f;
    }
  blow.strength=float(attribute(ATR_STRENGTH));
  return true;
  }

void Npc::commitDamage() {
  if(meleeKnockdown)
    return;
  auto* shadow = meleeShadow && meleeShadow->window ? meleeShadow.get() : nullptr;
  MeleeDestructionBlow blow;
  if(owner.physic()->destructionEnabled() && meleeDestructionBlow(blow)) {
    const float angle=rotationRad();
    // A body this swing's blade already struck when it stopped is not struck again.
    const size_t affected=owner.physic()->meleeMovableVobs(collosionCenter(),{std::cos(angle),0,std::sin(angle)},
                                                         blow.rangeCm,blow.weaponDamage,blow.strength,blow.kind,this,
                                                         shadow!=nullptr ? &shadow->struckVobs : nullptr);
    if(Bench::isActive())
      Bench::destructionMeleeHit(*this,affected,blow.weaponDamage+blow.strength);
    }
  const bool physicsHits = isHuman() && (shadow!=nullptr ? shadow->physicsHits :
      MeleeFighter::physicsHits(Gothic::physicsFlags().meleePhysicsHits,isPlayer(),
                               !isPlayer() && mvAlgo.characterVirtualReady()));
  const auto hitTrace = [&](const char* result) {
    if(shadow!=nullptr) {
      meleeShadow->legacyTarget = meleeShadowWho(currentTarget);
      meleeShadow->legacyNpc = currentTarget;
      meleeShadow->legacyResult = result;
      }
    if(physicsHits)
      return;
    BodyObservations::noteEvent(*this,currentTarget,result,owner.tickCount());
    if(Bench::bodyTracing())
      Bench::bodyEvent(*this,currentTarget,"hit_attempt",owner.tickCount(),
                          "\"result\":" + Bench::bodyJson(result),visual.pose().animNames());
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
  auto* shadowTarget = shadow!=nullptr ? currentTarget : nullptr;
  int32_t hpBefore = 0;
  if(shadow!=nullptr) {
    // Observe the guard takeDamage below meets; damage and its RNG still run once below. Like takeDamage, a body
    // down is not hit at all; this blow never ignores defence.
    const bool down = currentTarget->isDown();
    const auto guard = down ? MeleeDodge::Guard{} : currentTarget->meleeGuard(*this);
    shadow->legacyDodge = guard.evaded();
    shadow->legacyBlock = guard.blocked;
    shadow->legacyHit = !down && !shadow->legacyDodge && !shadow->legacyBlock;
    shadow->legacyResult = down ? "target_down" : (shadow->legacyBlock ? "blocked" :
                                                 (shadow->legacyDodge ? "evaded" : "applied"));
    hpBefore = currentTarget->attribute(ATR_HITPOINTS);
    }
  if(!physicsHits)
    currentTarget->takeDamage(*this,nullptr);
  if(shadow!=nullptr)
    shadow->damage = std::max(0,hpBefore-shadowTarget->attribute(ATR_HITPOINTS));
  }

Vec3 Npc::meleeImpulse(const Npc& attacker, const MeleeBladeContact* contact) const {
  const auto mode = attacker.weaponState();
  if(mode!=WeaponState::Fist && mode!=WeaponState::W1H && mode!=WeaponState::W2H)
    return {};
  const auto weapon = mode==WeaponState::Fist ? MeleeImpact::Weapon::Fist :
                      mode==WeaponState::W2H ? MeleeImpact::Weapon::TwoHand : MeleeImpact::Weapon::OneHand;
  Vec3 velocity;
  float angleDeg = 90.f;
  if(contact!=nullptr) {
    velocity = contact->velocityCmS;
    angleDeg = contact->surfaceAngleDeg;
    }
  else {
    // The legacy path has no geometric contact: use the same animation's typical
    // contact speed as damage scaling, directed from attacker to victim.
    const auto* shadow = attacker.meleeShadow.get();
    const float typical = shadow && shadow->window ? shadow->window->peakCmS*.7f :
                          mode==WeaponState::Fist ? 500.f : mode==WeaponState::W2H ? 1000.f : 800.f;
    auto direction = position()-attacker.position();
    direction.y = 0;
    if(direction.length()>0.f)
      velocity = Vec3::normalize(direction)*typical;
    }
  return MeleeImpact::impulse(weapon,velocity,angleDeg);
  }

float Npc::meleePostureSkillScale(const Npc& attacker) const {
  if(attacker.hasMeleeBeastPoise())
    return MeleeImpact::beastSkillThresholdScale(std::max(hitChance(TALENT_1H),hitChance(TALENT_2H)));
  const auto mode = attacker.weaponState();
  const int32_t attackSkill = mode==WeaponState::W1H ? attacker.hitChance(TALENT_1H) :
                              mode==WeaponState::W2H ? attacker.hitChance(TALENT_2H) : 0;
  return MeleeImpact::skillThresholdScale(attackSkill,attacker.attribute(ATR_STRENGTH),
                                        hasMeleeBeastPoise() ? 0 : std::max(hitChance(TALENT_1H),hitChance(TALENT_2H)));
  }

bool Npc::knockDownMelee(Npc& attacker, const Vec3& impulse, std::string_view reason) {
  interruptItemUse("knockdown");
  setInteraction(nullptr,true);
  attachToPoint(nullptr);
  updateRagdollQuestAnchor();
  const auto hazards = ragdollQuestReturn.armed ? ragdollQuestReturn.hazards : uint8_t(0);
  ragdollQuestReturn.armed = !isPlayer() && Gothic::physicsFlags().questGuard && ragdollQuestReturn.hasAnchor;
  ragdollQuestReturn.hazards = ragdollQuestReturn.armed ? hazards : 0;
  meleeSavePosition = position();
  living.reset();
  finishMeleeShadowSwing(true);
  const auto hpBeforeFall = attribute(ATR_HITPOINTS);
  if(createRagdoll()) {
    endMeleeStun(reason);
    meleeParadeOrigin.reset();
    meleeKnockdown = true;
    // Keep an ordinary live animation underneath the native body, so a save
    // omitting this transient fall loads standing rather than replaying a hit.
    visual.pose().stopAllAnim();
    setAnim(Anim::Idle);
    meleeFallTick = owner.tickCount();
    meleeRiseTick = 0;
    meleeGetupForced = false;
    meleeSlowSince = 0;
    meleeReturnCount = ragdollQuestReturns;
    // thrown along the blade's sweep and away from the one who struck: a chop from
    // above has little sweep left over the ground, the step behind it still pushes
    Vec3 away = position()-attacker.position();
    away.y = 0;
    if(away.length()>1e-3f)
      away = Vec3::normalize(away)*(impulse.length()*.5f);
    ragdoll->knockOff(Vec3(impulse.x,0,impulse.z)+away,impulse.length(),&attacker);
    fghAlgo.onTakeHit();
    clearSpeed();
    writeMeleePosture("{\"event\":\"knockdown_health\",\"tick_ms\":"+std::to_string(meleeFallTick)+
                     ",\"target\":"+meleeShadowWho(this)+",\"hp_before\":"+std::to_string(hpBeforeFall)+
                     ",\"hp_after\":"+std::to_string(attribute(ATR_HITPOINTS))+
                     ",\"unconscious\":"+(isUnconscious() ? "true" : "false")+"}");
    return true;
    }
  return false;
  }

void Npc::applyMeleePosture(Npc& attacker, const MeleeBladeContact* contact, bool blocked, float before) {
  if((isMeleeCharging() || isMeleeChargeReleasing()) && hasMeleeSuperArmor())
    return;
  const bool beast = hasMeleeBeastPoise();
  const bool beastAttacker = isHuman() && attacker.hasMeleeBeastPoise();
  if(!Gothic::physicsFlags().meleePosture || (!isHuman() && !beast) || (!attacker.isHuman() && !beastAttacker))
    return;
  if(beastAttacker && (DamageCalculator::damageTypeMask(attacker)&(1<<daedalus::DamageType::FLY))!=0)
    return;
  const auto mode = attacker.weaponState();
  if(!beastAttacker && mode!=WeaponState::Fist && mode!=WeaponState::W1H && mode!=WeaponState::W2H)
    return;
  const float beastMass = beastAttacker ? attacker.physicsMass() : 0.f;
  auto impulse = meleeImpulse(attacker,contact);
  if(beastAttacker) {
    // Bite posture uses body mass without changing the existing hit reaction impulse.
    auto direction = position()-attacker.position();
    direction.y = 0;
    impulse = direction.length()>0.f ? Vec3::normalize(direction)*MeleeImpact::beastImpulse(beastMass) : Vec3();
    }
  // Weapon skill is the progression of knockdowns: trained hitters knock down sooner, trained fighters stand longer.
  const float skillScale = meleePostureSkillScale(attacker);
  const bool stagger = Gothic::physicsFlags().meleeStagger;
  const auto* armor = invent.currentArmor();
  const float armorScale = !beast && stagger && armor!=nullptr ?
      MeleeImpact::armorThresholdScale(MeleeImpact::armorClass(armor->handle().protection[PROT_EDGE])) : 1.f;
  const float chargeMult = attacker.meleeChargeMultiplier();
  const float thresholdScale = skillScale*armorScale/chargeMult;
  const bool stunned = isMeleeStunned();
  const auto heavy = attacker.isMeleeChargeReleasing() ? MeleeImpact::HeavyHit::Charged :
                     MeleeImpact::heavyHit(beastAttacker ? MeleeImpact::Weapon::Fist :
                                         mode==WeaponState::W2H ? MeleeImpact::Weapon::TwoHand :
                                         mode==WeaponState::W1H ? MeleeImpact::Weapon::OneHand : MeleeImpact::Weapon::Fist,
                                         !beastAttacker && contact ? contact->velocityCmS : Vec3(),
                                         MeleeImpact::postureIncrease(impulse.length(),physicsMass(),thresholdScale,
                                                                     MeleeImpact::postureThresholdScale()));
  const bool heavyOnStaggered = !beast && stunned &&
      stagger && meleeStunReason=="posture" && heavy!=MeleeImpact::HeavyHit::None;
  bool knocked = false, staggered = false;
  const bool eligible = canAccumulateMeleePosture(true,blocked,meleeKnockdown,isDown()) &&
                        (!beast || MeleeImpact::beastCanAccumulatePosture(physicsMass()));
  const bool full = eligible && (!stagger || !stunned) &&
                    meleePosture.hit(impulse.length(),physicsMass(),owner.tickCount(),thresholdScale);
  if(full && stagger) {
    startMeleeStun(attacker,"posture");
    meleePosture.value = 0;
    staggered = isMeleeStunned();
    }
  if(!beast && eligible && ((full && !stagger) || heavyOnStaggered)) {
    knocked = knockDownMelee(attacker,impulse,"knockdown");
    }
  const auto who = meleeShadowWho(this);
  const auto beastFields = beast ? meleeBeastFields(physicsMass()) : std::string();
  const auto attackerFields = beastAttacker ? ",\"attacker_kind\":\"beast\",\"beast_mass\":"+std::to_string(beastMass)+
                                             ",\"size_class\":"+Bench::bodyJson(meleeBeastSizeClass(beastMass)) : std::string();
  writeMeleePosture("{\"event\":\"hit\",\"tick_ms\":"+std::to_string(owner.tickCount())+
                   ",\"attacker\":"+meleeShadowWho(&attacker)+",\"target\":"+who+beastFields+attackerFields+
                   ",\"impulse_ns\":"+std::to_string(impulse.length())+
                   ",\"skill_threshold_scale\":"+std::to_string(skillScale)+
                   ",\"armor_threshold_scale\":"+std::to_string(armorScale)+
                   ",\"charge_mult\":"+std::to_string(chargeMult)+
                   ",\"zone\":"+Bench::bodyJson(contact ? HitZones::name(HitZones::Zone(contact->zone)) : std::string_view("body"))+
                   ",\"before\":"+std::to_string(before)+",\"after\":"+std::to_string(meleePosture.value)+
                   ",\"blocked\":"+(blocked ? "true" : "false")+",\"knockdown\":"+(knocked ? "true" : "false")+"}");
  if(staggered)
    writeMeleePosture("{\"event\":\"stagger\",\"tick_ms\":"+std::to_string(owner.tickCount())+
                     ",\"attacker\":"+meleeShadowWho(&attacker)+",\"target\":"+who+beastFields+attackerFields+"}");
  const char* heavyReason = heavy==MeleeImpact::HeavyHit::TwoHanded ? "two_handed" :
                            heavy==MeleeImpact::HeavyHit::Downward ? "downward" :
                            heavy==MeleeImpact::HeavyHit::Charged ? "charged" : "posture";
  if(knocked)
    writeMeleePosture("{\"event\":\"knockdown\",\"tick_ms\":"+std::to_string(meleeFallTick)+
                     ",\"fall_ms\":"+std::to_string(meleeFallTick)+",\"target\":"+who+attackerFields+
                     (heavyOnStaggered ? ",\"cause\":"+Bench::bodyJson("heavy_on_staggered")+
                                          ",\"heavy\":"+Bench::bodyJson(heavyReason) : "")+"}");
  }

bool Npc::tickMeleePosture(uint64_t dt) {
  if(!meleeKnockdown) {
    if(Gothic::physicsFlags().meleePosture && (isHuman() || hasMeleeBeastPoise()))
      meleePosture.tick(owner.tickCount(),dt);
    else
      meleePosture.reset();
    return false;
    }
  const auto now = owner.tickCount();
  lastEventTime = now;
  visual.processLayers(owner);
  const bool enabled = Gothic::physicsFlags().meleePosture;
  if(ragdoll && (now-meleeFallTick>=1200 || !enabled)) {
    // Use the same settled-body gate as KO, with a deadline for starting
    // the authored get-up rather than a deadline for finishing it.
    if(!recoverRagdoll()) {
      bool rested = false;
      if(enabled) {
        // The live fall can recover before native sleep: require every part
        // below 20 cm/s and 0.5 rad/s continuously for 300 ms.
        const auto state = ragdoll->state();
        const bool slow = std::all_of(state.parts.begin(),state.parts.end(),[](const auto& part) {
          return part.linearVelocityMeters.quadLength()<=.04f && part.angularVelocity.quadLength()<=.25f;
          });
        if(!slow)
          meleeSlowSince = 0;
        else if(meleeSlowSince==0)
          meleeSlowSince = now;
        rested = meleeSlowSince!=0 && now-meleeSlowSince>=300;
        }
      if(rested || now-meleeFallTick>=3000 || !enabled)
        meleeGetupForced = recoverRagdoll(true) && enabled && !rested;
      }
    }
  if(!ragdoll) {
    // The wounded loop may reject Idle on its very first frame. Retry only
    // until the existing get-up transition accepts it, then let it finish.
    if(meleeRiseTick==0 && setAnim(Anim::Idle)) {
      meleeRiseTick = now;
      writeMeleePosture("{\"event\":\"getup_start\",\"tick_ms\":"+std::to_string(now)+
                       ",\"target\":"+meleeShadowWho(this)+",\"fall_ms\":"+std::to_string(meleeFallTick)+
                       ",\"getup_ms\":"+std::to_string(now)+",\"wait_ms\":"+std::to_string(now-meleeFallTick)+
                       ",\"forced\":"+(meleeGetupForced ? "true" : "false")+"}");
      }
    // Wounded-to-stand has no successor. Ordinary NPC ticking resumes Idle
    // after that transition ends; this isolated live fall must do the same.
    if(meleeRiseTick!=0 && !visual.pose().hasAnim())
      setAnim(Anim::Idle);
    // Locomotion can replace the get-up with a non-standing loop (fall/swim).
    // Give the authored transition its full time, but never leave controls locked.
    const bool getupTimedOut = meleeRiseTick!=0 && now-meleeRiseTick>=6000 && !visual.pose().isStanding();
    if(visual.pose().isStanding() || !enabled || getupTimedOut) {
      if(!visual.pose().isStanding()) {
        visual.pose().stopAllAnim();
        setAnim(Anim::Idle);
        }
      meleeKnockdown = false;
      ragdollRecovering = false;
      meleePosture.recover(now);
      writeMeleePosture("{\"event\":"+Bench::bodyJson(enabled ? "recover" : "cancel")+",\"tick_ms\":"+std::to_string(now)+
                       ",\"target\":"+meleeShadowWho(this)+",\"fall_ms\":"+std::to_string(meleeFallTick)+
                       ",\"getup_ms\":"+std::to_string(meleeRiseTick)+",\"rise_ms\":"+std::to_string(now)+
                       (enabled && meleeRiseTick!=0 ? ",\"getup_duration_ms\":"+std::to_string(now-meleeRiseTick) : "")+
                       (!enabled ? ",\"reason\":\"feature_disabled\"" : getupTimedOut ? ",\"reason\":\"getup_timeout\"" : "")+
                       ",\"returned\":"+(ragdollQuestReturns!=meleeReturnCount ? "true" : "false")+"}");
      if(!enabled)
        meleePosture.reset();
      return false;
      }
    mvAlgo.tick(dt,MoveAlgo::WaitMove);
    }
  return true;
  }

void Npc::takeDamage(Npc &other, const Bullet* b, bool ignoreDefence, float meleeScale, const MeleeBladeContact* contact) {
  if(isDown())
    return;
  // Inside a dodge's window nothing passes: no damage, balance, reaction or perception
  if(!ignoreDefence && meleeDodgeEvades(other,b==nullptr ? "melee" : "ranged"))
    return;

  stopPhysicsInvestigation(false);

  assert(b==nullptr || !b->isSpell());
  const auto& pose    = visual.pose();
  // The guard is read again after stopping an investigation, which may stop the walk and change the pose.
  // A blow that ignores defence meets none: no jump back, no parade, no dodge window above.
  const auto  guard   = ignoreDefence ? MeleeDodge::Guard{} : meleeGuard(other);
  const bool  isJumpb = guard.jumpedBack;
  const bool  isBlock = guard.blocked;
  if((isBlock || isJumpb) && b==nullptr)
    BodyObservations::noteEvent(*this,&other,isBlock ? "blocked" : "evaded",owner.tickCount());
  const auto parade = meleeParadeLayer(pose,owner.tickCount());
  const auto backstab = b==nullptr && !isBlock && !isJumpb ? meleeBackstabKind(other) : MeleeImpact::BackstabKind::None;
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,&other,"defence_result",owner.tickCount(),
                        std::string("\"damage_kind\":") + Bench::bodyJson(b==nullptr ? "melee" : "ranged") +
                        ",\"blocked\":" + (isBlock && b==nullptr ? "true" : "false") +
                        ",\"jumped_back\":" + (isJumpb && b==nullptr ? "true" : "false") +
                        ",\"stun_bonus\":" + std::to_string(b==nullptr && !isBlock && !isJumpb && backstab==MeleeImpact::BackstabKind::None ? meleeStunBonus(other) : 1.f) +
                        ",\"backstab_mult\":" + std::to_string(meleeBackstabMultiplier(backstab)) +
                        ",\"parade_ms\":" + (parade ? std::to_string(owner.tickCount()-parade->origin) : "null"),pose.animNames());

  lastHit = &other;
  if(!isPlayer())
    setOther(&other);
  owner.sendPassivePerc(*this,other,*this,PERC_ASSESSFIGHTSOUND);

  if(!(isBlock || isJumpb) || b!=nullptr) {
    takeDamage(other,b,COLL_DOEVERYTHING,0,false,meleeScale,contact);
    } else {
    if(isBlock) {
      if(Gothic::physicsFlags().meleeParry && isHuman() && parade)
        applyMeleeParry(other,contact);
      else
        applyMeleePosture(other,contact,true,meleePosture.value);
      }
    if(invent.activeWeapon()!=nullptr)
      visual.emitBlockEffect(*this,other);
    }
  }

void Npc::takeDamage(Npc& other, const Bullet* b, const VisualFx* vfx, int32_t splId) {
  if(isDown())
    return;
  if(meleeDodgeEvades(other,"spell"))
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

void Npc::takeDamage(Npc& other, const Bullet* b, const CollideMask bMask, int32_t splId, bool isSpell, float meleeScale, const MeleeBladeContact* contact) {
  const int32_t traceHpBefore = attribute(ATR_HITPOINTS);
  const bool traceKnockdownBefore = meleeKnockdown;
  const auto traceAttackerAnimation = Bench::bodyTracing() ? other.visual.pose().animNames() : std::string();
  const float postureBefore = meleePosture.value;
  const auto backstab = !isSpell && b==nullptr ? meleeBackstabKind(other) : MeleeImpact::BackstabKind::None;
  const float backstabAngle = MeleeImpact::backstabAngle(angle,angleDir(other.x-x,other.z-z));
  const float backstabMult = meleeBackstabMultiplier(backstab);
  const float stunBonus = !isSpell && b==nullptr && backstab==MeleeImpact::BackstabKind::None ? meleeStunBonus(other) : 1.f;
  const bool superArmor = hasMeleeSuperArmor();
  const float chargeMult = !isSpell && b==nullptr ? other.meleeChargeMultiplier() : 1.f;
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
    for(size_t i=0; i<daedalus::DamageType::NUM; ++i)
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
    // Keep a beast's in-flight bite and native wait intact under size super armor.
    if(!(superArmor && hasMeleeBeastPoise())) {
      const auto tracePrimitiveBefore = fghAlgo.queuedAction();
      fghAlgo.onTakeHit();
      BodyController::takeHit(*this);
      if(Bench::bodyTracing())
        Bench::bodyEvent(*this,&other,"native_queue_clear",owner.tickCount(),
                            "\"reason\":\"take_hit\",\"primitive_before\":" + std::to_string(int(tracePrimitiveBefore)));
      implFaiWait(0);
      }
    }

  hitResult = DamageCalculator::damageValue(other,*this,b,isSpell,dmg,bMask);
  const int32_t baseDamage = hitResult.value;
  if((contact!=nullptr || stunBonus!=1.f || backstabMult!=1.f || chargeMult!=1.f) && hitResult.hasHit && !hitResult.invincible)
    hitResult.value = int32_t(std::lround(float(hitResult.value)*meleeScale*stunBonus*backstabMult*chargeMult));
  if(hitResult.hasHit && !hitResult.invincible) {
    const auto now = owner.tickCount();
    meleeRecentHits.erase(std::remove_if(meleeRecentHits.begin(),meleeRecentHits.end(),[&](const auto& hit) {
      return hit.first==&other || !MeleeImpact::backstabRecentHit(now,hit.second);
      }),meleeRecentHits.end());
    meleeRecentHits.emplace_back(&other,now);
    if(backstab!=MeleeImpact::BackstabKind::None && Bench::bodyTracing())
      Bench::bodyEvent(*this,&other,"backstab",now,
                          "\"backstab\":{\"kind\":"+Bench::bodyJson(backstab==MeleeImpact::BackstabKind::Stunned ? "stunned" :
                                                         backstab==MeleeImpact::BackstabKind::Sleeping ? "sleeping" : "unaware")+
                          ",\"angle_deg\":"+std::to_string(backstabAngle)+
                          ",\"sneak_talent\":"+std::to_string(other.talentSkill(TALENT_SNEAK))+
                          ",\"mult\":"+std::to_string(backstabMult)+"}");
    }
  if(hitResult.hasHit && !isSpell && b!=nullptr)
    queueArrowPhysics(*b);
  const auto traceDamage = [&] {
    BodyObservations::noteEvent(*this,&other,hitResult.value>0 ? "damage_received" : "damage_no_loss",owner.tickCount());
    const int32_t lost = traceHpBefore-attribute(ATR_HITPOINTS);
    if(lost>0) {
      Gothic::inst().onCombatHit(*this,other,lost,isSpell ? "spell" : (b==nullptr ? "melee" : "ranged"));
      Bench::npcDuelHit(*this,other,traceHpBefore,attribute(ATR_HITPOINTS),baseDamage,
                           hitResult.critical,meleeScale,stunBonus,backstabMult);
      }
    if(Bench::bodyTracing())
      Bench::bodyEvent(*this,&other,"damage",owner.tickCount(),
                          "\"damage_kind\":" + Bench::bodyJson(isSpell ? "spell" : (b==nullptr ? "melee" : "ranged")) +
                          ",\"hp_before\":" + std::to_string(traceHpBefore) + ",\"hp_after\":" +
                          std::to_string(attribute(ATR_HITPOINTS)) + ",\"net\":" + std::to_string(hitResult.value) +
                          ",\"stun_bonus\":" + std::to_string(stunBonus) +
                          ",\"backstab_mult\":" + std::to_string(backstabMult) +
                          ",\"charge_mult\":" + std::to_string(chargeMult) +
                          ",\"super_armor\":" + (superArmor ? "true" : "false") +
                          (hasMeleeBeastPoise() ? meleeBeastFields(physicsMass()) : std::string()) +
                          ",\"has_hit\":" + (hitResult.hasHit ? "true" : "false") +
                          ",\"melee_knockdown_before\":" + (traceKnockdownBefore ? "true" : "false") +
                          ",\"attacker_animation\":" + Bench::bodyJson(traceAttackerAnimation) +
                          ",\"down\":" + (isDown() ? "true" : "false") + ",\"dead\":" + (isDead() ? "true" : "false"));
    };
  if(Bench::isActive()) {
    // what it did: the weapon's damage before the draw and the armour, k the draw times the zone (the zone
    // scales what the armour let through); the arrow's kind
    int32_t raw = 0;
    if(b!=nullptr)
      for(auto v:b->damage().val)
        raw += v;
    const char* kind = isSpell ? "spell" : (b==nullptr ? "melee" : (b->isPrecise() ? "precise" : "dice"));
    const float k    = b!=nullptr && b->isPrecise() ? Archery::damageAt(b->draw())*Archery::zoneDamage(b->hitZone()) : 1.f;
    const auto  zone = b!=nullptr && b->isPrecise() ? HitZones::name(b->hitZone()) : std::string_view("-");
    const auto  who  = Bench::who(this);
    const auto  by   = Bench::who(&other);
    char buf[256];
    std::snprintf(buf,sizeof(buf),"damage %.*s: raw %d zone %.*s k %.2f prot %d net %d hit %d, hp %d, %s, by %.*s stun_bonus %.2f",
                  int(who.size()),who.data(),int(raw),int(zone.size()),zone.data(),double(k),int(protection(PROT_POINT)),
                  int(hitResult.value),int(hitResult.hasHit),int(attribute(ATR_HITPOINTS)),kind,int(by.size()),by.data(),double(stunBonus));
    Bench::note(buf);
    }
  if(!isSpell && !isDown() && hitResult.hasHit)
    owner.addWeaponHitEffect(other,b,*this).play();
  if(!isSpell && hitResult.hasHit && hitResult.value>0) {
    // blood: at the blade's contact, the arrow's impact, or between the two
    Tempest::Vec3 wound, dir;
    if(contact!=nullptr) {
      wound = contact->point;
      dir   = contact->velocityCmS;
      }
    else if(b!=nullptr) {
      wound = b->impactPoint();
      dir   = b->direction();
      }
    else {
      wound = (other.centerPosition()+centerPosition())*0.5f;
      dir   = centerPosition()-other.centerPosition();
      }
    BloodSpawn::onHit(owner,*this,wound,dir,hitResult.value);
    }

  if(isDown()) {
    onNoHealth(dontKill,HS_NoSound);
    traceDamage();
    return;
    }

  if(hitResult.hasHit && !benchStill && !meleeKnockdown) {
    if(superArmor && hasMeleeBeastPoise() && Bench::bodyTracing())
      Bench::bodyEvent(*this,&other,"super_armor",owner.tickCount(),
                          "\"active\":true"+meleeBeastFields(physicsMass()));
    auto state   = bodyStateMasked();
    bool stumble = false;
    if(!superArmor && interactive()==nullptr && ((state&BS_FLAG_INTERRUPTABLE)!=BS_NONE || state==BS_RUN || state==BS_NONE)) {
      //NONE/RUN requires for monsters like waran
      const bool noInter = (hnpc->bodystate_interruptable_override!=0);
      if(!noInter) {
        //NOTE: kepp rotation animation: this results in more accurate fight with trolls
        // visual.setAnimRotate(*this,0);
        visual.interrupt(); // TODO: put down in pipeline, at Pose and merge with setAnimAngGet
        }

      if((damageType & (1<<daedalus::DamageType::FLY))==0)
        stumble = setAnimAngGet(lastHitType=='A' ? Anim::StumbleA  : Anim::StumbleB)!=nullptr;
      }
    if((damageType & (1<<daedalus::DamageType::FLY))==0 &&
       !(superArmor && (isMeleeCharging() || isMeleeChargeReleasing())))
      hitReaction(other,b,hitResult.value,stumble,contact);
    }

  // throw enemy
  const bool physicalWind = isSpell && Gothic::physicsFlags().spellPhysics &&
                            owner.script().physicalSpell(splId)==GameScript::PhysicalSpell::WindFist;
  if(hitResult.hasHit && (damageType & (1<<daedalus::DamageType::FLY)) && !physicalWind &&
     !(superArmor && (isMeleeCharging() || isMeleeChargeReleasing()))) {
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
  if(hitResult.hasHit && !isSpell && b==nullptr)
    applyMeleePosture(other,contact,false,postureBefore);
  traceDamage();
  }

void Npc::hitReaction(const Npc& other, const Bullet* b, int32_t damage, bool stumble, const MeleeBladeContact* contact) {
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
  if(contact!=nullptr)
    at = contact->point;
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
  if(b==nullptr && queueMeleeReaction(other,at,contact))
    hit.strength *= 0.5f; // the struck chain gives way physically, the fake bend only follows
  flinch(hit);
  }

bool Npc::queueMeleeReaction(const Npc& other, const Vec3& at, const MeleeBladeContact* contact) {
  syncDeferredAnimation();
  // the blow pushes the struck chain on the living rig like an arrow does, its motors bring it back
  if(!Gothic::physicsFlags().meleeReactions || !living || !isHuman())
    return false;
  size_t bone = contact!=nullptr ? contact->bone : size_t(-1);
  if(bone>=visual.pose().boneCount()) {
    const auto& transform = visual.transform();
    const float scale = Vec3(transform.at(0,0),transform.at(0,1),transform.at(0,2)).length();
    bone = visual.hitZoneShape().closestBone(visual.pose().transform(),visual.pose().boneCount(),scale,at);
    }
  if(bone>=visual.pose().boneCount())
    return false;
  // the same blow as the posture takes: weapon with the arm behind it, along the blade's motion
  const Vec3  blow     = meleeImpulse(other,contact);
  const float momentum = std::min(blow.length(),40.f);
  if(!std::isfinite(momentum) || momentum<1.f)
    return false;
  const Vec3 d = Vec3::normalize(blow);
  // the blade cuts on and the stance takes the rest: the chain gets a part of the momentum.
  // The stumble already throws the body, so the motors keep most of their hold: a weak
  // chain loses the authored swing of the arms and hangs like a rope
  constexpr float transfer = 0.15f;
  const float strength = 0.55f*std::clamp(momentum/40.f,0.25f,1.f);
  const Vec3  impulse  = d*(momentum*transfer);
  // twice the arrow's spring keeps up with the stumble's fast swing of the arms
  constexpr float stiffness = 2.f;
  arrowImpacts.push_back({bone,at,impulse,strength,other.handlePtr(),true,stiffness});
  meleeReaction.bone      = bone;
  meleeReaction.point     = at;
  meleeReaction.impulseNs = impulse;
  meleeReaction.strength  = strength;
  ++meleeReaction.hits;
  if(Bench::isActive()) {
    const auto who = Bench::who(this);
    char buf[200];
    std::snprintf(buf,sizeof(buf),"melee_reaction %.*s: bone %d momentum %.1f impulse %.1f strength %.2f",
                  int(who.size()),who.data(),int(bone),double(momentum),double(momentum*transfer),double(strength));
    Bench::note(buf);
    }
  return true;
  }

Npc* Npc::bodyImpactCulprit() const {
  const auto culprit=bodyImpactLaunchCulprit.lock();
  return culprit!=nullptr ? static_cast<Npc*>(culprit->user_ptr) : nullptr;
  }

void Npc::recordBodyImpact(float speedCmMs, Npc* culprit, const char* source) {
  if(!canTakeBodyImpact(Gothic::physicsFlags().bodyImpactDamage,isUnconscious(),isDead(),isImmortal()) ||
     !std::isfinite(speedCmMs) || speedCmMs<=0.f)
    return;
  applyBodyImpact();
  if(isDead()) return;
  if(bodyImpactWindow.add(owner.tickCount(),speedCmMs)) {
    bodyImpactPendingCulprit=culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<daedalus::INpc>();
    bodyImpactSource=source;
    }
  }

void Npc::applyBodyImpact() {
  if(!canTakeBodyImpact(Gothic::physicsFlags().bodyImpactDamage,isUnconscious(),isDead(),isImmortal())) {
    bodyImpactWindow.clear();
    bodyImpactPendingCulprit.reset();
    return;
    }
  const float speed=bodyImpactWindow.take(owner.tickCount());
  if(speed<=0.f) return;
  const auto culpritHandle=bodyImpactPendingCulprit.lock();
  auto* culprit=culpritHandle!=nullptr ? static_cast<Npc*>(culpritHandle->user_ptr) : nullptr;
  bodyImpactPendingCulprit.reset();
  const auto damage=DamageCalculator::damageFall(*this,speed);
  const int32_t before=attribute(ATR_HITPOINTS);
  if(damage.hasHit) {
    const auto decision=BodyImpact::decide(culprit!=nullptr && isNonlethalFight(*culprit),isHuman(),isPlayer(),
                                           culprit!=nullptr,damage.value>=before-1);
    auto deed=decision.captureMurder && damage.value>=before ? HeroDeed::capture(*culprit,"murder",this) : nullptr;
    if(culprit!=nullptr && (decision.knockOut || damage.value>=before)) {
      lastHit=culprit;
      currentOther=culprit;
      }
    changeAttribute(ATR_HITPOINTS,-damage.value,decision.knockOut);
    if(decision.perception==BodyImpact::Perception::Defeat && isUnconscious())
      owner.sendPassivePerc(*this,*culprit,*this,PERC_ASSESSDEFEAT);
    else if(decision.perception==BodyImpact::Perception::Murder && isDead())
      owner.sendPassivePerc(*this,*culprit,*this,PERC_ASSESSMURDER,std::move(deed));
    }
  if(Bench::isActive()) {
    const auto who=Bench::who(this),by=Bench::who(culprit);
    char buf[384];
    std::snprintf(buf,sizeof(buf),"body_impact %.*s: source %s speed_cm_s %.6f damage %d hp_before %d hp_after %d dead %d by %.*s",
                  int(who.size()),who.data(),bodyImpactSource,double(speed*1000.f),before-attribute(ATR_HITPOINTS),
                  before,attribute(ATR_HITPOINTS),int(isDead()),int(by.size()),by.data());
    Bench::note(buf);
    }
  if(Bench::bodyTracing())
    Bench::bodyEvent(*this,culprit,"body_impact",owner.tickCount(),
                        "\"source\":"+Bench::bodyJson(bodyImpactSource)+
                        ",\"speed_cm_s\":"+std::to_string(speed*1000.f)+
                        ",\"damage\":"+std::to_string(before-attribute(ATR_HITPOINTS))+
                        ",\"hp_before\":"+std::to_string(before)+
                        ",\"hp_after\":"+std::to_string(attribute(ATR_HITPOINTS))+
                        ",\"dead\":"+(isDead() ? "true" : "false"));
  }

void Npc::tickBodyImpact() {
  if(ragdoll) {
    const float speed=ragdoll->takeImpactSpeed();
    if(speed>0.f) {
      auto* culprit=ragdoll->impactCulprit();
      bodyImpactLaunchCulprit=culprit!=nullptr ? culprit->handlePtr() : std::weak_ptr<daedalus::INpc>();
      recordBodyImpact(speed,culprit,"ragdoll");
      }
    }
  applyBodyImpact();
  if(mvAlgo.state()!=MoveAlgo::InAir && mvAlgo.state()!=MoveAlgo::Falling && !ragdoll) bodyImpactSpellFlight=false;
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
  int32_t hp = attribute(ATR_HITPOINTS);
  // Keep vanilla landing feedback immediate; only HP joins the impact window.
  if(dmg.hasHit && hp>dmg.value) {
    emitSoundSVM("SVM_%d_AARGH");
    clearState(true);
    }
  if(bodyImpactSpellFlight) {
    bodyImpactSpellFlight=false;
    if(Gothic::physicsFlags().bodyImpactDamage) {
      recordBodyImpact(fallSpeed.length(),bodyImpactCulprit(),"spell_landing");
      return;
      }
    }
  if(!dmg.hasHit)
    return;
  changeAttribute(ATR_HITPOINTS,-dmg.value,false);
  }

void Npc::takeDrownDamage() {
  changeAttribute(Attribute::ATR_HITPOINTS, -attribute(Attribute::ATR_HITPOINTSMAX), false);
  }
