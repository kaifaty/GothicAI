#ifndef NAVMESH_H
#define NAVMESH_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/** Bench-only Recast/Detour mesh. All coordinates and dimensions are metres, Y up.
 * Nothing in the normal world load or NPC movement constructs this object. */
class NavMesh final {
  public:
    using Point = std::array<float,3>;
    struct Geometry {
      std::vector<float> vertices;
      std::vector<int>   triangles;
      std::vector<float> waterVertices;
      std::vector<int>   waterTriangles;
      };
    struct Bounds {
      Point min = {}, max = {};
      };
    enum class LinkType : uint8_t { Jump=1, Climb=2, Drop=3 };
    // A link is supplied only after a native NPC movement probe has succeeded.
    // Reverse traversal needs its own probe, or an explicitly verified bidirectional link.
    struct OffMeshLink {
      Point start = {}, end = {};
      float radius = 0.6f;
      LinkType type = LinkType::Jump;
      bool bidirectional = false;
      uint32_t id = 0;
      };
    struct Params {
      float radius = 0.2f;
      float height = 0.9f;
      float climb = 0.5f;
      float slope = 45.f;
      float cellSize = 0.2f;
      float cellHeight = 0.1f;
      float waterDepth = 0.5f;
      int   tileSize = 128;
      // Empty means the complete world; optional regions support bounded builds.
      std::vector<Bounds> regions;
      float maxBuildSeconds = 600.f;
      float nearestHorizontal = 1.f;
      float nearestVertical = 2.f;
      std::vector<OffMeshLink> links;
      };
    struct Stats {
      std::string key;
      std::filesystem::path cacheFile;
      bool fromCache = false;
      bool complete = false;
      double buildSeconds = 0;
      double loadSeconds = 0;
      uint64_t cacheBytes = 0;
      uint64_t triangles = 0;
      uint64_t waterTriangles = 0;
      uint64_t tiles = 0;
      uint64_t polygons = 0;
      uint64_t tilesConsidered = 0;
      };
    struct Path {
      struct Transition {
        Point start = {}, end = {};
        LinkType type = LinkType::Jump;
        uint32_t id = 0;
        // Both endpoints are explicit corners: start at this index, end at index+1.
        size_t cornerIndex = 0;
        };
      bool complete = false;
      std::string reason;
      std::vector<Point> corners;
      std::vector<Transition> transitions;
      float length = 0;
      float startSnapDistance = 0;
      float endSnapDistance = 0;
      };
    struct GapCandidate {
      Point start = {}, end = {};
      float horizontalDistance = 0;
      float verticalDistance = 0;
      };
    struct ShortcutCandidate : GapCandidate {
      bool groundComplete = false;
      float groundDistance = 0;
      };
    struct ShortcutSearch {
      std::vector<ShortcutCandidate> candidates;
      size_t boundarySamples = 0, geometricSites = 0, groundQueries = 0;
      size_t queryBudget = 0;
      };
    struct TakeoffSite {
      bool valid = false;
      std::string reason;
      Point snapped = {}, inset = {};
      uint64_t sourcePolygon = 0;
      float sourceHorizontalDistance = 0, sourceVerticalDistance = 0;
      float sourceWallDistance = 0, insetWallDistance = 0, insetDistance = 0;
      };
    struct GroundLocation {
      bool known = false;
      Point snapped = {};
      uint64_t polygon = 0, component = 0;
      float snapDistance = 0, horizontalDistance = 0, verticalOffset = 0;
      };
    struct TransitionSides {
      GroundLocation source, end, current;
      bool takeoffSide = false, landingSide = false;
      const char* method = "ambiguous";
      };

