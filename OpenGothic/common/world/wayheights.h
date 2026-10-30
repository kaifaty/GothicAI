#pragma once

#include <Tempest/Vec>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>

class DynamicWorld;
class WayPoint;

// Derived initial-vob ground positions; independent of the immutable game base.
class WayHeights final {
  public:
    static constexpr uint32_t BakeVersion = 1;
    struct Key {
      std::string base, world, content;
      uint32_t collisionFlags = 0;
      uint32_t version = BakeVersion;
      };

    WayHeights(const std::filesystem::path& cacheRoot, const Key& key);
    void adjust(WayPoint& point, const DynamicWorld& physics);
    void save();
    size_t hits() const { return hitCount; }
    size_t misses() const { return missCount; }
    const std::filesystem::path& path() const { return file; }

  private:
    using PointKey = std::tuple<std::string,std::array<uint32_t,3>>;
    static PointKey pointKey(const WayPoint& point);
    void read();

    std::filesystem::path file;
    std::string identity;
    std::map<PointKey,Tempest::Vec3> points;
    size_t hitCount = 0, missCount = 0;
  };
