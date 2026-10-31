#include "playercontrol.h"

#include <cmath>
#include <cstring>

#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/world.h"
#include "ui/dialogmenu.h"
#include "ui/inventorymenu.h"
#include "physics/dynamicworld.h"
#include "camera.h"
#include "graphics/mesh/aim.h"
#include "graphics/mesh/aimwalk.h"
#include "graphics/mesh/footik.h"
#include "gothic.h"

namespace {
// the crosshair of the manual aim
constexpr float MaxSpread   = 6.f*float(M_PI)/180.f; // wide open: the bow being raised, reloaded
constexpr float SpreadOpen  = 0.1f;                  // seconds: it opens about twice that long...
constexpr float SpreadClose = 0.2f;                  // ...closes
constexpr float StanceRate  = 6.f;                   // 1/s: crouching, leaning out eases in and out
constexpr float HeadOverHips = 70.f;                 // cm: leaning out tilts the trunk from the hips
constexpr float CantRate     = 10.f;                 // 1/s: the bow canted, straightened (a quick shot)

// a critically damped spring moves v toward goal, smooth seconds (about half the time it takes): it eases
// in and out, no jerks when the goal jumps; vel is its speed
float springTo(float v, float goal, float& vel, float smooth, float dt) {
  const float omega = 2.f/smooth;
  const float x     = omega*dt;
  const float k     = 1.f/(1.f + x + 0.48f*x*x + 0.235f*x*x*x);
  const float off   = v-goal;
  const float push  = (vel + omega*off)*dt;
  vel = (vel - omega*push)*k;
  return goal + (off+push)*k;
  }
}

PlayerControl::PlayerControl() {
  Gothic::inst().onSettingsChanged.bind(this,&PlayerControl::setupSettings);
  setupSettings();
  }

PlayerControl::PlayerControl(DialogMenu& dlg, InventoryMenu& inv)
  :PlayerControl() {
  this->dlg = &dlg;
  this->inv = &inv;
  }

PlayerControl::~PlayerControl() {
  Gothic::inst().onSettingsChanged.ubind(this,&PlayerControl::setupSettings);
  }

void PlayerControl::setupSettings() {
  if(Gothic::inst().version().game==2) {
    g2Ctrl = Gothic::inst().settingsGetI("GAME","USEGOTHIC1CONTROLS")==0;
    } else {
    g2Ctrl = false;
    }
  }

void PlayerControl::setTarget(Npc *other) {
  auto w  = Gothic::inst().world();
  auto pl = w ? w->player() : nullptr;
  if(pl==nullptr || pl->isFinishingMove())
    return;
  const auto ws    = pl->weaponState();
  const bool melle = (ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H);
  if(other==nullptr) {
    if(!(melle && ctrl[Action::ActionGeneric])) {
      // dont lose focus in melee combat
      pl->setTarget(nullptr);
      }
    } else {
    pl->setTarget(other);
    if(ws!=WeaponState::NoWeapon)
      assessThreat(*pl,*other);
    }
  }

void PlayerControl::assessThreat(Npc& pl, Npc& other) {
  // as in the original game: an npc the armed player aims at feels threatened (B_AssessThreat);
  // repeated while the aim is held, not every frame
  constexpr uint64_t ThreatPeriodMs = 2000;
  auto w = Gothic::inst().world();
  if(w==nullptr || other.isDead() || other.isDown())
    return;
  const uint64_t now = w->tickCount();
  if(&other==threatened && now<threatenedAt+ThreatPeriodMs)
    return;
  threatened   = &other;
  threatenedAt = now;
  other.perceptionProcess(pl,nullptr,other.qDistTo(pl),PERC_ASSESSTHREAT);
  }

