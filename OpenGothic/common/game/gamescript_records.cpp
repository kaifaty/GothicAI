#include "gamescript.h"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>

#include <Tempest/Log>

#include "game/serialize.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"

namespace {

struct RestoreContext {
  daedalus::DaedalusVm& vm;
  std::shared_ptr<daedalus::DaedalusInstance> caller;
  ~RestoreContext() { vm.unsafe_set_gi(std::move(caller)); }
  };

void pushTieredLearnCost(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol& result,
                        const std::shared_ptr<daedalus::DaedalusInstance>& learner, std::string_view slot,
                        std::optional<int32_t> points = std::nullopt) {
  const auto index = uint16_t(vm.find_symbol_by_name(slot)->get_int());
  auto* aivar = vm.find_symbol_by_name("C_NPC.AIVAR");
  // The result reference holds the learner's context before BL restores the caller's.
  RestoreContext restore{vm,vm.unsafe_get_gi()};
  vm.unsafe_set_gi(learner);
  int32_t cost = 5;
  for(; cost>1; --cost) {
    // The trained value excludes equipment and temporary bonuses.
    if(vm.get_int(learner,aivar,index)>=30*(cost-1))
      break;
    }
  result.set_int(cost);
  if(points)
    result.set_int(std::bit_cast<int32_t>(uint32_t(cost)*uint32_t(*points)));
  vm.push_reference(&result);
  }

int32_t fixedTalentLearnCost(int32_t talent, int32_t skill) {
  // Tables indexed by the verified NotR skill IDs.
  static constexpr uint8_t smith[] = {2,4,4,6,6,8,8,10,10,4,6,8,10};
  static constexpr uint8_t alchemy[] = {1,3,5,1,3,5,5,20,20,10,10,20,0,8,8};
  static constexpr uint8_t trophies[] = {3,3,5,3,3,1,1,1,1,3,1,3,3,3};
  static constexpr uint8_t languages[] = {5,10,15};
  static constexpr uint8_t wisp[] = {0,1,2,3,4,5,5};
  static constexpr uint8_t runes[] = {
     1, 3, 3, 5, 5,10,10, 5, 5, 5, //  0–9
     5, 5, 5, 5, 5, 5, 5, 5, 1, 5, // 10–19
     5, 3, 3,10, 3, 5, 5, 5, 5, 5, // 20–29
    10,10, 5,10,10,15,10,10,15,20, // 30–39
    20,10,20,20,20,20,20, 5, 5, 5, // 40–49
     5, 5, 5, 5, 5, 5, 5, 5, 5, 5, // 50–59
    15, 5, 5, 5, 5, 5, 5, 5, 5, 5, // 60–69
     5, 5,10, 5, 5,10              // 70–75
    };
  const auto price = [index=size_t(skill)](std::span<const uint8_t> table, int32_t fallback = 0) {
    return index<table.size() ? table[index] : fallback;
    };
  switch(talent) {
    case TALENT_MAGE:             return 1<=skill && skill<=6 ? 5 : 0;
    case TALENT_ACROBAT:
    case TALENT_SNEAK:            return 5;
    case TALENT_PICKPOCKET:
    case TALENT_PICKLOCK:         return 10;
    case TALENT_SMITH:            return price(smith);
    case TALENT_ALCHEMY:          return price(alchemy);
    case TALENT_TAKEANIMALTROPHY: return price(trophies);
    case TALENT_FOREIGNLANGUAGE:  return price(languages);
    case TALENT_WISPDETECTOR:     return price(wisp);
    case TALENT_RUNES:            return price(runes,5);
    default:                      return 0;
    }
  }

bool hasStockAttributeRaiseArguments(daedalus::DaedalusVm& vm) {
  using Type = daedalus::DaedalusDataType;
  try {
    // The stock BL sites pass global integer references or immediate integer values.
    for(uint16_t depth : {uint16_t(0),uint16_t(1)}) {
      const auto& frame = vm.stack_frame(depth);
      if(frame.reference) {
        const auto* symbol = std::get<daedalus::DaedalusSymbol*>(frame.value);
        if(symbol->type()!=Type::INT || symbol->is_member() || frame.index>=symbol->count())
          return false;
        } else if(!std::holds_alternative<int32_t>(frame.value)) {
        return false;
        }
      }
    const auto& frame = vm.stack_frame(2);
    if(frame.reference) {
      auto* symbol = std::get<daedalus::DaedalusSymbol*>(frame.value);
      return symbol->type()==Type::INSTANCE &&
             (symbol->get_instance()==nullptr || symbol->is_instance_of<daedalus::INpc>());
      }
    const auto* instance = std::get_if<std::shared_ptr<daedalus::DaedalusInstance>>(&frame.value);
    return instance && (!*instance || (*instance)->instance_type()==&typeid(daedalus::INpc));
    } catch (const daedalus::DaedalusVmException&) {
    // Keep the original frames for the VM's opcode-level error recovery.
    return false;
    }
  }

static constexpr struct AttributeRaise {
  int32_t attribute;
  const char* caption;
  std::optional<uint8_t> trainingSlot;
  } attributeRaises[] = {
  {ATR_STRENGTH,     "PRINT_LEARNSTR",           81},
  {ATR_DEXTERITY,    "PRINT_LEARNDEX",           82},
  {ATR_MANAMAX,      "PRINT_LEARNMANA_MAX",      83},
  {ATR_HITPOINTSMAX, "PRINT_LEARNHITPOINTS_MAX", {}},
  };

std::span<daedalus::DaedalusSymbol> bindAttributeRaise(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper) {
  auto args = vm.find_parameters_for_function(&helper);
  const auto caller = vm.unsafe_get_gi();
  vm.set_int(caller,&args[2],0,vm.pop_int());
  vm.set_int(caller,&args[1],0,vm.pop_int());
  args[0].set_instance(vm.pop_instance());
  return args;
  }

void increaseAttribute(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol& learner,
                       daedalus::DaedalusSymbol& points, const char* field, uint16_t index) {
  const auto npc = learner.get_instance();
  vm.unsafe_set_gi(npc);
  auto* member = vm.find_symbol_by_name(field);
  const auto value = vm.get_int(npc,member,index);
  const auto delta = vm.get_int(npc,&points,0);
  vm.set_int(npc,member,index,std::bit_cast<int32_t>(uint32_t(value)+uint32_t(delta)));
  }

void raiseRealAttributeLearnCounter(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper) {
  const auto args = bindAttributeRaise(vm,helper);
  const auto row = std::ranges::find(attributeRaises,args[1].get_int(),&AttributeRaise::attribute);
  if(row!=std::end(attributeRaises) && row->trainingSlot) {
    RestoreContext restore{vm,vm.unsafe_get_gi()};
    increaseAttribute(vm,args[0],args[2],"C_NPC.AIVAR",*row->trainingSlot);
    }
  }

void raiseAttribute(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper,
                    const daedalus::DaedalusSymbol& counter) {
  const auto args = bindAttributeRaise(vm,helper);
  const auto id = args[1].get_int();
  const auto row = std::ranges::find(attributeRaises,id,&AttributeRaise::attribute);
  RestoreContext restore{vm,vm.unsafe_get_gi()};
  int32_t printResults = 0;
  if(row!=std::end(attributeRaises)) {
    const auto symbol = [&](std::string_view name) { return vm.find_symbol_by_name(name); };
    auto& learner = args[0];
    increaseAttribute(vm,learner,args[2],"C_NPC.ATTRIBUTE",uint16_t(id));
    const auto print = [&](daedalus::DaedalusSymbol* text,int32_t y) {
      vm.push_reference(text);
      vm.push_int(-1);
      vm.push_int(y);
      vm.push_reference(symbol("FONT_SCREEN"));
      vm.push_int(2);
      printResults += vm.unsafe_call_external(symbol("PRINTSCREEN"));
      };
    if(id==ATR_DEXTERITY) {
      auto* acrobat = symbol("NPC_TALENT_ACROBAT");
      vm.push_reference(&learner);
      vm.push_reference(acrobat);
      vm.unsafe_call_external(symbol("NPC_GETTALENTSKILL"));
      const auto skill = vm.pop_int();
      const auto npc = learner.get_instance();
      vm.unsafe_set_gi(npc);
      const auto dexterity = vm.get_int(npc,symbol("C_NPC.ATTRIBUTE"),uint16_t(id));
      if(skill==0 && dexterity>=90) {
        vm.push_reference(&learner);
        vm.push_reference(acrobat);
        vm.push_int(1);
        vm.unsafe_call_external(symbol("NPC_SETTALENTSKILL"));
        print(symbol("PRINT_ADDON_ACROBATBONUS"),55);
        }
      }

    // Formatting updates the same temporary strings even if the message is hidden.
    vm.push_reference(symbol(row->caption));
    vm.push_reference(&args[2]);
    vm.unsafe_call_external(symbol("INTTOSTRING"));
    vm.unsafe_call_external(symbol("CONCATSTRINGS"));
    auto* text = symbol("B_RAISEATTRIBUTE.CONCATTEXT");
    vm.set_string(vm.unsafe_get_gi(),text,0,vm.pop_string());
    print(text,-1);
    }

  // The original calls the counter for every id and leaves its three arguments in saved slots.
  for(auto& arg:args)
    vm.push_reference(&arg);
  vm.unsafe_call_bl(&counter);
  // The stock void frame discards the screen calls' integer results on return.
  for(int32_t i=0; i<printResults; ++i)
    (void)vm.pop_int();
  }

void teachAttributePoints(daedalus::DaedalusVm& vm) {
  const auto symbol = [&](std::string_view name) { return vm.find_symbol_by_name(name); };
  const auto caller = vm.unsafe_get_gi();
  const auto popParameter = [&](std::string_view name) {
    auto* saved = symbol(name);
    vm.set_int(caller,saved,0,vm.pop_int());
    return saved;
    };
  auto* maximum = popParameter("B_TEACHATTRIBUTEPOINTS.TEACHERMAX");
  auto* points = popParameter("B_TEACHATTRIBUTEPOINTS.POINTS");
  auto* attribute = popParameter("B_TEACHATTRIBUTEPOINTS.ATTRIB");
  auto* learner = symbol("B_TEACHATTRIBUTEPOINTS.OTH");
  learner->set_instance(vm.pop_instance());
  auto* teacher = symbol("B_TEACHATTRIBUTEPOINTS.SLF");
  teacher->set_instance(vm.pop_instance());

  vm.push_reference(points);
  vm.push_reference(learner);
  vm.push_reference(attribute);
  vm.unsafe_call_bl(symbol("B_GETLEARNCOSTATTRIBUTE"));
  const auto unitPrice = vm.pop_int();
  const auto cost = std::bit_cast<int32_t>(uint32_t(unitPrice)*uint32_t(vm.pop_int()));
  vm.set_int(caller,symbol("B_TEACHATTRIBUTEPOINTS.KOSTEN"),0,cost);
  auto* failure = symbol("FALSE");
  const auto id = attribute->get_int();
  if(id!=ATR_STRENGTH && id!=ATR_DEXTERITY && id!=ATR_MANAMAX) {
    vm.push_reference(symbol("$27343")); // Stock wrong-parameter message.
    vm.unsafe_call_external(symbol("PRINT"));
    vm.push_reference(failure);
    return;
    }

  RestoreContext restore{vm,caller};
  const auto student = learner->get_instance();
  vm.unsafe_set_gi(student);
  // The price uses the trained value, while the teacher compares the current attribute.
  const auto value = vm.get_int(student,symbol("C_NPC.ATTRIBUTE"),uint16_t(id));
  vm.set_int(student,symbol("B_TEACHATTRIBUTEPOINTS.REALATTRIBUTE"),0,value);
  const auto refuse = [&](daedalus::DaedalusSymbol* notice, std::string_view speech) {
    vm.push_reference(notice);
    vm.push_int(-1);
    vm.push_int(-1);
    vm.push_reference(symbol("FONT_SCREEN"));
    vm.push_int(2);
    vm.unsafe_call_external(symbol("PRINTSCREEN"));
    vm.push_reference(teacher);
    vm.push_reference(learner);
    vm.push_reference(symbol(speech));
    vm.unsafe_call_bl(symbol("B_SAY"));
    (void)vm.pop_int(); // The script discards PRINTSCREEN's result on return.
    vm.push_reference(failure);
    };
  const auto limit = maximum->get_int();
  const bool alreadyAtLimit = value>=limit;
  if(alreadyAtLimit || std::bit_cast<int32_t>(uint32_t(value)+uint32_t(points->get_int()))>limit) {
    vm.push_reference(symbol("PRINT_NOLEARNOVERPERSONALMAX"));
    vm.push_reference(maximum);
    vm.unsafe_call_external(symbol("INTTOSTRING"));
    vm.unsafe_call_external(symbol("CONCATSTRINGS"));
    auto* notice = symbol("B_TEACHATTRIBUTEPOINTS.CONCATTEXT");
    vm.set_string(student,notice,0,vm.pop_string());
    // Stock speech IDs for being at the limit and crossing it in this request.
    refuse(notice,alreadyAtLimit ? "$27344" : "$27345");
    return;
    }
  auto* lp = symbol("C_NPC.LP");
  if(vm.get_int(student,lp,0)<cost) {
    refuse(symbol("PRINT_NOTENOUGHLP"),"$27346");
    return;
    }
  const auto balance = std::bit_cast<int32_t>(uint32_t(vm.get_int(student,lp,0))-uint32_t(cost));
  vm.set_int(student,lp,0,balance);
  vm.push_reference(learner);
  vm.push_reference(attribute);
  vm.push_reference(points);
  vm.unsafe_call_bl(symbol("B_RAISEATTRIBUTE"));
  vm.push_reference(symbol("TRUE"));
  }

} // namespace

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

