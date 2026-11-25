#include "gamescript_programs.h"

#include <Tempest/Log>
#include <algorithm>
#include <array>
#include <stdexcept>

#include "gamescript_inventorytransfer.h"
#include "game/definitions/nativebasecatalog.h"
#include "game/serialize.h"
#include "storygraph/nativeexec.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

namespace {

using storygraph::NativeFunction;
using storygraph::NativeInstance;
using storygraph::NativeSlot;
using storygraph::NativeValue;

constexpr uint32_t BennetNpcId      = 11668;
constexpr uint32_t BennetNugget    = 6928;

InventoryTransfer::Text inventoryNoticeText(const StockStoryPrograms::Catalog& catalog) {
  return {
    catalog.symbols.at("ITMI_GOLD"),
    catalog.strings.at("PRINT_GOLDGEGEBEN"),catalog.strings.at("PRINT_GOLDERHALTEN"),
    catalog.strings.at("PRINT_ADDON_GEGEBEN"),catalog.strings.at("PRINT_ADDON_ERHALTEN"),
    catalog.strings.at("PRINT_ITEMSGEGEBEN"),catalog.strings.at("PRINT_ITEMSERHALTEN"),
    catalog.integers.at("YPOS_GOLDGIVEN"),catalog.integers.at("YPOS_GOLDTAKEN"),
    catalog.integers.at("YPOS_ITEMGIVEN"),catalog.integers.at("YPOS_ITEMTAKEN")
    };
  }

class BennetOreHost final : public storygraph::NativeExecutionHost {
  public:
    struct Progress {
      int32_t& mission;
      int32_t& teaching;
      int32_t  chapter;
      int32_t  smithTalent;
      int32_t  rescueMission;
      };

    using Transfer = std::function<int32_t(Npc&,Npc&,int32_t)>;
    using Output = std::function<void(Npc&,Npc&,std::string_view)>;

    BennetOreHost(Npc& player, Npc& npc, Progress progress, Transfer transfer = {}, Output output = {})
      : actors{nullptr,&npc,&player}, progress(progress), transfer(std::move(transfer)), output(std::move(output)) {}

    bool hasVariable(std::string_view name, bool instanceOnly) const override {
      return name=="SELF" || name=="OTHER" ||
             (!instanceOnly && (name=="MIS_BENNET_BRINGORE" || name=="BENNET_TEACHSMITH" ||
                                name=="KAPITEL" || name=="PLAYER_TALENT_SMITH" || name=="MIS_RESCUEBENNET"));
      }

    std::optional<int32_t> symbolIndex(std::string_view name) const override {
      if(name=="ITMI_NUGGET")
        return int32_t(BennetNugget);
      return std::nullopt;
      }

    std::optional<NativeValue> read(const NativeSlot& slot, std::string& error) override {
      if(slot.index==0 && slot.kind==NativeSlot::Instance) {
        if(slot.name=="SELF")
          return NativeValue::instance(1);
        if(slot.name=="OTHER")
          return NativeValue::instance(2);
        }
      if(slot.index==0 && slot.kind==NativeSlot::Variable) {
        if(slot.name=="MIS_BENNET_BRINGORE")
          return NativeValue::integer(progress.mission);
        if(slot.name=="BENNET_TEACHSMITH")
          return NativeValue::integer(progress.teaching);
        if(slot.name=="KAPITEL")
          return NativeValue::integer(progress.chapter);
        if(slot.name=="PLAYER_TALENT_SMITH")
          return NativeValue::integer(progress.smithTalent);
        if(slot.name=="MIS_RESCUEBENNET")
          return NativeValue::integer(progress.rescueMission);
        }
      if(slot.index==0 && slot.kind==NativeSlot::Member && slot.name=="GUILD") {
        if(auto* npc = actor(slot.context))
          return NativeValue::integer(npc->handle().guild);
        }
      error = "unsupported Bennet ore variable: "+std::string(slot.name);
      return std::nullopt;
      }

    bool write(const NativeSlot& slot, const NativeValue& value, std::string& error) override {
      if(output && slot.index==0 && slot.kind==NativeSlot::Variable && value.type==NativeValue::Int) {
        if(slot.name=="MIS_BENNET_BRINGORE") {
          progress.mission = value.i;
          return true;
          }
        if(slot.name=="BENNET_TEACHSMITH") {
          progress.teaching = value.i;
          return true;
          }
        }
      error = "unsupported Bennet ore write: "+std::string(slot.name);
      return false;
      }