void PlayerControl::onKeyPressed(KeyCodec::Action a, Tempest::KeyEvent::KeyType key, KeyCodec::Mapping mapping) {
  auto       w    = Gothic::inst().world();
  auto       c    = Gothic::inst().camera();
  auto       pl   = w  ? w->player() : nullptr;
  auto       ws   = pl ? pl->weaponState() : WeaponState::NoWeapon;
  uint8_t    slot = pl ? pl->inventory().currentSpellSlot() : Item::NSLOT;

  if(w!=nullptr && w->isCutsceneLock())
    return;
  if(pl!=nullptr && pl->isMeleeKnockedDown() && (c==nullptr || !c->isFree())) {
    clearInput();
    return;
    }

  handleMovementAction(KeyCodec::ActionMapping{a,mapping}, true);
  if(pl!=nullptr && (a==Action::Forward || a==Action::Back || a==Action::Left || a==Action::Right ||
                    a==Action::RotateL || a==Action::RotateR || a==Action::Jump ||
                    a==Action::ActionGeneric || a==Action::ActionLeft || a==Action::ActionRight ||
                    a==Action::Parade || a==Action::Weapon || a==Action::WeaponMele ||
                    a==Action::WeaponBow || (a>=Action::WeaponMage3 && a<=Action::WeaponMage10)))
    pl->setPlayerPose(false);

  if(pl!=nullptr && !pl->spellFrozen() && pl->interactive()!=nullptr && c!=nullptr && !c->isFree()) {
    auto inter = pl->interactive();
    if(inter->needToLockpick(*pl)) {
      processPickLock(*pl,*inter,a);
      return;
      }
    if(inter->isLadder()) {
      ctrl[a] = true;
      return;
      }
    }

  if(pl!=nullptr) {
    if(a==Action::Weapon) {
      if(ws!=WeaponState::NoWeapon) //Currently a weapon is active
        wctrl[WeaponClose] = true;
      else {
        if(wctrlLast>=WeaponAction::Weapon3 && pl->inventory().currentSpell(static_cast<uint8_t>(wctrlLast-3))==nullptr)
          wctrlLast=WeaponAction::WeaponBow;  //Spell no longer available -> fallback to Bow.
        if(wctrlLast==WeaponAction::WeaponBow && pl->currentRangedWeapon()==nullptr)
          wctrlLast=WeaponAction::WeaponMele; //Bow no longer available -> fallback to Mele.
        wctrl[wctrlLast] = true;
        }
      return;
      }

    if(a==Action::WeaponMele) {
      if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H)
        wctrl[WeaponClose] = true; else
        wctrl[WeaponMele ] = true;
      return;
      }

    if(a==Action::WeaponBow) {
      if(ws==WeaponState::Bow || ws==WeaponState::CBow)
        wctrl[WeaponClose] = true; else
        wctrl[WeaponBow  ] = true;
      return;
      }

    if(a>=Action::WeaponMage3 && a<=Action::WeaponMage10) {
      int id = (a-Action::WeaponMage3+3);
      if(ws==WeaponState::Mage && slot==id)
        wctrl[WeaponClose] = true; else
        wctrl[id         ] = true;
      return;
      }

    if(key==Tempest::KeyEvent::K_Return)
      ctrl[Action::K_ENTER] = true;
    }

  // this odd behaviour is from original game, seem more like a bug
  // const bool actTunneling = (pl!=nullptr && pl->isAttackAnim());
  const bool actTunneling = false;

  int fk = -1;
  if((ctrl[KeyCodec::ActionGeneric] || actTunneling) && !g2Ctrl) {
    if(a==Action::Forward) {
      if(pl!=nullptr && pl->target()!=nullptr && pl->canFinish(*pl->target()) && !pl->isAttackAnim()) {
        fk = ActKill;
        } else {
        fk = ActForward;
        }
      }
    if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) {
      if(a==Action::Back)
        fk = ActBack;
      }
    if(ws!=WeaponState::NoWeapon && !g2Ctrl && !pl->hasState(BS_RUN)) {
      if(a==Action::Left  || a==Action::RotateL)
        fk = ActLeft;
      if(a==Action::Right || a==Action::RotateR)
        fk = ActRight;
      }
    }

  if(g2Ctrl) {
    if(ws!=WeaponState::NoWeapon) {
      if(a==Action::ActionGeneric) {
        if(pl!=nullptr && pl->target()!=nullptr && pl->canFinish(*pl->target()) && !pl->isAttackAnim()) {
          fk = ActKill;
          } else {
          if(this->wantsToMoveForward())
            fk = ActMove; else
            fk = ActForward;
          }
        }
      }
    if(ws==WeaponState::Fist || ws==WeaponState::W1H || ws==WeaponState::W2H) {
      if(a==Action::Parade)
        fk = ActBack;
      }
    if(ws!=WeaponState::NoWeapon && !pl->hasState(BS_RUN)) {
      if(a==Action::ActionLeft)
        fk = ActLeft;
      if(a==Action::ActionRight)
        fk = ActRight;
      }
    }

  if(fk>=0) {
    std::memset(actrl,0,sizeof(actrl));
    actrl[ActGeneric] = ctrl[KeyCodec::ActionGeneric];
    actrl[fk]         = true;

    ctrl[a] = true;
    return;
    }

  if(a==KeyCodec::ActionGeneric) {
    FocusAction fk = ActGeneric;
    if(this->wantsToMoveForward())
      fk = ActMove;
    std::memset(actrl,0,sizeof(actrl));
    actrl[fk] = true;
    ctrl[a]   = true;
    return;
    }

  if(a==Action::Walk) {
    if(manualAiming) {
      // aiming by hand: held, the breath is held
      ctrl[a] = true;
      return;
      }
    toggleWalkMode();
    return;
    }

  if(a==Action::Sneak) {
    if(manualAiming) {
      // aiming by hand: crouch (no talent needed), stand up
      aimCrouch = !aimCrouch;
      return;
      }
    toggleSneakMode();
    return;
    }

  if(a==Action::FirstPerson) {
    if(auto c = Gothic::inst().camera())
      c->setFirstPerson(!c->isFirstPerson());
    return;
    }

  if(a==Action::K_O && Gothic::inst().isMarvinEnabled())
    marvinO();

  ctrl[a] = true;
  }

void PlayerControl::onKeyReleased(KeyCodec::Action a, KeyCodec::Mapping mapping) {
  ctrl[a] = false;

  handleMovementAction(KeyCodec::ActionMapping{a, mapping}, false);

  auto w  = Gothic::inst().world();
  auto pl = w ? w->player() : nullptr;
  if(pl!=nullptr && pl->isMeleeKnockedDown()) {
    clearInput();
    return;
    }

  if(a==KeyCodec::Map && pl!=nullptr) {
    w->script().playerHotKeyScreenMap(*pl);
    }
  if(a==KeyCodec::Heal && pl!=nullptr) {
    w->script().playerHotLameHeal(*pl);
    }
  if(a==KeyCodec::Potion && pl!=nullptr) {
    w->script().playerHotLamePotion(*pl);
    }

  auto ws = pl==nullptr ? WeaponState::NoWeapon : pl->weaponState();
  if(ws==WeaponState::Bow || ws==WeaponState::CBow || ws==WeaponState::Mage) {
    if(a==KeyCodec::ActionGeneric || (!g2Ctrl && ws==WeaponState::Mage && a==KeyCodec::Forward))
      std::memset(actrl,0,sizeof(actrl));
    } else {
    std::memset(actrl,0,sizeof(actrl));
    }
  }

auto PlayerControl::handleMovementAction(KeyCodec::ActionMapping actionMapping, bool pressed) -> void {
  auto[action, mapping] = actionMapping;
  auto mappingIndex = (mapping == KeyCodec::Mapping::Primary ? size_t(0) : size_t(1));
  if (action == Action::Forward)
    movement.forwardBackward.main[mappingIndex] = pressed;
  else if (action == Action::Back)
    movement.forwardBackward.reverse[mappingIndex] = pressed;
  else if (action == Action::Right)
    movement.strafeRightLeft.main[mappingIndex] = pressed;
  else if (action == Action::Left)
    movement.strafeRightLeft.reverse[mappingIndex] = pressed;
  else if (action == Action::RotateR)
    movement.turnRightLeft.main[mappingIndex] = pressed;
  else if (action == Action::RotateL)
    movement.turnRightLeft.reverse[mappingIndex] = pressed;
  }

bool PlayerControl::isPressed(KeyCodec::Action a) const {
  return ctrl[a];
  }

void PlayerControl::onRotateMouse(float dAngleX, float dAngleY) {
  if(dAngleX!=0.f) {
    if(auto* w = Gothic::inst().world())
      if(auto* pl = w->player())
        pl->setPlayerPose(false);
    }
  rotMouse  += dAngleX;
  rotMouseY += dAngleY;
  }

bool PlayerControl::onMouseWheel(int delta) {
  // aiming a bow by hand: down cants it for quick shots, up straightens it; kept till changed (a crossbow
  // is not canted: the wheel does nothing then, the camera stays)
  if(!manualAiming || delta==0)
    return false;
  auto w  = Gothic::inst().world();
  auto pl = w!=nullptr ? w->player() : nullptr;
  if(pl!=nullptr && pl->weaponState()==WeaponState::Bow)
    aimCant = delta<0;
  return true;
  }

