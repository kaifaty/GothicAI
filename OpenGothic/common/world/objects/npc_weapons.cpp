#include "npc.h"

#include "graphics/mesh/skeleton.h"
#include "game/gamescript.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"
#include "camera.h"
#include "gothic.h"

#include <algorithm>
#include <cmath>

using namespace Tempest;

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

Gaze::Source Npc::gazeTarget(Tempest::Vec3& at, const Npc** whom) const {
  if(whom!=nullptr)
    *whom = nullptr;
  if(headInput) {
    const float yaw = rotationRad()+headInput->x*float(M_PI)/180.f;
    const float pitch = headInput->y*float(M_PI)/180.f;
    at = mapHeadBone() + Vec3(std::cos(yaw)*std::cos(pitch),std::sin(pitch),std::sin(yaw)*std::cos(pitch))*1000.f;
    return Gaze::Source::Input;
    }
  if(lookPointUntil>owner.tickCount()) {
    at = lookPoint;
    return Gaze::Source::Script;
    }
  static const float focusDist = 800;
  const Vec3 eyes = {0,6,0}; // the eyes are a bit above the head bone
  if(auto other = talkPartner()) {
    at = other->mapHeadBone() + eyes;
    if(whom!=nullptr)
      *whom = other;
    return Gaze::Source::Talk;
    }
  if(physicsNoiseUntil>owner.tickCount() && Gothic::physicsFlags().physicsNoise && canHearPhysicsNoise()) {
    at = physicsNoisePosition;
    return Gaze::Source::Script;
    }
  if(currentLookAtNpc!=nullptr && currentLookAtNpc!=this) {
    at = currentLookAtNpc->mapHeadBone() + eyes;
    if(whom!=nullptr)
      *whom = currentLookAtNpc;
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
    if(whom!=nullptr)
      *whom = t;
    return Gaze::Source::Focus;
    }
  if(auto t = attention.target()) {
    at = t->mapHeadBone() + eyes;
    if(whom!=nullptr)
      *whom = t;
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
  return visual.pose().aimIk().leanNeeded(*world().physic(),position(),rotationRad(),look,look,down,aimEdge(look));
  }
