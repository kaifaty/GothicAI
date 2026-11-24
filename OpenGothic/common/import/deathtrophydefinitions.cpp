#include "baseimport.h"

#include "daedalus/script.h"
#include "game/definitions/deathtrophydefinitions.h"
#include "game/definitions/gameplaydefinitions.h"
#include "npcsnapshot.h"

#include <stdexcept>

namespace {
using namespace daedalus;
using namespace DeathTrophyDefinitions;
using Op = DaedalusOpcode;
using Symbol = DaedalusSymbol;

Catalog capture(const DaedalusScript& script) {
  const auto findSymbol = [&](const char* name) {
    const auto* symbol = script.find_symbol_by_name(name);
    if(symbol==nullptr)
      throw std::runtime_error(std::string("Missing stock death trophy symbol ")+name);
    return symbol;
    };
  const auto* self = findSymbol("SELF");
  const auto* body = findSymbol("B_GIVEDEATHINV");
  const auto* actor = findSymbol("B_GIVEDEATHINV.SLF");
  const auto* talent = findSymbol("PLAYER_TALENT_TAKEANIMALTROPHY");
  const auto* aivar = findSymbol("C_NPC.AIVAR");
  const auto* yes = findSymbol("TRUE");
  const auto* instanceId = findSymbol("HLP_GETINSTANCEID");
  const auto* random = findSymbol("HLP_RANDOM");
  const auto* create = findSymbol("CREATEINVITEMS");
  auto pc = body->address();
  const auto malformed = [&] { throw std::runtime_error("Invalid stock death trophy rule at "+std::to_string(pc)); };
  const auto next = [&] { return script.instruction_at(pc).op; };
  const auto take = [&](Op expected) {
    const auto op = script.instruction_at(pc);
    if(op.op!=expected)
      malformed();
    pc += op.size;
    return op;
    };
  const auto expectSymbol = [&](Op op,const Symbol* symbol) {
    if(take(op).symbol!=symbol->index())
      malformed();
    };
  const auto expectAivar = [&](uint16_t slot) {
    const auto op = take(Op::PUSHVV);
    if(op.symbol!=aivar->index() || op.index!=slot)
      malformed();
    };
  expectSymbol(Op::PUSHVI,actor);
  take(Op::MOVVI);
  expectSymbol(Op::PUSHV,yes);
  expectSymbol(Op::GMOVI,actor);
  expectAivar(given);
  take(Op::EQ);
  const auto firstTalent = take(Op::BZ).address;
  take(Op::RSR);
  if(pc!=firstTalent)
    malformed();

  const auto readTrophy = [&](const Symbol* randomizer,bool allowChoice) {
    Trophy trophy{};
    if(next()==Op::PUSHI) {
      if(randomizer==nullptr)
        malformed();
      const auto limit = take(Op::PUSHI).immediate;
      expectSymbol(Op::PUSHV,randomizer);
      const bool exact = next()==Op::EQ;
      take(exact ? Op::EQ : Op::LTE);
      trophy.chance = Chance{limit,exact};
      }
    if(next()==Op::PUSHVI) {
      trophy.symbol = take(Op::PUSHVI).symbol;
      expectSymbol(Op::BE,instanceId);
      expectSymbol(Op::PUSHVI,actor);
      expectSymbol(Op::BE,instanceId);
      } else {
      trophy.symbol = take(Op::PUSHV).symbol;
      const auto context = take(Op::GMOVI).symbol;
      if(context==actor->index())
        trophy.match = Match::ActorSpecies;
      else if(context==self->index())
        trophy.match = Match::SelfSpecies;
      else
        malformed();
      expectAivar(species);
      }
    if(script.find_symbol_by_index(trophy.symbol)==nullptr)
      malformed();
    take(Op::EQ);
    if(trophy.chance)
      take(Op::AND);
    const auto end = take(Op::BZ).address;
    while(pc<end && next()==Op::PUSHVI) {
      expectSymbol(Op::PUSHVI,actor);
      const auto item = take(Op::PUSHI).immediate;
      const auto amount = take(Op::PUSHI).immediate;
      expectSymbol(Op::BE,create);
      trophy.grants.push_back({item,amount});
      }
    const auto choiceEnd = allowChoice && pc<end ? take(Op::B).address : 0u;
    if(pc!=end || trophy.grants.empty())
      malformed();
    return std::pair{std::move(trophy),choiceEnd};
    };

  Catalog catalog;
  for(auto& learned : catalog.taught) {
    expectSymbol(Op::PUSHV,yes);
    const auto access = next()==Op::PUSHV ? Op::PUSHV : Op::PUSHVV;
    const auto op = take(access);
    if(op.symbol!=talent->index() || op.index>=talent->count())
      malformed();
    learned.index = op.index;
    take(Op::EQ);
    const auto end = take(Op::BZ).address;
    while(pc<end)
      learned.trophies.push_back(readTrophy(nullptr,false).first);
    if(pc!=end || learned.trophies.empty())
      malformed();
    }
  // First-match choices share one exit. Talent rows grant independently.
  const auto readChoice = [&](const Symbol* randomizer) {
    auto [trophy,end] = readTrophy(randomizer,true);
    if(end<=pc)
      malformed();
    Trophies choices;
    choices.push_back(std::move(trophy));
    while(pc<end) {
      auto [choice,jump] = readTrophy(randomizer,true);
      if((pc<end && jump!=end) || (pc==end && jump!=0))
        malformed();
      choices.push_back(std::move(choice));
      }
    if(pc!=end)
      malformed();
    return choices;
    };
  catalog.unconditional = readChoice(nullptr);
  const std::array randomVariables{"GOBLINGREEN_RANDOMIZER","GOBLINBLACK_RANDOMIZER","ORC_RANDOMIZER"};
  for(size_t i=0;i<catalog.randomized.size();++i) {
    auto& roll = catalog.randomized[i];
    roll.bound = take(Op::PUSHI).immediate;
    expectSymbol(Op::BE,random);
    const auto* result = findSymbol(randomVariables[i]);
    roll.result = result->index();
    expectSymbol(Op::PUSHV,result);
    take(Op::MOVI);
    const size_t groupCount = i==2 ? 5 : 1;
    for(size_t j=0;j<groupCount;++j)
      roll.choices.push_back(readChoice(result));
    }
  expectSymbol(Op::PUSHV,yes);
  expectSymbol(Op::GMOVI,actor);
  expectAivar(given);
  take(Op::MOVI);
  take(Op::RSR);
  return catalog;
  }
}

void BaseImport::convertDeathTrophies(const std::filesystem::path& base) {
  auto source = NpcSnapshot::load(base);
  if(source.sha256!=GameplayDefinitions::stockProgram)
    return;
  const auto script = DaedalusScript::from_program(std::move(source.data));
  const auto catalog = capture(script);
  DeathTrophyDefinitions::write(base,catalog);
  if(DeathTrophyDefinitions::load(base)!=catalog)
    throw std::runtime_error("Stock death trophy catalog differs after reading");
  }