void PlayerControl::drawVobRay(DbgPainter& p) const {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return;
  auto pl    = w->player();
  auto focus = findFocus(&currentFocus);
  if(focus.interactive!=nullptr) {
    focus.interactive->drawVobRay(p, *pl);
    }
  if(focus.item!=nullptr) {
    focus.item->drawVobRay(p, *pl);
    }
  if(focus.npc!=nullptr) {
    pl->drawVobRay(p, *focus.npc);
    }
  }

void PlayerControl::tickFocus() {
  auto w = Gothic::inst().world();
  if(auto pl = w!=nullptr ? w->player() : nullptr; pl!=nullptr && pl->isMeleeKnockedDown()) {
    clearFocus();
    return;
    }
  if(manualAiming) {
    // aiming by hand: the focus is under the crosshair
    auto w = Gothic::inst().world();
    if(auto pl = w ? w->player() : nullptr) {
      Tempest::Vec3 at;
      Npc*          npc = aimRayHit(*pl,at);
      currentFocus = (npc!=nullptr && !npc->isDead()) ? Focus(*npc) : Focus();
      setTarget(currentFocus.npc);
      }
    return;
    }
  currentFocus = findFocus(&currentFocus);
  setTarget(currentFocus.npc);

  if(!ctrl[Action::ActionGeneric])
    return;

  auto focus = currentFocus;
  if(focus.interactive!=nullptr && interact(*focus.interactive)) {
    clearInput();
    }
  else if(focus.npc!=nullptr && interact(*focus.npc)) {
    clearInput();
    }
  else if(focus.item!=nullptr && interact(*focus.item)) {
    clearInput();
    }

  if(focus.npc)
    actionFocus(*focus.npc); else
    emptyFocus();
  }

void PlayerControl::clearFocus() {
  currentFocus = Focus();
  }

void PlayerControl::actionFocus(Npc& other) {
  setTarget(&other);
  }

void PlayerControl::emptyFocus() {
  setTarget(nullptr);
  }

Focus PlayerControl::focus() const {
  return currentFocus;
  }

bool PlayerControl::hasActionFocus() const {
  if(!ctrl[Action::ActionGeneric])
    return false;
  return currentFocus.npc!=nullptr;
  }

bool PlayerControl::interact(Interactive &it) {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return false;
  auto pl = w->player();
  if(pl->isDown() || pl->isMeleeKnockedDown())
    return true;
  if(!canInteract())
    return false;
  if(it.isContainer()){
    if(inv==nullptr)
      return false;
    inv->open(*pl,it);
    return true;
    }
  if(pl->setInteraction(&it)){
    }
  return true;
  }

bool PlayerControl::interact(Npc &other) {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return false;
  auto pl = w->player();
  if(pl->isDown() || pl->isMeleeKnockedDown())
    return true;
  if(!canInteract())
    return false;
  if(other.isMeleeKnockedDown())
    return true;
  auto state = pl->bodyStateMasked();
  if(other.isDown()) {
    if(state!=BS_STAND && state!=BS_SNEAK && state!=BS_SWIM && state!=BS_DIVE)
      return false;
    if(inv==nullptr)
      return false;
    if(!inv->ransack(*w->player(),other))
      w->script().printNothingToGet();
    } else {
    if((state&BS_MAX)!=BS_NONE)
      return false;
    other.startDialog(*pl);
    }
  return true;
  }

bool PlayerControl::interact(Item &item) {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return false;
  auto pl = w->player();
  if(item.isTorchBurn() && pl->isUsingTorch())
    return false;
  if(pl->isDown() || pl->isMeleeKnockedDown())
    return true;
  if(!canInteract())
    return false;
  return pl->takeItem(item)!=nullptr;
  }

void PlayerControl::moveFocus(FocusAction act) {
  auto w = Gothic::inst().world();
  auto c = Gothic::inst().camera();
  if(w==nullptr || c==nullptr || currentFocus.npc==nullptr)
    return;

  auto vp  = c->viewProj();
  auto pos = currentFocus.npc->centerPosition();
  vp.project(pos);

  Npc* next = nullptr;
  auto npos = Tempest::Vec3();
  for(uint32_t i=0; i<w->npcCount(); ++i) {
    auto npc = w->npcById(i);
    if(npc->isPlayer())
      continue;
    auto p = npc->centerPosition();
    vp.project(p);

    if(std::abs(p.x)>1.f || std::abs(p.y)>1.f || p.z<0.f)
      continue;

    if(!w->testFocusNpc(npc))
      continue;

    if(act==ActLeft && p.x<pos.x && (next==nullptr || npos.x<p.x)) {
      npos = p;
      next = npc;
      }
    if(act==ActRight && p.x>pos.x && (next==nullptr || npos.x>p.x)) {
      npos = p;
      next = npc;
      }
    }

  if(next==nullptr)
    return;
  currentFocus.npc = next;
  }

void PlayerControl::toggleWalkMode() {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return;
  auto pl = w->player();
  pl->setWalkMode(pl->walkMode()^WalkBit::WM_Walk);
  }

void PlayerControl::toggleSneakMode() {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return;
  auto pl = w->player();
  if(!pl->canSneak() || (pl->walkMode()&WalkBit::WM_Sneak)==WalkBit::WM_Sneak) {
    pl->setWalkMode(pl->walkMode() & (~WalkBit::WM_Sneak));
    } else {
    pl->setWalkMode(pl->walkMode()^WalkBit::WM_Sneak);
    }
  }

bool PlayerControl::canInteract() const {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return false;
  auto pl = w->player();
  if(pl->spellFrozen() || pl->isMeleeKnockedDown() || pl->weaponState()!=WeaponState::NoWeapon || pl->isAiBusy())
    return false;
  return true;
  }

void PlayerControl::clearInput() {
  movement.reset();
  std::memset(ctrl, 0,sizeof(ctrl));
  std::memset(actrl,0,sizeof(actrl));
  std::memset(wctrl,0,sizeof(wctrl));
  }

void PlayerControl::marvinF8(uint64_t dt) {
  auto w = Gothic::inst().world();
  if(w==nullptr || w->player()==nullptr)
    return;

  auto& pl  = *w->player();
  auto  pos = pl.position();
  float rot = pl.rotationRad();
  float s   = std::sin(rot), c = std::cos(rot);

  Tempest::Vec3 dp(c,0.8f,s);
  pos += dp*6000*float(dt)/1000.f;

  pl.changeAttribute(ATR_HITPOINTS,pl.attribute(ATR_HITPOINTSMAX),false);
  pl.changeAttribute(ATR_MANA,     pl.attribute(ATR_MANAMAX),     false);
  pl.clearState(false);
  pl.clearSpeed();
  pl.clearAiQueue();
  pl.setPosition(pos);
  pl.setInteraction(nullptr,true);
  pl.setAnim(AnimationSolver::Idle);

  if(auto c = Gothic::inst().camera())
    c->reset();
  }

