#pragma once

#include "bloodstain.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Blood on the world (physics stage 8, step 4c; [GAME] blood). Research, target and
// acceptance: reports/physics/2026-11-02-blood/README.md.
//
// A stain is a projected decal plus a record: splashes from flesh hits, a pool that
// grows under a dead body, later a smear behind a dragged body (step 4d). Stains age
// on game time like real blood and leave the world when their lifetime ends.
// Human and animal blood is evidence: World::physicsTraces() reports it as `blood`.
// The rules here are pure so the native contract checks them.
namespace BloodStains {
  constexpr size_t  Limit           = 256;               // stains kept in a world
  constexpr float   SightRangeCm    = 1500.f;            // as other traces

  inline bool isEvidence(const Stain& s) {
    return s.blood==Blood::Red || s.blood==Blood::Yellow || s.blood==Blood::Gore;
    }

  inline bool expired(const Stain& s, int64_t now) {
    return now-s.bornAt>=lifetimeMs(s);
    }

  // Evidence (physics stage 8, step 4c): evidence stains closer than this to one another
  // (single linkage) are one `blood` trace.
  constexpr float   GroupLinkCm     = 200.f;
  constexpr float   SightLiftCm     = 5.f;   // sight points sit above the surface, along its normal

  // The older of two stains: by birth, then by id (ids grow with every new stain).
  inline bool older(const Stain& a, const Stain& b) {
    return a.bornAt!=b.bornAt ? a.bornAt<b.bornAt : a.id<b.id;
    }

  // A pool under a body that still lies in the world (`bodyPresent(bodyId)`, step 4b).
  template<class BodyPresent>
  bool heldByBody(const Stain& s, BodyPresent&& bodyPresent) {
    return s.kind==Kind::Pool && s.bodyId!=0 && bodyPresent(s.bodyId);
    }

  // Which stain leaves when a new one comes to a full world (Limit): the oldest; a pool
  // whose body is still in the world leaves last. stains.size() for an empty list.
  template<class BodyPresent>
  size_t evictIndex(const std::vector<Stain>& stains, BodyPresent&& bodyPresent) {
    size_t best     = stains.size();
    bool   bestHeld = true;
    for(size_t i=0;i<stains.size();++i) {
      const bool held = heldByBody(stains[i],bodyPresent);
      if(best==stains.size() || (bestHeld && !held) || (held==bestHeld && older(stains[i],stains[best]))) {
        best     = i;
        bestHeld = held;
        }
      }
    return best;
    }

  // Stains that go with the world's minute sweep: past their lifetime, or a pool whose
  // body has left the world (it leaves with the body).
  template<class BodyPresent>
  bool gone(const Stain& s, int64_t now, BodyPresent&& bodyPresent) {
    return expired(s,now) || (s.kind==Kind::Pool && s.bodyId!=0 && !bodyPresent(s.bodyId));
    }

  // World::addBloodStain: a new stain takes the next id (never reused, never 0) and, born
  // at 0, the current game time; at the Limit evictIndex makes room. [GAME] blood=0: nothing
  // is added, the id is 0.
  template<class BodyPresent>
  uint32_t add(std::vector<Stain>& stains, uint32_t& nextId, Stain s, int64_t now, bool enabled,
               BodyPresent&& bodyPresent) {
    if(!enabled)
      return 0;
    while(stains.size()>=Limit) {
      const auto i = evictIndex(stains,bodyPresent);
      if(i>=stains.size())
        break;
      stains.erase(stains.begin()+std::ptrdiff_t(i));
      }
    if(nextId==0)
      nextId = 1;
    s.id = nextId++;
    if(s.bornAt==0)
      s.bornAt = now;
    stains.push_back(s);
    return s.id;
    }

  // The world's minute sweep: stains past their lifetime go, a pool goes with its body;
  // [GAME] blood=0 leaves no stain behind, as on main.
  template<class BodyPresent>
  void sweep(std::vector<Stain>& stains, int64_t now, bool enabled, BodyPresent&& bodyPresent) {
    if(!enabled) {
      stains.clear();
      return;
      }
    stains.erase(std::remove_if(stains.begin(),stains.end(),[&](const Stain& s) {
      return gone(s,now,bodyPresent);
      }),stains.end());
    }

