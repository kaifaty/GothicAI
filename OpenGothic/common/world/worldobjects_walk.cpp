#include "worldobjects.h"

#include "world/objects/npc.h"
#include "world/world.h"
#include "worldchunkmanifest.h"
#include "frameprofile.h"

#include <algorithm>
#include <cassert>

void WorldObjects::invalidateNpcOrder() {
  npcDispatchDirty = true;
  npcCellsDirty = true;
  npcSortDirty = true;
  idleCadenceInvalid = true;
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
  const auto* previous = existing!=npcCellMembers.end() ? &existing->second : nullptr;
  const auto override = npc.simTierOverride();
  const auto policy = npc.processPolicy();
  const bool combat = npc.target()!=nullptr;
  const bool bodyCandidate = npc.hasCampfireActivity() || npc.bodySuspended() ||
      (AnimationLod::config().enabled && NpcSimTiers::mode()==NpcSimTiers::Mode::Auto && npc.hasRestingBodyActivity());
  if(!previous || previous->key!=key || previous->tierOverride!=override)
    npcTierChanges.insert(&npc);
  if(!previous || previous->position!=position || previous->policy!=policy)
    npcPolicyChanges.insert(&npc);
  if(!previous || previous->combat!=combat) {
    if(combat)
      npcCombat.insert(&npc);
    else
      npcCombat.erase(&npc);
    }
  if(!previous || previous->bodyCandidate!=bodyCandidate) {
    if(bodyCandidate)
      npcBodyCandidates.insert(&npc);
    else
      npcBodyCandidates.erase(&npc);
    }
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

  const NpcCellMember next{key,position,lo,hi,override,policy,deferred,combat,bodyCandidate};
  if(previous) {
    auto& member = existing->second;
    auto& cell   = npcCells.at(member.key);
    if(member.key==key) {
      if(member.deferred!=deferred || (deferred && (member.lo!=lo || member.hi!=hi))) {
        cell.deferred -= size_t(member.deferred);
        cell.deferred += size_t(deferred);
        cell.dirtyBounds = true;
        }
      member = next;
      return;
      }
    std::erase(cell.actors,&npc);
    cell.deferred -= size_t(member.deferred);
    cell.dirtyBounds = true;
    if(cell.actors.empty())
      npcCells.erase(member.key);
    }

  auto& cell = npcCells[key];
  cell.actors.push_back(&npc);
  cell.deferred += size_t(deferred);
  cell.dirtyBounds = true;
  npcCellMembers.insert_or_assign(&npc,next);
  }

