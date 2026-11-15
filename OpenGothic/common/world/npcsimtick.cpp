#include "npcsimtick.h"

#include <Tempest/Log>

#include <cstdlib>
#include <cstring>
#include <optional>

#include "game/bodycontroller.h"
#include "game/bodyobservations.h"
#include "game/npckeys.h"
#include "world/objects/npc.h"

using namespace NpcSimTick;

namespace {
std::optional<Config>& replaced() {
  static std::optional<Config> cfg;
  return cfg;
  }
}

Config NpcSimTick::parse(const char* period, const char* scope, std::string* problem) {
  Config cfg;
  if(period!=nullptr && period[0]!='\0') {
    char* end = nullptr;
    const long ms = std::strtol(period,&end,10);
    if(end!=nullptr && *end=='\0' && (ms==0 || (ms>=long(MinPeriodMs) && ms<=long(MaxPeriodMs))))
      cfg.periodMs = uint64_t(ms);
    else if(problem!=nullptr)
      *problem += std::string("OPENGOTHIC_NPC_SIM_TICK_MS=")+period+" is not 0 or 16..66: using 33. ";
    }
  if(scope!=nullptr && scope[0]!='\0') {
    if(std::strcmp(scope,"tier")==0)
      cfg.scope = Scope::Tier;
    else if(std::strcmp(scope,"graph")!=0 && problem!=nullptr)
      *problem += std::string("OPENGOTHIC_NPC_SIM_TICK_SCOPE=")+scope+" is not graph or tier: using graph. ";
    }
  return cfg;
  }

const Config& NpcSimTick::config() {
  static const Config env = [] {
    const char* period = std::getenv("OPENGOTHIC_NPC_SIM_TICK_MS");
    const char* scope  = std::getenv("OPENGOTHIC_NPC_SIM_TICK_SCOPE");
    std::string problem;
    const Config cfg = parse(period,scope,&problem);
    if(!problem.empty())
      Tempest::Log::e(problem);
    else if((period!=nullptr && period[0]!='\0') || (scope!=nullptr && scope[0]!='\0'))
      Tempest::Log::i("NPC far tick: period ",cfg.periodMs," ms, scope ",name(cfg.scope));
    return cfg;
    }();
  const auto& test = replaced();
  return test ? *test : env;
  }

void NpcSimTick::setConfig(const Config& cfg) {
  replaced() = cfg;
  }

const char* NpcSimTick::name(Scope scope) {
  return scope==Scope::Tier ? "tier" : "graph";
  }

bool NpcSimTick::qualifies(const Npc& npc, Scope scope) {
  if(npc.simTier()!=NpcSimTier::Simplified || npc.isPlayer())
    return false;
  // The physics step simulates or poses these bodies every frame (syncRagdoll, the living rig).
  if(npc.hasRagdoll() || npc.livingBody()!=nullptr)
    return false;
  // Body policies, the stand's keys and the observations act or sample on each step of the NPC.
  if((BodyController::hasOwners() && BodyController::owns(npc)) || NpcKeys::enabled(npc) || BodyObservations::watched(npc))
    return false;
  if(scope==Scope::Graph)
    return npc.usesSimplifiedMovement();
  // Fighters are paired and their blades swept every frame; spell fields push airborne bodies every frame.
  return !npc.isMeleeFighter() && !npc.spellFrozen() && !npc.spellWhirling() && !npc.isInAir();
  }

uint32_t NpcSimTick::phase(const Npc& npc) {
  // Fibonacci hashing of the creation number: consecutive numbers fall evenly over the period.
  return uint32_t((uint64_t(npc.animationLodSeed())*0x9e3779b97f4a7c15ull)>>32);
  }

bool NpcSimTick::due(const State& state, uint64_t owed, uint64_t dt, uint32_t phase, const Config& cfg) {
  const uint64_t period = cfg.periodMs;
  if(period==0 || dt>=period)
    return true;
  if(dt==0)
    return false;
  if(!state.rhythm) {
    // The first tick: at the NPC's phase within the frames that make one period at this step.
    const uint64_t frames = (2*period+dt-1)/(2*dt);
    return (owed<<32) > uint64_t(phase)*frames*dt;
    }
  // Now, unless one frame more (as long as this one) would bring the time nearer to the period.
  return 2*owed+dt>=2*period;
  }