    std::optional<NativeFunction> findFunction(std::string_view name) const override {
      if(name=="NPC_HASITEMS")
        return NativeFunction{{NativeValue::Inst,NativeValue::Int},NativeValue::Int,nullptr};
      if(name=="AI_OUTPUT" && output)
        return NativeFunction{{NativeValue::Inst,NativeValue::Inst,NativeValue::Str},std::nullopt,nullptr};
      if(name=="B_GIVEINVITEMS" && transfer)
        return NativeFunction{{NativeValue::Inst,NativeValue::Inst,NativeValue::Int,NativeValue::Int},NativeValue::Int,nullptr};
      return std::nullopt;
      }

    std::optional<NativeValue> invoke(std::string_view name, const std::vector<NativeValue>& args,
                                      std::string& error) override {
      const auto fn = findFunction(name);
      if(!fn || args.size()!=fn->parameters.size()) {
        error = "unexpected Bennet ore call: "+std::string(name);
        return std::nullopt;
        }
      auto* from = actor(args[0].inst);
      if(from!=nullptr) {
        if(name=="NPC_HASITEMS" && args[1].i==int32_t(BennetNugget))
          return NativeValue::integer(int32_t(from->itemCount(BennetNugget)));
        if(auto* to = actor(args[1].inst); to!=nullptr && from!=to) {
          if(name=="AI_OUTPUT") {
            output(*from,*to,args[2].s);
            return NativeValue{};
            }
          if(name=="B_GIVEINVITEMS" && args[2].i==int32_t(BennetNugget) && args[3].i>0)
            return NativeValue::integer(transfer(*from,*to,args[3].i));
          }
        }
      error = "invalid Bennet ore arguments for "+std::string(name);
      return std::nullopt;
      }

  private:
    Npc* actor(NativeInstance id) const {
      return id<actors.size() ? actors[size_t(id)] : nullptr;
      }

    const std::array<Npc*,3> actors;
    Progress                 progress;
    Transfer                 transfer;
    Output                   output;
  };

class OrnamentDialogHost final : public storygraph::NativeExecutionHost {
  public:
    OrnamentDialogHost(const StockStoryPrograms::Catalog& catalog, Npc* self, Npc* other,
                       const World& world, InventoryTransfer::Variables& variables, std::string_view previousItemName)
      : catalog(catalog), actors{nullptr,self,other}, world(world), variables(variables), previousItemName(previousItemName) {}

    bool hasVariable(std::string_view name, bool) const override {
      return name=="SELF" || name=="OTHER";
      }

    std::optional<int32_t> symbolIndex(std::string_view name) const override {
      const auto it = catalog.symbols.find(name);
      if(it==catalog.symbols.end())
        return std::nullopt;
      return int32_t(it->second);
      }

    std::optional<NativeValue> read(const NativeSlot& slot, std::string& error) override {
      if(slot.index==0 && slot.kind!=NativeSlot::Member) {
        if(slot.name=="SELF")
          return NativeValue::instance(actors[1]!=nullptr ? 1 : 0);
        if(slot.name=="OTHER")
          return NativeValue::instance(actors[2]!=nullptr ? 2 : 0);
        }
      error = "unsupported ornament dialog variable: "+std::string(slot.name);
      return std::nullopt;
      }

    bool write(const NativeSlot& slot, const NativeValue&, std::string& error) override {
      error = "unsupported ornament dialog write: "+std::string(slot.name);
      return false;
      }

    std::optional<NativeFunction> findFunction(std::string_view name) const override {
      if(name!="B_GIVEINVITEMS")
        return std::nullopt;
      return NativeFunction{{NativeValue::Inst,NativeValue::Inst,NativeValue::Int,NativeValue::Int},NativeValue::Int,nullptr};
      }

    std::optional<NativeValue> invoke(std::string_view name, const std::vector<NativeValue>& args,
                                      std::string& error) override {
      if(name!="B_GIVEINVITEMS" || args.size()!=4 || args[0].type!=NativeValue::Inst ||
         args[1].type!=NativeValue::Inst || args[2].type!=NativeValue::Int || args[3].type!=NativeValue::Int ||
         args[0].inst>=actors.size() || args[1].inst>=actors.size() || transfer) {
        error = "invalid B_GIVEINVITEMS call in ornament dialog";
        return std::nullopt;
        }
      auto* giver = actors[size_t(args[0].inst)];
      auto* taker = actors[size_t(args[1].inst)];
      const auto item = args[2].i;
      const auto amount = args[3].i;
      variables.amount = amount;
      variables.item = item;
      writes.emplace_back("B_GIVEINVITEMS.TAKER",world.npcId(taker));
      writes.emplace_back("B_GIVEINVITEMS.GIVER",world.npcId(giver));

      transfer = InventoryTransfer::run(giver,taker,uint32_t(item),amount,previousItemName,inventoryNoticeText(catalog));
      if(transfer->itemName)
        variables.itemName = *transfer->itemName;
      if(transfer->notice)
        variables.notice = transfer->notice->text;
      return NativeValue::integer(transfer->value);
      }

