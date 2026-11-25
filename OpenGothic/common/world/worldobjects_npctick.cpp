#include "worldobjects.h"
#include "world.h"

#include "world/npcsimtick.h"
#include "world/objects/npc.h"
#include "frameprofile.h"

#include <bit>
#include <chrono>
#include <optional>

void WorldObjects::refreshSocietyDispatch() {
  if(!npcDispatchDirty)
    return;
  nativeNpcMask.assign((npcArr.size()+63)/64,0);
  for(size_t i=0; i<npcArr.size(); ++i) {
    auto& npc = *npcArr[i];
    npc.societyDispatchIndex = uint32_t(i);
    if(!npc.societySleeping)
      nativeNpcMask[i/64] |= uint64_t(1)<<(i%64);
    }
  npcDispatchDirty = false;
  poseDispatchDirty = true;
  }

void WorldObjects::updateSocietyDispatch(Npc& npc) {
  if(npc.hasDeferredAnimation())
    noteNpcCellChange(npc);
  if(!npcDispatchDirty) {
    const auto i = npc.societyDispatchIndex;
    const auto bit = uint64_t(1)<<(i%64);
    if(npc.societySleeping)
      nativeNpcMask[i/64] &= ~bit;
    else
      nativeNpcMask[i/64] |= bit;
    }
  poseDispatchDirty = true;
  }

void WorldObjects::refreshSocietyPoseDispatch() {
  refreshSocietyDispatch();
  if(!poseDispatchDirty)
    return;
  nativePoseIndices.clear();
  for(size_t i=0; i<npcArr.size(); ++i)
    if(!npcArr[i]->societyPoseSuspended)
      nativePoseIndices.push_back(i);
  poseDispatchDirty = false;
  }

size_t WorldObjects::nextNativeNpc(size_t first) const {
  auto word = first/64;
  if(word==nativeNpcMask.size())
    return npcArr.size();
  auto remaining = nativeNpcMask[word] & (~uint64_t(0)<<(first%64));
  while(remaining==0) {
    if(++word==nativeNpcMask.size())
      return npcArr.size();
    remaining = nativeNpcMask[word];
    }
  return word*64+size_t(std::countr_zero(remaining));
  }

void WorldObjects::noteNpcCadence(uint64_t dt) {
  npcCadence[npcCadenceEnd] = {simTickClock+dt,dt};
  npcCadenceEnd = (npcCadenceEnd+1)%npcCadence.size();
  npcCadenceCount = std::min(npcCadenceCount+1,npcCadence.size());
  }

uint64_t WorldObjects::societyDueStep(const Npc& npc, uint64_t entered) const {
  const auto& cfg = NpcSimTick::config();
  const auto& current = npcCadence[(npcCadenceEnd+npcCadence.size()-1)%npcCadence.size()];
  if(cfg.periodMs==0 || !NpcSimTick::qualifies(npc,cfg.scope))
    return current.dt;
  const auto phase = NpcSimTick::phase(npc,cfg.periodMs);
  const auto stepAt = [&](size_t ago) -> const NpcCadenceStep& {
    return npcCadence[(npcCadenceEnd+npcCadence.size()-1-ago)%npcCadence.size()];
    };
  // Search for the last cadence node, then replay the steps that may have
  // dispatched between nodes. The sleep entry itself ran on a native step.
  auto last = entered;
  size_t ago = 1;
  for(; ago<npcCadenceCount; ++ago) {
    const auto& step = stepAt(ago);
    if(step.clock<=entered)
      break;
    if(NpcSimTick::due(step.clock,step.dt,step.dt,phase,cfg.periodMs)) {
      last = step.clock;
      break;
      }
    }
  while(ago>1) {
    const auto& step = stepAt(--ago);
    if(NpcSimTick::due(step.clock,step.dt,step.clock-last,phase,cfg.periodMs))
      last = step.clock;
    }
  const auto owed = current.clock-last;
  return NpcSimTick::due(current.clock,current.dt,owed,phase,cfg.periodMs) ? owed : 0;
  }

