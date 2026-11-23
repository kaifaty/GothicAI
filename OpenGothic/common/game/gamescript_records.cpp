#include "gamescript.h"

#include <algorithm>
#include <array>
#include <span>

#include <Tempest/Log>

#include "game/serialize.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"

auto GameScript::createNpc(Npc& owner, size_t instance, std::string_view waypoint, uint32_t& kind) -> std::shared_ptr<NpcRecord> {
  auto npc = std::make_shared<daedalus::INpc>();
  owner.hnpc = npc;
  npc->user_ptr = &owner;
  npc->id = int32_t(instance & 0x7FFFFFFF);
  npc->wp = std::string(waypoint);
  kind = npc->symbol_index();
  if(instance!=size_t(-1))
    initializeInstanceNpc(npc,instance);
  kind = npc->symbol_index();
  return npc;
  }

uint32_t GameScript::initializeInstanceNpc(const std::shared_ptr<NpcRecord>& record, size_t instance) {
  auto npc = std::static_pointer_cast<daedalus::INpc>(record);
  // These stock spell forms and the necromancy skeleton can overlay a live
  // record. Their constructors and helpers do not read the existing fields.
  static constexpr std::array<std::string_view,13> forms={
    "SHEEP", "SCAVENGER", "GIANT_RAT", "GIANT_BUG", "WOLF", "WARAN", "SNAPPER",
    "WARG", "FIREWARAN", "LURKER", "SHADOWBEAST", "DRAGONSNAPPER", "SKELETON"
    };
  bool useRecords=false;
  if(nativeRoutineProgram && NpcDefinitionRuntime::enabled())
    if(const auto* symbol=vm.find_symbol_by_index(uint32_t(instance)))
      useRecords=std::ranges::find(forms,symbol->name())!=forms.end();
  initializeInstanceNpc(npc,instance,useRecords);
  return npc->symbol_index();
  }

auto GameScript::loadNpc(Serialize& fin, Npc* owner, uint32_t& kind) -> std::shared_ptr<NpcRecord> {
  auto npc = std::make_shared<daedalus::INpc>();
  if(owner!=nullptr)
    owner->hnpc = npc;
  npc->user_ptr = owner;
  uint32_t symbol = 0;
  fin.read(symbol);
  if(auto* sym = vm.find_symbol_by_index(symbol)) {
    vm.allocate_instance(npc,sym);
    } else {
    Tempest::Log::e("Cannot load serialized NPC ",symbol,": Symbol not found.");
    }
  kind = npc->symbol_index();
  fin.readNpc(*npc);
  return npc;
  }

void GameScript::bindNpc(const std::shared_ptr<NpcRecord>& record) {
  auto npc = std::static_pointer_cast<daedalus::INpc>(record);
  if(auto* sym = vm.find_symbol_by_index(npc->symbol_index()))
    sym->set_instance(npc);
  }

auto GameScript::copyNpc(const std::shared_ptr<NpcRecord>& record) -> std::shared_ptr<NpcRecord> {
  return std::make_shared<daedalus::INpc>(*std::static_pointer_cast<daedalus::INpc>(record));
  }

void GameScript::restoreNpc(const std::shared_ptr<NpcRecord>& dst, const std::shared_ptr<NpcRecord>& src, Npc& owner) {
  auto npc = std::static_pointer_cast<daedalus::INpc>(dst);
  *npc = *std::static_pointer_cast<daedalus::INpc>(src);
  npc->user_ptr = &owner;
  }

void GameScript::detachNpc(const std::shared_ptr<NpcRecord>& record, Npc& owner) {
  auto npc = std::static_pointer_cast<daedalus::INpc>(record);
  if(npc && npc->user_ptr==&owner)
    npc->user_ptr = nullptr;
  }

Npc* GameScript::findNpc(NpcRecord* record) {
  return record==nullptr ? nullptr : static_cast<Npc*>(static_cast<daedalus::INpc*>(record)->user_ptr);
  }

auto GameScript::npcRecord(daedalus::DaedalusSymbol* symbol) -> std::shared_ptr<NpcRecord> {
  if(symbol->is_instance_of<daedalus::INpc>())
    return std::static_pointer_cast<daedalus::INpc>(symbol->get_instance());
  return nullptr;
  }

