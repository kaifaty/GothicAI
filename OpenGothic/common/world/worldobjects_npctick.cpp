#include "worldobjects.h"
#include "world.h"

#include "game/bodycontroller.h"
#include "game/bodyobservations.h"
#include "world/npcsimtick.h"
#include "world/objects/npc.h"
#include "frameprofile.h"

#include <bit>
#include <cassert>
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

size_t WorldObjects::nextNativeNpc(size_t first, bool persistent, bool idleCadence) const {
  for(auto word=first/64; word<nativeNpcMask.size(); ++word) {
    auto remaining = persistent ? nativeNpcMask[word] : ~uint64_t(0);
    if(idleCadence)
      remaining &= ~idleParked[word] | idleReady[word] | idleUrgent[word].load(std::memory_order_relaxed);
    if(word==first/64)
      remaining &= ~uint64_t(0)<<(first%64);
    if(remaining!=0)
      return std::min(word*64+size_t(std::countr_zero(remaining)),npcArr.size());
    }
  return npcArr.size();
  }

void WorldObjects::noteIdleNpcChange(Npc& npc) {
  if(idleCadenceInvalid)
    return;
  const auto i = npc.societyDispatchIndex;
  idleUrgent[i/64].fetch_or(uint64_t(1)<<(i%64),std::memory_order_relaxed);
  }

size_t WorldObjects::clearIdleNpcCadence(uint64_t clock, size_t visited) {
  if(idlePeriod==0)
    return 0;
  size_t deferred = 0;
  for(auto& ptr:npcArr) {
    auto& npc = *ptr;
    auto& state = npc.simTick();
    if(!state.idleParked)
      continue;
    const auto i = npc.societyDispatchIndex;
    const bool passed = i<visited;
    const auto bit = uint64_t(1)<<(i%64);
    deferred += passed && (idleReady[i/64]&bit)==0;
    state.pending = (passed ? simTickClock : clock)-state.lastIdleTick;
    state.idleParked = false;
    }
  idlePeriod = 0;
  return deferred;
  }

void WorldObjects::prepareIdleNpcCadence(uint64_t dt, uint64_t period) {
  const auto previous = simTickClock-dt;
  if(idleCadenceInvalid || idlePeriod!=period) {
    clearIdleNpcCadence(previous);
    if(period==0)
      return;
    refreshSocietyDispatch();
    const auto words = nativeNpcMask.size();
    idleParked.assign(words,0);
    idleReady.resize(words);
    idleUrgent = std::make_unique<std::atomic<uint64_t>[]>(words);
    for(auto& mask:idlePhase)
      mask.assign(words,0);
    for(auto& mask:idleAge)
      mask.assign(words,0);
    idlePeriod = period;
    idleCadenceInvalid = false;
    }
  if(idlePeriod==0)
    return;
  if(dt>=idlePeriod) {
    idleReady = idleParked;
    return;
    }
  std::fill(idleReady.begin(),idleReady.end(),0);
  const auto include = [&](const std::vector<uint64_t>& mask) {
    for(size_t i=0; i<mask.size(); ++i)
      idleReady[i] |= mask[i];
    };
  for(auto t=previous+1; t<=simTickClock; ++t)
    include(idlePhase[t%idlePeriod]);
  // due() also dispatches an NPC when one more step of this length would
  // exceed one and a half periods. The deadline stays tied to its last tick.
  for(auto t=previous+1; t<=simTickClock+dt; ++t)
    include(idleAge[t%IdleAgeSlots]);
  }

void WorldObjects::parkIdleNpc(Npc& npc) {
  auto& state = npc.simTick();
  state.lastIdleTick = simTickClock-state.pending;
  state.idleParked = true;
  const auto i = npc.societyDispatchIndex;
  const auto word = i/64;
  const auto bit = uint64_t(1)<<(i%64);
  const auto phase = NpcSimTick::phase(npc,idlePeriod);
  const auto age = state.lastIdleTick+3*idlePeriod/2+1;
  idleParked[word] |= bit;
  idleReady[word] |= bit; // This slot has already been visited this step.
  idlePhase[phase][word] |= bit;
  idleAge[age%IdleAgeSlots][word] |= bit;
  }

