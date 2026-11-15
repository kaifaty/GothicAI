#include "worldobjects.h"

#include "world/objects/npc.h"
#include "world/world.h"
#include "game/bodyarena.h"
#include "navdemos.h"
#include "frameprofile.h"

#include <algorithm>

void WorldObjects::updateSimTiers() {
  updateSimTiers(false);
  }

void WorldObjects::updateSimTiers(bool beforeTick) {
  FrameProfile::Scope timing(FrameProfile::Zone::NpcPreTick);
  bool needSort = false;
  int32_t previousId = 0;
  bool first = true;
  const auto  mode      = NpcSimTiers::mode();
  const auto* residency = owner.chunkResidency();
  const auto* pl        = owner.player();
  size_t   full = 0;
  uint64_t toFull = 0, toSimplified = 0;
  for(auto& ptr:npcArr) {
    auto&      npc   = *ptr;
    const auto pos   = npc.position();
    const bool hero  = (&npc==pl || npc.isPlayer());
    // Training and arena actors keep native simulation at every distance.
    const bool keep  = NavDemos::fullSimulation(npc) || BodyArena::isActor(npc);
    const auto prev  = npc.simTier();
    const auto next  = !hero && !keep && npc.simTierOverride().has_value() ? *npc.simTierOverride() :
                      NpcSimTiers::desired(mode,hero,keep,residency,ChunkId::fromPosition(pos.x,pos.z),prev);
    if(npc.hasSimTier() && next!=prev) {
      if(next==NpcSimTier::Full)
        ++toFull; else
        ++toSimplified;
      }
    npc.setSimTier(next);
    full += (next==NpcSimTier::Full);
    if(beforeTick) {
      const auto id = npc.handle().id;
      needSort |= !first && previousId>id;
      previousId = id;
      first = false;
      // Both participants are marked before anyone moves. Tier assignment cannot change the inputs
      // of refreshMeleeFighter; refreshing the unsorted array writes the same expiry to both actors.
      npc.refreshMeleeFighter();
      }
    }
  if(beforeTick && needSort) {
    FrameProfile::Scope sorting(FrameProfile::Zone::NpcSort);
    std::sort(npcArr.begin(),npcArr.end(),[](const auto& a,const auto& b) {
      return a->handle().id<b->handle().id;
      });
    }
  simTiers.mode                = mode;
  simTiers.full                = full;
  simTiers.simplified          = npcArr.size()-full;
  simTiers.toFull             += toFull;
  simTiers.toSimplified       += toSimplified;
  simTiers.transitionsLastTick = toFull+toSimplified;
  FrameProfile::add(FrameProfile::Amount::NpcFull,full);
  FrameProfile::add(FrameProfile::Amount::NpcSimplified,npcArr.size()-full);
  FrameProfile::add(FrameProfile::Amount::NpcTierTransitions,toFull+toSimplified);
  }


void WorldObjects::updateNpcPolicies(const Npc* player, const Tempest::Vec3& position) {
  FrameProfile::Scope timing(FrameProfile::Zone::NpcPolicy);
  npcNear.clear();
  preparePlan.entries.resize(npcArr.size());
  preparePlan.stamp = WorldWalk::revision(WorldWalk::Prepare);
  for(size_t at=0; at<npcArr.size(); ++at) {
    auto& npc = *npcArr[at];
    const float dist = (npc.position()-position).quadLength();
    if(dist<3000.f*3000.f || NavDemos::fullSimulation(npc)) {
      npcNear.push_back(&npc);
      if(&npc!=player)
        npc.setProcessPolicy(NpcProcessPolicy::AiNormal);
      } else if(dist<6000.f*6000.f) {
      npc.setProcessPolicy(NpcProcessPolicy::AiFar);
      } else {
      npc.setProcessPolicy(NpcProcessPolicy::AiFar2);
      }
    preparePlan.entries[at] = {&npc,npc.worldWalkPlan()};
    }
  }

void WorldObjects::prepareRagdolls() {
  const bool check = WorldWalk::checking();
  syncPlan.entries.resize(npcArr.size());
  syncPlan.stamp = WorldWalk::revision(WorldWalk::Sync);
  for(size_t at=0; at<npcArr.size(); ++at) {
    auto* npc = npcArr[at].get();
    if(preparePlan.skips(at,npc,WorldWalk::Prepare)) {
      syncPlan.entries[at] = preparePlan.entries[at];
      if(check)
        npc->checkSkippedWorldWalk(WorldWalk::Prepare);
      continue;
      }
    npc->prepareRagdoll();
    npc->prepareLivingBody();
    syncPlan.entries[at] = {npc,uint8_t(npc->hasRagdoll() ? WorldWalk::Sync : 0)};
    }
  }

void WorldObjects::syncRagdolls() {
  const bool check = WorldWalk::checking();
  for(size_t at=0; at<npcArr.size(); ++at) {
    auto* npc = npcArr[at].get();
    if(syncPlan.skips(at,npc,WorldWalk::Sync)) {
      if(check)
        npc->checkSkippedWorldWalk(WorldWalk::Sync);
      continue;
      }
    npc->syncRagdoll();
    }
  // No plan survives into the next world step or a standalone call from a tool.
  preparePlan.stamp = syncPlan.stamp = uint64_t(-1);
  }
