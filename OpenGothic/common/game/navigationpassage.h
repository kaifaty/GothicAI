#pragma once

#include "navigationmemory.h"

#include <Tempest/Vec>

#include <cstdint>

// A straight passage through the walker's own visual memory (NavigationMemory::View).
// No world, physics or NavMesh access. The follow trail checks its cuts on the
// near map; familiar-road guidance checks a cut of the road and the start of a
// rebuilt route on the near map and, beyond it, on the far map.
namespace NavigationPassage {
  enum class Verdict : uint8_t {
    Unknown, // neither: unseen ground somewhere on the segment
    Free,    // every sample lies on ground the walker has seen free
    Closed,  // the walker has seen a static hazard on the segment
    };

  // A wall, ledge, drop or water the walker has seen itself. A familiar tile
  // is a prior, exactly as in the memory teacher, and never counts.
  bool staticHazard(const NavigationMemory::Cell& cell);
  bool seenFree(const NavigationMemory::Cell& cell);

  // Near map only. seen: every sample needs seen free ground within corridorCm
  // and none may have a static hazard there; points outside the near map are
  // not seen. !seen: only a static hazard within corridorCm fails; unknown cells
  // and points outside the near map do not. The first footprintCm from `from`
  // is skipped by both. This is FollowTrail::nearFree / nearClear.
  bool near(const NavigationMemory::View& memory, const Tempest::Vec3& from, const Tempest::Vec3& to,
            bool seen, float corridorCm, float footprintCm);

  // The near map where a sample lies inside it, the far map beyond. On the near
  // map a cell counts as in near(); a far cell is 2 m wide and often holds both
  // a road and the wall beside it, so it closes the passage only when it shows a
  // static hazard and no free ground, and is free only without a hazard. A
  // sample outside both maps is unseen.
  Verdict check(const NavigationMemory::View& memory, const Tempest::Vec3& from, const Tempest::Vec3& to,
                float corridorCm, float footprintCm);
}
