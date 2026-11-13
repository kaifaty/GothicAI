#include "mdlvisual.h"
#include "mdlvisual_internal.h"

#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/protomesh.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

using namespace Tempest;
using namespace MdlVisualInternal;

bool MdlVisual::updateGaze(Npc& npc, uint64_t dt) {
  Pose& pose = *skInst;
  auto& gaze = pose.gazeIk();
  if(!gaze.isValid()) {
    lookSource = Gaze::Source::None;
    if(&npc==Gaze::probeNpc)
      Gaze::subjectProbe = Gaze::Probe();
    return false;
    }

  const float a = npc.rotationRad();
  Gaze::Input in;
  in.dt      = dt;
  in.forward = {std::cos(a), 0, std::sin(a)};
  in.talk    = {gestures.head().pitch, gestures.head().yaw, gestures.head().roll};
  // its hands gesture as it speaks: the head is not the gesture's (updateGestures)
  in.steady  = Gaze::holdsGestureHead() && npc.world().tickCount()<gestureHead;
  // a blow throws the head around: the gaze lets go while one stumbles
  if(!npc.isDown() && (npc.bodyStateMasked() & BS_MAX)!=BS_STUMBLE)
    in.source = npc.gazeTarget(in.target);
  // the eyes of an own head look there too (updateEyes)
  lookSource = Gaze::isEnabled() ? in.source : Gaze::Source::None;
  lookAt     = in.target;
  // thinking of an answer: the eyes go aside and up from the one it talks to (TalkGestures); with eyes of its
  // own the head goes only a part of the way
  if(const auto& as = gestures.eyesAside(); as.weight>0.f && in.source==Gaze::Source::Talk) {
    const float k = as.weight*(eyes.isActive() ? EyeAsideHead : 1.f);
    in.target = asidePoint(npc.mapHeadBone(),in.target,as.yaw*k,as.pitch*k);
    }
  // an own head with eyes turns so that its face points at the target from its eyes, not from the bone 11 cm below
  // them (a point above the eyes would raise the head past it); the heads of the original aim from the bone
  in.eyes = eyes.isActive() && eyesInHead(in.eyesAt);
  if(in.source!=Gaze::Source::None) {
    in.active   = true;
    // the hero's body is the player's; one walking or busy at a bench, a book turns the head only;
    // a four-legged spine does not turn like a man's
    const auto bs = npc.bodyStateMasked() & BS_MAX;
    // aiming by hand the trunk is the aim's: a turn of it would throw the arrow off
    in.headOnly = in.source==Gaze::Source::Input || npc.isPlayer() || npc.hasManualAim() || npc.interactive()!=nullptr || pose.footIk().isQuadruped() ||
                  bs==(BS_WALK & BS_MAX) || bs==BS_RUN || bs==BS_SPRINT || bs==(BS_SNEAK & BS_MAX);
    }
  const bool changed = pose.updateGaze(in);
  if(npc.isPlayer())
    gaze.probe(Gaze::playerProbe);
  if(&npc==Gaze::probeNpc)
    gaze.probe(Gaze::subjectProbe);
  if(changed)
    pushBodyPose();
  return changed;
  }

bool MdlVisual::updateLean(Npc& npc, uint64_t dt) {
  Pose& pose = *skInst;
  auto& lean = pose.leanIk();
  if(!lean.isValid()) {
    if(npc.isPlayer())
      Lean::playerProbe = Lean::Probe();
    if(&npc==Lean::probeNpc)
      Lean::subjectProbe = Lean::Probe();
    return false;
    }

  // on the ground, going somewhere; four legs turn and climb their own way (FootIk)
  const float a  = npc.rotationRad();
  const auto  bs = npc.bodyStateMasked() & BS_MAX;
  Lean::Input in;
  in.dt       = dt;
  in.position = npc.position();
  in.forward  = {std::cos(a), 0, std::sin(a)};
  const bool ground = !npc.isDown() && !npc.isInAir() && !npc.isSwim() && !npc.isDive() && !npc.isSlide() &&
                      npc.interactive()==nullptr && !pose.footIk().isQuadruped();
  in.active   = (bs==(BS_WALK & BS_MAX) || bs==BS_RUN || bs==BS_SPRINT || bs==(BS_SNEAK & BS_MAX)) && ground;
  in.grounded = in.active || (bs==(BS_STAND & BS_MAX) && ground);
  const bool changed = pose.updateLean(in);
  if(npc.isPlayer())
    lean.probe(Lean::playerProbe);
  if(&npc==Lean::probeNpc)
    lean.probe(Lean::subjectProbe);
  if(changed)
    pushBodyPose();
  return changed;
  }