void WorldObjects::unparkIdleNpc(Npc& npc, uint64_t clock) {
  auto& state = npc.simTick();
  const auto i = npc.societyDispatchIndex;
  const auto word = i/64;
  const auto bit = uint64_t(1)<<(i%64);
  const auto phase = NpcSimTick::phase(npc,idlePeriod);
  const auto age = state.lastIdleTick+3*idlePeriod/2+1;
  idleParked[word] &= ~bit;
  idleUrgent[word].fetch_and(~bit,std::memory_order_relaxed);
  idlePhase[phase][word] &= ~bit;
  idleAge[age%IdleAgeSlots][word] &= ~bit;
  state.pending = clock-state.lastIdleTick;
  state.idleParked = false;
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
  size_t idleDeferred = 0;
  const auto previous = simTickClock;
  simTickClock += dt;
  const auto period = !frozen && cfg.scope==NpcSimTick::Scope::Graph &&
                      !BodyController::hasOwners() && BodyObservations::watchedCount()==0 ? cfg.periodMs : 0;
  {
    FrameProfile::Scope prepare(FrameProfile::Zone::NpcPreTick);
    prepareIdleNpcCadence(dt,period);
  }
  // One clock read per run of consecutive NPCs of the same tier keeps the cost
  // proportional to tier changes along the array, not to the NPC count.
  using Clock = std::chrono::steady_clock;
  std::optional<FrameProfile::Scope> tierZone;
  NpcSimTier        runTier  = NpcSimTier::Full;
  bool              runOpen  = false;
  Clock::time_point runStart;
  double            tierMs[2] = {};
  uint64_t          tierTicks[2] = {};
  size_t i = 0;
  for(; i<npcArr.size();) {
    if(idlePeriod!=0 && idleCadenceInvalid)
      idleDeferred += clearIdleNpcCadence(previous,i);
    if(persistent || idlePeriod!=0) {
      refreshSocietyDispatch();
      i = nextNativeNpc(i,persistent,idlePeriod!=0);
      if(i==npcArr.size())
        break;
      }
    auto& npc = *npcArr[i++];
    if(npc.simTick().idleParked)
      unparkIdleNpc(npc,previous);
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
    if(idlePeriod!=0 && !idleCadenceInvalid && dt<idlePeriod && &npc!=pl && tier==NpcSimTier::Simplified &&
       npc.weaponState()==WeaponState::Fist && !npc.graphMovementFallback() && NpcSimTick::qualifies(npc,cfg.scope))
      parkIdleNpc(npc);
    if(tier==NpcSimTier::Simplified && (nativeCombat || npc.nativeCombatMovement()))
      ++simTiers.nativeCombatNpcTicks;
    if(tier==NpcSimTier::Simplified && (nativeMobsi || npc.nativeMobsiApproach()))
      ++simTiers.nativeMobsiApproachNpcTicks;
    if(graphFallback || npc.graphMovementFallback())
      ++simTiers.graphFallbackNpcTicks;
    }
  if(idlePeriod!=0 && idleCadenceInvalid)
    idleDeferred += clearIdleNpcCadence(previous,i);
  if(idlePeriod!=0) {
    for(size_t word=0; word<idleParked.size(); ++word)
      idleDeferred += size_t(std::popcount(idleParked[word]&~idleReady[word]));
#ifndef NDEBUG
    for(const auto& ptr:npcArr) {
      const auto& npc = *ptr;
      if(!npc.simTick().idleParked)
        continue;
      const auto j = npc.societyDispatchIndex;
      const auto bit = uint64_t(1)<<(j%64);
      if(((idleReady[j/64]|idleUrgent[j/64].load(std::memory_order_relaxed))&bit)!=0)
        continue;
      assert(NpcSimTick::qualifies(npc,cfg.scope));
      assert(!NpcSimTick::due(simTickClock,dt,npc.pendingSimTick(),NpcSimTick::phase(npc,idlePeriod),idlePeriod));
      }
#endif
    }
  simDeferred += idleDeferred;
  simNative += profiled ? idleDeferred : 0;
  tierTicks[size_t(NpcSimTier::Simplified)] += idleDeferred;
  simTiers.nativeCombatNpcTicks += idleDeferred;
  FrameProfile::add(FrameProfile::Amount::NpcIdleWheelDeferred,idleDeferred);
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
