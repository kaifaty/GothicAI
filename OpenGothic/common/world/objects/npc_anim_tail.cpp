#include "npc_melee_shadow.h"

#include "world/animationtail.h"
#include "world/npclogicalstate.h"

#include <string>

// What the steps after the parallel animation pass still have to do for this NPC (world/animationtail.h).
// The pass calls animationTailPlan on its worker, after this NPC's own work; it reads this NPC only.

bool Npc::hasMeleeWindow() const {
  return meleeShadow && meleeShadow->window;
  }

bool Npc::meleeShadowSettled() const {
  // updateMeleeShadow would republish the body this NPC published last, or keep it absent, and find no swing.
  if(hasMeleeWindow())
    return false;
  if(!wantsMeleeShadowBody())
    return !meleeShadowBody; // clearMeleeShadow: no window to finish, no body to remove
  uint32_t scale = 0;
  uint64_t shape = 0;
  const auto& cached = meleeShadowPose;
  if(!meleeShadowBody || cached.published==0 || cached.published!=cached.body.serial ||
     !meleeShadowPoseCurrent(scale,shape))
    return false;
  // syncMeleeShadowSwing's first branch, with no window to finish; an armed human looks for an attack.
  const auto mode = weaponState();
  return !isHuman() || isDown() || meleeKnockdown ||
         (mode!=WeaponState::Fist && mode!=WeaponState::W1H && mode!=WeaponState::W2H);
  }

uint8_t Npc::animationTailPlan(bool shadowSkipped, bool keepShadow) const {
  using namespace AnimationTail;
  // updateFace can start gestures on this body and read another's eyes: such an NPC keeps every step.
  if(visual.updatesFace(*this))
    return uint8_t(shadowSkipped ? All&~Shadow : All);
  uint8_t plan = 0;
  if(hasMeleeWindow())
    plan |= Contacts;
  if(!shadowSkipped && (keepShadow || !meleeShadowSettled()))
    plan |= Shadow;
  if(!logicalBody.settled(*this))
    plan |= Logic;
  return plan;
  }

namespace {
struct ShadowState final {
  bool     body = false, window = false, downStrike = false;
  uint64_t swing = 0, serial = 0, published = 0;
  bool operator==(const ShadowState&) const = default;
  };

bool sameEvents(const std::vector<NpcLogicalState::Event>& a, const std::vector<NpcLogicalState::Event>& b) {
  if(a.size()!=b.size())
    return false;
  for(size_t i=0; i<a.size(); ++i)
    if(a[i].expected!=b[i].expected || a[i].source.kind!=b[i].source.kind || a[i].source.time!=b[i].source.time ||
       a[i].source.frame!=b[i].source.frame || a[i].source.index!=b[i].source.index)
      return false;
  return true;
  }

bool sameLayers(const std::vector<NpcLogicalState::Layer>& a, const std::vector<NpcLogicalState::Layer>& b) {
  if(a.size()!=b.size())
    return false;
  for(size_t i=0; i<a.size(); ++i) {
    const auto& x = a[i];
    const auto& y = b[i];
    if(x.sequence!=y.sequence || x.start!=y.start || x.origin!=y.origin || x.expectedEnd!=y.expectedEnd ||
       x.speed!=y.speed || x.state!=y.state || x.combo!=y.combo || x.action!=y.action || x.finished!=y.finished ||
       x.weaponEvents!=y.weaponEvents || !sameEvents(x.events,y.events))
      return false;
    }
  return true;
  }

bool sameLogicalState(const NpcLogicalState& a, const NpcLogicalState& b) {
  if(a.admissions.size()!=b.admissions.size())
    return false;
  for(size_t i=0; i<a.admissions.size(); ++i) {
    const auto& x = a.admissions[i];
    const auto& y = b.admissions[i];
    if(x.kind!=y.kind || x.sequence!=y.sequence || x.at!=y.at || x.wait!=y.wait)
      return false;
    }
  return a.body==b.body && a.weapon==b.weapon && a.interactiveState==b.interactiveState &&
         a.action.kind==b.action.kind && a.action.sequence==b.action.sequence && a.action.start==b.action.start &&
         a.action.origin==b.action.origin && a.action.expectedEnd==b.action.expectedEnd &&
         a.action.predictable==b.action.predictable && a.nativeWaitEnd==b.nativeWaitEnd &&
         a.actionWait==b.actionWait && a.initialized==b.initialized && a.worldStep==b.worldStep &&
         a.externalState==b.externalState && a.executorState==b.executorState && a.speeds==b.speeds &&
         sameLayers(a.layers,b.layers);
  }
}

void Npc::checkSkippedTailStep(uint8_t step) {
  using namespace AnimationTail;
  const std::string name(displayName());
  switch(step) {
    case Face: {
      checked("face",name,!visual.updatesFace(*this));
      updateFace();
      return;
      }
    case Shadow: {
      const auto state = [this] {
        return ShadowState{meleeShadowBody,hasMeleeWindow(),meleeDownStrike,meleeShadow ? meleeShadow->id : 0,
                           meleeShadowPose.body.serial,meleeShadowPose.published};
        };
      const auto before = state();
      if(before.window) {
        // Its scripts may remove this NPC: report before the step.
        checked("shadow",name,false);
        updateMeleeShadow();
        return;
        }
      updateMeleeShadow();
      checked("shadow",name,state()==before);
      return;
      }
    case Contacts: {
      // Without a window the step returns at once; with one its scripts may remove this NPC.
      checked("contacts",name,!hasMeleeWindow());
      resolveMeleeContacts();
      return;
      }
    case Logic: {
      const NpcLogicalState before = logicalBody;
      const bool inputs = !visual.pose().logicalInputs().empty();
      syncLogicalBodyState();
      checked("logic",name,!inputs && sameLogicalState(before,logicalBody));
      return;
      }
    }
  }