bool MdlVisual::updateAim(Npc& npc, uint64_t dt) {
  Pose& pose = *skInst;
  auto& aim  = pose.aimIk();
  if(!aim.isValid()) {
    if(npc.isPlayer())
      Aim::playerProbe = Aim::Probe();
    if(&npc==Aim::probeNpc)
      Aim::subjectProbe = Aim::Probe();
    return false;
    }

  // drawing the bow at the target (a crossbow, a spell): where the shot leaves to, as World shoots it.
  // From where the bow is held in the stance, not from the hand: the turn moves the hand, the aim would
  // chase itself (straight down most of all)
  const float a  = npc.rotationRad();
  const auto  bs = npc.bodyStateMasked() & BS_MAX;
  const auto  ws = npc.weaponState();
  const Npc*  t  = npc.target();
  // aimed by hand (game/archery): at the crosshair, else at the target
  const bool  manual = npc.hasManualAim();
  Aim::Input in;
  in.dt      = dt;
  in.forward = {std::cos(a), 0, std::sin(a)};
  if((manual || (t!=nullptr && t!=&npc)) && !npc.isDown()) {
    // only while aiming: the target may not be valid yet (a save loading)
    auto aimAt = [&]() { return manual ? npc.manualAimPoint() : t->collosionCenter(); };
    float shoulder = 145; // human, bind pose
    if(skeleton!=nullptr) {
      const size_t sh = skeleton->findNode("BIP01 R UPPERARM");
      if(sh<skeleton->tr.size())
        shoulder = skeleton->tr[sh].at(3,1);
      }
    const Tempest::Vec3 from = npc.position() + Tempest::Vec3(0,shoulder,0) + in.forward*35.f;
    // a shot is a frame of BS_HIT, the reload goes on aiming
    if((ws==WeaponState::Bow || ws==WeaponState::CBow) && (bs==BS_AIMNEAR || bs==BS_AIMFAR || bs==(BS_HIT & BS_MAX))) {
      in.active = true;
      in.arrow  = true;
      if(manual) {
        // by hand: the arrow straight at the crosshair (the archer holds over by himself), from where
        // the arrow is (not too close: the turn moves it)
        in.dir    = npc.aimDirection(aimAt());
        in.referenceDir = aimAt()-from;
        in.sway   = npc.manualAimSway();
        in.tremor = npc.manualAimTremor();
        in.shake  = Tempest::Vec2(npc.manualAimShake().x,npc.manualAimShake().y);
        in.manual = true;
        in.follow = (1.f - CrouchSlow*npc.aimCrouch())*(1.f + CantQuick*(ws==WeaponState::Bow ? npc.aimCant() : 0.f));
        in.reload = pose.animProgress(ws==WeaponState::CBow ? Archery::CBowReload : Archery::BowReload,npc.world().tickCount())>=0.f;
        in.holdOrigin = npc.archer().stage()==Archer::Reloading;
        // the shot leaves along the arrow (the bolt) as it lies: in the crossbow it is in the left hand
        in.slot   = ammunition.boneId;
        } else {
        in.dir  = World::bulletVelocity(from,aimAt());
        }
      }
    else if(ws==WeaponState::Mage && bs==BS_CASTING) {
      in.active = true;
      in.dir    = aimAt() - from;
      }
    if(in.active && manual && npc.aimPeek()!=0.f) {
      // round a corner: lean out sideways as far as there is room beside the head
      const float           peek  = npc.aimPeek();
      const Tempest::Vec3   right = Tempest::Vec3::normalize(Tempest::Vec3::crossProduct(Tempest::Vec3(0,1,0),in.forward));
      // from where the head is upright (not leaned: the room does not chase itself)
      const Tempest::Vec3   head  = npc.position() + Tempest::Vec3(0,shoulder+HeadOverShoulder,0);
      const Tempest::Vec3   dir   = right*(peek>0.f ? 1.f : -1.f);
      const float           reach = Aim::SideReach + PeekClear;
      auto                  r     = npc.world().physic()->ray(head,head+dir*reach);
      const float           room  = r.hasCol ? std::clamp(((r.v-head).length()-PeekClear)/Aim::SideReach,0.f,1.f) : 1.f;
      in.side = peek*room*Aim::SideMax*float(M_PI)/180.f;
      }
    if(in.active) {
      if(npc.isPlayer() && manual) {
        in.leanDirection = npc.aimLeanDirection(aimAt());
        in.lean = npc.aimLean(aimAt(),true).angle;
        if(Aim::playerLeanOverride>=0.f)
          in.lean = Aim::playerLeanOverride;
        } else {
        // Classic aiming and NPCs keep their original angular rule and room check.
        static constexpr float reach = 58.f, drop = 28.f;
        const Tempest::Vec3 d = in.dir;
        const float down = -std::atan2(d.y,std::sqrt(d.x*d.x+d.z*d.z));
        in.lean = Aim::leanForNpc(down);
        if(in.lean>0.f) {
          const float k = in.lean/(Aim::NpcLeanMax*float(M_PI)/180.f);
          const auto dir = Tempest::Vec3::normalize(Tempest::Vec3::normalize(Tempest::Vec3(d.x,0,d.z)) + in.forward*0.25f);
          const auto head = mapHeadBone();
          const auto to = head + dir*(reach*k) - Tempest::Vec3(0,drop*k,0);
          if(npc.world().physic()->ray(head,to).hasCol)
            in.lean = 0;
          }
        }
      }
    }
  // steps with the bow drawn: the legs walk under the aim (graphics/mesh/aimwalk), sneaking with the bow
  // (the crossbow), slowly
  AimWalk::Input wk;
  wk.dt   = dt;
  wk.active = manual && in.active;
  const float requestedWalk = manual && in.active ? npc.aimWalk() : 0.f;
  Tempest::Vec3 walkDirection(0,0,requestedWalk);
  if(requestedWalk==0.f && pose.aimWalk().isActive())
    walkDirection = pose.aimWalk().move(dt);
  const bool edgeBlocked = manual && in.active && npc.isEdgeMoveBlocked(walkDirection);
  wk.blocked = edgeBlocked || npc.isAimWalkStopped();
  wk.walk = edgeBlocked ? 0.f : requestedWalk;
  // Keep the gait while the stopped legs ease back into the stance, without further root motion.
  if(wk.walk!=0.f || pose.aimWalk().isActive()) {
    const bool cbow = ws==WeaponState::CBow;
    wk.legs = solver.solveFrm(cbow ? "S_CBOWSNEAKL" : "S_BOWSNEAKL");
    if(wk.legs==nullptr)
      wk.legs = solver.solveFrm(cbow ? "S_CBOWWALKL" : "S_BOWWALKL");
    }
  bool changed = pose.updateAimWalk(wk);
  if(npc.isPlayer())
    pose.aimWalk().probe(AimWalk::playerProbe);
  changed |= pose.updateAim(in);
  if(npc.isPlayer())
    aim.probe(Aim::playerProbe);
  if(&npc==Aim::probeNpc)
    aim.probe(Aim::subjectProbe);

  // a bow drawn by hand: the hand goes to the string as far as it is not drawn, the string with it (a
  // crossbow is not drawn). The bow of G2 is drawn by its morph: S_READY holds it as the animation does,
  // StringDrawn straight back as the bow is held by hand (graphics/mesh/bowdraw)
  auto&          ar = npc.archer();
  BowDraw::Input bd;
  bd.dt     = dt;
  bd.active = manual && in.active && ws==WeaponState::Bow;
  bd.ready  = ar.stage()==Archer::Nocked || ar.stage()==Archer::Drawing || ar.stage()==Archer::Held;
  bd.span   = bowSpan();
  // straining, the drawing hand loses ground along the arrow and wins it back (seen only: the arrow leaves
  // drawn as far as Archer says)
  bd.draw   = std::max(0.f,ar.draw() - (ar.stage()==Archer::Drawing || ar.stage()==Archer::Held ?
                                        npc.manualAimShake().z/std::max(bd.span,1.f) : 0.f));
  bd.arrow  = !ammunition.view.isEmpty() && ammunition.bone=="ZS_RIGHTHAND";
  bd.cant   = npc.aimCant();
  bd.kick   = ar.releaseKick();
  bd.settle = ar.releaseSettle();
  if(auto p = bow.view.protoMesh()) {
    bool high = false, straight = false;
    for(auto& m:p->morph) {
      if(m.name==BowHeld) {
        high          = true;
        bd.stringHigh = m.peakMove;
        }
      if(m.name==BowDraw::StringDrawn) {
        straight      = true;
        bd.stringRest = m.peakAt;
        bd.stringPull = m.peakMove;
        }
      }
    // along the limbs: the longest way of the bow
    const Tempest::Vec3 ext = p->bboxMesh[1]-p->bboxMesh[0];
    bd.bowAxis   = ext.x>=ext.y && ext.x>=ext.z ? Tempest::Vec3(1,0,0) : (ext.y>=ext.z ? Tempest::Vec3(0,1,0) : Tempest::Vec3(0,0,1));
    bd.hasString = high && straight;
    }
  if(quiv.isShown() && skeleton!=nullptr && quiv.bone()<skInst->boneCount()) {
    // the quiver as it hung last (the reload's hand is led to it), relative to the pelvis
    const size_t sp = quiv.spineBone()<skInst->boneCount() ? quiv.spineBone() : quiv.bone();
    auto pelvisInv = renderedBone(quiv.bone());
    pelvisInv.inverse();
    bd.quiverKnown = true;
    bd.quiverNow   = pelvisInv*quiv.frameNow(renderedBone(quiv.bone()),renderedBone(sp));
    bd.quiverClip  = quiv.reloadFrame();
    }
  if(ar.stage()==Archer::Reloading) {
    // how far the reload is: not begun (the shot), the animation, over (the bow comes to rest)
    const float p = pose.animProgress("T_BOWRELOAD",npc.world().tickCount());
    bd.reload = p>=0.f ? p : (ar.isNocking() ? 1.f : 0.f);
    }
  changed  |= pose.updateBowDraw(bd);
  // the right hand on the quiver while it slides along the belt (reports/archery/2026-11-12-quiver-to-hand)
  {
    QuiverHand::Input qh;
    if(quiverHand>0.f && quiv.isShown() && skeleton!=nullptr && quiv.bone()<skInst->boneCount()) {
      const size_t sp = quiv.spineBone()<skInst->boneCount() ? quiv.spineBone() : quiv.bone();
      auto pelvisInv = renderedBone(quiv.bone());
      pelvisInv.inverse();
      Tempest::Matrix4x4 at;
      at.identity();
      at.translate(Tempest::Vec3(0,-Quiver::GripDown,Quiver::MouthRadius+Quiver::GripOff));
      qh.weight = quiverHand;
      qh.grip   = pelvisInv*quiv.frameNow(renderedBone(quiv.bone()),renderedBone(sp))*at;
      }
    changed |= pose.updateQuiverHand(qh);
  }
  // the string as drawn by hand: straight back as far as the bow is held so, else as G2 draws it
  const float string = pose.bowDrawIk().string();
  const float held   = pose.bowDrawIk().aligned();
  if(string>=0.f) {
    bow.view.setMMShape(BowHeld,0,0,0,string*(1.f-held));
    bow.view.setMMShape(BowDraw::StringDrawn,0,0,0,string*held);
    stringByHand = true;
    }
  else if(stringByHand) {
    // let go: the string goes (the shot: it trembles at once, the animation's release is late), else the
    // animation's morphs from here
    bow.view.startMMAnim(BowHeld,0,0);
    bow.view.startMMAnim(BowDraw::StringDrawn,0,0);
    if(pose.bowDrawIk().loosed()) {
      // as hard as the shot: a heavy bow drawn fully rings, a light one drawn short less
      bow.view.startMMAnim(held>0.5f ? BowDraw::StringLoose : BowLoose,BowDraw::looseIntensity(ar.releaseKick()),uint64_t(-1));
      looseAt = npc.world().tickCount();
      }
    stringByHand = false;
    }
  if(npc.isPlayer())
    pose.bowDrawIk().probe(BowDraw::playerProbe);

  if(changed)
    pushBodyPose();
  return changed;
  }

