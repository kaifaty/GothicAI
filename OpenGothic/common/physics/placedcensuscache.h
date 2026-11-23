#pragma once

#include "dynamicworld.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Derived result of DynamicWorld::placedItemCensus for one world. The census
// is a joint decision (neighbour overlaps, supports by other items, excluded
// stacks), and it always runs on the authored ZEN poses before a save is
// applied. One file per world therefore serves fresh starts and saves alike.
class PlacedCensusCache final {
  public:
    // Cache file format only (3: stored builder fingerprint). Census rules and
    // body construction are covered by the build fingerprint; extend its source
    // list in cmake/PlacedCensusBuilder.cmake when adding code outside that list.
    static constexpr uint32_t RulesVersion = 3;
    static std::string_view builderFingerprint();
    using Entry = DynamicWorld::PlacedItemCensusEntry;

    // OPENGOTHIC_PLACED_CENSUS_CACHE=0 disables the cache entirely.
    static bool enabled();
    // OPENGOTHIC_PLACED_CENSUS_CHECK=1 also computes the live census on a hit
    // and fails the process on the first differing item.
    static bool checkMode();

    struct Key {
      std::string base, world, content;
      uint32_t flags = 0;
      std::string inputs; // DynamicWorld::placedItemCensusInputs
      uint32_t version = RulesVersion;
      std::string builder = std::string(builderFingerprint());
      };
    enum class Status { Disabled, Hit, Absent, Version, Builder, Inputs, Corrupt };
    static std::string_view statusName(Status status);

    PlacedCensusCache(const std::filesystem::path& cacheRoot, const Key& key);
    Status read(size_t count, std::vector<Entry>& entries) const;
    void   save(const std::vector<Entry>& entries) const;
    const std::filesystem::path& path() const { return file; }

    // Bit-exact comparison of every field; floats are printed with their bits.
    struct Difference { std::string_view field; std::string a, b; };
    static std::vector<Difference> differences(const Entry& a, const Entry& b);

  private:
    std::filesystem::path file;
    std::string identity;
    std::string builder;
    uint32_t version = RulesVersion;
  };
