#pragma once

#include <Tempest/Vec>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

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
    ~WayHeights();
    void adjust(WayPoint& point, const DynamicWorld& physics);
    void save();
    size_t hits() const { return hitCount; }
    size_t misses() const { return missCount; }
    const std::filesystem::path& path() const { return file; }

    struct Profile {
      struct Sample { Tempest::Vec3 position, normal; };
      Tempest::Vec3 from, to;
      float length = 0, spacing = 25;
      std::vector<Sample> samples; // Distance from the destination: 0, spacing, ..., length.
      float heightAt(const Tempest::Vec3& position) const;
      float terminalHeightAt(const Tempest::Vec3& position) const;
      };
    const Profile& profile(const Tempest::Vec3& from, const Tempest::Vec3& to,
                           float stepHeight, float slopeCos, const DynamicWorld& physics);

  private:
    using PointKey = std::tuple<std::string,std::array<uint32_t,3>>;
    static PointKey pointKey(const WayPoint& point);
    void read();
    void readProfiles();
    void saveProfiles();
    using ProfileKey = std::array<uint32_t,8>;

    std::filesystem::path file;
    std::string identity;
    std::map<PointKey,Tempest::Vec3> points;
    std::vector<std::pair<PointKey,Tempest::Vec3>> baked;
    size_t hitCount = 0, missCount = 0;
    std::filesystem::path profileFile;
    std::string profileIdentity;
    std::map<ProfileKey,Profile> profiles;
    bool profilesRead = false, profilesDirty = false;
  };