bool MdlVisual::updateFlinch(Npc& npc, uint64_t dt) {
  Pose& pose = *skInst;
  auto& fl   = pose.flinchIk();
  if(!fl.isValid()) {
    if(npc.isPlayer())
      Flinch::playerProbe = Flinch::Probe();
    if(&npc==Flinch::probeNpc)
      Flinch::subjectProbe = Flinch::Probe();
    return false;
    }
  const float a = npc.rotationRad();
  Flinch::Input in;
  in.dt       = dt;
  in.forward  = {std::cos(a), 0, std::sin(a)};
  in.headOnly = pose.footIk().isQuadruped();
  const bool changed = pose.updateFlinch(in);
  if(npc.isPlayer())
    fl.probe(Flinch::playerProbe);
  if(&npc==Flinch::probeNpc)
    fl.probe(Flinch::subjectProbe);
  if(changed)
    pushBodyPose();
  return changed;
  }

void MdlVisual::flinch(const Npc& npc, const Flinch::Hit& hit) {
  const float a = npc.rotationRad();
  skInst->flinchIk().hit(hit,{std::cos(a), 0, std::sin(a)});
  }

float MdlVisual::bowSpan() const {
  // how far the string goes from rest to held drawn: the string's middle moves the most
  if(auto p = bow.view.protoMesh())
    for(auto& m:p->morph)
      if(m.name==BowHeld)
        return m.peak;
  return 0;
  }

