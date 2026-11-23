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
    static constexpr float corridorCm = 20.f;   // no static hazard this close to a passage
    static constexpr float pullbackMinCm = 100.f; // a pulled-back trail carrot stays this far away
    static constexpr float footprintCm = 25.f;  // the follower's own footprint is not checked
    static constexpr float reachedCm = 50.f;    // the follower stands at the last seen point

    enum class Source : uint8_t {
      Hold,   // the follower is within the standoff: the carrot is the follower itself
      Direct, // the straight carrot before the object; near memory sees the passage free
      Cut,    // a trail point beyond the lookahead; near memory sees the passage free
      Trail,  // the trail point at the lookahead (capped by the standoff), pulled
              // back along the trail (not nearer than pullbackMinCm) where near
              // memory has seen a static hazard before it
      Line,   // no trail point ahead is standoffCm from the object (it comes toward
              // the follower, or has not yet gone that far): the straight carrot
      Unseen,   // never seen, or lost at the last seen point: the straight carrot toward
                // the object (the companion hears it); never seen and closed by a seen
                // static hazard: turned aside up to 90 degrees, else stopped before it
      Heading,  // lost, the straight carrot is closed: where the object was last seen heading
      Aside,    // lost, both closed: the direction toward the object turned aside up to 90 degrees
      LastSeen, // lost, all closed: the last seen point
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
      // Diagnosis of the review fixes, never used to steer:
      // the forward standoff differs from the first rule's newest one (a loop of
      // the trail passes the object earlier); loopLine: that turned a trail or cut
      // carrot into the straight one; detour: the straight carrot toward an object
      // never seen was closed by a seen static hazard, so the carrot turned aside
      // or stopped before it; guarded: the 2.5 m guard fired.
      bool loopLimited = false, loopLine = false, detour = false, guarded = false;
      };

    void reset();
    // Lost with the straight and heading carrots closed: turn aside toward the
    // object (default), or stand at the last seen point (before 2026-11-04).
    // A stand comparison switch, never changed during an episode.
    void setLostAside(bool enabled) { lostAside = enabled; }
    // Once per decision, before guide(): the object's position and the result
    // of the follower's single eye-to-body ray. Unseen positions never enter the trail.
    void observe(const Tempest::Vec3& object, bool visible);
    // follower and object are physical feet positions, centimetres.
    Result guide(const Tempest::Vec3& follower, const Tempest::Vec3& object,
                 const NavigationMemory::View& memory);

    // True when every sample of the segment touches near-memory ground seen
    // free (live evidence) and no static hazard (wall, ledge, drop, water the
    // follower has seen) lies within corridorCm of it. Movable evidence does not
    // block: it is people and the followed object's own recent footprints.
    // Points outside the near map are not seen. The first footprintCm from
    // `from` (the follower's own footprint) is skipped by both checks.
    static bool nearFree(const NavigationMemory::View& memory, const Tempest::Vec3& from,
                         const Tempest::Vec3& to);
    // True when the near memory has seen no static hazard within corridorCm of
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
    double lastStandoffArc(const Tempest::Vec3& object) const;
    Result unseen(const Tempest::Vec3& follower, const Tempest::Vec3& object, const Tempest::Vec3& direct,
                  const NavigationMemory::View& memory) const;
    static Result checked(Result result, const Tempest::Vec3& object, const Tempest::Vec3& direct);
    static Tempest::Vec3 clearAlong(const NavigationMemory::View& memory, const Tempest::Vec3& from,
                                    const Tempest::Vec3& to);
    static bool passage(const NavigationMemory::View& memory, const Tempest::Vec3& from,
                        const Tempest::Vec3& to, bool seen);

    std::deque<Point> trail;
    double progress = 0;
    bool visibleNow = false, lostAtEnd = false, lostAside = true;
  };
