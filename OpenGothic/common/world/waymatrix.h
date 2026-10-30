#pragma once

#include <Tempest/Painter>
#include <Tempest/Matrix4x4>

#include "asset/worldtypes.h"

#include <vector>
#include <functional>
#include <array>
#include <span>

#include "waypath.h"
#include "waypoint.h"
#include "physics/dynamicworld.h"

class World;
class DbgPainter;

class WayMatrix final {
  public:
    WayMatrix(World& owner, const asset::WayNet& dat);

    const WayPoint* findWayPoint (const Tempest::Vec3& at, const std::function<bool(const WayPoint&)>& filter) const;
    const WayPoint* findFreePoint(const Tempest::Vec3& at, std::string_view name, const std::function<bool(const WayPoint&)>& filter) const;
    const WayPoint* findNextPoint(const Tempest::Vec3& at) const;

    struct RoutineEntry final {
      struct Candidate final {
        const WayPoint* point = nullptr;
        float           distance = 0;
        bool            passable = false;
        DynamicWorld::WalkResult walk;
        bool            excluded = false;
        };
      std::array<Candidate,8> candidates = {};
      size_t                 count = 0;
      size_t                 totalInRange = 0;
      const WayPoint*        accepted = nullptr;
      bool                   checked = false;
      size_t                 reselections = 0;
      bool                   exhausted = false;
      };
    RoutineEntry findRoutineEntry(const Tempest::Vec3& at,
                                  const std::function<DynamicWorld::WalkResult(const WayPoint&)>& walk,
                                  const std::function<bool(const WayPoint&)>& excluded = {}) const;
    static RoutineEntry findRoutineEntry(std::span<const WayPoint> points, const Tempest::Vec3& at,
                                         const std::function<DynamicWorld::WalkResult(const WayPoint&)>& walk,
                                         const std::function<bool(const WayPoint&)>& excluded = {});

    void            addFreePoint (const Tempest::Vec3& pos, const Tempest::Vec3& dir, std::string_view name);
    void            addStartPoint(const Tempest::Vec3& pos, const Tempest::Vec3& dir, std::string_view name);

    const WayPoint& startPoint() const;
    const WayPoint& deadPoint() const;
    void            buildIndex();

    const WayPoint* findWayPoint(std::string_view name) const;
    const WayPoint* findPoint(std::string_view name, bool inexact) const;
    void            marchPoints(DbgPainter& p) const;
    // every waypoint and freepoint of the world
    void            forEachPoint(const std::function<void(const WayPoint&)>& f) const;

    WayPath         wayTo(const WayPoint** begin, size_t beginSz, const Tempest::Vec3 exactBegin, const WayPoint& end) const;

  private:
    friend struct WorldDataContract;
    struct WayEdge {
      size_t a = 0;
      size_t b = 0;
      };

    World&                 world;
    // scripting doc says 20m, but number seems to be incorrect
    // Vatras requires at least 8.1 meters; Vatras is broken in vanilla
    // Keroloth requires less than 8.18 meters
    // Abuyin requires less than 10 meters
    // Harry(CoM) requires less(!) than 8 meters (~7.96)
    // Gothic 1 range is identical
    // Vanilla is buggy here as Vatras can't reach his praying spot from teaching location
    float                  distanceThreshold = 800.f;

    std::vector<WayEdge>   edges;

    std::vector<WayPoint>  wayPoints;
    std::vector<WayPoint>  freePoints, startPoints;
    std::vector<WayPoint*> indexPoints;

    std::vector<WayPoint*> fpInd;

    struct FpIndex {
      std::string                  key;
      std::vector<const WayPoint*> index;
      };
    mutable std::vector<FpIndex>          fpIndex;

    mutable uint16_t                      pathGen=0;
    mutable std::vector<const WayPoint*>  stk[2];

    void                   adjustWaypoints(std::vector<WayPoint> &wp);
    void                   calculateLadderPoints();

    const FpIndex&         findFpIndex(std::string_view name) const;
    const WayPoint*        findFreePoint(const Tempest::Vec3& at, const FpIndex &ind,
                                         const std::function<bool(const WayPoint&)>& filter) const;
  };
