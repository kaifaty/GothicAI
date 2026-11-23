#include "gamescript.h"

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <stdexcept>
#include <vector>

bool GameScript::applyCumulativeFoodEffect(std::string_view callback) {
  const bool apple = callback=="USE_APPLE";
  if(!apple && callback!="USE_MUSHROOM_01")
    return false;

  auto* self = vm.global_self();
  auto* change = findSymbol("NPC_CHANGEATTRIBUTE");
  vm.push_reference(self);
  vm.push_reference(findSymbol("ATR_HITPOINTS"));
  vm.push_reference(findSymbol(apple ? "HP_APPLE" : "HP_MUSHROOM_01"));
  vm.unsafe_call_external(change);
  vm.push_reference(self);
  vm.unsafe_call_external(findSymbol("NPC_ISPLAYER"));
  if(vm.pop_int()==0)
    return true;

  auto* counter = findSymbol(apple ? "APPLE_BONUS" : "DUNKELPILZ_BONUS");
  const auto caller = vm.unsafe_get_gi();
  const auto count = [&] { return vm.get_int(caller,counter,0); };
  vm.set_int(caller,counter,0,std::bit_cast<int32_t>(uint32_t(count())+1u));

  const auto print = [&](const char* notice) {
    vm.push_reference(findSymbol(notice));
    vm.unsafe_call_external(findSymbol("PRINT"));
    };
  if(apple) {
    if(count()==7)
      print("PRINT_EAT1");
    if(count()==15)
      print("PRINT_EAT2");
    }
  if(count()!=(apple ? 25 : 50))
    return true;

  if(apple)
    print("PRINT_EAT3");
  vm.push_reference(self);
  vm.push_reference(findSymbol(apple ? "ATR_STRENGTH" : "ATR_MANA_MAX"));
  vm.push_int(apple ? 1 : 5);
  vm.unsafe_call_bl(findSymbol("B_RAISEATTRIBUTE"));
  if(!apple) {
    vm.push_reference(self);
    vm.push_reference(findSymbol("ATR_MANA"));
    vm.push_int(5);
    vm.unsafe_call_external(change);
    }
  vm.push_reference(findSymbol(apple ? "$14176" : "$14605")); // Each callback's stock LevelUp string.
  vm.unsafe_call_external(findSymbol("SND_PLAY"));
  vm.set_int(caller,counter,0,0);
  return true;
  }