    std::vector<std::pair<std::string_view,uint32_t>> writes;
    std::optional<InventoryTransfer::Result> transfer;

  private:
    const StockStoryPrograms::Catalog& catalog;
    const std::array<Npc*,3> actors;
    const World& world;
    InventoryTransfer::Variables& variables;
    std::string_view previousItemName;
  };

}

void GameScript::initBennetOre() {
  const auto& native = *baseNativeCatalog();
  auto& programs = storyPrograms();
  const auto& catalog = programs.prepared;
  if(!itemDefinitions.items.contains(BennetNugget))
    throw std::runtime_error("Bennet ore quest lacks prepared game data");

  const auto bindDialog = [&](uint32_t infoId, uint32_t conditionId, uint32_t answerId, const std::string& name) {
    const auto* info = NativeBaseCatalog::findIdentity(native.identities.dialogs,infoId);
    if(info==nullptr || info->name!=name)
      throw std::runtime_error("Bennet ore dialog identity differs from the native catalog: "+name);
    const auto* condition = programs.program(conditionId);
    const auto* answer = programs.program(answerId);
    if(condition==nullptr || condition->name!=name+"_CONDITION" || !condition->parameters.empty() || condition->returnType!=2 ||
       answer==nullptr || answer->name!=name+"_INFO" || !answer->parameters.empty() || answer->returnType!=0)
      throw std::runtime_error("Bennet ore dialog lacks prepared game data: "+name);
    return BennetOreDialog{infoId,conditionId,answerId,&condition->program,&answer->program};
    };
  const std::array dialogs = {
    bindDialog(23816,23817,23818,"DIA_BENNET_WANNASMITHORE"),
    bindDialog(23819,23820,23821,"DIA_BENNET_WHEREORE"),
    bindDialog(23822,23823,23824,"DIA_BENNET_BRINGORE")
    };

  const auto* nugget = NativeBaseCatalog::findIdentity(native.identities.items,BennetNugget);
  if(nugget==nullptr || nugget->name!="ITMI_NUGGET" || ownedInstance(BennetNugget)==nullptr ||
     !globalIntSymbol("PLAYER_TALENT_SMITH") || !globalIntSymbol("MIS_RESCUEBENNET"))
    throw std::runtime_error("Bennet ore quest lacks native item or progress bindings");
  for(const auto name : {"ITEM","B_GIVEINVITEMS.GIVER","B_GIVEINVITEMS.TAKER"})
    (void)storyActorSlot(name);
  auto ore = std::make_shared<ItemInstance>();
  static_cast<ItemRecord&>(*ore) = itemDefinitions.items.at(BennetNugget);
  ore->setRecordIdentity(BennetNugget);
  bennetOre = std::make_unique<BennetOre>(BennetOre{
    dialogs,std::move(ore),inventoryNoticeText(catalog),catalog.strings.at("FONT_SCREENSMALL")
    });
  }

void GameScript::saveBennetProgress(Serialize& fout) const {
  fout.write(bennetOreMission,bennetOreTeaching);
  }

void GameScript::loadBennetProgress(Serialize& fin) {
  fin.read(bennetOreMission,bennetOreTeaching);
  }

bool GameScript::ownsNativeBennetProgram(uint32_t function) const {
  return bennetOre!=nullptr && std::any_of(bennetOre->dialogs.begin(),bennetOre->dialogs.end(),[&](const BennetOreDialog& dialog) {
    return dialog.conditionId==function || dialog.answerId==function;
    });
  }

const GameScript::BennetOreDialog* GameScript::bennetOreDialog(const DialogInstance& info) const {
  if(bennetOre==nullptr || info.npc!=int32_t(BennetNpcId))
    return nullptr;
  for(const auto& dialog : bennetOre->dialogs)
    if(dialog.infoId==info.symbol_index() && info.condition==int32_t(dialog.conditionId) &&
       info.information==int32_t(dialog.answerId))
      return &dialog;
  return nullptr;
  }

