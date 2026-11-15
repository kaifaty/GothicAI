#include "game/deedwitness.h"
#include "game/gothicguildtraits.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
  ++checks;
  if(condition) return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }
}

int main() {
  const auto& traits = gothicGuildTraits();
  require(traits.count()==GIL_MAX,"guild count is unchanged");
  require(traits.none==GIL_NONE && traits.publicGuild==GIL_PUBLIC,"special guild numbers are unchanged");
  for(uint32_t gil=0;gil<GIL_MAX;++gil) {
    require(traits.has(gil,GuildTraits::Human)==(gil < GIL_SEPERATOR_HUM),"human guild parity");
    require(traits.has(gil,GuildTraits::Monster)==(GIL_SEPERATOR_HUM<gil && gil<GIL_SEPERATOR_ORC),"monster guild parity");
    require(traits.has(gil,GuildTraits::Orc)==(gil>GIL_SEPERATOR_ORC),"orc guild parity");
    const auto G2_UNDEAD = (gil == GIL_GOBBO_SKELETON ||
      gil == GIL_SUMMONED_GOBBO_SKELETON || gil == GIL_SKELETON      ||
      gil == GIL_SUMMONED_SKELETON       || gil == GIL_SKELETON_MAGE ||
      gil == GIL_SHADOWBEAST_SKELETON    || gil == GIL_ZOMBIE);
    require(traits.has(gil,GuildTraits::Undead)==G2_UNDEAD,"undead guild parity");
    const auto guild=gil;
    const bool sinks = guild==GIL_GOBBO_SKELETON || guild==GIL_SUMMONED_GOBBO_SKELETON ||
                       guild==GIL_SKELETON || guild==GIL_SUMMONED_SKELETON || guild==GIL_SKELETON_MAGE ||
                       guild==GIL_SHADOWBEAST_SKELETON || guild==GIL_STONEGOLEM || guild==GIL_SUMMONED_GOLEM ||
                       guild==GIL_Stoneguardian || guild==GIL_SummonedGuardian;
    require(traits.has(gil,GuildTraits::SinkingBody)==sinks,"sinking body guild parity");
    require(traits.has(gil,GuildTraits::Dragon)==(gil==GIL_DRAGON),"dragon guild parity");
    }

  require(NpcDeedSimulation::admitted(NpcProcessPolicy::AiNormal),
          "a fully simulated marked NPC may transfer or capture a deed");
  require(NpcDeedSimulation::refusal(NpcProcessPolicy::AiNormal)==nullptr,
          "normal simulation has no failure reason");
  for(auto policy:std::array{NpcProcessPolicy::Player,NpcProcessPolicy::AiFar,NpcProcessPolicy::AiFar2}) {
    require(!NpcDeedSimulation::admitted(policy),
            "a marked NPC outside normal simulation cannot transfer or capture a deed");
    const auto* reason = NpcDeedSimulation::refusal(policy);
    require(reason!=nullptr && std::string_view(reason)=="left_simulation",
            "the same production admission reports the native left_simulation failure");
    }
  bool simulationLost = false;
  require(NpcDeedSimulation::refusal(NpcProcessPolicy::AiNormal,simulationLost)==nullptr,
          "a new deed begins without a latched simulation failure");
  require(NpcDeedSimulation::refusal(NpcProcessPolicy::AiFar,simulationLost)!=nullptr && simulationLost,
          "a refused queued pickup remembers leaving simulation");
  const auto* latched = NpcDeedSimulation::refusal(NpcProcessPolicy::AiNormal,simulationLost);
  require(latched!=nullptr && std::string_view(latched)=="left_simulation",
          "returning before the observer tick cannot revive the refused deed or lose its exact failure");
  simulationLost = false; // setServerDeed/clearServerDeed start a new scoped action
  require(NpcDeedSimulation::refusal(NpcProcessPolicy::AiNormal,simulationLost)==nullptr,
          "resetting the command admits a fresh local deed");
  require(!DeedWitness::foreignCombat(false,false),"standing NPC with a stale unrelated target remains a witness");
  require(DeedWitness::foreignCombat(true,false),"a participant in another fight is occupied");
  require(!DeedWitness::foreignCombat(true,true),"a defender fighting this culprit can witness a fresh deed");
  DeedWitness witness;
  witness.range = 2000;
  require(witness.admitted(traits),"ready nearby witness is admitted");
  witness.distanceSquared = 2000*2000;
  require(witness.admitted(traits),"perception range includes its exact boundary");
  witness.distanceSquared += 1;
  require(!witness.admitted(traits),"one centimetre squared beyond native range is rejected");
  witness.distanceSquared = 0;
  for(bool DeedWitness::* flag:std::array{
      &DeedWitness::culprit,&DeedWitness::victim,&DeedWitness::player,
      &DeedWitness::down,&DeedWitness::lying,&DeedWitness::sleeping,&DeedWitness::foreignFight}) {
    auto candidate = witness;
    candidate.*flag = true;
    require(!candidate.admitted(traits),"excluded participants, sleeping/down NPCs and foreign combat cannot witness");
    candidate.*flag = false;
    require(candidate.admitted(traits),"the same otherwise ready witness is admitted after its gate clears");
    }
  witness.simulated = false;
  require(!witness.admitted(traits),"far simulation cannot witness an engine deed");
  witness.simulated = true;
  witness.stateStarted = false;
  require(!witness.admitted(traits),"a pending native state cannot receive a perception yet");
  witness.stateStarted = true;
  witness.guild = GIL_SHEEP;
  require(!witness.admitted(traits),"a sheep never becomes a human crime witness");
  witness.guild = GIL_SEPERATOR_HUM;
  require(!witness.admitted(traits),"the vanilla human guild separator is excluded");
  witness.guild = GIL_OUT;
  require(witness.admitted(traits),"an outlander below the human guild separator may witness");
  witness.perceptionEnabled = false;
  require(!witness.admitted(traits),"disabled perception cannot witness even with native range and visibility");
  witness.perceptionEnabled = true;
  require(witness.admitted(traits),"enabling the relevant perception admits the otherwise ready human");
  std::printf("NPC deed witness contract: %d checks passed\n",checks);
  }