void GameScript::loadNativeTradeInventory() {
  if(!nativeRoutineProgram)
    return;

  using Op = daedalus::DaedalusOpcode;
  using Symbol = daedalus::DaedalusSymbol;
  using NpcRef = std::shared_ptr<daedalus::INpc>;
  const auto enabled = [this] {
    return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
    };
  const auto* ambient=findSymbol("B_CREATEAMBIENTINV");
  if(npcDefinitions.helpers.helpers.contains(ambient->index())) {
    vm.override_function("B_CREATEAMBIENTINV",std::function<void(NpcRef)>{[this,ambient](const NpcRef& actor) {
      vm.push_instance(actor);
      if(!actor || !NpcHelperRuntime::invoke(vm,actor,ambient,npcDefinitions.helpers))
        vm.unsafe_call(ambient);
      }},[enabled] { return enabled() && NpcHelperRuntime::enabled(); });
    }
  auto* lookup = findSymbol("HLP_GETNPC");
  auto* instanceId = findSymbol("HLP_GETINSTANCEID");
  auto* hasItems = findSymbol("NPC_HASITEMS");
  auto* removeItems = findSymbol("NPC_REMOVEINVITEMS");
  auto* validNpc = findSymbol("HLP_ISVALIDNPC");
  auto* inState = findSymbol("NPC_ISINSTATE");
  auto* runes = findSymbol("B_CLEARRUNEINV");
  auto* junk = findSymbol("B_CLEARJUNKTRADEINV");
  auto* dead = findSymbol("B_CLEARDEADTRADER");
  auto* runesActor = findSymbol("B_CLEARRUNEINV.SLF");
  auto* runeHero = findSymbol("B_CLEARRUNEINV.HER");
  auto* junkActor = findSymbol("B_CLEARJUNKTRADEINV.SLF");
  auto* tradeActor = findSymbol("B_GIVETRADEINV.SLF");
  auto* aivar = findSymbol("C_NPC.AIVAR");
  auto* chapter = findSymbol("KAPITEL");
  const auto deadState = int32_t(findSymbolIndex("ZS_DEAD"));
  const auto unconsciousState = int32_t(findSymbolIndex("ZS_UNCONSCIOUS"));
  constexpr uint16_t stockChapter = 49; // AIV_INVCHAPTER.
  uint32_t pc = runes->address();
  const auto malformed = [&] { throw std::runtime_error("Invalid stock inventory rule at "+std::to_string(pc)); };
  const auto take = [&](Op expected) {
    const auto op = vm.instruction_at(pc);
    if(op.op!=expected)
      malformed();
    pc += op.size;
    return op;
    };
  const auto expectSymbol = [&](Op op,const Symbol* symbol) {
    if(take(op).symbol!=symbol->index())
      malformed();
    };
  const auto saveActor = [&](const Symbol* symbol) {
    expectSymbol(Op::PUSHVI,symbol);
    take(Op::MOVVI);
    };
  const auto expectCall = [&](const Symbol* symbol) {
    if(take(Op::BL).address!=symbol->address())
      malformed();
    };
  const auto expectChapterSlot = [&] {
    const auto op = take(Op::PUSHVV);
    if(op.symbol!=aivar->index() || op.index!=stockChapter)
      malformed();
    };
  const auto readPair = [&](const Symbol* actor) {
    expectSymbol(Op::PUSHVI,actor);
    const auto remove = take(Op::PUSHI).immediate;
    expectSymbol(Op::PUSHVI,actor);
    const auto count = take(Op::PUSHI).immediate;
    expectSymbol(Op::BE,hasItems);
    expectSymbol(Op::BE,removeItems);
    return std::pair{remove,count};
    };

  saveActor(runesActor);
  const auto heroIndex = take(Op::PUSHI).immediate;
  if(heroIndex!=int32_t(findSymbolIndex("PC_HERO")))
    malformed();
  expectSymbol(Op::BE,lookup);
  saveActor(runeHero);
  expectSymbol(Op::PUSHVI,runeHero);
  expectSymbol(Op::BE,instanceId);
  expectSymbol(Op::PUSHVI,vm.global_self());
  expectSymbol(Op::BE,instanceId);
  take(Op::NEQ);
  const auto runesEnd = take(Op::BZ).address;
  std::array<std::pair<int32_t,int32_t>,54> runePairs;
  for(auto& pair : runePairs)
    pair = readPair(runesActor);
  if(pc!=runesEnd)
    malformed();
  take(Op::RSR);

  pc = junk->address();
  saveActor(junkActor);
  expectSymbol(Op::PUSHVI,junkActor);
  expectSymbol(Op::BE,validNpc);
  const auto junkEnd = take(Op::BZ).address;
  std::array<std::pair<int32_t,int32_t>,73> junkPairs;
  for(auto& pair : junkPairs)
    pair = readPair(junkActor);
  if(pc!=junkEnd)
    malformed();
  take(Op::RSR);

  struct Trader {
    int32_t source;
    Symbol* alias;
    };
  std::array<Trader,50> traders;
  pc = findSymbol("B_GIVETRADEINV")->address();
  saveActor(tradeActor);
  for(auto& trader : traders) {
    trader.source = take(Op::PUSHI).immediate;
    expectSymbol(Op::BE,lookup);
    trader.alias = findSymbol(take(Op::PUSHVI).symbol);
    if(trader.alias==nullptr || !trader.alias->name().starts_with("B_GIVETRADEINV.TRD_"))
      malformed();
    take(Op::MOVVI);
    }
  expectSymbol(Op::PUSHVI,tradeActor);
  expectCall(runes);
  expectSymbol(Op::PUSHV,chapter);
  expectSymbol(Op::GMOVI,tradeActor);
  expectChapterSlot();
  take(Op::LTE);
  const auto restockEnd = take(Op::BZ).address;
  const auto readTraderBranch = [&] {
    auto* alias = findSymbol(take(Op::PUSHVI).symbol);
    if(std::ranges::find(traders,alias,&Trader::alias)==traders.end())
      malformed();
    expectSymbol(Op::BE,instanceId);
    expectSymbol(Op::PUSHVI,tradeActor);
    expectSymbol(Op::BE,instanceId);
    take(Op::EQ);
    return std::pair{alias,take(Op::BZ).address};
    };
  // Lookup, restock and cleanup each have their own authored order.
  std::array<std::pair<Symbol*,const Symbol*>,50> restocks;
  for(auto& restock : restocks) {
    const auto [alias,next] = readTraderBranch();
    expectSymbol(Op::PUSHVI,tradeActor);
    expectCall(junk);
    expectSymbol(Op::PUSHVI,tradeActor);
    const auto* body = vm.find_symbol_by_address(take(Op::BL).address);
    if(body==nullptr || !body->name().starts_with("B_GIVETRADEINV_") || pc!=next)
      malformed();
    restock = {alias,body};
    }
  if(take(Op::PUSHI).immediate!=1)
    malformed();
  expectSymbol(Op::PUSHV,chapter);
  take(Op::ADD);
  expectSymbol(Op::GMOVI,tradeActor);
  expectChapterSlot();
  take(Op::MOVI);
  if(pc!=restockEnd)
    malformed();
  for(const auto state : {unconsciousState,deadState}) {
    expectSymbol(Op::PUSHVI,tradeActor);
    if(take(Op::PUSHI).immediate!=state)
      malformed();
    expectSymbol(Op::BE,inState);
    }
  take(Op::ORR);
  const auto tradeEnd = take(Op::BZ).address;
  std::array<Symbol*,50> defeatedTraders;
  for(auto& defeated : defeatedTraders) {
    const auto [alias,next] = readTraderBranch();
    expectSymbol(Op::PUSHVI,tradeActor);
    expectCall(dead);
    if(pc!=next)
      malformed();
    defeated = alias;
    }
  if(pc!=tradeEnd)
    malformed();
  take(Op::RSR);

  struct StockGrant {
    int32_t item = 0, amount = 0;
    Symbol* recipient = nullptr;
    Symbol* itemSymbol = nullptr;
    };
  struct StockBatch {
    Symbol* granted = nullptr;
    std::optional<int32_t> minimumChapter;
    std::vector<StockGrant> grants;
    };
  auto* create = findSymbol("CREATEINVITEMS");
  auto* yes = findSymbol("TRUE");
  auto* no = findSymbol("FALSE");
  const auto bindStock = [&](const Symbol& body) {
    pc = body.address();
    auto* actor = findSymbol(body.name()+".SLF");
    saveActor(actor);
    std::vector<StockBatch> stock;
    while(vm.instruction_at(pc).op!=Op::RSR) {
      StockBatch batch;
      uint32_t end = 0;
      if(vm.instruction_at(pc).op==Op::PUSHV) {
        expectSymbol(Op::PUSHV,no);
        batch.granted = findSymbol(take(Op::PUSHV).symbol);
        if(batch.granted==nullptr || batch.granted->name().find("_ITEMSGIVEN_CHAPTER_")==std::string::npos)
          malformed();
        take(Op::EQ);
        if(vm.instruction_at(pc).op==Op::PUSHI) {
          batch.minimumChapter = take(Op::PUSHI).immediate;
          expectSymbol(Op::PUSHV,chapter);
          take(Op::GTE);
          take(Op::AND);
          }
        end = take(Op::BZ).address;
        }
      while(vm.instruction_at(pc).op==Op::PUSHVI) {
        StockGrant grant;
        grant.recipient = findSymbol(take(Op::PUSHVI).symbol);
        if(grant.recipient!=actor && grant.recipient!=vm.global_self())
          malformed();
        if(vm.instruction_at(pc).op==Op::PUSHV) {
          // Samuel's stock reads the live item index.
          grant.itemSymbol = findSymbol("RI_PROTFIRE02");
          expectSymbol(Op::PUSHV,grant.itemSymbol);
          } else {
          grant.item = take(Op::PUSHI).immediate;
          }
        grant.amount = take(Op::PUSHI).immediate;
        expectSymbol(Op::BE,create);
        batch.grants.push_back(grant);
        }
      if(batch.granted!=nullptr) {
        expectSymbol(Op::PUSHV,yes);
        expectSymbol(Op::PUSHV,batch.granted);
        take(Op::MOVI);
        if(pc!=end)
          malformed();
        } else if(batch.grants.empty() || !stock.empty() || vm.instruction_at(pc).op!=Op::RSR) {
        malformed();
        }
      stock.push_back(std::move(batch));
      }
    take(Op::RSR);
    vm.override_function(body.name(),std::function<void(NpcRef)>{
      [this,actor,create,yes,no,stock=std::move(stock)](NpcRef npc) {
        actor->set_instance(std::move(npc));
        for(const auto& batch : stock) {
          const bool pending = batch.granted==nullptr || batch.granted->get_int()==no->get_int();
          const bool chapterReached = !batch.minimumChapter || storyChapter>=*batch.minimumChapter;
          if(!pending || !chapterReached)
            continue;
          for(const auto& grant : batch.grants) {
            vm.push_reference(grant.recipient);
            if(grant.itemSymbol)
              vm.push_reference(grant.itemSymbol);
            else
              vm.push_int(grant.item);
            vm.push_int(grant.amount);
            vm.unsafe_call_external(create);
            }
          if(batch.granted)
            batch.granted->set_int(yes->get_int());
          }
        }},enabled);
    };
  for(const auto& restock : restocks) {
    const auto& body = *restock.second;
    // These authored shops have additional guild or quest conditions.
    if(body.name()=="B_GIVETRADEINV_GORAX" || body.name()=="B_GIVETRADEINV_KARDIF" || body.name()=="B_GIVETRADEINV_KARRAS")
      continue;
    bindStock(body);
    }

  const auto getNpc = [this,lookup](int32_t source,Symbol* alias) {
    vm.push_int(source);
    vm.unsafe_call_external(lookup);
    alias->set_instance(vm.pop_instance());
    };
  const auto id = [this,instanceId](Symbol* actor) {
    vm.push_reference(actor);
    vm.unsafe_call_external(instanceId);
    return vm.pop_int();
    };
  const auto removePairs = [this,hasItems,removeItems](Symbol* actor,const auto& pairs) {
    for(const auto& [remove,count] : pairs) {
      vm.push_reference(actor);
      vm.push_int(remove);
      vm.push_reference(actor);
      vm.push_int(count);
      vm.unsafe_call_external(hasItems);
      // Keep the original external's INT on the void helper's call stack.
      vm.unsafe_call_external(removeItems);
      }
    };
  const auto call = [this](Symbol* actor,const Symbol* fn) {
    vm.push_reference(actor);
    vm.unsafe_call_bl(fn);
    };
  const auto member = [this](Symbol* actor) {
    auto npc = std::static_pointer_cast<daedalus::INpc>(actor->get_instance());
    vm.unsafe_set_gi(npc);
    return npc;
    };

  vm.override_function("B_CLEARRUNEINV",std::function<void(NpcRef)>{
    [this,runesActor,runeHero,heroIndex,runePairs,getNpc,id,removePairs](NpcRef actor) {
      runesActor->set_instance(std::move(actor));
      getNpc(heroIndex,runeHero);
      const auto hero = id(runeHero);
      if(hero!=id(vm.global_self()))
        removePairs(runesActor,runePairs);
    }},enabled);
  vm.override_function("B_CLEARJUNKTRADEINV",std::function<void(NpcRef)>{
    [this,junkActor,junkPairs,validNpc,removePairs](NpcRef actor) {
      junkActor->set_instance(std::move(actor));
      vm.push_reference(junkActor);
      vm.unsafe_call_external(validNpc);
      if(vm.pop_int()!=0)
        removePairs(junkActor,junkPairs);
    }},enabled);
  vm.override_function("B_CLEARDEADTRADER",std::function<void(NpcRef)>{
    [this,call,member](NpcRef actor) {
      auto* trader = findSymbol("B_CLEARDEADTRADER.TRADER");
      trader->set_instance(std::move(actor));
      vm.push_reference(trader);
      vm.unsafe_call_external(findSymbol("NPC_CLEARINVENTORY"));
      const auto defeated = member(trader);
      if(!defeated || defeated->aivar[16]==0) // AIV_VICTORYXPGIVEN.
        call(trader,findSymbol("B_CREATEAMBIENTINV"));
      if(const auto current = member(trader))
        current->aivar[stockChapter] = std::bit_cast<int32_t>(uint32_t(storyChapter)-1u);
    }},enabled);
  vm.override_function("B_GIVETRADEINV",std::function<void(NpcRef)>{
    [this,traders,restocks,defeatedTraders,tradeActor,runes,junk,dead,inState,unconsciousState,deadState,getNpc,id,call,member](NpcRef actor) {
      const auto sameTrader = [&](Symbol* alias) {
        const auto selected = id(alias);
        return selected==id(tradeActor);
        };
      const auto isInState = [&](int32_t state) {
        vm.push_reference(tradeActor);
        vm.push_int(state);
        vm.unsafe_call_external(inState);
        return vm.pop_int()!=0;
        };
      tradeActor->set_instance(std::move(actor));
      for(const auto& trader : traders)
        getNpc(trader.source,trader.alias);
      call(tradeActor,runes);
      const auto current = member(tradeActor);
      if((current ? current->aivar[stockChapter] : 0)<=storyChapter) {
        for(const auto& [alias,body] : restocks) {
          if(!sameTrader(alias))
            continue;
          call(tradeActor,junk);
          call(tradeActor,body);
          }
        if(const auto restocked = member(tradeActor))
          restocked->aivar[stockChapter] = std::bit_cast<int32_t>(uint32_t(storyChapter)+1u);
        }
      const bool unconscious = isInState(unconsciousState);
      const bool deceased = isInState(deadState);
      if(unconscious || deceased) {
        for(auto* alias : defeatedTraders) {
          if(sameTrader(alias))
            call(tradeActor,dead);
          }
        }
    }},enabled);
  }

