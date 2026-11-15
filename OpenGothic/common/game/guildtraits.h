#pragma once

#include <cstdint>
#include <vector>

constexpr uint32_t GuildNone = 0;

struct GuildTraits {
  enum Flag : uint8_t {
    Human       = 1 << 0,
    Orc         = 1 << 1,
    Monster     = 1 << 2,
    Undead      = 1 << 3,
    SinkingBody = 1 << 4,
    Dragon      = 1 << 5,
    };

  uint32_t none = GuildNone;
  uint32_t publicGuild = GuildNone;
  std::vector<uint8_t> flags;
  uint8_t beyondFlags = 0; // Nonnegative guild IDs at or above count().

  uint32_t count() const { return uint32_t(flags.size()); }
  bool has(int64_t guild, Flag flag) const noexcept {
    if(guild<0)
      return false;
    return ((guild<count() ? flags[uint32_t(guild)] : beyondFlags)&flag)!=0;
    }
  };