void PlayerControl::marvinK(uint64_t dt) {
  auto w = Gothic::inst().world();
  if (w == nullptr || w->player() == nullptr)
    return;

  auto& pl = *w->player();
  auto  pos = pl.position();
  float rot = pl.rotationRad();
  float s = std::sin(rot), c = std::cos(rot);

  Tempest::Vec3 dp(c, 0.0f, s);
  pos += dp * 6000 * float(dt) / 1000.f;

  pl.clearState(false);
  pl.clearSpeed();
  pl.setPosition(pos);
  pl.setInteraction(nullptr,true);
  // pl.setAnim(AnimationSolver::Idle); // Original G2 behaviour: K doesn't stop running
  }

void PlayerControl::marvinO() {
  auto w = Gothic::inst().world();
  if (w == nullptr || w->player() == nullptr || w->player()->target() == nullptr)
    return;

  auto target = w->player()->target();

  w->setPlayer(target);
  }

Focus PlayerControl::findFocus(const Focus* prev) const {
  auto w = Gothic::inst().world();
  auto c = Gothic::inst().camera();
  if(w==nullptr)
    return Focus();
  if(w->player()!=nullptr && (w->player()->isDown() || w->player()->isMeleeKnockedDown()))
    return Focus();
  if(c!=nullptr && c->isCutscene())
    return Focus();
  if(!cacheFocus)
    prev = nullptr;

  if(prev)
    return w->findFocus(*prev);
  return w->findFocus(Focus());
  }

bool PlayerControl::tickCameraMove(uint64_t dt) {
  auto w = Gothic::inst().world();
  if(w==nullptr)
    return false;

  Npc*  pl     = w->player();
  auto  camera = Gothic::inst().camera();
  if(camera==nullptr || (pl!=nullptr && !camera->isFree()))
    return false;

  rotMouse = 0;
  if(ctrl[KeyCodec::Left] || (ctrl[KeyCodec::RotateL] && ctrl[KeyCodec::Jump])) {
    camera->moveLeft(dt);
    return true;
    }
  if(ctrl[KeyCodec::Right] || (ctrl[KeyCodec::RotateR] && ctrl[KeyCodec::Jump])) {
    camera->moveRight(dt);
    return true;
    }

  auto turningVal = movement.turnRightLeft.value();
  if(turningVal > 0.f)
    camera->rotateRight(dt);
  else if(turningVal < 0.f)
    camera->rotateLeft(dt);

  auto forwardVal = movement.forwardBackward.value();
  if(forwardVal > 0.f)
    camera->moveForward(dt);
  else if(forwardVal < 0.f)
    camera->moveBack(dt);
  return true;
  }

bool PlayerControl::tickMove(uint64_t dt) {
  auto w = Gothic::inst().world();
  if(w==nullptr)
    return false;
  const float dtF = float(dt)/1000.f;

  Npc*  pl     = w->player();
  auto  camera = Gothic::inst().camera();

  if(w->isCutsceneLock())
    clearInput();

  if(tickCameraMove(dt))
    return true;

  if(ctrl[Action::K_F8] && Gothic::inst().isMarvinEnabled())
    marvinF8(dt);
  if(ctrl[Action::K_K] && Gothic::inst().isMarvinEnabled())
    marvinK(dt);
  cacheFocus = ctrl[Action::ActionGeneric];
  if(camera!=nullptr)
    camera->setLookBack(ctrl[Action::LookBack]);

  if(pl==nullptr)
    return true;

  if(pl->isMeleeKnockedDown()) {
    clearInput();
    rotMouse = rotMouseY = 0;
    runAngleDest = 0;
    if(manualAiming || pl->hasManualAim() || pl->archer().isRaised())
      endManualAim(*pl);
    manualAiming = false;
    return true;
    }

  if(pl->spellFrozen()) {
    // Input must not move an attached actor or change root lean outside Npc::tick.
    // Explicit Marvin teleports above already leave the scripted state first.
    clearInput();
    rotMouse = rotMouseY = 0;
    runAngleDest = 0;
    return true;
    }

  if(movement.forwardBackward.any() || movement.strafeRightLeft.any() || movement.turnRightLeft.any() ||
     ctrl[Action::ActionGeneric] || ctrl[Action::Jump])
    pl->setPlayerPose(false);
  if(pl->tickPlayerPose()) {
    runAngleDest = 0;
    return true;
    }

  // implMove says if the bow is raised by hand
  const bool wasAiming = manualAiming;
  manualAiming = false;
  implMove(dt);
  if(!manualAiming && (wasAiming || pl->hasManualAim() || pl->archer().isRaised()))
    endManualAim(*pl);

  float runAngle = pl->runAngle();
  if(runAngle!=0.f || std::fabs(runAngleDest)>0.01f) {
    const float speed = 35.f;
    if(runAngle<runAngleDest) {
      runAngle+=speed*dtF;
      if(runAngle>runAngleDest)
        runAngle = runAngleDest;
      pl->setRunAngle(runAngle);
      }
    else if(runAngle>runAngleDest) {
      runAngle-=speed*dtF;
      if(runAngle<runAngleDest)
        runAngle = runAngleDest;
      pl->setRunAngle(runAngle);
      }
    }

  rotMouseY = 0;
  return true;
  }