void GameScript::loadNativeDeathTrophies() {
  if(!nativeRoutineProgram)
    return;

  using Op = daedalus::DaedalusOpcode;
  using Symbol = daedalus::DaedalusSymbol;
  using NpcRef = std::shared_ptr<daedalus::INpc>;
  struct Grant { int32_t item, amount; };
  struct Chance { int32_t threshold; bool exact; };
  struct Trophy {
    Symbol* match;
    Symbol* context;
    bool matchInstance;
    std::optional<Chance> chance;
    std::vector<Grant> grants;
    };
  using Trophies = std::vector<Trophy>;
  struct Talent { uint8_t index; Trophies trophies; };
  struct RandomTrophies {
    int32_t bound;
    Symbol* result;
    std::vector<Trophies> choices;
    };

  auto* body = findSymbol("B_GIVEDEATHINV");
  auto* actor = findSymbol("B_GIVEDEATHINV.SLF");
  auto* talent = findSymbol("PLAYER_TALENT_TAKEANIMALTROPHY");
  auto* aivar = findSymbol("C_NPC.AIVAR");
  auto* yes = findSymbol("TRUE");
  auto* instanceId = findSymbol("HLP_GETINSTANCEID");
  auto* random = findSymbol("HLP_RANDOM");
  auto* create = findSymbol("CREATEINVITEMS");
  constexpr uint16_t given = 11, species = 43; // AIV_DEATHINVGIVEN and AIV_MM_REAL_ID.
  auto pc = body->address();
  const auto malformed = [&] { throw std::runtime_error("Invalid stock death trophy rule at "+std::to_string(pc)); };
  const auto next = [&] { return vm.instruction_at(pc).op; };
  const auto take = [&](Op expected) {
    const auto op = vm.instruction_at(pc);
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

  size_t grantCount = 0;
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
    trophy.matchInstance = next()==Op::PUSHVI;
    if(trophy.matchInstance) {
      trophy.match = findSymbol(take(Op::PUSHVI).symbol);
      trophy.context = actor;
      expectSymbol(Op::BE,instanceId);
      expectSymbol(Op::PUSHVI,actor);
      expectSymbol(Op::BE,instanceId);
      } else {
      trophy.match = findSymbol(take(Op::PUSHV).symbol);
      trophy.context = findSymbol(take(Op::GMOVI).symbol);
      if(trophy.context!=actor && trophy.context!=vm.global_self())
        malformed();
      expectAivar(species);
      }
    if(trophy.match==nullptr)
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
      ++grantCount;
      }
    const auto choiceEnd = allowChoice && pc<end ? take(Op::B).address : 0u;
    if(pc!=end || trophy.grants.empty())
      malformed();
    return std::pair{std::move(trophy),choiceEnd};
    };

  std::array<Talent,14> taught;
  for(auto& learned : taught) {
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
  const auto unconditional = readChoice(nullptr);
  std::array<RandomTrophies,3> randomized;
  const std::array randomVariables{"GOBLINGREEN_RANDOMIZER","GOBLINBLACK_RANDOMIZER","ORC_RANDOMIZER"};
  for(size_t i=0;i<randomized.size();++i) {
    auto& roll = randomized[i];
    roll.bound = take(Op::PUSHI).immediate;
    expectSymbol(Op::BE,random);
    roll.result = findSymbol(randomVariables[i]);
    expectSymbol(Op::PUSHV,roll.result);
    take(Op::MOVI);
    const size_t groupCount = i==2 ? 5 : 1;
    for(size_t j=0;j<groupCount;++j)
      roll.choices.push_back(readChoice(roll.result));
    }
  expectSymbol(Op::PUSHV,yes);
  expectSymbol(Op::GMOVI,actor);
  expectAivar(given);
  take(Op::MOVI);
  take(Op::RSR);
  if(grantCount!=123)
    malformed();

  vm.override_function("B_GIVEDEATHINV",std::function<void(NpcRef)>{
    [this,actor,talent,yes,instanceId,random,create,taught=std::move(taught),
     unconditional=std::move(unconditional),randomized=std::move(randomized)](NpcRef npc) {
      const auto member = [&](Symbol* symbol) {
        auto current = std::static_pointer_cast<daedalus::INpc>(symbol->get_instance());
        vm.unsafe_set_gi(current);
        return current;
        };
      const auto id = [&](Symbol* symbol) {
        vm.push_reference(symbol);
        vm.unsafe_call_external(instanceId);
        return vm.pop_int();
        };
      const auto matches = [&](const Trophy& trophy) {
        if(trophy.matchInstance) {
          const auto expected = id(trophy.match);
          return expected==id(trophy.context);
          }
        const auto expected = trophy.match->get_int();
        const auto current = member(trophy.context);
        return (current ? current->aivar[species] : 0)==expected;
        };
      const auto grant = [&](const Trophy& trophy) {
        for(const auto& item : trophy.grants) {
          vm.push_reference(actor);
          vm.push_int(item.item);
          vm.push_int(item.amount);
          vm.unsafe_call_external(create);
          }
        };
      const auto choose = [&](const Trophies& choices,const Symbol* randomizer) {
        for(const auto& trophy : choices) {
          bool chanceMatches = true;
          if(trophy.chance) {
            const auto roll = randomizer->get_int();
            chanceMatches = trophy.chance->exact ? roll==trophy.chance->threshold : roll<=trophy.chance->threshold;
            }
          const bool targetMatches = matches(trophy);
          if(chanceMatches && targetMatches) {
            grant(trophy);
            return;
            }
          }
        };

      actor->set_instance(std::move(npc));
      if(const auto current = member(actor); current && current->aivar[given]==yes->get_int())
        return;
      for(const auto& learned : taught) {
        if(talent->get_int(learned.index)!=yes->get_int())
          continue;
        for(const auto& trophy : learned.trophies) {
          if(matches(trophy))
            grant(trophy);
          }
        }
      choose(unconditional,nullptr);
      // All three draws and their globals are observable even for non-monsters.
      for(const auto& roll : randomized) {
        vm.push_int(roll.bound);
        vm.unsafe_call_external(random);
        roll.result->set_int(vm.pop_int());
        for(const auto& choices : roll.choices)
          choose(choices,roll.result);
        }
      if(const auto current = member(actor))
        current->aivar[given] = yes->get_int();
    }},[this] {
    return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
    });
  }

