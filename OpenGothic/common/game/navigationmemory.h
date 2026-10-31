#pragma once

#include "navigationeye.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Visual memory. Both the live grids and the familiar map accept only the eye's
// observations and the actor's own body pose; no world or physics access.
class NavigationMemory final {
  public:
    static constexpr uint32_t RepresentationVersion = 4;

    struct Constants final {
      static constexpr size_t nearSize = 64, farSize = 100;
      static constexpr float nearCellCm = 25.f, farCellCm = 200.f;
      static constexpr float staticTauSeconds = 300.f, movableTauSeconds = 5.f;
      static constexpr float distanceLambdaCm = 30000.f;
      static constexpr float familiarConfidence = .35f;
      static constexpr float stepCm = 50.f;
      static constexpr float maxSlopeDegrees = 45.f, bodyHeightCm = 180.f;
      static constexpr float groundNormalY = .7f, wallNormalY = .3f;
      static constexpr float clearHeightCm = 25.f;
      };

    struct Pose final {
      Tempest::Vec3 position = {}; // feet, centimetres
      Tempest::Vec3 forward = {1.f,0.f,0.f}; // body, not head; XZ only
      uint64_t gameMs = 0;
      // The same guild limits passed to CharacterVirtual, supplied by the caller.
      float stepHeightCm = Constants::stepCm;
      float maxSlopeDegrees = Constants::maxSlopeDegrees;
      float bodyHeightCm = Constants::bodyHeightCm;
      };

    struct Cell final {
      float confidence = 0, free = 0, wall = 0, ledge = 0, drop = 0, water = 0, movable = 0;
      float familiar = 0;
      float groundHeightCm = std::numeric_limits<float>::quiet_NaN();
      };

    struct FamiliarTile final {
      enum : uint8_t { Free=1, Wall=2, Ledge=4, Drop=8, Water=16 };
      int32_t x = 0, z = 0; // world coordinates at farCellCm resolution
      float referenceHeightCm = 0;
      float groundHeightCm = std::numeric_limits<float>::quiet_NaN();
      uint32_t source = 0; // eye pose that produced this tile, for familiarity eligibility
      uint8_t channels = 0;
      };

    class FamiliarMap final {
      public:
        std::vector<FamiliarTile> tiles;
        // The source is a classified live memory made by the same eye. Bake
        // appends records; finish once before assigning stable tile-mask bits.
        void bake(const NavigationMemory& observed, uint32_t source = 0);
        void finish();
        size_t storageBytes() const { return tiles.capacity()*sizeof(FamiliarTile); }
      private:
        bool finished = false;
        friend class NavigationMemory;
      };
    using FamiliarMask = std::vector<uint64_t>;

    // Row-major [forward * size + right]. Cell centres in the current body frame
    // are (index + .5 - size/2) * cellCm. Unknown cells have confidence zero.
    struct Layer final {
      size_t size = 0;
      float cellCm = 0;
      std::vector<Cell> cells;
      };

    struct View final {
      Pose pose;
      double traveledCm = 0;
      Layer near, far;
      };

    NavigationMemory();
    // Clears live observations; familiar knowledge survives a new eye session.
    void reset();
    // Bench comparison with v3 decay; the representation and classifier stay v4.
    void setJourneyMemory(bool enabled) { journeyMemory = enabled; }
    bool journeyMemoryEnabled() const { return journeyMemory; }
    bool familiarMapEnabled() const { return familiarMap && !familiarMask.empty(); }
    uint32_t representationVersion() const { return journeyMemory || familiarMapEnabled() ? RepresentationVersion : 3; }
    // The map outlives this memory. Empty mask means no familiar knowledge.
    void setFamiliarMap(const FamiliarMap* map, FamiliarMask mask);
    // Call for every body tick, including ticks without new eye observations.
    // A stable ray order (NavigationEye::samples()) lets cached rays be skipped.
    void update(const std::vector<NavigationEye::Ray>& rays, const Pose& pose);
    // Reuses the caller's allocations. Rotation happens only at readout, never
    // in the stored world grids. Sparse observations are splatted, not blurred.
    void view(View& result) const;
    const Pose& pose() const { return body; }
    double traveledCm() const { return pathCm; }

  private:
    friend class NavMemoryCostTrace;
    enum Channel : uint8_t { Free, Clearance, Ground, Wall, Water, Movable, Surface, ChannelCount };
    struct Evidence final {
      uint64_t gameMs = 0;
      float pathCm = 0, heightCm = 0;
      float hitX = 0, hitZ = 0;
      };
    struct WorldCell final {
      std::array<Evidence,ChannelCount> evidence = {};
      uint8_t known = 0;
      };
    struct Grid final {
      size_t size = 0;
      float cellCm = 0;
      int minX = 0, minZ = 0;
      bool positioned = false;
      std::vector<WorldCell> cells;

      Grid(size_t size, float cellCm);
      void center(const Tempest::Vec3& position);
      bool contains(int x, int z) const;
      WorldCell& at(int x, int z);
      const WorldCell& at(int x, int z) const;
      };
    struct RayStamp final {
      uint64_t gameMs = 0;
      bool seen = false;
      };
    struct PathStamp final { uint64_t gameMs = 0; double pathCm = 0; };
    struct ClassificationLimits final { float step = 0, slope = 0, bodyHeight = 0; };

    void ingest(Grid& grid, const NavigationEye::Ray& ray, float observedPath);
    void reconstructWater(Grid& grid, const std::vector<const NavigationEye::Ray*>& observations);
    ClassificationLimits classificationLimits() const;
    Cell classify(const Grid& grid, int x, int z, const ClassificationLimits& limits) const;
    void refreshFamiliarWindow();
    Cell familiarCell(const FamiliarTile& tile, const Cell& live) const;
    void read(const Grid& grid, Layer& result) const;
    float confidence(const Evidence& evidence, bool movable) const;
    float pathAt(uint64_t gameMs) const;

    Grid nearGrid, farGrid;
    Pose body;
    double pathCm = 0;
    bool posed = false;
    bool journeyMemory = true;
    const FamiliarMap* familiarMap = nullptr;
    FamiliarMask familiarMask;
    // Ring slots match farGrid. Only window/map changes search the shared atlas.
    std::vector<const FamiliarTile*> familiarWindow;
    int familiarMinX = 0, familiarMinZ = 0;
    std::vector<RayStamp> rayStamps;
    // Only the interval since the oldest not-yet-consumed eye timestamp is
    // needed to assign cached observations their original travelled distance.
    std::vector<PathStamp> pathHistory;
  };