void PlayerControl::implMove(uint64_t dt) {
  auto  w         = Gothic::inst().world();
  Npc&  pl        = *w->player();
  float rot       = pl.rotation();
  float rotY      = pl.rotationY();
  // 100 / 200 according to some sources, yet my mesures are 90/180
  float rspeed    = (pl.weaponState()==WeaponState::NoWeapon ? 90.f : 180.f)*(float(dt)/1000.f);
  auto  ws        = pl.weaponState();
  auto  bs        = pl.bodyStateMasked();
  bool  allowRot  = !ctrl[KeyCodec::ActionGeneric] && pl.isRotationAllowed();

  Npc::Anim ani = Npc::Anim::Idle;

  if(bs==BS_DEAD)
    return;
  if(bs==BS_UNCONSCIOUS)
    return;

  if(!pl.isAiQueueEmpty()) {
    runAngleDest = 0;
    return;
    }

  if(pl.interactive()!=nullptr) {
    runAngleDest = 0;
    implMoveMobsi(pl,dt);
    return;
    }

  if(pl.canSwitchWeapon()) {
    if(wctrl[WeaponClose]) {
      wctrl[WeaponClose] = !(pl.closeWeapon(false) || pl.isMonster());
      return;
      }
    if(wctrl[WeaponMele]) {
      bool ret=false;
      if(pl.currentMeleeWeapon()!=nullptr)
        ret = pl.drawWeaponMelee(); else
        ret = pl.drawWeaponFist();
      wctrl[WeaponMele] = !ret;
      wctrlLast         = WeaponMele;
      if(!wctrl[WeaponMele])
        return;
      }
    if(wctrl[WeaponBow]) {
      if(pl.currentRangedWeapon()!=nullptr) {
        wctrl[WeaponBow] = !pl.drawWeaponBow();
        wctrlLast        = WeaponBow;
        } else {
        wctrl[WeaponBow] = false;
        }
      if(!wctrl[WeaponBow])
        return;
      }
    for(uint8_t i=0;i<8;++i) {
      if(wctrl[Weapon3+i]){
        if(pl.inventory().currentSpell(i)!=nullptr){
          bool ret = pl.drawMage(uint8_t(3+i));
          wctrl[Weapon3+i] = !ret;
          wctrlLast = static_cast<WeaponAction>(Weapon3+i);
          if(ret) {
            if(auto spl = pl.inventory().currentSpell(i)) {
              Gothic::inst().onPrint(spl->description());
              }
            }
          } else {
          wctrl[Weapon3+i] = false;
          return;
          }
        }
      }
    }

  if(!pl.isInState(ScriptFn()) || (dlg!=nullptr && dlg->isActive())) {
    runAngleDest = 0;
    return;
    }

  if(implManualAim(pl,dt))
    return;

  int rotation = 0;
  if(allowRot) {
    if(this->wantsToTurnLeft()) {
      rot += rspeed;
      rotation = -1;
      rotMouse=0;
      }
    if(this->wantsToTurnRight()) {
      rot -= rspeed;
      rotation = 1;
      rotMouse=0;
      }
    if(std::fabs(rotMouse)>0.f) {
      if(rotMouse>0)
        rotation = -1; else
        rotation = 1;
      rot += rotMouse;
      rotMouse  = 0;
      }
    rotY+=rotMouseY;
    } else {
    rotMouse  = 0;
    rotMouseY = 0;
    }

  pl.setDirectionY(rotY);
  if(pl.isFalling() || pl.isSlide() || pl.isInAir() || pl.isJump() || pl.isJumpUp()){
    pl.setDirection(rot);
    runAngleDest = 0;
    return;
    }

  if(casting) {
    if(!actrl[ActForward] || (Gothic::inst().version().game==1 && pl.attribute(ATR_MANA)==0)) {
      casting = false;
      pl.endCastSpell(true);
      }
    return;
    }

  if(ctrl[Action::K_ENTER]) {
    pl.transformBack();
    ctrl[Action::K_ENTER] = false;
    }

  if((ws==WeaponState::Bow || ws==WeaponState::CBow) && pl.hasAmmunition()) {
    if(actrl[ActGeneric] || actrl[ActForward]) {
      if(auto other = pl.target()) {
        auto dp = other->position()-pl.position();
        pl.turnTo(dp.x,dp.z,true,dt);
        pl.aimBow();
        }
      else if(currentFocus.interactive!=nullptr) {
        auto dp = currentFocus.interactive->position()-pl.position();
        pl.turnTo(dp.x,dp.z,false,dt);
        pl.aimBow();
        }
      else {
        pl.aimBow();
        }

      if(actrl[ActLeft]) {
        moveFocus(ActLeft);
        actrl[ActLeft]  = false;
        }
      if(actrl[ActRight]) {
        moveFocus(ActRight);
        actrl[ActRight]  = false;
        }
      if(!actrl[ActForward])
        return;
      }
    }

  if(ws==WeaponState::Mage) {
    if(actrl[ActGeneric] || actrl[ActForward] || ctrl[KeyCodec::ActionGeneric]) {
      if(auto other = pl.target()) {
        auto dp = other->centerPosition() - pl.centerPosition();
        pl.turnTo(dp.x,dp.z,true,dt);
        } else
      if(currentFocus.interactive!=nullptr) {
        auto dp = currentFocus.interactive->position()-pl.position();
        pl.turnTo(dp.x,dp.z,false,dt);
        }

      if(actrl[ActLeft]) {
        moveFocus(ActLeft);
        actrl[ActLeft]  = false;
        }
      if(actrl[ActRight]) {
        moveFocus(ActRight);
        actrl[ActRight]  = false;
        }
      if(!actrl[ActForward]) {
        pl.setAnim(Npc::Anim::Idle);
        return;
        }
      }
    }

  if(actrl[ActForward] || actrl[ActMove]) {
    ctrl [Action::Forward] = actrl[ActMove];
    actrl[ActMove]         = false;
    if(ws!=WeaponState::Mage && !(g2Ctrl && (ws==WeaponState::Bow || ws==WeaponState::CBow))) {
      actrl[ActForward] = false;
      if(!ctrl[Action::Forward])
        movement.reset();
      }
    switch(ws) {
      case WeaponState::NoWeapon:
        break;
      case WeaponState::Fist:
        pl.fistShoot();
        return;
      case WeaponState::W1H:
      case WeaponState::W2H: {
        pl.swingSword();
        return;
        }
      case WeaponState::Bow:
      case WeaponState::CBow: {
        pl.shootBow(currentFocus.interactive);
        return;
        }
      case WeaponState::Mage: {
        casting = (pl.beginCastSpell()==Npc::BC_Invest);
        if(!casting)
          actrl[ActForward] = false;
        return;
        }
      }
    }

  if(actrl[ActKill]) {
    if((ws==WeaponState::W1H || ws==WeaponState::W2H) && pl.target()!=nullptr && pl.canFinish(*pl.target()))
      pl.finishingMove();
    actrl[ActKill] = false;
    }

  if(actrl[ActLeft] || actrl[ActRight] || actrl[ActBack]) {
    auto ws = pl.weaponState();
    if(ws==WeaponState::Fist) {
      if(actrl[ActBack])
        pl.blockFist();
      return;
      }
    else if(ws==WeaponState::W1H || ws==WeaponState::W2H) {
      if(actrl[ActLeft] && pl.swingSwordL()) {
        movement.strafeRightLeft.reset();
        }
      else if(actrl[ActRight] && pl.swingSwordR()) {
        movement.strafeRightLeft.reset();
        }
      else if(actrl[ActBack] && pl.blockSword()) {
        // movement.forwardBackward.reset();
        }

      actrl[ActLeft]  = false;
      actrl[ActRight] = false;
      // actrl[ActBack]  = false;
      return;
      }
    else if(ws==WeaponState::Mage) {
      if(actrl[ActLeft]) {
        moveFocus(ActLeft);
        actrl[ActLeft]  = false;
        }
      if(actrl[ActRight]) {
        moveFocus(ActRight);
        actrl[ActRight]  = false;
        }
      }
    }

  if(this->wantsToStrafeLeft()) {
    ani = Npc::Anim::MoveL;
    }
  else if(this->wantsToStrafeRight()) {
    ani = Npc::Anim::MoveR;
    }
  else if(this->wantsToMoveForward()) {
    if((pl.walkMode()&WalkBit::WM_Dive)!=WalkBit::WM_Dive) {
      ani = Npc::Anim::Move;
      }
    else if(pl.isDive()) {
      pl.setDirectionY(rotY - rspeed);
      return;
      }
    }
  else if(this->wantsToMoveBackward()) {
    if((pl.walkMode()&WalkBit::WM_Dive)!=WalkBit::WM_Dive) {
      ani = Npc::Anim::MoveBack;
      } else if(pl.isDive()) {
      pl.setDirectionY(rotY + rspeed);
      return;
      }
    }


  if(ctrl[Action::Jump]) {
    if(pl.bodyStateMasked()==BS_JUMP) {
      ani = Npc::Anim::Idle;
      }
    else if(pl.isDive()) {
      ani = Npc::Anim::Move;
      }
    else if(pl.isSwim()) {
      pl.startDive();
      }
    else if(pl.isInWater()) {
      auto& g  = w->script().guildVal();
      auto  gl = pl.guild();

      if(0<=gl && gl<GIL_MAX && pl.isStanding()) {
        MoveAlgo::JumpStatus jump;
        jump.anim   = Npc::Anim::JumpUp;
        jump.height = float(g.jumpup_height[gl])+pl.position().y;
        pl.startClimb(jump);
        }
      }
    else if(pl.isStanding()) {
      auto jump = pl.tryJump();
      if(!pl.isFalling() && !pl.isSlide() && jump.anim!=Npc::Anim::Jump){
        pl.startClimb(jump);
        return;
        }
      ani = Npc::Anim::Jump;
      }
    else if(!pl.isAttackAnim() && !pl.isCasting()) {
      ani = Npc::Anim::Jump;
      }
    }

  if(!pl.isCasting()) {
    if(ani==Npc::Anim::Jump) {
      pl.setAnimRotate(0);
      rotation = 0;
      }

    if(pl.isAttackAnim()) {
      if((ani==Npc::Anim::MoveL || ani==Npc::Anim::MoveR/* || ani==Npc::Anim::MoveBack*/) && pl.hasState(BS_RUN)) {
        ani = Npc::Anim::Idle;
        }

      if(!pl.hasState(BS_RUN) && ani==Npc::Anim::Idle) {
        // charge-run
        ani = Npc::Anim::NoAnim;
        }
      if((ani==Npc::Anim::MoveL || ani==Npc::Anim::MoveR) &&
          pl.hasState(BS_STAND) && pl.hasState(BS_HIT)) {
        // no charge to strafe transition
        ani = Npc::Anim::NoAnim;
        }
      }

    if(bs==BS_LIE) {
      ani = (ani==Npc::Anim::Move) ? Npc::Anim::Idle : Npc::Anim::NoAnim;
      rot = pl.rotation();
      }

    if(ani!=Npc::Anim::NoAnim)
      pl.setAnim(ani);
    }

  setAnimRotate(pl, rot, ani==Npc::Anim::Idle ? rotation : 0, movement.turnRightLeft.any(), dt);
  if(actrl[ActGeneric] || ani==Npc::Anim::MoveL || ani==Npc::Anim::MoveR || pl.isFinishingMove()) {
    processAutoRotate(pl,rot,dt);
    }

  if(ani==Npc::Anim::Move && (rotation!=0 || rotY!=0)) {
    assignRunAngle(pl,rot,dt);
    } else {
    assignRunAngle(pl,pl.rotation(),dt);
    }
  pl.setDirection(rot);
  }