auto GameScript::readDialogInfo(daedalus::DaedalusSymbol& sym) -> std::shared_ptr<daedalus::IInfo> {
  using Op = daedalus::DaedalusOpcode;
  using Type = daedalus::DaedalusDataType;

  const auto* parent = vm.find_symbol_by_index(sym.parent());
  if(parent==nullptr || parent->type()!=Type::CLASS)
    return nullptr;

  // A plain C_INFO constructor only assigns its scalar members from literals or constants.
  // Read the whole constructor before binding the instance so computed descriptions can use the VM.
  struct Field {
    daedalus::DaedalusSymbol* member = nullptr;
    int32_t value = 0;
    const std::string* text = nullptr;
    };
  std::array<Field,8> fields;
  size_t count = 0;
  uint32_t pc = sym.address();
  const auto next = [&] {
    const auto instruction = pc<vm.size() ? vm.instruction_at(pc) : daedalus::DaedalusInstruction{};
    pc += instruction.size;
    return instruction;
    };
  for(;;) {
    const auto source = next();
    if(source.op==Op::RSR)
      break;
    if(count==fields.size() || (source.op!=Op::PUSHI && source.op!=Op::PUSHV))
      return nullptr;
    const auto target = next();
    const auto store = next().op;
    auto* member = target.op==Op::PUSHV ? vm.find_symbol_by_index(target.symbol) : nullptr;
    if(member==nullptr || member->parent()!=parent->index() || !member->is_member() ||
       member->is_const() || member->count()!=1)
      return nullptr;
    const auto* constant = source.op==Op::PUSHV ? vm.find_symbol_by_index(source.symbol) : nullptr;
    if(source.op==Op::PUSHV && (constant==nullptr || !constant->is_const() ||
                              constant->is_member() || constant->count()!=1))
      return nullptr;

    auto& field = fields[count++];
    field.member = member;
    if(member->type()==Type::STRING && store==Op::MOVS && constant && constant->type()==Type::STRING)
      field.text = &constant->get_string();
    else if(member->type()==Type::INT && store==Op::MOVI && (!constant || constant->type()==Type::INT))
      field.value = constant ? constant->get_int() : source.immediate;
    else if(member->type()==Type::FUNCTION && store==Op::MOVVF && source.op==Op::PUSHI)
      field.value = source.immediate;
    else
      return nullptr;
    }

  auto info = vm.allocate_instance<daedalus::IInfo>(&sym);
  for(const auto& field : std::span(fields).first(count)) {
    if(field.text!=nullptr)
      field.member->set_string(*field.text,0,info.get());
    else
      field.member->set_int(field.value,0,info.get());
    }
  return info;
  }

void GameScript::setNpcInfoKnown(const NpcRecord& npc, const daedalus::IInfo& info) {
  auto id = std::make_pair(vm.find_symbol_by_instance(static_cast<const daedalus::INpc&>(npc))->index(),vm.find_symbol_by_instance(info)->index());
  dlgKnownInfos.insert(id);
  }

bool GameScript::doesNpcKnowInfo(const NpcRecord& npc, size_t infoInstance) const {
  auto id = std::make_pair(vm.find_symbol_by_instance(static_cast<const daedalus::INpc&>(npc))->index(),infoInstance);
  return dlgKnownInfos.find(id)!=dlgKnownInfos.end();
  }

auto GameScript::createItem(Item& owner, size_t instance, uint32_t& kind) -> std::shared_ptr<ItemRecord> {
  auto item = std::make_shared<daedalus::IItem>();
  owner.hitem = item;
  item->user_ptr = &owner;
  kind = item->symbol_index();
  if(auto* sym = vm.find_symbol_by_index(uint32_t(instance)))
    kind = sym->index();
  initializeInstanceItem(item,instance);
  kind = item->symbol_index();
  return item;
  }

auto GameScript::createItem(size_t instance) -> std::shared_ptr<ItemRecord> {
  auto item = std::make_shared<daedalus::IItem>();
  initializeInstanceItem(item,instance);
  return item;
  }

auto GameScript::loadItem(Serialize& fin, Item& owner, uint32_t& kind) -> std::shared_ptr<ItemRecord> {
  auto item = std::make_shared<daedalus::IItem>();
  owner.hitem = item;
  item->user_ptr = &owner;
  uint32_t symbol = 0;
  fin.read(symbol);
  if(auto* sym = vm.find_symbol_by_index(symbol)) {
    vm.allocate_instance(item,sym);
    } else {
    Tempest::Log::e("Loading unknown item from save: " , symbol);
    }
  kind = item->symbol_index();
  fin.readItem(*item);
  return item;
  }

void GameScript::attachItem(const std::shared_ptr<ItemRecord>& record, Item& owner) {
  std::static_pointer_cast<daedalus::IItem>(record)->user_ptr = &owner;
  }