// The NPC loop of WorldObjects::tick: Npc::tick of every NPC in the order of npcArr, the far ones at the rate of
// world/npcsimtick.h. Scripts in a tick may add and remove NPCs; the loop keeps indices, never iterators.
void WorldObjects::tickNpcs(uint64_t dt, uint64_t dtPlayer, const Npc* pl, bool freeCam, bool frozen) {
  const auto& cfg      = NpcSimTick::config();
  const bool  profiled = FrameProfile::enabled();
  const bool  societyEarly = owner.societyEarlyTickEnabled();
  const bool  persistent = owner.persistentSocietyEnabled();
  uint64_t simTicked = 0, simDeferred = 0, simNative = 0;
  uint64_t societySkipped = 0;
  simTickClock += dt;
  // One clock read per run of consecutive NPCs of the same tier keeps the cost
  // proportional to tier changes along the array, not to the NPC count.
  using Clock = std::chrono::steady_clock;
  std::optional<FrameProfile::Scope> tierZone;
  NpcSimTier        runTier  = NpcSimTier::Full;
  bool              runOpen  = false;
  Clock::time_point runStart;
  double            tierMs[2] = {};
  uint64_t          tierTicks[2] = {};
  for(size_t i=0; i<npcArr.size();) {
    if(persistent) {
      refreshSocietyDispatch();
      i = nextNativeNpc(i);
      if(i==npcArr.size())
        break;
      }
    auto& npc = *npcArr[i++];
    uint64_t d = (pl==&npc ? dtPlayer : dt);
    if(freeCam && pl==&npc)
      continue;
    // The frozen world of body training: the same NPCs the animation pass passes over.
    if(animationPassSkips(npc,pl,false,frozen))
      continue;
    const auto tier = npc.simTier();
    if(!runOpen || tier!=runTier) {
      const auto now = Clock::now();
      if(runOpen)
        tierMs[size_t(runTier)] += std::chrono::duration<double,std::milli>(now-runStart).count();
      tierZone.reset();
      tierZone.emplace(tier==NpcSimTier::Full ? FrameProfile::Zone::NpcFullTick : FrameProfile::Zone::NpcSimplifiedTick);
      runTier  = tier;
      runStart = now;
      runOpen  = true;
      }
    ++tierTicks[size_t(tier)];
    const bool nativeCombat=tier==NpcSimTier::Simplified && npc.nativeCombatMovement();
    const bool nativeMobsi=tier==NpcSimTier::Simplified && npc.nativeMobsiApproach();
    const bool graphFallback=npc.graphMovementFallback();
    const bool simplified=tier==NpcSimTier::Simplified;
    if(simplified && profiled && !npc.usesSimplifiedMovement())
      ++simNative;
    // Whether this NPC may wait is read from its own state now; one that stops qualifying ticks in this frame.
    auto&          state = npc.simTick();
    const uint64_t owed  = state.pending+d;
    if(simplified && &npc!=pl &&
       !NpcSimTick::due(simTickClock,dt,owed,NpcSimTick::phase(npc,cfg.periodMs),cfg.periodMs) &&
       NpcSimTick::qualifies(npc,cfg.scope)) {
      state.pending = owed;
      ++simDeferred;
      // Preserve Society's hold only if its native and visibility gates still pass.
      owner.retainDeferredSocietyRoutine(npc);
      } else {
      state.pending = 0;
      if(societyEarly && npc.processPolicy()==NpcProcessPolicy::AiFar2 &&
         owner.societyHoldsRoutineAfterTick(npc) && npc.trySocietyEarlyTick()) {
        ++societySkipped;
        if(persistent && owner.persistentSocietyEnabled())
          owner.sleepSocietyNpc(npc);
        continue;
        }
      simTicked += simplified ? 1 : 0;
      npc.tick(owed);
      }
    if(tier==NpcSimTier::Simplified && (nativeCombat || npc.nativeCombatMovement()))
      ++simTiers.nativeCombatNpcTicks;
    if(tier==NpcSimTier::Simplified && (nativeMobsi || npc.nativeMobsiApproach()))
      ++simTiers.nativeMobsiApproachNpcTicks;
    if(graphFallback || npc.graphMovementFallback())
      ++simTiers.graphFallbackNpcTicks;
    }
  if(runOpen)
    tierMs[size_t(runTier)] += std::chrono::duration<double,std::milli>(Clock::now()-runStart).count();
  tierZone.reset();
  simTiers.fullTickMs             = tierMs[size_t(NpcSimTier::Full)];
  simTiers.simplifiedTickMs       = tierMs[size_t(NpcSimTier::Simplified)];
  simTiers.fullTickMsTotal       += simTiers.fullTickMs;
  simTiers.simplifiedTickMsTotal += simTiers.simplifiedTickMs;
  simTiers.fullNpcTicks         += tierTicks[size_t(NpcSimTier::Full)];
  simTiers.simplifiedNpcTicks   += tierTicks[size_t(NpcSimTier::Simplified)];
  ++simTiers.ticks;
  FrameProfile::add(FrameProfile::Amount::NpcSimplifiedTicked,simTicked);
  FrameProfile::add(FrameProfile::Amount::NpcSimplifiedDeferred,simDeferred);
  FrameProfile::add(FrameProfile::Amount::NpcSimplifiedNative,simNative);
  if(societySkipped!=0)
    FrameProfile::add(FrameProfile::Amount::SocietyNpcTickSkipped,societySkipped);
  }