bool PlayerControl::crosshair(Tempest::Vec3& at, float& spread) const {
  if(!manualAiming)
    return false;
  at     = aimAt;
  spread = aimSpread;
  return true;
  }

void PlayerControl::setAimOverride(bool on, const Tempest::Vec3& at) {
  aimOverride   = on;
  aimOverrideAt = at;
  }

Npc* PlayerControl::aimRayHit(Npc& pl, Tempest::Vec3& at) const {
  // what the center of the screen falls on (the bones of an npc, the ground); the test bench aims at a
  // point instead
  static float reach = 10000.f; // cm
  auto w = Gothic::inst().world();
  auto c = Gothic::inst().camera();
  Tempest::Vec3 from, dir;
  if(aimOverride || c==nullptr) {
    from = pl.mapHeadBone();
    dir  = Tempest::Vec3::normalize(aimOverrideAt-from);
    } else {
    c->aimRay(from,dir);
    }
  const Tempest::Vec3 to = from + dir*reach;
  auto r = w->physic()->rayArrow(from,to,[&pl](Npc& n, const Tempest::Vec3& a, const Tempest::Vec3& b, float& t) {
    return &n!=&pl && n.rayHitZone(a,b,t)!=HitZones::None;
    });
  at = aimOverride ? aimOverrideAt : (r.hasCol ? r.v : to);
  return r.npcHit;
  }