bool GameScript::applyArmorSetEffect(Npc& npc, uint32_t callback) {
  const auto it = itemDefinitions.armorSetEffects.find(callback);
  if(it==itemDefinitions.armorSetEffects.end())
    return false;
  if(npc.isPlayer()) {
    const auto& effect = it->second;
    vm.find_symbol_by_index(effect.armorFlag)->set_int(effect.equip ? 1 : 0);
    if(vm.find_symbol_by_index(effect.beltFlag)->get_int()==1) {
      const auto amount = findSymbol("BA_BONUS01")->get_int();
      const auto delta = effect.equip ? amount : -amount;
      for(const auto protection : {PROT_EDGE,PROT_BLUNT,PROT_POINT,PROT_MAGIC,PROT_FIRE})
        npc.changeProtection(protection,npc.protection(protection)+delta);
      }
    }
  return true;
  }

bool GameScript::applyFightSkillItemEffect(Npc& npc, uint32_t callback) {
  const auto it = itemDefinitions.fightSkillEffects.find(callback);
  if(it==itemDefinitions.fightSkillEffects.end())
    return false;
  const auto& effect = it->second;
  if(effect.playerOnly && !npc.isPlayer())
    return true;
  return NpcHelperRuntime::addFightSkill(vm,std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()),
                                       effect.talent,effect.delta,npcDefinitions.helpers);
  }