    // Privileged evaluation-only 2.5D truth raster. Ground is sampled at exact
    // native cell centres, never by snapping an uncovered XZ point across a gap.
    // Off-mesh jump/climb/drop links do not make ground cells traversable.
    struct RasterCell {
      bool traversable = false, heightValid = false;
      float groundHeight = 0; // world metres; valid only when heightValid
      uint64_t polygon = 0;
      bool heightBoundary = false; // blocked ground/floor seam, not free ground
      };
    struct RasterLayer {
      size_t size = 0;
      float cellSize = 0; // metres
      // Row-major [forward,right], centre=(index+.5-size/2)*cellSize.
      std::vector<RasterCell> cells;
      };
    struct LocalRaster {
      bool supportValid = false, supportExact = false;
      Point support = {};
      uint64_t supportPolygon = 0;
      float supportSnapDistance = 0, supportHorizontalDistance = 0, supportVerticalOffset = 0;
      RasterLayer near, far; // 64x64 at .25m; 100x100 at 2m
      };
    // bodyYawDegrees: NPC convention, 0=+X, 90=+Z; positive right maps to
    // (-sin(yaw),0,cos(yaw)). Proper floors are selected by ground-only walk
    // distance from the support anchor, without a global Y cutoff. The anchor
    // alone may snap by nearestHorizontal and climb+2*cellHeight; this handles
    // physical feet outside already eroded mesh edges without filling any gap.
    // Mesh erosion (Params::radius) is retained; no second erosion is applied.
    // Quantized edges that cannot join their exact selected ground polygons are
    // conservatively blocked, including near/far seams and stacked floor edges.
    LocalRaster rasterLocal(const Point& feet, float bodyYawDegrees) const;

    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;
    NavMesh(NavMesh&&) noexcept;
    NavMesh& operator=(NavMesh&&) noexcept;

    void buildOrLoad(const Geometry& geometry, const Params& params,
                     const std::filesystem::path& cacheDirectory, const std::string& worldName);
    static uint64_t cacheKey(const Geometry& geometry, const Params& params, const std::string& worldName);
    Path findPath(const Point& start, const Point& end, bool allowTransitions=true) const;
    // A ground ray must reach the end's actual polygon. Detour's raycast alone ignores
    // end height; checking its final polygon also rejects a balcony above the ray.
    // Off-mesh links never make a segment directly walkable.
    bool straightWalkable(const Point& start, const Point& end) const;
    // Nearest opposing boundaries of the two disconnected ground components.
    // Candidates are geometric probe sites, never permission to add a link.
    std::vector<GapCandidate> gapCandidates(const Point& start, const Point& end,
                                          float maxHorizontal=4.f, float maxVertical=3.f,
                                          size_t maxCandidates=8) const;
    // Whole-world boundary search, including shortcuts within one ground component.
    // No candidate is installed as an off-mesh link. Sites and query work are bounded,
    // and selected round-robin over 50m world cells rather than nearest-first globally.
    ShortcutSearch shortcutCandidates(size_t siteBudget=1200, float maxHorizontal=4.f,
                                      float maxVertical=3.f) const;
    // Local, ground-only takeoff preparation. Never searches another storey or
    // crosses the selected convex polygon: move at most insetDistance towards
    // its interior, then let the native probe resolve its bounded physical floor.
    TakeoffSite takeoffSite(const Point& source, float insetDistance=.15f,
                            float maxHorizontalSnap=.1f, float maxVerticalSnap=.25f) const;
    // Diagnostic projection onto the 3D-closest ground polygon in the existing
    // nearest-poly query extents. Snap distances are metrics, never side gates.
    GroundLocation groundLocation(const Point& position) const;
    // Stand placement candidates in a 2-6m horizontal annulus. Native ground Y
    // is retained; the source ground component is preferred, then 3D distance.
    // At most 80 deterministic samples; no random state or off-mesh links.
    std::vector<GroundLocation> nearbyGroundLocations(const Point& position) const;
    // Distinct ground components identify the two banks. Within one component,
    // only the exact endpoint polygon identifies a side; coincident endpoint
    // polygons are explicitly ambiguous. Proven off-mesh links never merge banks.
    TransitionSides transitionSides(const Point& source, const Point& end, const Point& current) const;
    void writePolygons(const std::filesystem::path& output) const;
    const Stats& stats() const;
    const Params& params() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
  };

#endif