bool PlayerControl::implManualAim(Npc& pl, uint64_t dt) {
  // the bow aimed by hand (game/archery): the left button raises it and draws the string, letting it go
  // shoots along the arrow; the right one lowers the bow, a jump too; shift holds the breath; forward and
  // back step with the bow drawn (slowly, sneaking); the sneak key crouches, the side steps lean out round
  // a corner, the wheel cants the bow (onMouseWheel)
  const auto ws = pl.weaponState();
  if(!Archery::isEnabled() || (ws!=WeaponState::Bow && ws!=WeaponState::CBow) || pl.isDown() ||
     pl.isInAir() || pl.isSwim() || pl.isDive() || pl.interactive()!=nullptr)
    return false;
  // the auto aim does not run: no shots on 'forward'
  std::memset(actrl,0,sizeof(actrl));

  auto&      ar     = pl.archer();
  const bool lmb    = ctrl[KeyCodec::ActionGeneric];
  const bool moving = ctrl[Action::Jump];
  bool raised = ar.isRaised();
  if(!raised && lmb && !moving && pl.hasAmmunition())
    raised = true;
  if(raised && (ctrl[Action::Parade] || moving))
    raised = false;
  if(raised && !pl.hasAmmunition() && ar.stage()!=Archer::Reloading)
    raised = false;
  if(!raised)
    return false;

  auto camera = Gothic::inst().camera();
  if(!ar.isRaised() && camera!=nullptr) {
    // the aim starts at the focus, else straight ahead: the center of the screen passes over the
    // shoulder, the hero turns so that it falls on the focus
    float pitch = 0;
    if(auto t = pl.target()) {
      const Tempest::Vec3 tg = t->collosionCenter();
      Tempest::Vec3       d  = tg - pl.position();
      for(int i=0; i<3; ++i) {
        const float a = std::atan2(d.z,d.x);
        d = tg - Camera::aimShoulder(pl.position(),Tempest::Vec3(std::cos(a),0,std::sin(a)));
        }
      pl.setDirection(std::atan2(d.z,d.x)*180.f/float(M_PI));
      pitch = -std::atan2(d.y,std::sqrt(d.x*d.x+d.z*d.z))*180.f/float(M_PI);
      }
    camera->setAim(true,0);
    camera->setAimPitch(pitch);
    }
  if(!ar.isRaised()) {
    aimSpread    = MaxSpread;
    aimSpreadVel = 0;
    // sneaking, it is raised crouched
    aimCrouch    = bool(pl.walkMode() & WalkBit::WM_Sneak);
    }

  // the stance: crouched, leaning out round a corner (held), the bow canted (not a crossbow), eased
  const float dtS  = float(dt)/1000.f;
  const float peek = (wantsToStrafeRight() ? 1.f : 0.f) - (wantsToStrafeLeft() ? 1.f : 0.f);
  const float cant = aimCant && ws==WeaponState::Bow ? 1.f : 0.f;
  float       step = (wantsToMoveForward() ? 1.f : 0.f) - (wantsToMoveBackward() ? 1.f : 0.f);
  const float ease = 1.f - std::exp(-StanceRate*dtS);
  crouchK += ((aimCrouch ? 1.f : 0.f) - crouchK)*ease;
  peekK   += (peek - peekK)*ease;
  cantK   += (cant - cantK)*(1.f - std::exp(-CantRate*dtS));
  if(std::abs(crouchK-(aimCrouch ? 1.f : 0.f))<1e-3f)
    crouchK = aimCrouch ? 1.f : 0.f;
  if(std::abs(peekK-peek)<1e-3f)
    peekK = peek;
  if(std::abs(cantK-cant)<1e-3f)
    cantK = cant;
  pl.setAimStance(crouchK,peekK,cantK,step);
  manualAiming = true;

  // the mouse (the keys) turn the hero, the camera follows; up and down is the camera's
  const float rspeed = 180.f*(float(dt)/1000.f);
  float       rot    = pl.rotation() + rotMouse;
  if(wantsToTurnLeft())
    rot += rspeed;
  if(wantsToTurnRight())
    rot -= rspeed;
  rotMouse  = 0;
  rotMouseY = 0;
  if(aimOverride) {
    // the test bench aims: the hero faces it
    const Tempest::Vec3 d = aimOverrideAt - pl.position();
    if(d.x!=0.f || d.z!=0.f)
      rot = std::atan2(d.z,d.x)*180.f/float(M_PI);
    }
  pl.setDirection(rot);
  pl.aimBow();

  Tempest::Vec3 at;
  aimRayHit(pl,at);
  // Lean and approach toward the crosshair from the hero, not parallel to the camera:
  // its forward offset must not make the stance aim more steeply at the same target.
  const auto look = pl.aimLookAt(at);
  pl.setAimLook(look);

  AimStep::Input approach;
  approach.drawn = lmb && ar.stage()==Archer::Held && Aim::isEnabled() && AimWalk::isEnabled() &&
                   Aim::playerLeanOverride<0.f;
  approach.moving = movement.forwardBackward.any() || movement.strafeRightLeft.any();
  approach.down = -std::atan2(look.y,std::sqrt(look.x*look.x+look.z*look.z))*180.f/float(M_PI);
  approach.yaw = rot;
  if(approach.drawn && approach.down>40.f) {
    const float a = pl.rotationRad();
    const auto edge = pl.aimEdge({std::cos(a),0,std::sin(a)},AimStep::SearchRange);
    approach.edge = edge.found;
    approach.distance = edge.distance;
    approach.blocked = pl.isEdgeMoveBlocked({0,0,1});
    const auto clearance = pl.aimLean(at,true);
    approach.shotClear = clearance.edge && clearance.shotClear;
    }
  const bool wasStepping = aimStep.isActive();
  if(aimStep.update(approach)) {
    step = 1.f;
    aimStepStopped = false;
    }
  else if(wasStepping) {
    // Cancel the automatic root motion immediately; the planted pose still fades into the stance.
    aimStepStopped = true;
    }
  if(step!=0.f && !aimStep.isActive())
    aimStepStopped = false;
  // The same gait and speed as forward input; its disturbance also shakes the drawn aim.
  walkK += (std::abs(step) - walkK)*ease;
  if(std::abs(walkK-std::abs(step))<1e-3f)
    walkK = std::abs(step);
  pl.setAimStance(crouchK,peekK,cantK,step,aimStepStopped);

  // canted: a quick shot (drawn fast, settles fast, looser)
  const auto    st = Archery::canted(Archery::stats(pl,pl.currentRangedWeapon()),cantK);
  Archer::Input in;
  in.draw   = lmb;
  in.breath = ctrl[Action::Walk];
  in.crouch = crouchK;
  in.peek   = peekK;
  in.walk   = walkK;
  in.dt     = dt;
  pl.tickManualAim(at,in,st);
  ar.probe(Archer::playerProbe);

  // the crosshair is where it aims; it opens as far as the arrow may go off: the sway, the trembling,
  // the arrow not turned to it yet (a flick of the mouse), the bow being raised, the next arrow nocked.
  // A spring, critically damped: it eases into opening and out of it, no jerks; opens faster than closes
  const float err  = pl.aimTrackError();
  float       goal = std::min(ar.spread() + std::max(err,0.f)*float(M_PI)/180.f, MaxSpread);
  if(err<0.f || ar.stage()==Archer::Raising || ar.stage()==Archer::Reloading)
    goal = MaxSpread;
  aimSpread = springTo(aimSpread,goal,aimSpreadVel,goal>aimSpread ? SpreadOpen : SpreadClose,float(dt)/1000.f);
  aimSpread = std::max(aimSpread,0.f);
  aimAt     = at;
  if(camera!=nullptr) {
    camera->setAim(true,ar.draw());
    // the shoulder goes down crouched, aside leaning out (as far as the body does: a wall stops it); over
    // the left one leaning out to the left
    const float side = std::sin(Aim::playerProbe.side*float(M_PI)/180.f)*HeadOverHips;
    camera->setAimStance(crouchK*FootIk::aimCrouchDepth(),side,1.f-2.f*std::max(0.f,-peekK));
    // over an edge ahead and down as the head goes (as far as the trunk bends: eased, no room no lean)
    const auto shift = pl.aimLeanShift();
    const float facing = pl.rotationRad();
    camera->setAimLean(shift.x*std::cos(facing)+shift.z*std::sin(facing),-shift.y);
    }
  return true;
  }