namespace {
enum class Kind { HoldItem, Brush, Eat, Drink, Joint, Spit, Wash, Hammer };

struct ItemRoutine {
  std::string_view state, point, item;
  Kind             kind;
  int32_t          enterState;
  int32_t          timerSeconds = -1;
  };

constexpr ItemRoutine itemRoutines[] = {
  {"ZS_PLAY_LUTE",      "STAND", "ITMI_LUTE",       Kind::HoldItem, 1},
  {"ZS_RAKE_FP",        "PICK",  "ITMI_RAKE",       Kind::HoldItem, 1},
  {"ZS_SMOKE_JOINT",    "STAND", "ITMI_JOINT",      Kind::Joint,    0, 5},
  {"ZS_SPIT_FIRE",      "STAND", "ITLSTORCHFIRESPIT", Kind::Spit,   0, 10},
  {"ZS_STAND_DRINKING", "STAND", "ITFO_BOOZE",      Kind::Drink,    0, 7},
  {"ZS_STAND_EATING",   "STAND", {},                Kind::Eat,      0, 5},
  {"ZS_STAND_SWEEPING", "SWEEP", "ITMI_BROOM",      Kind::HoldItem, 1},
  {"ZS_SWEEP_FP",       "SWEEP", "ITMI_BRUSH",      Kind::Brush,    1},
  {"ZS_WASH_FP",        "WASH",  {},                Kind::Wash,     0},
  {"ZS_GUARD_HAMMER",   "STAND", {},                Kind::Hammer,   0, 5},
  };

constexpr std::array<std::string_view,4> food={"ITFO_APPLE","ITFO_CHEESE","ITFO_BACON","ITFOMUTTON"};
constexpr std::array<std::string_view,4> eatingGesture={
  "T_FOOD_RANDOM_1","T_FOODHUGE_RANDOM_1","T_MEAT_RANDOM_1","T_MEAT_RANDOM_1"
  };
}

