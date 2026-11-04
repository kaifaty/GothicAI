#pragma once

#include "navigationmemory.h"

#include <Tempest/Vec>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

// Follow guidance from the leader's trail. The follower remembers where it has
// seen the object it follows; the carrot moves along that short trail with a
// lookahead, like the road carrot, and never comes closer than the standoff to
// the object. A straight cut is taken only where the follower's own near memory
// shows the passage free. No world, physics or NavMesh access: the caller casts
// the single eye-to-body visibility ray and passes the learner's memory view.
class FollowTrail final {
  public:
    static constexpr float spacingCm = 25.f;    // a new trail point after this much object motion
    static constexpr float memoryCm = 4000.f;   // short memory: trail length kept behind its newest point
    static constexpr float standoffCm = 250.f;  // the carrot never comes closer to the object
    static constexpr float lookaheadCm = 600.f; // along the trail, from the follower's projection
    static constexpr float corridorCm = 20.f;   // no live hazard this close to a free passage
    static constexpr float footprintCm = 25.f;  // the follower's own footprint is not checked
    static constexpr float reachedCm = 50.f;    // the follower stands at the last seen point

    enum class Source : uint8_t {
      Hold,   // the follower is within the standoff: the carrot is the follower itself
      Direct, // the straight carrot before the object; near memory sees the passage free
      Cut,    // a trail point beyond the lookahead; near memory sees the passage free
      Trail,  // the trail point at the lookahead (capped by the standoff), pulled
              // back along the trail where near memory has seen a hazard before it
      Line,   // no trail point ahead is standoffCm from the object (it comes toward
              // the follower, or has not yet gone that far): the straight carrot
      Unseen, // never seen, or lost at the last seen point: the straight carrot
      Count
      };
    static const char* name(Source source);

    struct Point final {
      Tempest::Vec3 position = {};
      double arcCm = 0; // cumulative along the trail since the episode start
      };
    struct Result final {
      Tempest::Vec3 carrot = {};
      Source source = Source::Line;
      };

    void reset();
    // Once per decision, before guide(): the object's position and the result
    // of the follower's single eye-to-body ray. Unseen positions never enter the trail.
    void observe(const Tempest::Vec3& object, bool visible);
    // follower and object are physical feet positions, centimetres.
    Result guide(const Tempest::Vec3& follower, const Tempest::Vec3& object,
                 const NavigationMemory::View& memory);

    // True when every sample of the segment touches near-memory ground seen
    // free (live evidence) and no live hazard lies within corridorCm of it.
    // Points outside the near map are not seen. The first footprintCm from
    // `from` (the follower's own footprint) is skipped by both checks.
    static bool nearFree(const NavigationMemory::View& memory, const Tempest::Vec3& from,
                         const Tempest::Vec3& to);
    // True when the near memory has seen no live hazard within corridorCm of
    // the segment. Unknown cells and points outside the near map do not block.
    static bool nearClear(const NavigationMemory::View& memory, const Tempest::Vec3& from,
                          const Tempest::Vec3& to);

    const std::deque<Point>& points() const { return trail; }
    double progressCm() const { return progress; }
    bool lost() const { return lostAtEnd; }

  private:
    void advance(const Tempest::Vec3& follower);
    Tempest::Vec3 at(double arcCm) const;
    double standoffArc(const Tempest::Vec3& object) const;
    static bool passage(const NavigationMemory::View& memory, const Tempest::Vec3& from,
                        const Tempest::Vec3& to, bool seen);

    std::deque<Point> trail;
    double progress = 0;
    bool visibleNow = false, lostAtEnd = false;
  };
