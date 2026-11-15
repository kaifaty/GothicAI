#include "worldobjects.h"

#include "world/objects/npc.h"
#include "world/world.h"
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
    const bool keep  = keepsFullSimulation(npc);
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
  for(size_t at=0; at<npcArr.size(); ++at) {
    auto& npc = *npcArr[at];
    const float dist = (npc.position()-position).quadLength();
    if(dist<3000.f*3000.f || keepsNearPolicy(npc)) {
      npcNear.push_back(&npc);
      if(&npc!=player)
        npc.setProcessPolicy(NpcProcessPolicy::AiNormal);
      } else if(dist<6000.f*6000.f) {
      npc.setProcessPolicy(NpcProcessPolicy::AiFar);
      } else {
      npc.setProcessPolicy(NpcProcessPolicy::AiFar2);
      }
    }
  }