int32_t GameScript::giveInventoryItems(const std::shared_ptr<daedalus::INpc>& giver,
                                       const std::shared_ptr<daedalus::INpc>& taker, int32_t item, int32_t amount) {
  findSymbol("B_GIVEINVITEMS.AMOUNT")->set_int(amount);
  findSymbol("B_GIVEINVITEMS.ITEMINSTANCE")->set_int(item);
  findSymbol("B_GIVEINVITEMS.TAKER")->set_instance(taker);
  findSymbol("B_GIVEINVITEMS.GIVER")->set_instance(giver);
  const bool giving = npc_isplayer(giver);
  if(giving && amount>npc_hasitems(giver,item))
    return 0;
  if(amount==0)
    return 1;

  // Stock scripts may grant the full amount even if an NPC giver has fewer items.
  npc_removeinvitems(giver,item,amount);
  createinvitems(taker,item,amount);
  const auto held = std::dynamic_pointer_cast<daedalus::IItem>(vm.global_item()->get_instance());
  const auto name = held ? held->name : std::string{};
  findSymbol("B_GIVEINVITEMS.ITEMNAME")->set_string(name);

  if(giving || npc_isplayer(taker)) {
    const auto string = [this](const char* symbol) -> const std::string& { return findSymbol(symbol)->get_string(); };
    const bool gold = uint32_t(item)==findSymbolIndex("ITMI_GOLD");
    std::string message;
    if(gold)
      message = std::to_string(amount)+string(giving ? "PRINT_GOLDGEGEBEN" : "PRINT_GOLDERHALTEN");
    else if(amount==1)
      message = name+string(giving ? "PRINT_ADDON_GEGEBEN" : "PRINT_ADDON_ERHALTEN");
    else
      message = std::to_string(amount)+string(giving ? "PRINT_ITEMSGEGEBEN" : "PRINT_ITEMSERHALTEN")+" ("+name+")";
    const auto position = gold ? (giving ? "YPOS_GOLDGIVEN" : "YPOS_GOLDTAKEN")
                               : (giving ? "YPOS_ITEMGIVEN" : "YPOS_ITEMTAKEN");
    findSymbol("B_GIVEINVITEMS.CONCATTEXT")->set_string(message);
    ai_printscreen(message,-1,findSymbol(position)->get_int(),string("FONT_SCREENSMALL"),2);
    }
  return 1;
  }

