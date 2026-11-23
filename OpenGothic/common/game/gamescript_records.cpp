#include "gamescript.h"

#include <algorithm>
#include <array>
#include <optional>
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

void GameScript::loadNativeTrainingHelpers() {
  if(!nativeRoutineProgram)
    return;
  const auto enabled = [this] {
    return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
    };
  vm.override_function("B_BUILDLEARNSTRING",std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [this](auto& machine) -> daedalus::DaedalusNakedCall {
      machine.find_symbol_by_name("B_BUILDLEARNSTRING.KOSTEN")->set_int(machine.pop_int());
      // The script returns its global string slot, which a later call can overwrite before it is read.
      machine.push_reference(&buildLearnString(machine.pop_string()));
      return {};
      }
    },enabled);
  vm.override_function("B_GETLEARNCOSTATTRIBUTE",std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [](auto& machine) -> daedalus::DaedalusNakedCall {
      const auto attribute = machine.pop_int();
      machine.find_symbol_by_name("B_GETLEARNCOSTATTRIBUTE.ATTRIBUT")->set_int(attribute);
      const auto learner = machine.pop_instance();
      machine.find_symbol_by_name("B_GETLEARNCOSTATTRIBUTE.OTH")->set_instance(learner);
      auto* result = machine.find_symbol_by_name("B_GETLEARNCOSTATTRIBUTE.KOSTEN");
      result->set_int(0);
      std::string_view slot;
      switch(attribute) {
        case ATR_STRENGTH:  slot = "REAL_STRENGTH";  break;
        case ATR_DEXTERITY: slot = "REAL_DEXTERITY"; break;
        case ATR_MANAMAX:   slot = "REAL_MANA_MAX";  break;
        default: machine.push_reference(result); return {};
        }
      const auto index = uint16_t(machine.find_symbol_by_name(slot)->get_int());
      auto* aivar = machine.find_symbol_by_name("C_NPC.AIVAR");
      // A supported attribute returns the shared result with the learner's context.
      struct RestoreContext {
        daedalus::DaedalusVm& vm;
        std::shared_ptr<daedalus::DaedalusInstance> caller;
        ~RestoreContext() { vm.unsafe_set_gi(std::move(caller)); }
        } restore{machine,machine.unsafe_get_gi()};
      machine.unsafe_set_gi(learner);
      int32_t cost = 5;
      for(; cost>1; --cost) {
        // The trained value excludes temporary attribute bonuses.
        if(machine.get_int(learner,aivar,index)>=30*(cost-1))
          break;
        }
      result->set_int(cost);
      machine.push_reference(result);
      return {};
      }
    },enabled);
  }

auto GameScript::buildLearnString(std::string_view title) -> daedalus::DaedalusSymbol& {
  auto& savedTitle = *vm.find_symbol_by_name("B_BUILDLEARNSTRING.TEXT");
  savedTitle.set_string(title);
  const auto cost = vm.find_symbol_by_name("B_BUILDLEARNSTRING.KOSTEN")->get_int();
  const auto text = savedTitle.get_string() + vm.find_symbol_by_name("PRINT_KOSTEN")->get_string() +
                    std::to_string(cost) + vm.find_symbol_by_name("PRINT_LP")->get_string();
  // The formatting externals reuse one temporary slot, which the legacy save also stores.
  vm.push_string(text);
  auto& result = *vm.find_symbol_by_name("B_BUILDLEARNSTRING.CONCATTEXT");
  result.set_string(vm.pop_string());
  return result;
  }

