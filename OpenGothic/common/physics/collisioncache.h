#pragma once

#include "physics.h"

#include <cstdint>
#include <filesystem>
#include <string>

// Disposable Jolt shapes; the source CPU mesh still owns material/sector data.
class CollisionCache final {
  public:
    static constexpr uint32_t BakeVersion = 1;
    struct Key {
      std::string base, world, content;
      uint32_t collisionFlags = 0;
      uint32_t version = BakeVersion;
      };

    CollisionCache(const std::filesystem::path& cacheRoot, const Key& key);
    bool read(JPH::ShapeRefC& landscape, JPH::ShapeRefC& water) const;
    void save(const JPH::Shape* landscape, const JPH::Shape* water) const;
    const std::filesystem::path& path() const { return file; }

  private:
    std::filesystem::path file;
    std::string identity;
  };