bool GameScript::runItemRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  using Phase=ActivityDefinitions::Phase;
  auto phase=Phase::Init;
  std::string_view name=symbol.name();
  if(name.ends_with("_LOOP")) {
    phase=Phase::Loop;
    name.remove_suffix(5);
    }
  else if(name.ends_with("_END")) {
    phase=Phase::End;
    name.remove_suffix(4);
    }
  const auto it=std::find_if(std::begin(itemRoutines),std::end(itemRoutines),[name](const auto& activity) {
    return activity.state==name;
    });
  if(it==std::end(itemRoutines))
    return false;
  const auto& a=*it;
  result=LOOP_CONTINUE;
  // The position and meal choice are already saved in the NPC record.
  auto& foodChoice=npc.handle().aivar[18];
  auto& stage=npc.handle().aivar[19];
  constexpr auto held=0, enter=1, walking=2;
  const auto itemName=[&]() -> std::string_view {
    if(a.kind!=Kind::Eat)
      return a.item;
    return foodChoice>=0 && size_t(foodChoice)<food.size() ? food[size_t(foodChoice)] : std::string_view{};
    };
  const auto roll=[&](uint32_t bound) {
    const auto value=rollRandom(bound,&npc,true);
    if(auto* local=findSymbol(symbol.name()+".RANDOM"))
      local->set_int(int32_t(value));
    return value;
    };
  const auto animate=[&](std::string_view animation) {
    npc.aiPush(AiQueue::aiPlayAnimBs(animation,BS_ITEMINTERACT));
    };
  const auto use=[&](std::string_view item, int32_t state) {
    npc.aiPush(AiQueue::aiUseItemToState(int32_t(findSymbolIndex(item)),state));
    };
  if(phase==Phase::Init) {
    const auto ensureItem=[&](std::string_view item) {
      const auto id=uint32_t(findSymbolIndex(item));
      if(npc.itemCount(id)==0)
        storeItem(npc.addItem(id,1));
      };
    NpcActivityHost host(world(),npc,[this](std::string_view) {
      setPerceptionPreset(true);
      });
    ActivityDefinitions::resetForActivity(host,"PERCEPTION_SET_NORMAL");
    host.setWalkMode(1);
    const auto& wp=npc.handle().wp;
    if(a.kind==Kind::Brush ? host.nearestWaypoint()!=wp : host.distanceToWaypoint(wp)>500)
      host.gotoWaypoint(wp);
    if(a.kind==Kind::Eat) {
      stage=enter;
      const auto choice=roll(uint32_t(food.size()));
      ensureItem(food[choice]);
      foodChoice=int32_t(choice);
      }
    else {
      if(!a.item.empty())
        ensureItem(a.item);
      if(a.kind!=Kind::Brush && a.kind!=Kind::Spit)
        stage=enter;
      }
    return true;
    }
  if(phase==Phase::End) {
    if(a.kind==Kind::Wash)
      npc.aiPush(AiQueue::aiPlayAnim("T_WASH_2_STAND"));
    else if(a.kind==Kind::Hammer)
      npc.aiPush(AiQueue::aiPlayAnim("T_HGUARD_2_STAND"));
    else {
      if(a.kind==Kind::Eat)
        animate("T_POTION_RANDOM_2");
      if(const auto item=itemName(); !item.empty())
        use(item,-1);
      }
    return true;
    }
  const bool onPoint=a.kind!=Kind::Spit && npc.isOnFreePoint(a.point);
  if(onPoint) {
    npc.aiPush(AiQueue::aiAlignToFp());
    if(stage==walking)
      stage=enter;
    }
  else if(const auto* point=world().findFreePoint(npc,a.point)) {
    npc.aiPush(AiQueue::aiGoToPoint(*point));
    if(a.kind!=Kind::Spit)
      npc.aiPush(AiQueue::aiStandup());
    npc.aiPush(AiQueue::aiAlignToFp());
    if(a.kind!=Kind::Spit)
      stage=walking;
    }
  else {
    npc.aiPush(AiQueue::aiAlignToWp());
    if(a.kind!=Kind::Spit && stage==walking)
      stage=enter;
    }

  if(a.kind!=Kind::Spit && stage==enter) {
    if(a.kind==Kind::Wash) {
      if(onPoint) {
        npc.aiPush(AiQueue::aiAlignToFp());
        npc.aiPush(AiQueue::aiStandup());
        npc.aiPush(AiQueue::aiPlayAnim("T_STAND_2_WASH"));
        stage=held;
        }
      }
    else if(a.kind==Kind::Hammer) {
      npc.aiPush(AiQueue::aiPlayAnim("T_STAND_2_HGUARD"));
      stage=held;
      }
    else if(const auto item=itemName(); !item.empty()) {
      use(item,a.enterState);
      if(a.kind==Kind::Eat && foodChoice==0)
        animate("T_FOOD_RANDOM_2");
      stage=held;
      }
    }
  if(a.kind==Kind::Hammer && NpcEngineCalls::distanceToWaypoint(world(),npc,"NW_MONASTERY_SANCTUM_01")<=500) {
    const auto hammer=int32_t(findSymbolIndex("HOLY_HAMMER_MIS"));
    if(npc.itemCount(uint32_t(hammer))==1) {
      npc.delItem(uint32_t(hammer),1);
      wld_insertitem(hammer,"FP_HAMMER");
      }
    }
  if(a.timerSeconds<0 || (a.kind!=Kind::Spit && stage!=held) || int32_t(npc.stateTime()/1000)<=a.timerSeconds)
    return true;

  switch(a.kind) {
    case Kind::Eat:
      animate(foodChoice>=0 && size_t(foodChoice)<eatingGesture.size() ? eatingGesture[size_t(foodChoice)] : eatingGesture[1]);
      break;
    case Kind::Drink: {
      const auto value=roll(10);
      if(value==0)
        animate("T_POTION_RANDOM_3");
      animate("T_POTION_RANDOM_1");
      if(value==1)
        animate("T_POTION_RANDOM_2");
      break;
      }
    case Kind::Joint:
      animate("T_JOINT_RANDOM_1");
      break;
    case Kind::Spit:
      use(a.item,5);
      use(a.item,0);
      break;
    case Kind::Hammer:
      if(roll(10)==0)
        npc.aiPush(AiQueue::aiPlayAnim("T_HGUARD_LOOKAROUND"));
      break;
    default:
      break;
    }
  npc.setStateTime(0);
  return true;
  }

