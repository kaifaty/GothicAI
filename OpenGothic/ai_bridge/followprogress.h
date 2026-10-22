#pragma once

#include <cmath>
#include <cstdint>

// Engine-independent follow progress. Times are simulation milliseconds.
class FollowProgress final {
  public:
    static constexpr uint64_t StuckMs = 2000;
    static constexpr float    MovedCm = 75.f;

    struct Point { float x=0, y=0, z=0; };

    bool update(uint64_t now, Point follower, Point leader) {
      const float distance = std::sqrt(distanceSquared(follower,leader));
      if(!initialized || distance<=bestDistance-MovedCm ||
         (distanceSquared(leader,leaderFrom)>MovedCm*MovedCm &&
          distanceSquared(follower,followerFrom)>=MovedCm*MovedCm)) {
        reset(now,follower,leader);
        }
      return idleMs(now)>=StuckMs;
      }

    void reset(uint64_t now, Point follower, Point leader) {
      initialized  = true;
      progressedAt = now;
      followerFrom = follower;
      leaderFrom   = leader;
      bestDistance = std::sqrt(distanceSquared(follower,leader));
      }

    uint64_t idleMs(uint64_t now) const { return initialized ? now-progressedAt : 0; }

  private:
    static float distanceSquared(Point a, Point b) {
      return (a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z);
      }

    bool     initialized = false;
    uint64_t progressedAt = 0;
    Point    followerFrom, leaderFrom;
    float    bestDistance = 0;
  };
