#pragma once

#include <array>
#include <cstdint>

// Bodies as physical traces (physics stage 8, step 4b; [GAME] body_traces).
// A dead person is evidence: World::physicsTraces() reports it as a `body` trace.
// Every body, a person's or a monster's, leaves the world three game days after
// death, and only while the hero is farther than 30 m and none of its sight points
// is visible to him. These rules are pure so the native contract checks them.
namespace BodyTraces {
  constexpr int64_t LifetimeMinutes = 3*24*60; // game minutes from death (or from loading an old save)
  constexpr float   HeroAwayCm      = 3000.f;  // the hero must be farther than this
  constexpr float   SightLiftCm     = 20.f;    // sight points sit above the bones, off the ground
  // Main script types of Gothic II (NPCTYPE_MAIN, NPCTYPE_OCMAIN, NPCTYPE_BL_MAIN):
  // fallback values when the scripts lack the symbols.
  constexpr std::array<int32_t,3> StoryTypes = {1, 4, 7};

  struct Body {
    bool    enabled      = true;  // [GAME] body_traces
    bool    dead         = true;
    bool    player       = false;
    int64_t clockFrom    = -1;    // game minute the lifetime started; -1: not known
    int64_t now          = 0;     // game minutes
    float   heroDistance = 0;     // cm
    bool    missionItems = false; // Inventory::hasMissionItems()
    bool    dragon       = false; // Gothic II GIL_DRAGON
    bool    immortal     = false; // NpcFlag::IMMORTAL
    bool    storyType    = false; // npctype is one of the main script types
    bool    heroSees     = false; // a ray from the hero's eyes reaches a sight point
    };

  // Everything but the hero's sight: rays are cast only for bodies that pass this.
  inline bool removableUnseen(const Body& b) {
    return b.enabled && b.dead && !b.player && b.clockFrom>=0 &&
           b.now-b.clockFrom>=LifetimeMinutes && b.heroDistance>HeroAwayCm &&
           !b.missionItems && !b.dragon && !b.immortal && !b.storyType;
    }

  inline bool removable(const Body& b) {
    return removableUnseen(b) && !b.heroSees;
    }

  // Only a person's body is a trace; a monster's is not evidence.
  inline bool isTrace(bool enabled, bool dead, bool player, int32_t guild, int32_t separatorHum) {
    return enabled && dead && !player && guild<separatorHum;
    }

  inline bool isStoryType(int32_t type, const std::array<int32_t,3>& story) {
    for(auto t:story)
      if(t==type)
        return true;
    return false;
    }
  }