namespace {
void refreshSmithWeaponDescriptions(daedalus::DaedalusVm& vm) {
  struct Recipe {
    std::string_view info, title, cost = {};
    };
  static constexpr Recipe recipes[] = {
    {"PC_ITMW_1H_COMMON",       "NAME_ITMW_1H_COMMON_01"},
    {"PC_ITMW_1H_SPECIAL_01",   "NAME_ITMW_1H_SPECIAL_01", "PRINT_SMITH_1H_SPECIAL_01"},
    {"PC_ITMW_2H_SPECIAL_01",   "NAME_ITMW_2H_SPECIAL_01", "PRINT_SMITH_2H_SPECIAL_01"},
    {"PC_ITMW_1H_SPECIAL_02",   "NAME_ITMW_1H_SPECIAL_02", "PRINT_SMITH_1H_SPECIAL_02"},
    {"PC_ITMW_2H_SPECIAL_02",   "NAME_ITMW_2H_SPECIAL_02", "PRINT_SMITH_2H_SPECIAL_02"},
    {"PC_ITMW_1H_SPECIAL_03",   "NAME_ITMW_1H_SPECIAL_03", "PRINT_SMITH_1H_SPECIAL_03"},
    {"PC_ITMW_2H_SPECIAL_03",   "NAME_ITMW_2H_SPECIAL_03", "PRINT_SMITH_2H_SPECIAL_03"},
    {"PC_ITMW_1H_SPECIAL_04",   "NAME_ITMW_1H_SPECIAL_04", "PRINT_SMITH_1H_SPECIAL_04"},
    {"PC_ITMW_2H_SPECIAL_04",   "NAME_ITMW_2H_SPECIAL_04", "PRINT_SMITH_2H_SPECIAL_04"},
    {"PC_WEAPON_1H_HARAD_01",   "NAME_ADDON_HARAD_01"},
    {"PC_WEAPON_1H_HARAD_02",   "NAME_ADDON_HARAD_02"},
    {"PC_WEAPON_1H_HARAD_03",   "NAME_ADDON_HARAD_03"},
    {"PC_WEAPON_1H_HARAD_04",   "NAME_ADDON_HARAD_04"},
    };
  auto* description=vm.find_symbol_by_name("C_INFO.DESCRIPTION");
  for(const auto& recipe:recipes) {
    const auto info=vm.find_symbol_by_name(recipe.info)->get_instance();
    const auto& title=vm.find_symbol_by_name(recipe.title)->get_string();
    if(recipe.cost.empty()) {
      vm.set_string(info,description,0,title);
      } else {
      std::string text=title;
      text+=vm.find_symbol_by_name(recipe.cost)->get_string();
      vm.set_string(info,description,0,text);
      }
    }
  }
}

bool GameScript::invokeNativeMobState(const daedalus::DaedalusSymbol& symbol) {
  if(!nativeRoutineProgram || !activityEvents || floorDefer.active() ||
     ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off)
    return false;

  // Ordinary NPCs bind the hero without opening documents or production menus.
  // Sleep also checks the diagnostic Rockefeller instance. Smithing updates recipe captions.
  static constexpr std::string_view callbacks[] = {
    "SLEEPABIT_S1", "POTIONALCHEMY_S1", "MAKERUNE_S1", "GOLDHACKEN_S1", "PRAYIDOL_S1", "PRAYSHRINE_S1",
    "SMITHWEAPON_S1",
    "USE_BOOKSTAND_01_S1", "USE_BOOKSTAND_ADDON_BL_S1", "USE_BOOKSTANDDEMENTOR_S1",
    "USE_BOOKSTANDALCHEMY1_S1", "USE_BOOKSTANDALCHEMY2_S1", "USE_BOOKSTANDALCHEMY3_S1",
    "USE_BOOKSTANDANIMALS1_S1", "USE_BOOKSTANDANIMALS2_S1", "USE_BOOKSTANDANIMALS3_S1",
    "USE_BOOKSTANDHISTORY1_S1", "USE_BOOKSTANDHISTORY2_S1", "USE_BOOKSTANDHISTORY3_S1",
    "USE_BOOKSTAND_KREISE_01_S1", "USE_BOOKSTAND_KREISE_02_S1", "USE_BOOKSTAND_KREISE_03_S1",
    "USE_BOOKSTAND_KREISE_04_S1", "USE_BOOKSTAND_KREISE_05_S1", "USE_BOOKSTAND_KREISE_06_S1",
    "BOOKSTAND_MILTEN_01_S1", "BOOKSTAND_MILTEN_02_S1", "BOOKSTAND_MILTEN_03_S1", "BOOKSTAND_ENGOR_01_S1",
    };
  const auto& name = symbol.name();
  if(std::ranges::find(callbacks,name)==std::end(callbacks))
    return false;
  const auto self = hlp_getinstanceid(vm.global_self()->get_instance());
  const auto hero = hlp_getnpc(int32_t(findSymbolIndex("PC_HERO")));
  if(self==hlp_getinstanceid(hero))
    return false;
  if(name=="SLEEPABIT_S1") {
    const auto rock = hlp_getnpc(int32_t(findSymbolIndex("PC_ROCKEFELLER")));
    if(self==hlp_getinstanceid(rock))
      return false;
    findSymbol("SLEEPABIT_S1.HER")->set_instance(hero);
    findSymbol("SLEEPABIT_S1.ROCK")->set_instance(rock);
    } else {
    findSymbol(name+".HER")->set_instance(hero);
    }
  if(name=="SMITHWEAPON_S1")
    refreshSmithWeaponDescriptions(vm);
  return true;
  }