void GameScript::loadNativeAmbientInfos() {
  if(!nativeRoutineProgram)
    return;

  using Op = daedalus::DaedalusOpcode;
  using Type = daedalus::DaedalusDataType;
  using Symbol = daedalus::DaedalusSymbol;
  using Actor = std::shared_ptr<daedalus::INpc>;
  constexpr uint32_t ambient = 1u<<uint32_t(NpcType::G2_AMBIENT);
  constexpr uint32_t oldCamp = 1u<<uint32_t(NpcType::G2_OC_AMBIENT);
  constexpr uint32_t banditCamp = 1u<<uint32_t(NpcType::G2_BL_AMBIENT);
  constexpr uint32_t valley = 1u<<uint32_t(NpcType::G2_TAL_AMBIENT);
  constexpr int32_t anyVoice = -1;
  static constexpr struct Selector {
    int32_t guild;
    uint32_t types;
    int32_t voice;
    std::string_view suffix;
    } selectors[] = {
    {GIL_VLK, ambient, 1,  "VLK_1"},
    {GIL_VLK, ambient, 6,  "VLK_6"},
    {GIL_VLK, ambient, 8,  "VLK_8"},
    {GIL_VLK, ambient, 16, "VLK_16"},
    {GIL_VLK, ambient, 17, "VLK_17"},
    {GIL_MIL, ambient, 6, "MIL_6"},
    {GIL_MIL, ambient, 7, "MIL_7"},
    {GIL_MIL, oldCamp, 1, "OCVLK_1"},
    {GIL_MIL, oldCamp, 6, "OCVLK_6"},
    {GIL_PAL, ambient, 4,  "PAL_4"},
    {GIL_PAL, ambient, 9,  "PAL_9"},
    {GIL_PAL, ambient, 12, "PAL_12"},
    {GIL_PAL, oldCamp, 4, "OCPAL_4"},
    {GIL_PAL, oldCamp, 9, "OCPAL_9"},
    {GIL_BAU, ambient, 1,  "BAU_1"},
    {GIL_BAU, ambient, 7,  "BAU_7"},
    {GIL_BAU, ambient, 13, "BAU_13"},
    {GIL_BAU, ambient, 16, "BAU_16"},
    {GIL_SLD, ambient, 6, "SLD_6"},
    {GIL_SLD, ambient, 7, "SLD_7"},
    {GIL_NOV, ambient, 3, "NOV_3"},
    {GIL_NOV, ambient, 8, "NOV_8"},
    {GIL_PIR, ambient, 6, "ADDON_PIR_6"},
    {GIL_PIR, ambient, 7, "ADDON_PIR_7"},
    {GIL_OUT, ambient, 1,  "OUT_1"},
    {GIL_OUT, ambient, 7,  "OUT_7"},
    {GIL_OUT, ambient, 13, "OUT_13"},
    {GIL_OUT, oldCamp, 4, "OWPAL_4"},
    {GIL_STRF, ambient|oldCamp, 1,  "STRF_1"},
    {GIL_STRF, ambient|oldCamp, 13, "STRF_13"},
    {GIL_STRF, banditCamp, anyVoice,"ADDON_SKLAVEN_3"},
    {GIL_BDT, ambient|oldCamp, 1,  "BDT_1"},
    {GIL_BDT, ambient|oldCamp, 13, "BDT_13"},
    {GIL_BDT, banditCamp, 1,  "ADDON_BL_BDT_1"},
    {GIL_BDT, banditCamp, 13, "ADDON_BL_BDT_13"},
    {GIL_BDT, valley, 1,  "ADDON_TAL_BDT_1"},
    {GIL_BDT, valley, 13, "ADDON_TAL_BDT_13"},
    {GIL_DMT, ambient|oldCamp, 19, {}},
    };
  struct Binding {
    Symbol* savedActor = nullptr;
    std::vector<Symbol*> topics;
    };
  std::array<Binding,std::size(selectors)> bindings;
  auto* infoClass = findSymbol("C_INFO");
  auto* npcMember = findSymbol("C_INFO.NPC");
  auto* instanceId = findSymbol("HLP_GETINSTANCEID");
  for(size_t i=0; i<std::size(selectors); ++i) {
    if(selectors[i].suffix.empty())
      continue;
    const auto name = std::string("B_ASSIGNAMBIENTINFOS_").append(selectors[i].suffix);
    auto* helper = findSymbol(name);
    auto& binding = bindings[i];
    binding.savedActor = findSymbol(name+".SLF");
    if(helper==nullptr || binding.savedActor==nullptr)
      return;

    // Accept only the saved parameter followed by ordered C_INFO.NPC assignments.
    uint32_t pc = helper->address();
    const auto take = [&](Op expected) -> std::optional<daedalus::DaedalusInstruction> {
      if(pc>=vm.size())
        return {};
      const auto instr = vm.instruction_at(pc);
      if(instr.op!=expected)
        return {};
      pc += instr.size;
      return instr;
      };
    const auto symbol = [&](Op op,const Symbol* expected) {
      const auto instr = take(op);
      return instr && instr->symbol==expected->index();
      };
    if(!symbol(Op::PUSHVI,binding.savedActor) || !take(Op::MOVVI))
      return;
    while(!take(Op::RSR)) {
      if(!symbol(Op::PUSHVI,binding.savedActor) || !symbol(Op::BE,instanceId))
        return;
      const auto target = take(Op::GMOVI);
      auto* info = target ? vm.find_symbol_by_index(target->symbol) : nullptr;
      if(info==nullptr || info->type()!=Type::INSTANCE || info->parent()!=infoClass->index() ||
         !symbol(Op::PUSHV,npcMember) || !take(Op::MOVI))
        return;
      binding.topics.push_back(info);
      }
    }

  const auto enabled = [this] {
    if(!activityEvents || floorDefer.active() || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off)
      return false;
    try {
      const auto& argument = vm.stack_frame(0);
      if(argument.reference)
        return std::get<Symbol*>(argument.value)->is_instance_of<daedalus::INpc>();
      const auto* actor = std::get_if<std::shared_ptr<daedalus::DaedalusInstance>>(&argument.value);
      return actor && *actor && (*actor)->instance_type()==&typeid(daedalus::INpc);
      } catch(const daedalus::DaedalusVmException&) {
      return false;
      }
    };
  auto* savedActor = findSymbol("B_ASSIGNAMBIENTINFOS.SLF");
  auto* dementor = findSymbol("B_ASSIGNDEMENTORTALK");
  vm.override_function("B_ASSIGNAMBIENTINFOS",std::function<void(Actor)>{
    [this,savedActor,dementor,npcMember,instanceId,bindings=std::move(bindings)](const Actor& actor) {
      savedActor->set_instance(actor);
      const auto type = uint32_t(actor->type);
      if(type>=32u)
        return;
      const auto typeBit = 1u<<type;
      for(size_t i=0; i<std::size(selectors); ++i) {
        const auto& selector = selectors[i];
        if(actor->guild!=selector.guild || (selector.types&typeBit)==0 ||
           (selector.voice!=anyVoice && actor->voice!=selector.voice))
          continue;

        vm.unsafe_set_gi(actor);
        const auto& binding = bindings[i];
        if(binding.savedActor==nullptr) {
          vm.push_reference(savedActor);
          vm.unsafe_call_bl(dementor);
          } else {
          binding.savedActor->set_instance(actor);
          for(auto* topic : binding.topics) {
            vm.push_reference(binding.savedActor);
            vm.unsafe_call_external(instanceId);
            const auto owner = vm.pop_int();
            const auto context = topic->get_instance();
            vm.unsafe_set_gi(context);
            vm.set_int(context,npcMember,0,owner);
            }
          }
        }
      }},enabled);
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
      pushTieredLearnCost(machine,*result,learner,slot);
      return {};
      }
    },enabled);
  vm.override_function("B_GETLEARNCOSTTALENT",std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [](auto& machine) -> daedalus::DaedalusNakedCall {
      const auto skill = machine.pop_int();
      machine.find_symbol_by_name("B_GETLEARNCOSTTALENT.SKILL")->set_int(skill);
      const auto talent = machine.pop_int();
      machine.find_symbol_by_name("B_GETLEARNCOSTTALENT.TALENT")->set_int(talent);
      const auto learner = machine.pop_instance();
      machine.find_symbol_by_name("B_GETLEARNCOSTTALENT.OTH")->set_instance(learner);
      auto* result = machine.find_symbol_by_name("B_GETLEARNCOSTTALENT.KOSTEN");
      result->set_int(0);
      if(TALENT_1H<=talent && talent<=TALENT_CROSSBOW) {
        static constexpr std::string_view slots[] = {
          "REAL_TALENT_1H","REAL_TALENT_2H","REAL_TALENT_BOW","REAL_TALENT_CROSSBOW"
          };
        pushTieredLearnCost(machine,*result,learner,slots[size_t(talent-TALENT_1H)],skill);
        } else {
        result->set_int(fixedTalentLearnCost(talent,skill));
        machine.push_reference(result);
        }
      return {};
      }
    },enabled);
  auto* raise = findSymbol("B_RAISEATTRIBUTE");
  auto* counter = findSymbol("B_RAISEREALATTRIBUTELEARNCOUNTER");
  const auto canRaise = [this,enabled] { return enabled() && hasStockAttributeRaiseArguments(vm); };
  vm.override_function(raise->name(),std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [raise,counter](auto& machine) -> daedalus::DaedalusNakedCall {
      raiseAttribute(machine,*raise,*counter);
      return {};
      }
    },canRaise);
  vm.override_function(counter->name(),std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [counter](auto& machine) -> daedalus::DaedalusNakedCall {
      raiseRealAttributeLearnCounter(machine,*counter);
      return {};
      }
    },canRaise);
  vm.override_function("B_TEACHATTRIBUTEPOINTS",std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [](auto& machine) -> daedalus::DaedalusNakedCall {
      teachAttributePoints(machine);
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
      static constexpr std::string_view teachers[] = {
        "DIA_BENNET_TEACHCOMMON", "DIA_WOLF_TEACHCRAWLERPLATES", "DIA_DOBAR_TEACH",
        "DIA_BARTOK_TEACHSNEAK", "DIA_THORBEN_TEACH",
        };
      const auto teacher = std::ranges::find(teachers,sym.name());
      const auto talent = integer(next());
      const auto skill = integer(next());
      const auto costCall = next();
      const auto textCall = next();
      if(teacher==std::end(teachers) || !talent || !skill ||
         costCall.op!=Op::BL || costCall.address!=vm.find_symbol_by_name("B_GETLEARNCOSTTALENT")->address() ||
         textCall.op!=Op::BL || textCall.address!=vm.find_symbol_by_name("B_BUILDLEARNSTRING")->address())
        return nullptr;
      field.lesson = Lesson{*talent,*skill};
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
      const auto cost = fixedTalentLearnCost(lesson.talent,lesson.skill);
      vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.KOSTEN")->set_int(cost);
      vm.find_symbol_by_name("B_BUILDLEARNSTRING.KOSTEN")->set_int(cost);
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
