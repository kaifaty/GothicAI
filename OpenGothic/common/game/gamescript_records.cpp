#include "gamescript.h"

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
    initializeInstanceNpc(owner.hnpc,instance);
  kind = npc->symbol_index();
  return npc;
  }

uint32_t GameScript::initializeInstanceNpc(const std::shared_ptr<NpcRecord>& record, size_t instance) {
  auto npc = std::static_pointer_cast<daedalus::INpc>(record);
  if(auto* sym = vm.find_symbol_by_index(uint32_t(instance)); sym && npc->user_ptr)
    static_cast<Npc*>(npc->user_ptr)->npcInstanceSymbol = sym->index();
  initializeInstanceNpc(npc,instance);
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
