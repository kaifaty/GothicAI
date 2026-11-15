#include "worldobjects.h"

#include "world/animationlod.h"
#include "world/animationtail.h"
#include "world/benchhooks.h"
#include "world/npclogicalstate.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/objects/npc_melee_shadow.h"
#include "world/world.h"
#include "game/bodyarena.h"
#include "game/bodyobservations.h"
#include "physics/dynamicworld.h"
#include "frameprofile.h"

// The steps of WorldObjects::updateAnimation after its parallel pass over the NPCs, in their order: faces,
// melee shadow bodies, melee contacts, interactive objects, logical bodies. Each loop runs on this thread in
// the order of npcArr; the pass's plan lets it pass over the NPCs it has nothing to do with (animationtail.h).

bool WorldObjects::animationPassSkips(const Npc& npc, const Npc* player, bool skipFar, bool frozen) {
  // Headless body training may skip NPCs beyond the far range (>60 m from the hero)
  // that are not arena actors: nobody sees them, and they are most of the world.
  if(skipFar && npc.processPolicy()==NpcProcessPolicy::AiFar2 && !BodyObservations::watched(npc))
    return true;
  return frozen && &npc!=player && !BodyArena::isActor(npc);
  }

bool WorldObjects::animationTailKeepsShadow() {
  // Both look at every published body, every frame.
  return Bench::bodyTracing() || NpcMeleeShadow::checking();
  }

void WorldObjects::updateAnimationTail(uint64_t dt, const Npc* player, bool skipFar, bool frozen) {
  using namespace AnimationTail;
  struct Watch final {
    Watch()  { begin(); }
    ~Watch() { end();   }
    } watch;
  const auto& plan = tailPlan;
  uint64_t shadows = 0, logical = 0;

  // the own heads' faces: the eyes aim from the heads as posed now; the deformation goes out on this thread
  loop(npcArr,plan,Face,[](Npc& npc,bool) {
    npc.updateFace();
    },[](Npc& npc) {
    npc.checkSkippedTailStep(Face);
    });

  // Retiring a shadow window can also run damage scripts here. Scripts may
  // append or remove NPCs, so retain neither vector iterators nor references.
  FrameProfile::Scope meleeShadow(FrameProfile::Zone::MeleeShadow);
  loop(npcArr,plan,Shadow,[&](Npc& npc,bool planned) {
    if(!planned && animationPassSkips(npc,player,skipFar,frozen))
      return;
    ++shadows;
    const bool window = npc.hasMeleeWindow();
    npc.updateMeleeShadow();
    // A window's scripts can change any NPC (and remove this one); a window opened now has contacts ahead.
    if(window || npc.hasMeleeWindow())
      touch();
    },[&](Npc& npc) {
    if(animationPassSkips(npc,player,skipFar,frozen))
      return; // the full loop passes it over as well
    const bool window = npc.hasMeleeWindow();
    npc.checkSkippedTailStep(Shadow);
    if(window || npc.hasMeleeWindow())
      touch();
    });
  // Query the final published interval before callers can decide a terminal
  // combat outcome. The flush does not advance physics, animation or clocks.
  meleeShadow.finish();

  FrameProfile::Scope meleeContacts(FrameProfile::Zone::MeleeContacts);
  owner.physic()->flushShadowBlades();
  loop(npcArr,plan,Contacts,[](Npc& npc,bool) {
    const bool window = npc.hasMeleeWindow();
    npc.resolveMeleeContacts();
    if(window)
      touch();
    },[](Npc& npc) {
    const bool window = npc.hasMeleeWindow();
    npc.checkSkippedTailStep(Contacts);
    if(window)
      touch();
    });
  meleeContacts.finish();

  if(!frozen) {
    FrameProfile::Scope interactive(FrameProfile::Zone::InteractiveAnimation);
    AnimationLod::beginPass();
    interactiveObj.parallelFor([dt](Interactive& i){
      i.updateAnimation(dt);
      });
    AnimationLod::endPass();
    }

  // Audit the previous prediction after the body has advanced, then consume
  // this tick's mutation inputs on the main thread (including worker speed changes).
  const auto& logic = NpcLogicAudit::stats();
  const uint64_t logicBefore[] = {logic.ticks,logic.bodyMismatch,logic.weaponMismatch,
                                 logic.early,logic.late,logic.events,logic.eventMismatch,
                                 logic.waitEarly,logic.waitLate,logic.reanchoredCount(),logic.bodySamples,logic.weaponSamples};
  {
  FrameProfile::Scope logicalBodies(FrameProfile::Zone::LogicalBodySync);
  loop(npcArr,plan,Logic,[&](Npc& npc,bool) {
    ++logical;
    npc.syncLogicalBodyState();
    },[](Npc& npc) {
    npc.checkSkippedTailStep(Logic);
    });
  }
  FrameProfile::add(FrameProfile::Amount::NpcLogicSamples,logic.ticks-logicBefore[0]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicBodyMismatch,logic.bodyMismatch-logicBefore[1]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicWeaponMismatch,logic.weaponMismatch-logicBefore[2]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicEarly,logic.early-logicBefore[3]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicLate,logic.late-logicBefore[4]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicEvents,logic.events-logicBefore[5]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicEventMismatch,logic.eventMismatch-logicBefore[6]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicWaitEarly,logic.waitEarly-logicBefore[7]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicWaitLate,logic.waitLate-logicBefore[8]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicReanchored,logic.reanchoredCount()-logicBefore[9]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicBodySamples,logic.bodySamples-logicBefore[10]);
  FrameProfile::add(FrameProfile::Amount::NpcLogicWeaponSamples,logic.weaponSamples-logicBefore[11]);
  FrameProfile::add(FrameProfile::Amount::NpcTailShadow,shadows);
  FrameProfile::add(FrameProfile::Amount::NpcTailLogic,logical);
  FrameProfile::add(FrameProfile::Amount::NpcTailFull,stale() ? 1 : 0);
  }
