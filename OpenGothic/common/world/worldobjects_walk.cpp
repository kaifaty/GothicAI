#include "worldobjects.h"

#include "world/objects/npc.h"
#include "world/world.h"
#include "worldchunkmanifest.h"
#include "frameprofile.h"

#include <algorithm>

void WorldObjects::invalidateNpcOrder() {
  npcDispatchDirty = true;
  npcCellsDirty = true;
  }

void WorldObjects::noteNpcCellChange(Npc& npc) {
  if(npcCellsDirty || !npcCellMembers.contains(&npc))
    return;
  std::lock_guard guard(npcCellChangeLock);
  if(npc.npcCellChangePending)
    return;
  npc.npcCellChangePending = true;
  npcCellChanges.push_back(&npc);
  }

void WorldObjects::updateNpcCell(Npc& npc) {
  npc.npcCellChangePending = false;
  const auto position = npc.position();
  const auto chunk = ChunkId::fromPosition(position.x,position.z);
  const NpcCellKey key{chunk.x,chunk.z};
  const auto existing = npcCellMembers.find(&npc);
  const bool deferred = npc.hasDeferredAnimation();
  Tempest::Vec3 lo = position, hi = position;
  if(deferred) {
    AnimationLod::View::bodyBounds(npc,lo,hi);
    lo = {std::min(lo.x,position.x),std::min(lo.y,position.y),std::min(lo.z,position.z)};
    hi = {std::max(hi.x,position.x),std::max(hi.y,position.y),std::max(hi.z,position.z)};
    }
  if(deferred && npc.hasSocietyAnimationDebt())
    societyPoseDebts.insert(&npc);
  else
    societyPoseDebts.erase(&npc);

  if(existing!=npcCellMembers.end()) {
    auto& member = existing->second;
    auto& cell   = npcCells.at(member.key);
    if(member.key==key) {
      if(member.deferred!=deferred || (deferred && (member.lo!=lo || member.hi!=hi))) {
        cell.deferred -= size_t(member.deferred);
        cell.deferred += size_t(deferred);
        cell.dirtyBounds = true;
        }
      member = {key,lo,hi,deferred};
      return;
      }
    std::erase(cell.actors,&npc);
    cell.deferred -= size_t(member.deferred);
    cell.dirtyBounds = true;
    if(cell.actors.empty())
      npcCells.erase(member.key);
    } else if(!npcCellsDirty) {
    // Construction can touch position before the NPC joins npcArr.
    return;
    }

  auto& cell = npcCells[key];
  cell.actors.push_back(&npc);
  cell.deferred += size_t(deferred);
  cell.dirtyBounds = true;
  npcCellMembers.insert_or_assign(&npc,NpcCellMember{key,lo,hi,deferred});
  }

void WorldObjects::refreshNpcCells() {
  // The animation workers have joined and NPC list mutations are on this thread.
  if(npcCellsDirty) {
    npcCellChanges.clear();
    npcCells.clear();
    npcCellMembers.clear();
    societyPoseDebts.clear();
    for(auto& npc:npcArr)
      updateNpcCell(*npc);
    npcCellsDirty = false;
    return;
    }
  for(auto* npc:npcCellChanges)
    updateNpcCell(*npc);
  npcCellChanges.clear();
  }

void WorldObjects::collectLatePoses(const AnimationLod::View& view, bool persistent) {
  refreshSocietyDispatch();
  refreshNpcCells();
  if(!latePoseView || !latePoseView->sameView(view)) {
    latePoseView = view;
    ++latePoseViewRevision;
    }
  latePoses.clear();
  for(auto& [key,cell]:npcCells) {
    if(cell.deferred==0)
      continue;
    if(cell.dirtyBounds) {
      bool first = true;
      for(auto* npc:cell.actors) {
        const auto& member = npcCellMembers.at(npc);
        if(!member.deferred)
          continue;
        if(first) {
          cell.lo = member.lo;
          cell.hi = member.hi;
          first = false;
          } else {
          cell.lo = {std::min(cell.lo.x,member.lo.x),std::min(cell.lo.y,member.lo.y),std::min(cell.lo.z,member.lo.z)};
          cell.hi = {std::max(cell.hi.x,member.hi.x),std::max(cell.hi.y,member.hi.y),std::max(cell.hi.z,member.hi.z)};
          }
        }
      }
    if(cell.dirtyBounds || cell.viewRevision!=latePoseViewRevision) {
      cell.couldSee     = view.couldSee(cell.lo,cell.hi);
      cell.viewRevision = latePoseViewRevision;
      cell.dirtyBounds  = false;
      }
    if(!cell.couldSee)
      continue;
    for(auto* npc:cell.actors)
      if(npc->hasDeferredAnimation() && !npc->hasSocietyAnimationDebt())
        latePoses.push_back(npc);
    }
  // Society can resume a pose for an interaction outside the view.
  for(auto* npc:societyPoseDebts)
    if(!persistent || !npc->societyPoseSuspended)
      latePoses.push_back(npc);
  std::sort(latePoses.begin(),latePoses.end(),[](const Npc* a,const Npc* b) {
    return a->societyDispatchIndex<b->societyDispatchIndex;
    });
  }

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
  campfireBodiesSuspended = false;
  std::optional<AnimationLod::View> campfireView;
  bool hasCampfireView = false;
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
    bool suspend = false;
    if(mode==NpcSimTiers::Mode::Auto && residency && next==NpcSimTier::Simplified && npc.canSuspendCampfireBody()) {
      if(!campfireView) {
        campfireView.emplace();
        hasCampfireView = makeAnimationLodView(*campfireView);
        }
      suspend = hasCampfireView && !mustUpdateAnimation(npc,*campfireView) && !campfireView->visible(npc);
      }
    if(suspend) {
      campfireBodiesSuspended |= npc.suspendCampfireBody();
      } else if(npc.campfireBodySuspended()) {
      npc.resumeCampfireBody();
      }
    }
  if(beforeTick && needSort) {
    FrameProfile::Scope sorting(FrameProfile::Zone::NpcSort);
    invalidateNpcOrder();
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