void PlayerControl::endManualAim(Npc& pl) {
  const bool stopStep = aimStep.isActive() || aimStepStopped;
  pl.endManualAim(true);
  if(stopStep)
    pl.setAimStance(0,0,0,0,true);
  aimCrouch = false;
  crouchK   = 0;
  peekK     = 0;
  cantK     = 0;
  walkK     = 0;
  aimStep   = AimStep();
  aimStepStopped = false;
  Archer::playerProbe = Archer::Probe();
  ctrl[Action::Walk] = false;
  if(auto camera = Gothic::inst().camera()) {
    camera->setAim(false,0);
    camera->setAimStance(0,0,1);
    }
  }

void PlayerControl::implMoveMobsi(Npc& pl, uint64_t /*dt*/) {
  // animation handled in MOBSI
  auto inter = pl.interactive();

  if(ctrl[KeyCodec::Back] && !inter->isLadder()) {
    pl.setInteraction(nullptr);
    return;
    }

  if(inter->needToLockpick(pl) && !inter->isCracked()) {
    return;
    }

  if(!inter->isLadder() && inter->isStaticState() && !inter->isDetachState(pl)) {
    auto stateId = inter->stateId();
    if(inter->canQuitAtState(pl,stateId))
      pl.setInteraction(nullptr,false);
    }

  if(inter->isLadder()) {
    if(ctrl[KeyCodec::ActionGeneric]) {
      inter->onKeyInput(KeyCodec::ActionGeneric);
      ctrl[KeyCodec::ActionGeneric] = false;
      }
    else if(ctrl[KeyCodec::Forward]) {
      inter->onKeyInput(KeyCodec::Forward);
      }
    else if(ctrl[KeyCodec::Back]) {
      inter->onKeyInput(KeyCodec::Back);
      }
    }
  }

void PlayerControl::processPickLock(Npc& pl, Interactive& inter, KeyCodec::Action k) {
  auto                   w             = Gothic::inst().world();
  auto&                  script        = w->script();
  const size_t           ItKE_lockpick = script.lockPickId();

  char ch = '\0';
  if(k==KeyCodec::Left || k==KeyCodec::RotateL)
    ch = 'L';
  else if(k==KeyCodec::Right || k==KeyCodec::RotateR)
    ch = 'R';
  else if(k==KeyCodec::Back) {
    quitPicklock(pl);
    return;
    }
  else
    return;

  auto cmp = inter.pickLockCode();
  while(pickLockProgress<cmp.size()) {
    auto c = cmp[pickLockProgress];
    if(c=='l' || c=='L' || c=='r' || c=='R')
      break;
    ++pickLockProgress;
    }

  if(pickLockProgress<cmp.size() && std::toupper(cmp[pickLockProgress])!=ch) {
    pickLockProgress = 0;
    const int32_t dex = Gothic::inst().version().game==2 ? pl.attribute(ATR_DEXTERITY) : (100 - pl.talentValue(TALENT_PICKLOCK));
    if(dex<=int32_t(script.rand(100)))  {
      script.invokePickLock(pl,0,1);
      pl.delItem(ItKE_lockpick,1);
      if(pl.inventory().itemCount(ItKE_lockpick)==0) {
        quitPicklock(pl);
        return;
        }
      } else {
      script.invokePickLock(pl,0,0);
      }
    } else {
    pickLockProgress++;
    if(pickLockProgress>=cmp.size()) {
      script.invokePickLock(pl,1,1);
      inter.setAsCracked(true);
      pickLockProgress = 0;
      } else {
      script.invokePickLock(pl,1,0);
      }
    }
  }

void PlayerControl::processLadder(Npc& pl, Interactive& inter, KeyCodec::Action key) {
  if(key!=KeyCodec::ActionGeneric && key!=KeyCodec::Forward && key!=KeyCodec::Back)
    return;

  ctrl[key] = true;
  inter.onKeyInput(key);
  }

void PlayerControl::quitPicklock(Npc& pl) {
  if(inv!=nullptr)
    inv->close();
  pickLockProgress = 0;
  pl.setInteraction(nullptr);
  }

void PlayerControl::assignRunAngle(Npc& pl, float rotation, uint64_t dt) {
  float dtF    = (float(dt)/1000.f);
  auto  camera = Gothic::inst().camera();

  float dest = 0;
  // the lean post-pass (graphics/mesh/lean) leans into a turn from the turn itself, for npcs too
  if(camera!=nullptr && pl.walkMode()==WalkBit::WM_Run && pl.bodyState()==BS_RUN && !Lean::isEnabled()) {
    const float az   = camera->azimuth();
    const float maxV = 14.5f;
    dest = std::min(std::abs(az), maxV)*(az>=0 ? 1 : -1);
    }

  float a = std::min(dtF*5.f, 1.f);
  runAngleDest = runAngleDest*(1.f-a)+dest*a;
  }

void PlayerControl::setAnimRotate(Npc& pl, float rotation, int anim, bool force, uint64_t dt) {
  float dtF    = (float(dt)/1000.f);
  float angle  = pl.rotation();
  float dangle = (rotation-angle)/dtF;
  auto& wrld   = pl.world();

  if(std::fabs(dangle)<30.f && !force) // 30 deg per second threshold
    anim = 0;
  if(anim!=0 && pl.isAttackAnim())
    anim = 0;
  if(rotationAni==anim && anim!=0)
    force = true;
  if(!force && wrld.tickCount()<turnAniSmooth)
    return;
  turnAniSmooth = wrld.tickCount() + 100;
  rotationAni   = anim;
  pl.setAnimRotate(anim);
  }

void PlayerControl::processAutoRotate(Npc& pl, float& rot, uint64_t dt) {
  if(auto other = pl.target()) {
    if(pl.weaponState()==WeaponState::NoWeapon || pl.isFinishingMove()){
      pl.setTarget(nullptr);
      }
    else if(!pl.isAttack()) {
      auto  dp   = other->centerPosition() - pl.centerPosition();
      auto  gl   = pl.guild();
      float step = float(pl.world().script().guildVal().turn_speed[gl]);
      if(actrl[ActGeneric])
        step*=2.f;
      pl.rotateTo(dp.x,dp.z,step,AnimationSolver::TurnType::Std,dt);
      rot = pl.rotation();
      }
    }
  }
