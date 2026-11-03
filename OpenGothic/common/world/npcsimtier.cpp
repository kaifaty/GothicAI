#include "npcsimtier.h"

#include <Tempest/Log>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "chunkresidency.h"

using namespace NpcSimTiers;

Mode NpcSimTiers::parseMode(const char* tier, const char* streaming, bool* valid) {
  if(valid!=nullptr)
    *valid=true;
  Mode ret=Mode::Auto;
  if(tier==nullptr || tier[0]=='\0' || std::strcmp(tier,"auto")==0)
    ret=Mode::Auto;
  else if(std::strcmp(tier,"full")==0)
    ret=Mode::Full;
  else if(std::strcmp(tier,"simplified")==0)
    ret=Mode::Simplified;
  else if(valid!=nullptr)
    *valid=false;
  if(streaming!=nullptr && std::strcmp(streaming,"0")==0)
    ret=Mode::Full;
  return ret;
  }

Mode NpcSimTiers::mode() {
  static const Mode value=[] {
    const char* tier=std::getenv("OPENGOTHIC_NPC_SIM_TIER");
    const char* streaming=std::getenv("OPENGOTHIC_STREAMING");
    bool valid=true;
    const Mode ret=parseMode(tier,streaming,&valid);
    if(!valid)
      Tempest::Log::e("OPENGOTHIC_NPC_SIM_TIER=",tier," is not auto, full or simplified: using auto");
    if(tier!=nullptr && tier[0]!='\0')
      Tempest::Log::i("NPC simulation tier: ",name(ret),(ret==Mode::Full && streaming!=nullptr &&
                      std::strcmp(streaming,"0")==0 ? " (OPENGOTHIC_STREAMING=0)" : ""));
    return ret;
    }();
  return value;
  }

const char* NpcSimTiers::name(Mode mode) {
  switch(mode) {
    case Mode::Auto:       return "auto";
    case Mode::Full:       return "full";
    case Mode::Simplified: return "simplified";
    }
  return "?";
  }

const char* NpcSimTiers::name(NpcSimTier tier) {
  return tier==NpcSimTier::Full ? "full" : "simplified";
  }

NpcSimTier NpcSimTiers::desired(Mode mode, bool hero, bool alwaysFull, const ChunkResidency* residency,
                                ChunkId cell, NpcSimTier previous) {
  if(hero || alwaysFull)
    return NpcSimTier::Full;
  switch(mode) {
    case Mode::Full:
      return NpcSimTier::Full;
    case Mode::Simplified:
      return NpcSimTier::Simplified;
    case Mode::Auto:
      break;
    }
  if(residency==nullptr)
    return NpcSimTier::Full;
  // The previous tier selects the Near exit radius, exactly as a vob view
  // keeps its previous residency state.
  const auto before=previous==NpcSimTier::Full ? ChunkResidency::State::Near : ChunkResidency::State::Outside;
  return residency->desired(cell,before)==ChunkResidency::State::Near ? NpcSimTier::Full : NpcSimTier::Simplified;
  }

std::string Stats::probeFields() const {
  char buf[768];
  std::snprintf(buf,sizeof(buf),
                "npc_full=%zu npc_simplified=%zu npc_to_full=%llu npc_to_simplified=%llu "
                "npc_full_tick_ms=%.6f npc_simplified_tick_ms=%.6f npc_full_tick_ms_total=%.3f "
                "npc_simplified_tick_ms_total=%.3f npc_tier_ticks=%llu npc_tier_mode=%s npc_native_combat_ticks=%llu "
                "npc_native_mobsi_approach_ticks=%llu npc_graph_fp_load_resolutions=%llu "
                "npc_graph_fallback_ground=%llu npc_graph_fallback_segment=%llu npc_graph_fallback_ticks=%llu",
                full,simplified,(unsigned long long)toFull,(unsigned long long)toSimplified,
                fullTickMs,simplifiedTickMs,fullTickMsTotal,simplifiedTickMsTotal,
                (unsigned long long)ticks,name(mode),(unsigned long long)nativeCombatNpcTicks,
                (unsigned long long)nativeMobsiApproachNpcTicks,(unsigned long long)fpLoadResolutions,
                (unsigned long long)graphFallbackGround,(unsigned long long)graphFallbackSegment,
                (unsigned long long)graphFallbackNpcTicks);
  return buf;
  }