auto GameScript::readDialogInfo(daedalus::DaedalusSymbol& sym) -> std::shared_ptr<daedalus::IInfo> {
  using Op = daedalus::DaedalusOpcode;
  using Type = daedalus::DaedalusDataType;

  const auto* parent = vm.find_symbol_by_index(sym.parent());
  if(parent==nullptr || parent->type()!=Type::CLASS)
    return nullptr;

  // Read the whole constructor before binding the instance or updating helper locals.
  struct Lesson {
    int32_t talent;
    int32_t skill;
    int32_t cost;
    };
  struct Field {
    daedalus::DaedalusSymbol* member = nullptr;
    int32_t value = 0;
    const std::string* text = nullptr;
    std::optional<Lesson> lesson;
    };
  std::array<Field,8> fields;
  size_t count = 0;
  uint32_t pc = sym.address();
  const auto next = [&] {
    const auto instruction = pc<vm.size() ? vm.instruction_at(pc) : daedalus::DaedalusInstruction{};
    pc += instruction.size;
    return instruction;
    };
  const auto scalar = [&](const daedalus::DaedalusInstruction& operand) -> const daedalus::DaedalusSymbol* {
    const auto* symbol = operand.op==Op::PUSHV ? vm.find_symbol_by_index(operand.symbol) : nullptr;
    return symbol && symbol->is_const() && !symbol->is_member() && symbol->count()==1 ? symbol : nullptr;
    };
  const auto integer = [&](const daedalus::DaedalusInstruction& operand) -> std::optional<int32_t> {
    if(operand.op==Op::PUSHI)
      return operand.immediate;
    const auto* symbol = scalar(operand);
    if(symbol && symbol->type()==Type::INT)
      return symbol->get_int();
    return std::nullopt;
    };
  for(;;) {
    const auto source = next();
    if(source.op==Op::RSR)
      break;
    if(count==fields.size() || (source.op!=Op::PUSHI && source.op!=Op::PUSHV))
      return nullptr;
    auto target = next();
    auto& field = fields[count++];
    const auto* constant = scalar(source);
    if(target.op==Op::PUSHVI && target.symbol==vm.global_other()->index() &&
       constant && constant->type()==Type::STRING) {
      // Only these stock topics use fixed prices in the constructor's learn-string expression.
      struct Teacher {
        std::string_view topic;
        int32_t          cost;
        };
      static constexpr Teacher teachers[] = {
        {"DIA_BENNET_TEACHCOMMON", 2}, {"DIA_WOLF_TEACHCRAWLERPLATES", 3}, {"DIA_DOBAR_TEACH", 4},
        {"DIA_BARTOK_TEACHSNEAK", 5}, {"DIA_THORBEN_TEACH", 10},
        };
      const auto teacher = std::ranges::find(teachers,sym.name(),&Teacher::topic);
      const auto talent = integer(next());
      const auto skill = integer(next());
      const auto costCall = next();
      const auto textCall = next();
      if(teacher==std::end(teachers) || !talent || !skill ||
         costCall.op!=Op::BL || costCall.address!=vm.find_symbol_by_name("B_GETLEARNCOSTTALENT")->address() ||
         textCall.op!=Op::BL || textCall.address!=vm.find_symbol_by_name("B_BUILDLEARNSTRING")->address())
        return nullptr;
      field.lesson = Lesson{*talent,*skill,teacher->cost};
      target = next();
      }
    const auto store = next().op;
    auto* member = target.op==Op::PUSHV ? vm.find_symbol_by_index(target.symbol) : nullptr;
    if(member==nullptr || member->parent()!=parent->index() || !member->is_member() ||
       member->is_const() || member->count()!=1)
      return nullptr;
    if(source.op==Op::PUSHV && constant==nullptr)
      return nullptr;

    field.member = member;
    if(member->type()==Type::STRING && store==Op::MOVS && constant && constant->type()==Type::STRING)
      field.text = &constant->get_string();
    else if(field.lesson)
      return nullptr;
    else if(member->type()==Type::INT && store==Op::MOVI && (!constant || constant->type()==Type::INT))
      field.value = constant ? constant->get_int() : source.immediate;
    else if(member->type()==Type::FUNCTION && store==Op::MOVVF && source.op==Op::PUSHI)
      field.value = source.immediate;
    else
      return nullptr;
    }

  auto info = vm.allocate_instance<daedalus::IInfo>(&sym);
  for(const auto& field : std::span(fields).first(count)) {
    if(field.lesson) {
      const auto& lesson = *field.lesson;
      vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.SKILL")->set_int(lesson.skill);
      vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.TALENT")->set_int(lesson.talent);
      vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.OTH")->set_instance(vm.global_other()->get_instance());
      vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.KOSTEN")->set_int(lesson.cost);
      vm.find_symbol_by_name("B_BUILDLEARNSTRING.KOSTEN")->set_int(lesson.cost);
      field.member->set_string(buildLearnString(*field.text).get_string(),0,info.get());
      }
    else if(field.text!=nullptr)
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
