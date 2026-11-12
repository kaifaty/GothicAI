#include "npc.h"

#include <Tempest/Log>

#include "graphics/visualfx.h"
#include "graphics/effect.h"
#include "game/damagecalculator.h"
#include "game/gamescript.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "testbench.h"

#include <cstdio>
#include <string>

using namespace Tempest;

// Spells and the bow of Npc: casting, the bow shot, manual aim, arrow release and hit zones.
// Moved from npc.cpp unchanged.

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
  setQuiverTaken(0);
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
  // the inventory shows as it is again (the arrow taken from the quiver is gone)
  setQuiverTaken(0);
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