  // Evidence stains grouped by single linkage within GroupLinkCm. Indices into `stains`;
  // groups and members in the stains' order, so the result does not depend on a viewer.
  struct Group {
    std::vector<size_t> members;
    size_t oldest   = 0; // its trace id: "<world>:blood:<id of the oldest>"
    size_t largest  = 0; // its position and sight points
    size_t freshest = 0; // its age
    bool   heldByBody = false; // has a pool of a body still in the world
    };

  template<class BodyPresent>
  std::vector<Group> evidenceGroups(const std::vector<Stain>& stains, BodyPresent&& bodyPresent) {
    std::vector<size_t> parent(stains.size());
    for(size_t i=0;i<parent.size();++i)
      parent[i] = i;
    auto root = [&](size_t i) {
      while(parent[i]!=i)
        i = parent[i] = parent[parent[i]];
      return i;
      };
    for(size_t i=0;i<stains.size();++i) {
      if(!isEvidence(stains[i]))
        continue;
      for(size_t r=i+1;r<stains.size();++r) {
        if(!isEvidence(stains[r]) || (stains[i].pos-stains[r].pos).length()>GroupLinkCm)
          continue;
        const auto a = root(i), b = root(r);
        if(a!=b)
          parent[std::max(a,b)] = std::min(a,b);
        }
      }
    std::vector<Group> groups;
    std::vector<size_t> slot(stains.size(),size_t(-1));
    for(size_t i=0;i<stains.size();++i) {
      if(!isEvidence(stains[i]))
        continue;
      const auto r = root(i);
      if(slot[r]==size_t(-1)) {
        slot[r] = groups.size();
        groups.emplace_back();
        groups.back().oldest = groups.back().largest = groups.back().freshest = i;
        }
      auto& g = groups[slot[r]];
      g.members.push_back(i);
      const auto& s = stains[i];
      if(older(s,stains[g.oldest]))
        g.oldest = i;
      if(s.radiusCm>stains[g.largest].radiusCm || (s.radiusCm==stains[g.largest].radiusCm && older(s,stains[g.largest])))
        g.largest = i;
      if(older(stains[g.freshest],s))
        g.freshest = i;
      g.heldByBody = g.heldByBody || heldByBody(s,bodyPresent);
      }
    return groups;
    }

  // The groups that are `blood` traces: a group with the pool of a body still in the world
  // is not, the body is the evidence there already (no second find). Blood off: none.
  template<class BodyPresent>
  std::vector<Group> traceGroups(const std::vector<Stain>& stains, bool enabled, BodyPresent&& bodyPresent) {
    if(!enabled)
      return {}; // [GAME] blood=0: no `blood` traces
    auto groups = evidenceGroups(stains,bodyPresent);
    groups.erase(std::remove_if(groups.begin(),groups.end(),[](const Group& g) { return g.heldByBody; }),groups.end());
    return groups;
    }

  // The centre and four points on the rim of the stain, SightLiftCm off its surface.
  inline std::array<Tempest::Vec3,5> sightPoints(const Stain& s) {
    using Tempest::Vec3;
    auto n = s.normal.length()>1e-4f ? Vec3::normalize(s.normal) : Vec3(0,1,0);
    auto t = s.dir-n*Vec3::dotProduct(s.dir,n);
    if(t.length()<1e-3f)
      t = Vec3::crossProduct(n,std::abs(n.x)<.9f ? Vec3(1,0,0) : Vec3(0,0,1));
    t = Vec3::normalize(t);
    const auto b      = Vec3::crossProduct(n,t);
    const auto center = s.pos+n*SightLiftCm;
    const float r     = std::max(0.f,s.radiusCm);
    return {center, center+t*r, center-t*r, center+b*r, center-b*r};
    }
  }