bool GameScript::bennetOffer(const BennetOreDialog& dialog, Npc& player, Npc& npc) {
  const auto smith = globalIntSymbol("PLAYER_TALENT_SMITH").value();
  const auto rescue = globalIntSymbol("MIS_RESCUEBENNET").value();
  BennetOreHost host(player,npc,{bennetOreMission,bennetOreTeaching,storyChapter,
                               smith,rescue});
  const auto result = storygraph::NativeInterpreter(host).run(*dialog.condition);
  if(!result.ok)
    Tempest::Log::e("[story] Bennet ore condition: ",result.error);
  return result.ok && result.returned.type==NativeValue::Int && result.returned.i!=0;
  }

bool GameScript::bennetExecute(const BennetOreDialog& dialog, Npc& player, Npc& npc) {
  auto& binding = *bennetOre;
  auto& vars = inventoryTransferVariables;
  const auto smith = globalIntSymbol("PLAYER_TALENT_SMITH").value();
  const auto rescue = globalIntSymbol("MIS_RESCUEBENNET").value();
  BennetOreHost host(player,npc,{bennetOreMission,bennetOreTeaching,storyChapter,smith,rescue},
    [&](Npc& giver, Npc& taker, int32_t amount) {
      const bool newStack = taker.itemCount(BennetNugget)==0;
      storyActorSlot("B_GIVEINVITEMS.GIVER") = std::static_pointer_cast<NpcInstance>(giver.handlePtr());
      storyActorSlot("B_GIVEINVITEMS.TAKER") = std::static_pointer_cast<NpcInstance>(taker.handlePtr());
      vars.item = int32_t(BennetNugget);
      vars.amount = amount;
      const auto result = InventoryTransfer::run(&giver,&taker,BennetNugget,amount,vars.itemName,
                                                binding.notices,binding.ore.get());
      if(result.itemChanged) {
        auto item = std::static_pointer_cast<ItemInstance>(result.item->handlePtr());
        if(newStack)
          *ownedInstance(BennetNugget) = item;
        storyActorSlot("ITEM") = std::move(item);
        }
      if(result.itemName)
        vars.itemName = *result.itemName;
      if(result.notice) {
        vars.notice = result.notice->text;
        printScreenFor(&npc,vars.notice,-1,result.notice->y,binding.font,2);
        }
      if(bennetTransferObserver)
        bennetTransferObserver(player,npc);
      return result.value;
    },
    [&](Npc& from, Npc& to, std::string_view line) {
      from.aiPush(AiQueue::aiOutput(to,line,aiOutOrderId));
      ++aiOutOrderId;
    });
  const auto result = storygraph::NativeInterpreter(host).run(*dialog.answer);
  if(!result.ok)
    Tempest::Log::e("[story] Bennet ore answer: ",result.error);
  return result.started;
  }

bool GameScript::StockPrograms::runPrepared(uint32_t id, std::string_view name, int32_t& result, bool* failed) {
  if(name!="DIA_ADDON_LARES_PATCH_INFO" || !script.activityEvents || script.floorDefer.active() ||
     ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off)
    return false;
  const auto entry = prepared.programs.find(id);
  const auto transfer = prepared.symbols.find("B_GIVEINVITEMS");
  if(entry==prepared.programs.end() || transfer==prepared.symbols.end() || entry->second.name!=name ||
     entry->second.returnType!=0 || !entry->second.parameters.empty() || entry->second.calls.size()!=1 ||
     entry->second.calls.front()!=transfer->second)
    throw std::runtime_error("prepared ornament dialog identity differs from game symbols");

  const auto held = std::dynamic_pointer_cast<ItemInstance>(script.storyActorSlot("ITEM"));
  const auto [self,other] = script.storyActors();
  OrnamentDialogHost host(prepared,self,other,script.world(),script.inventoryTransferVariables,
                          held ? std::string_view(held->name) : std::string_view{});
  const auto run = storygraph::NativeInterpreter(host).run(entry->second.program);

  for(const auto& [name,id] : host.writes) {
    auto* npc = script.world().npcById(id);
    script.storyActorSlot(name) = npc!=nullptr ? std::static_pointer_cast<NpcInstance>(npc->handlePtr()) : nullptr;
    }
  if(host.transfer) {
    if(host.transfer->itemChanged)
      script.storeItem(host.transfer->item);
    if(host.transfer->notice)
      script.printScreenFor(self,host.transfer->notice->text,-1,host.transfer->notice->y,
                            prepared.strings.at("FONT_SCREENSMALL"),2);
    }
  if(failed!=nullptr)
    *failed = !run.ok;
  if(!run.ok)
    Tempest::Log::e("[story] prepared program ",name,": ",run.error);
  result = run.returned.type==NativeValue::Int ? run.returned.i : 0;
  return true;
  }