void WorldObjects::refreshNpcCells() {
  // The animation workers have joined and NPC list mutations are on this thread.
  if(npcCellsDirty) {
    npcCellChanges.clear();
    npcCells.clear();
    npcCellMembers.clear();
    societyPoseDebts.clear();
    npcTierChanges.clear();
    npcPolicyChanges.clear();
    npcCombat.clear();
    npcBodyCandidates.clear();
    npcTiersRebuild = true;
    npcPoliciesRebuild = true;
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

void WorldObjects::orderNpcCandidates(const std::unordered_set<Npc*>& candidates) {
  refreshSocietyDispatch();
  npcSpatialOrder.assign(candidates.begin(),candidates.end());
  std::sort(npcSpatialOrder.begin(),npcSpatialOrder.end(),[](const Npc* a,const Npc* b) {
    return a->societyDispatchIndex<b->societyDispatchIndex;
    });
  }

void WorldObjects::updateSimTiers() {
  updateSimTiers(false);
  }

void WorldObjects::updateSimTiers(bool beforeTick) {
  FrameProfile::Scope timing(FrameProfile::Zone::NpcPreTick);
  if(beforeTick && npcSortDirty) {
    const auto less = [](const auto& a,const auto& b) { return a->handle().id<b->handle().id; };
    if(!std::is_sorted(npcArr.begin(),npcArr.end(),less)) {
      FrameProfile::Scope sorting(FrameProfile::Zone::NpcSort);
      invalidateNpcOrder();
      std::sort(npcArr.begin(),npcArr.end(),less);
      }
    npcSortDirty = false;
    }
  refreshNpcCells();
  const auto  mode      = NpcSimTiers::mode();
  const auto* residency = owner.chunkResidency();
  const auto* pl        = owner.player();
  const bool  fixture   = npcSimulationFixtureActive();
  std::optional<std::pair<ChunkId,ChunkId>> focus;
  if(residency && residency->viewKnown()) {
    const auto& h = residency->viewHero();
    const auto& c = residency->viewCamera();
    focus = std::pair{ChunkId::fromPosition(h.x,h.z),ChunkId::fromPosition(c.x,c.z)};
    }
  const auto cellTiers = [&](NpcCellKey key) {
    return std::array{
      NpcSimTiers::desired(mode,false,false,residency,{key.first,key.second},NpcSimTier::Full),
      NpcSimTiers::desired(mode,false,false,residency,{key.first,key.second},NpcSimTier::Simplified)};
    };
  const bool recount = npcTiersRebuild;
  const bool all = recount || !beforeTick || fixture || npcTierFixture || npcTierHero!=pl;
  if(npcTierFocus!=focus || npcTierResidency!=residency) {
    // Only Near matters to simulation. Far and Outside can change within a
    // focus cell without changing either tier outcome.
    for(auto& [key,cell]:npcCells) {
      const auto next = cellTiers(key);
      if(cell.tiers!=next && !all)
        npcTierChanges.insert(cell.actors.begin(),cell.actors.end());
      cell.tiers = next;
      }
    }
  npcTierFocus = focus;
  npcTierResidency = residency;
  npcTierHero = pl;
  npcTierFixture = fixture;

  if(all) {
    npcSpatialOrder.clear();
    for(auto& npc:npcArr)
      npcSpatialOrder.push_back(npc.get());
    } else {
    if(beforeTick)
      npcTierChanges.insert(npcCombat.begin(),npcCombat.end());
    npcTierChanges.insert(npcBodyCandidates.begin(),npcBodyCandidates.end());
    orderNpcCandidates(npcTierChanges);
    }
  npcTierChanges.clear();
  npcTiersRebuild = false;

  size_t full = recount ? 0 : simTiers.full;
  uint64_t toFull = 0, toSimplified = 0;
  npcBodiesSuspended = false;
  std::optional<AnimationLod::View> bodyView;
  bool hasBodyView = false;
  for(auto* ptr:npcSpatialOrder) {
    auto& npc = *ptr;
    const auto key = npcCellMembers.at(&npc).key;
    auto& cell = npcCells.at(key);
    if(!cell.tiers)
      cell.tiers = cellTiers(key);
    const bool hero = (&npc==pl || npc.isPlayer());
    const bool keep = fixture && keepsFullSimulation(npc);
    const auto prev = npc.simTier();
    const auto next = hero || keep ? NpcSimTier::Full :
                      npc.simTierOverride().value_or((*cell.tiers)[size_t(prev)]);
    if(npc.hasSimTier() && next!=prev) {
      if(next==NpcSimTier::Full)
        ++toFull;
      else
        ++toSimplified;
      }
    if(!recount && npc.hasSimTier())
      full -= size_t(prev==NpcSimTier::Full);
    npc.applySimTier(next);
    full += size_t(next==NpcSimTier::Full);
    if(beforeTick && npc.target()!=nullptr)
      npc.refreshMeleeFighter();
    if(!npcBodyCandidates.contains(&npc))
      continue;
    bool suspend = false;
    bool resting = false;
    const bool campfire = npc.hasCampfireActivity();
    const bool restActivity = AnimationLod::config().enabled && npc.hasRestingBodyActivity();
    if(!campfire && !restActivity)
      noteNpcCellChange(npc); // A timed wait can expire without another change to the cell.
    if(mode==NpcSimTiers::Mode::Auto && residency && next==NpcSimTier::Simplified) {
      resting = !campfire && restActivity && npc.canSuspendRestingBody() && npc.canPauseDistantPose(0);
      if(resting || (campfire && npc.canSuspendCampfireBody())) {
        if(!bodyView) {
          bodyView.emplace();
          hasBodyView = makeAnimationLodView(*bodyView);
          }
        suspend = hasBodyView && !bodyNeedsSimulation(npc,*bodyView,resting);
        }
      }
    if(suspend)
      npcBodiesSuspended |= npc.suspendDistantBody(resting);
    else if(npc.bodySuspended())
      npc.resumeBody();
    }
#ifndef NDEBUG
  for(const auto& ptr:npcArr) {
    const auto& npc = *ptr;
    const auto pos  = npc.position();
    const bool hero = &npc==pl || npc.isPlayer();
    const bool keep = keepsFullSimulation(npc);
    const auto expected = !hero && !keep && npc.simTierOverride() ? *npc.simTierOverride() :
                          NpcSimTiers::desired(mode,hero,keep,residency,ChunkId::fromPosition(pos.x,pos.z),npc.simTier());
    assert(npc.simTier()==expected);
    }
#endif
  simTiers.mode                = mode;
  simTiers.full                = full;
  simTiers.simplified          = npcArr.size()-full;
  simTiers.toFull             += toFull;
  simTiers.toSimplified       += toSimplified;
  simTiers.transitionsLastTick = toFull+toSimplified;
  FrameProfile::add(FrameProfile::Amount::NpcFull,full);
  FrameProfile::add(FrameProfile::Amount::NpcSimplified,npcArr.size()-full);
  FrameProfile::add(FrameProfile::Amount::NpcTierTransitions,toFull+toSimplified);
  FrameProfile::add(FrameProfile::Amount::NpcTierCandidates,npcSpatialOrder.size());
  }

void WorldObjects::updateNpcPolicies(const Npc* player, const Tempest::Vec3& position) {
  FrameProfile::Scope timing(FrameProfile::Zone::NpcPolicy);
  refreshNpcCells();
  const bool fixture = npcSimulationFixtureActive();
  bool nearChanged = npcPoliciesRebuild;
  if(npcPoliciesRebuild)
    npcNearMembers.clear();
  if(npcPoliciesRebuild || fixture || npcPolicyFixture || player!=npcPolicyHero) {
    npcSpatialOrder.clear();
    for(auto& npc:npcArr)
      npcSpatialOrder.push_back(npc.get());
    } else {
    if(npcPolicyPosition && *npcPolicyPosition!=position) {
      // Revisit the old and new rings so a teleport also demotes actors left
      // behind. Cells are only a broad phase; the policy still uses 3D distance.
      const auto collect = [&](const Tempest::Vec3& center) {
        const auto lo = ChunkId::fromPosition(center.x-6000.f,center.z-6000.f);
        const auto hi = ChunkId::fromPosition(center.x+6000.f,center.z+6000.f);
        for(int32_t x=lo.x; x<=hi.x; ++x)
          for(int32_t z=lo.z; z<=hi.z; ++z) {
            const auto cell = npcCells.find({x,z});
            if(cell!=npcCells.end())
              npcPolicyChanges.insert(cell->second.actors.begin(),cell->second.actors.end());
            }
        };
      collect(*npcPolicyPosition);
      collect(position);
      }
    orderNpcCandidates(npcPolicyChanges);
    }
  npcPolicyChanges.clear();
  npcPoliciesRebuild = false;
  npcPolicyHero = player;
  npcPolicyPosition = position;
  npcPolicyFixture = fixture;

  for(auto* npc:npcSpatialOrder) {
    const float dist = (npc->position()-position).quadLength();
    if(dist<NpcNearPolicyRadius*NpcNearPolicyRadius || (fixture && keepsNearPolicy(*npc))) {
      nearChanged |= npcNearMembers.insert(npc).second;
      if(npc!=player)
        npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
      } else {
      nearChanged |= npcNearMembers.erase(npc)!=0;
      npc->setProcessPolicy(dist<6000.f*6000.f ? NpcProcessPolicy::AiFar : NpcProcessPolicy::AiFar2);
      }
    }
  FrameProfile::add(FrameProfile::Amount::NpcPolicyCandidates,npcSpatialOrder.size());
  if(nearChanged) {
    orderNpcCandidates(npcNearMembers);
    npcNear.assign(npcSpatialOrder.begin(),npcSpatialOrder.end());
    }
#ifndef NDEBUG
  size_t near = 0;
  for(const auto& ptr:npcArr) {
    const auto dist = (ptr->position()-position).quadLength();
    if(dist<NpcNearPolicyRadius*NpcNearPolicyRadius || keepsNearPolicy(*ptr)) {
      assert(near<npcNear.size() && npcNear[near++]==ptr.get());
      assert(ptr.get()==player || ptr->processPolicy()==NpcProcessPolicy::AiNormal);
      } else {
      assert(ptr->processPolicy()==(dist<6000.f*6000.f ? NpcProcessPolicy::AiFar : NpcProcessPolicy::AiFar2));
      }
    }
  assert(near==npcNear.size());
#endif
  }
