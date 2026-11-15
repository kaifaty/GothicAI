#pragma once

#include "constants.h"
#include "guildtraits.h"

static_assert(GIL_NONE==GuildNone);

inline const GuildTraits& gothicGuildTraits() {
  static const GuildTraits traits = [] {
    GuildTraits ret{GIL_NONE,GIL_PUBLIC,std::vector<uint8_t>(GIL_MAX)};
    for(uint32_t guild=0;guild<GIL_MAX;++guild) {
      auto& flags = ret.flags[guild];
      if(guild<GIL_SEPERATOR_HUM)
        flags |= GuildTraits::Human;
      if(guild>GIL_SEPERATOR_ORC)
        flags |= GuildTraits::Orc;
      if(GIL_SEPERATOR_HUM<guild && guild<GIL_SEPERATOR_ORC)
        flags |= GuildTraits::Monster;
      if(guild==GIL_GOBBO_SKELETON || guild==GIL_SUMMONED_GOBBO_SKELETON ||
         guild==GIL_SKELETON || guild==GIL_SUMMONED_SKELETON || guild==GIL_SKELETON_MAGE ||
         guild==GIL_SHADOWBEAST_SKELETON || guild==GIL_ZOMBIE)
        flags |= GuildTraits::Undead;
      if(guild==GIL_GOBBO_SKELETON || guild==GIL_SUMMONED_GOBBO_SKELETON ||
         guild==GIL_SKELETON || guild==GIL_SUMMONED_SKELETON || guild==GIL_SKELETON_MAGE ||
         guild==GIL_SHADOWBEAST_SKELETON || guild==GIL_STONEGOLEM || guild==GIL_SUMMONED_GOLEM ||
         guild==GIL_Stoneguardian || guild==GIL_SummonedGuardian)
        flags |= GuildTraits::SinkingBody;
      if(guild==GIL_DRAGON)
        flags |= GuildTraits::Dragon;
      }
    return ret;
    }();
  return traits;
  }
