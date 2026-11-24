#include "baseimport.h"
#include "npcsnapshot.h"

#include "daedalus/script.h"
#include "game/definitions/gameplaydefinitions.h"
#include "game/definitions/tradeinventorydata.h"

namespace {
using namespace TradeInventoryData;
using Op = daedalus::DaedalusOpcode;
using Symbol = daedalus::DaedalusSymbol;

Record capture(const daedalus::DaedalusScript& script) {
  const auto index = [&](std::string_view name) { return script.find_symbol_by_name(name)->index(); };
  Record record;
  auto* lookup = script.find_symbol_by_name("HLP_GETNPC");
  auto* instanceId = script.find_symbol_by_name("HLP_GETINSTANCEID");
  auto* hasItems = script.find_symbol_by_name("NPC_HASITEMS");
  auto* removeItems = script.find_symbol_by_name("NPC_REMOVEINVITEMS");
  auto* validNpc = script.find_symbol_by_name("HLP_ISVALIDNPC");
  auto* inState = script.find_symbol_by_name("NPC_ISINSTATE");
  auto* runes = script.find_symbol_by_name("B_CLEARRUNEINV");
  auto* junk = script.find_symbol_by_name("B_CLEARJUNKTRADEINV");
  auto* dead = script.find_symbol_by_name("B_CLEARDEADTRADER");
  auto* runesActor = script.find_symbol_by_name("B_CLEARRUNEINV.SLF");
  auto* runeHero = script.find_symbol_by_name("B_CLEARRUNEINV.HER");
  auto* junkActor = script.find_symbol_by_name("B_CLEARJUNKTRADEINV.SLF");
  auto* tradeActor = script.find_symbol_by_name("B_GIVETRADEINV.SLF");
  auto* aivar = script.find_symbol_by_name("C_NPC.AIVAR");
  auto* chapter = script.find_symbol_by_name("KAPITEL");
  const auto deadState = int32_t(index("ZS_DEAD"));
  const auto unconsciousState = int32_t(index("ZS_UNCONSCIOUS"));
  constexpr uint16_t stockChapter = 49; // AIV_INVCHAPTER.
  uint32_t pc = runes->address();
  const auto malformed = [&] { throw std::runtime_error("Invalid stock inventory rule at "+std::to_string(pc)); };
  const auto take = [&](Op expected) {
    const auto op = script.instruction_at(pc);
    if(pc>=script.size() || op.op!=expected)
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
  record.hero = take(Op::PUSHI).immediate;
  if(record.hero!=int32_t(index("PC_HERO")))
    malformed();
  expectSymbol(Op::BE,lookup);
  saveActor(runeHero);
  expectSymbol(Op::PUSHVI,runeHero);
  expectSymbol(Op::BE,instanceId);
  expectSymbol(Op::PUSHVI,script.find_symbol_by_name("SELF"));
  expectSymbol(Op::BE,instanceId);
  take(Op::NEQ);
  const auto runesEnd = take(Op::BZ).address;
  auto& runePairs = record.runes;
  runePairs.resize(54);
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
  auto& junkPairs = record.junk;
  junkPairs.resize(73);
  for(auto& pair : junkPairs)
    pair = readPair(junkActor);
  if(pc!=junkEnd)
    malformed();
  take(Op::RSR);

  pc = script.find_symbol_by_name("B_GIVETRADEINV")->address();
  saveActor(tradeActor);
  for(int i=0;i<50;++i) {
    const auto source = take(Op::PUSHI).immediate;
    expectSymbol(Op::BE,lookup);
    const auto* alias = script.find_symbol_by_index(take(Op::PUSHVI).symbol);
    if(alias==nullptr || !alias->name().starts_with("B_GIVETRADEINV.TRD_"))
      malformed();
    take(Op::MOVVI);
    record.traders.emplace_back(source,alias->index());
    }
  expectSymbol(Op::PUSHVI,tradeActor);
  expectCall(runes);
  expectSymbol(Op::PUSHV,chapter);
  expectSymbol(Op::GMOVI,tradeActor);
  expectChapterSlot();
  take(Op::LTE);
  const auto restockEnd = take(Op::BZ).address;
  const auto readTraderBranch = [&] {
    auto* alias = script.find_symbol_by_index(take(Op::PUSHVI).symbol);
    const auto known = [&](const auto& trader) { return trader.second==alias->index(); };
    if(alias==nullptr || std::ranges::none_of(record.traders,known))
      malformed();
    expectSymbol(Op::BE,instanceId);
    expectSymbol(Op::PUSHVI,tradeActor);
    expectSymbol(Op::BE,instanceId);
    take(Op::EQ);
    return std::pair{alias,take(Op::BZ).address};
    };
  // Lookup, restock and cleanup each have their own authored order.
  for(int i=0;i<50;++i) {
    const auto [alias,next] = readTraderBranch();
    expectSymbol(Op::PUSHVI,tradeActor);
    expectCall(junk);
    expectSymbol(Op::PUSHVI,tradeActor);
    const auto* body = script.find_symbol_by_address(take(Op::BL).address);
    if(body==nullptr || !body->name().starts_with("B_GIVETRADEINV_") || pc!=next)
      malformed();
    record.restocks.emplace_back(alias->index(),body->index());
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
  for(int i=0;i<50;++i) {
    const auto [alias,next] = readTraderBranch();
    expectSymbol(Op::PUSHVI,tradeActor);
    expectCall(dead);
    if(pc!=next)
      malformed();
    record.defeated.push_back(alias->index());
    }
  if(pc!=tradeEnd)
    malformed();
  take(Op::RSR);

  auto* create = script.find_symbol_by_name("CREATEINVITEMS");
  auto* yes = script.find_symbol_by_name("TRUE");
  auto* no = script.find_symbol_by_name("FALSE");
  auto* oldWorldFlag = script.find_symbol_by_name("ENTEROW_KAPITEL2");
  auto* secretSign = script.find_symbol_by_name("KNOWS_SECRETSIGN");
  auto* militia = script.find_symbol_by_name("GIL_MIL");
  auto* paladin = script.find_symbol_by_name("GIL_PAL");
  auto* guild = script.find_symbol_by_name("C_NPC.GUILD");
  const auto pushes = [&](const Symbol* symbol) {
    const auto next = script.instruction_at(pc);
    return next.op==Op::PUSHV && next.symbol==symbol->index();
    };
  const auto readGrants = [&](const Symbol* actor, uint32_t end) {
    std::vector<Grant> grants;
    while(pc<end && script.instruction_at(pc).op==Op::PUSHVI) {
      Grant grant;
      grant.recipient = take(Op::PUSHVI).symbol;
      if(grant.recipient!=actor->index() && grant.recipient!=index("SELF"))
        malformed();
      if(script.instruction_at(pc).op==Op::PUSHV) {
        // Samuel's stock reads the live item index.
        const auto* item = script.find_symbol_by_name("RI_PROTFIRE02");
        expectSymbol(Op::PUSHV,item);
        grant.itemSymbol = item->index();
        } else {
        grant.item = take(Op::PUSHI).immediate;
        }
      grant.amount = take(Op::PUSHI).immediate;
      expectSymbol(Op::BE,create);
      grants.push_back(grant);
      }
    return grants;
    };
  const auto readStock = [&](const Symbol& body) {
    pc = body.address();
    auto* actor = script.find_symbol_by_name(body.name()+".SLF");
    saveActor(actor);
    Stock stock{body.index(),actor->index(),{}};
    while(script.instruction_at(pc).op!=Op::RSR) {
      Batch batch;
      uint32_t end = script.size();
      if(script.instruction_at(pc).op==Op::PUSHV) {
        expectSymbol(Op::PUSHV,no);
        const auto* granted = script.find_symbol_by_index(take(Op::PUSHV).symbol);
        if(granted==nullptr || granted->name().find("_ITEMSGIVEN_CHAPTER_")==std::string::npos)
          malformed();
        batch.granted = granted->index();
        take(Op::EQ);
        if(pushes(yes)) {
          expectSymbol(Op::PUSHV,yes);
          expectSymbol(Op::PUSHV,oldWorldFlag);
          take(Op::EQ);
          batch.requiresOldWorld = true;
          }
        if(script.instruction_at(pc).op==Op::PUSHI) {
          batch.minimumChapter = take(Op::PUSHI).immediate;
          expectSymbol(Op::PUSHV,chapter);
          take(Op::GTE);
          take(Op::AND);
          }
        if(batch.requiresOldWorld) {
          if(!batch.minimumChapter)
            malformed();
          take(Op::AND);
          }
        end = take(Op::BZ).address;
        }
      batch.grants = readGrants(actor,end);
      std::optional<Bonus::Condition> bonus;
      const auto bonusStart = script.instruction_at(pc);
      if(pushes(militia)) {
        for(auto* allowed : {militia,paladin}) {
          expectSymbol(Op::PUSHV,allowed);
          expectSymbol(Op::GMOVI,script.find_symbol_by_name("HERO"));
          expectSymbol(Op::PUSHV,guild);
          take(Op::EQ);
          }
        take(Op::ORR);
        bonus = Bonus::Condition::HeroGuild;
        } else if(pushes(yes) && script.instruction_at(pc+bonusStart.size).symbol==secretSign->index()) {
        expectSymbol(Op::PUSHV,yes);
        expectSymbol(Op::PUSHV,secretSign);
        take(Op::EQ);
        bonus = Bonus::Condition::SecretSign;
        }
      if(bonus) {
        const auto bonusEnd = take(Op::BZ).address;
        if(bonusEnd>=end)
          malformed();
        auto grants = readGrants(actor,bonusEnd);
        if(pc!=bonusEnd || grants.empty())
          malformed();
        // Gorax's third-chapter batch includes a grant after the guild check.
        batch.bonus = Bonus{*bonus,std::move(grants),readGrants(actor,end)};
        }
      if(batch.granted) {
        expectSymbol(Op::PUSHV,yes);
        expectSymbol(Op::PUSHV,script.find_symbol_by_index(*batch.granted));
        take(Op::MOVI);
        if(pc!=end)
          malformed();
        } else if(batch.grants.empty() || !stock.batches.empty() || script.instruction_at(pc).op!=Op::RSR) {
        malformed();
        }
      stock.batches.push_back(std::move(batch));
      }
    take(Op::RSR);
    return stock;
    };
  for(const auto& restock : record.restocks)
    record.stock.push_back(readStock(*script.find_symbol_by_index(restock.second)));
  return record;
  }
}

void BaseImport::convertTradeInventory(const std::filesystem::path& base) {
  auto source = NpcSnapshot::load(base);
  if(source.sha256!=GameplayDefinitions::stockProgram)
    return;

  const auto script = daedalus::DaedalusScript::from_program(std::move(source.data));
  TradeInventoryData::write(base,capture(script));
  }
