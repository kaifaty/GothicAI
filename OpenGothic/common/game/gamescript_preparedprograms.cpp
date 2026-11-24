#include "gamescript_programs.h"

#include <Tempest/Log>
#include <array>
#include <stdexcept>

#include "gamescript_inventorytransfer.h"
#include "game/definitions/scriptvars.h"
#include "storygraph/nativeexec.h"
#include "world/objects/npc.h"
#include "world/world.h"

namespace {

using storygraph::NativeFunction;
using storygraph::NativeInstance;
using storygraph::NativeSlot;
using storygraph::NativeValue;

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
      const auto npcRef = [&](Npc* npc) {
        return ScriptVars::Reference{ScriptVars::ReferenceKind::Npc,world.npcId(npc)};
        };
      variables.amount = amount;
      variables.item = item;
      writes.push_back({"B_GIVEINVITEMS.TAKER",npcRef(taker)});
      writes.push_back({"B_GIVEINVITEMS.GIVER",npcRef(giver)});

      const InventoryTransfer::Text text {
        catalog.symbols.at("ITMI_GOLD"),
        catalog.strings.at("PRINT_GOLDGEGEBEN"),catalog.strings.at("PRINT_GOLDERHALTEN"),
        catalog.strings.at("PRINT_ADDON_GEGEBEN"),catalog.strings.at("PRINT_ADDON_ERHALTEN"),
        catalog.strings.at("PRINT_ITEMSGEGEBEN"),catalog.strings.at("PRINT_ITEMSERHALTEN"),
        catalog.integers.at("YPOS_GOLDGIVEN"),catalog.integers.at("YPOS_GOLDTAKEN"),
        catalog.integers.at("YPOS_ITEMGIVEN"),catalog.integers.at("YPOS_ITEMTAKEN")
        };
      transfer = InventoryTransfer::run(giver,taker,uint32_t(item),amount,previousItemName,text);
      if(transfer->itemName)
        variables.itemName = *transfer->itemName;
      if(transfer->notice)
        variables.notice = transfer->notice->text;
      return NativeValue::integer(transfer->value);
      }

    std::vector<ScriptVars::Row> writes;
    std::optional<InventoryTransfer::Result> transfer;

  private:
    const StockStoryPrograms::Catalog& catalog;
    const std::array<Npc*,3> actors;
    const World& world;
    InventoryTransfer::Variables& variables;
    std::string_view previousItemName;
  };

}

bool GameScript::StockPrograms::runPrepared(const daedalus::DaedalusSymbol& fn, int32_t& result, bool* failed) {
  if(fn.name()!="DIA_ADDON_LARES_PATCH_INFO" || !script.activityEvents || script.floorDefer.active() ||
     ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off)
    return false;
  const auto* catalog = preparedCatalog();
  if(catalog==nullptr)
    return false;
  const auto entry = catalog->programs.find(fn.index());
  const auto transfer = catalog->symbols.find("B_GIVEINVITEMS");
  if(entry==catalog->programs.end() || transfer==catalog->symbols.end() || entry->second.name!=fn.name() ||
     entry->second.returnType!=0 || !entry->second.parameters.empty() || entry->second.calls.size()!=1 ||
     entry->second.calls.front()!=transfer->second)
    throw std::runtime_error("prepared ornament dialog identity differs from game symbols");

  // Bridge the current session context. The prepared executor and transfer rule do not read VM state.
  const auto held = std::dynamic_pointer_cast<daedalus::IItem>(vm.global_item()->get_instance());
  auto* self = script.findNpc(vm.global_self());
  OrnamentDialogHost host(*catalog,self,script.findNpc(vm.global_other()),script.world(),
                          script.inventoryTransferVariables,held ? std::string_view(held->name) : std::string_view{});
  const auto run = storygraph::NativeInterpreter(host).run(entry->second.program);

  // The compatibility session still supplies typed instance aliases to legacy callers.
  for(const auto& [name,value] : host.writes) {
    auto* symbol = vm.find_symbol_by_name(name);
    const auto& ref = std::get<ScriptVars::Reference>(value);
    auto* npc = script.world().npcById(ref.object);
    symbol->set_instance(npc!=nullptr ? std::static_pointer_cast<daedalus::INpc>(npc->handlePtr()) : nullptr);
    }
  if(host.transfer) {
    if(host.transfer->itemChanged)
      script.storeItem(host.transfer->item);
    if(host.transfer->notice)
      script.printScreenFor(self,host.transfer->notice->text,-1,host.transfer->notice->y,
                            catalog->strings.at("FONT_SCREENSMALL"),2);
    }
  if(failed!=nullptr)
    *failed = !run.ok;
  if(!run.ok)
    Tempest::Log::e("[story] prepared program ",fn.name(),": ",run.error);
  result = run.returned.type==NativeValue::Int ? run.returned.i : 0;
  return true;
  }