bool MdlVisual::arrowRay(Vec3& p, Vec3& d) const {
  // the arrow (the bolt) lies along -X of its slot, the nock at the string (graphics/mesh/aim)
  size_t id = ammunition.boneId;
  if(id==size_t(-1) && skeleton!=nullptr)
    id = skeleton->findNode("ZS_RIGHTHAND");
  if(id==size_t(-1) || id>=skInst->boneCount())
    return false;
  const auto& m = skInst->bone(id);
  // where the arrow is for the aim: the follow through after the shot moves the hand only to be seen
  // (graphics/mesh/bowdraw), the aim goes on from where the hand would be without it
  p = Vec3(m.at(3,0),m.at(3,1),m.at(3,2)) - skInst->bowDrawIk().seenOnly();
  d = Vec3::normalize(Vec3(-m.at(0,0),-m.at(0,1),-m.at(0,2)));
  return true;
  }

bool MdlVisual::aimAttachmentBounds(bool ammo, Matrix4x4& matrix, Vec3 (&bounds)[2]) const {
  const auto& mesh = ammo ? ammunition.view : bow.view;
  const auto* proto = mesh.protoMesh();
  if(mesh.isEmpty() || proto==nullptr)
    return false;
  matrix = mesh.node(0).position();
  bounds[0] = proto->bboxMesh[0];
  bounds[1] = proto->bboxMesh[1];
  // Frame interpolation stays within each morph's component bounds. Sum the currently
  // shown morphs, as the vertex shader does; a string pulled along the arrow must not
  // expand the bow by its full draw length sideways as well.
  for(const auto& morph:proto->morph) {
    const float weight = mesh.morphWeight(morph.name);
    bounds[0] += morph.moveMin*weight;
    bounds[1] += morph.moveMax*weight;
    }
  return true;
  }

float MdlVisual::aimTrackError() const {
  return skInst->aimIk().trackError();
  }

bool MdlVisual::isAimSteady() const {
  return skInst->aimIk().isSteady();
  }

bool MdlVisual::isAimValid() const {
  return skInst->aimIk().isValid() && Aim::isEnabled();
  }
