#include "npc.h"

#include "game/bodycontroller.h"
#include "game/gamescript.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

using namespace Tempest;

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
