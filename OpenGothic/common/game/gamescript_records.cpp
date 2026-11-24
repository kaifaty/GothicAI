#include "gamescript.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <optional>
#include <span>

#include <Tempest/Log>

#include "game/definitions/basescripts.h"
#include "basestore.h"
#include "game/definitions/dialoginfodata.h"
#include "game/serialize.h"
#include "programdata.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"

namespace {

struct RestoreContext {
  daedalus::DaedalusVm& vm;
  std::shared_ptr<daedalus::DaedalusInstance> caller;
  ~RestoreContext() { vm.unsafe_set_gi(std::move(caller)); }
  };

void pushTieredLearnCost(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol& result,
                        const std::shared_ptr<daedalus::DaedalusInstance>& learner,
                        daedalus::DaedalusSymbol* aivar, uint16_t index,
                        std::optional<int32_t> points = std::nullopt) {
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

bool hasStockNpcThreeArguments(daedalus::DaedalusVm& vm) {
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

std::span<daedalus::DaedalusSymbol> bindNpcThreeArguments(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper) {
  auto args = vm.find_parameters_for_function(&helper);
  const auto caller = vm.unsafe_get_gi();
  vm.set_int(caller,&args[2],0,vm.pop_int());
  vm.set_int(caller,&args[1],0,vm.pop_int());
  args[0].set_instance(vm.pop_instance());
  return args;
  }

void increaseNpcField(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol& learner,
                       daedalus::DaedalusSymbol& points, const char* field, uint16_t index) {
  const auto npc = learner.get_instance();
  vm.unsafe_set_gi(npc);
  auto* member = vm.find_symbol_by_name(field);
  const auto value = vm.get_int(npc,member,index);
  const auto delta = vm.get_int(npc,&points,0);
  vm.set_int(npc,member,index,std::bit_cast<int32_t>(uint32_t(value)+uint32_t(delta)));
  }

void raiseRealAttributeLearnCounter(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper) {
  const auto args = bindNpcThreeArguments(vm,helper);
  const auto row = std::ranges::find(attributeRaises,args[1].get_int(),&AttributeRaise::attribute);
  if(row!=std::end(attributeRaises) && row->trainingSlot) {
    RestoreContext restore{vm,vm.unsafe_get_gi()};
    increaseNpcField(vm,args[0],args[2],"C_NPC.AIVAR",*row->trainingSlot);
    }
  }

void raiseAttribute(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper,
                    const daedalus::DaedalusSymbol& counter) {
  const auto args = bindNpcThreeArguments(vm,helper);
  const auto id = args[1].get_int();
  const auto row = std::ranges::find(attributeRaises,id,&AttributeRaise::attribute);
  RestoreContext restore{vm,vm.unsafe_get_gi()};
  int32_t printResults = 0;
  if(row!=std::end(attributeRaises)) {
    const auto symbol = [&](std::string_view name) { return vm.find_symbol_by_name(name); };
    auto& learner = args[0];
    increaseNpcField(vm,learner,args[2],"C_NPC.ATTRIBUTE",uint16_t(id));
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

bool printLessonNotice(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol* text,
                       std::string_view font = "FONT_SCREEN") {
  vm.push_reference(text);
  vm.push_int(-1);
  vm.push_int(-1);
  vm.push_reference(vm.find_symbol_by_name(font));
  vm.push_int(2);
  return vm.unsafe_call_external(vm.find_symbol_by_name("PRINTSCREEN"));
  }

void refuseLesson(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol* notice, daedalus::DaedalusSymbol* teacher,
                  daedalus::DaedalusSymbol* learner, std::string_view speech,
                  std::string_view font = "FONT_SCREEN") {
  const bool printed=printLessonNotice(vm,notice,font);
  vm.push_reference(teacher);
  vm.push_reference(learner);
  vm.push_reference(vm.find_symbol_by_name(speech));
  vm.unsafe_call_bl(vm.find_symbol_by_name("B_SAY"));
  if(printed)
    (void)vm.pop_int(); // The script discards PRINTSCREEN's result on return.
  vm.push_reference(vm.find_symbol_by_name("FALSE"));
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
    refuseLesson(vm,notice,teacher,learner,alreadyAtLimit ? "$27344" : "$27345");
    return;
    }
  auto* lp = symbol("C_NPC.LP");
  if(vm.get_int(student,lp,0)<cost) {
    refuseLesson(vm,symbol("PRINT_NOTENOUGHLP"),teacher,learner,"$27346");
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

static constexpr struct FightLesson {
  uint8_t counter;
  int32_t pairedTalent;
  const char* talentSymbol;
  const char* singleNotice;
  const char* pairedNotice;
  } fightLessons[] = {
  {84,TALENT_2H,       "NPC_TALENT_1H",       "PRINT_LEARN1H",       "PRINT_LEARN1H_AND_2H"},
  {85,TALENT_1H,       "NPC_TALENT_2H",       "PRINT_LEARN2H",       "PRINT_LEARN2H_AND_1H"},
  {86,TALENT_CROSSBOW, "NPC_TALENT_BOW",      "PRINT_LEARNBOW",      "PRINT_LEARNBOW_AND_CROSSBOW"},
  {87,TALENT_BOW,      "NPC_TALENT_CROSSBOW", "PRINT_LEARNCROSSBOW", "PRINT_LEARNCROSSBOW_AND_BOW"},
  };

void raiseRealFightTalent(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper) {
  const auto args=bindNpcThreeArguments(vm,helper);
  const auto talent=args[1].get_int();
  if(TALENT_1H<=talent && talent<=TALENT_CROSSBOW) {
    RestoreContext restore{vm,vm.unsafe_get_gi()};
    increaseNpcField(vm,args[0],args[2],"C_NPC.AIVAR",fightLessons[size_t(talent-TALENT_1H)].counter);
    }
  }

void raiseFightTalent(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper) {
  const auto args=bindNpcThreeArguments(vm,helper);
  for(const auto* child:{ "B_RAISEREALFIGHTTALENTPERCENT", "B_ADDFIGHTSKILL" }) {
    for(auto& arg:args)
      vm.push_reference(&arg);
    vm.unsafe_call_bl(vm.find_symbol_by_name(child));
    }
  }

void teachFightTalent(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper) {
  const auto symbol=[&](std::string_view name) { return vm.find_symbol_by_name(name); };
  auto args=vm.find_parameters_for_function(&helper);
  auto& teacher=args[0];
  auto& learner=args[1];
  auto& talent=args[2];
  auto& percent=args[3];
  auto& maximum=args[4];
  const auto caller=vm.unsafe_get_gi();
  for(auto* arg:{&maximum,&percent,&talent})
    vm.set_int(caller,arg,0,vm.pop_int());
  learner.set_instance(vm.pop_instance());
  teacher.set_instance(vm.pop_instance());

  vm.push_reference(&percent);
  vm.push_reference(&learner);
  vm.push_reference(&talent);
  vm.push_int(1);
  vm.unsafe_call_bl(symbol("B_GETLEARNCOSTTALENT"));
  const auto unitPrice=vm.pop_int();
  const auto cost=std::bit_cast<int32_t>(uint32_t(unitPrice)*uint32_t(vm.pop_int()));
  vm.set_int(caller,symbol("B_TEACHFIGHTTALENTPERCENT.KOSTEN"),0,cost);
  const auto id=talent.get_int();
  if(id<TALENT_1H || id>TALENT_CROSSBOW) {
    vm.push_reference(symbol("$27347")); // Stock wrong-parameter message.
    vm.unsafe_call_external(symbol("PRINT"));
    vm.push_reference(symbol("FALSE"));
    return;
    }

  RestoreContext restore{vm,caller};
  const auto student=learner.get_instance();
  vm.unsafe_set_gi(student);
  const auto value=vm.get_int(student,symbol("C_NPC.HITCHANCE"),uint16_t(id));
  vm.set_int(student,symbol("B_TEACHFIGHTTALENTPERCENT.REALHITCHANCE"),0,value);
  const auto limit=maximum.get_int();
  const bool alreadyAtLimit=value>=limit;
  if(alreadyAtLimit || std::bit_cast<int32_t>(uint32_t(value)+uint32_t(percent.get_int()))>limit) {
    vm.push_reference(symbol("PRINT_NOLEARNOVERPERSONALMAX"));
    vm.push_reference(&maximum);
    vm.unsafe_call_external(symbol("INTTOSTRING"));
    vm.unsafe_call_external(symbol("CONCATSTRINGS"));
    auto* notice=symbol("B_TEACHFIGHTTALENTPERCENT.CONCATTEXT");
    vm.set_string(student,notice,0,vm.pop_string());
    refuseLesson(vm,notice,&teacher,&learner,alreadyAtLimit ? "$27348" : "$27349");
    return;
    }

  auto* lp=symbol("C_NPC.LP");
  const auto balance=vm.get_int(student,lp,0);
  if(balance<cost) {
    refuseLesson(vm,symbol("PRINT_NOTENOUGHLP"),&teacher,&learner,"$27350");
    return;
    }
  vm.set_int(student,lp,0,std::bit_cast<int32_t>(uint32_t(balance)-uint32_t(cost)));
  const auto raise=[&](const FightLesson& lesson) {
    vm.push_reference(&learner);
    vm.push_reference(symbol(lesson.talentSymbol));
    vm.push_reference(&percent);
    vm.unsafe_call_bl(symbol("B_RAISEFIGHTTALENT"));
    };
  const auto& lesson=fightLessons[size_t(id-TALENT_1H)];
  const auto& pairedLesson=fightLessons[size_t(lesson.pairedTalent-TALENT_1H)];
  raise(lesson);
  auto* counters=symbol("C_NPC.AIVAR");
  // Pairing uses trained skill after the increase, including the exact 30-point boundary.
  const auto pairedLimit=std::bit_cast<int32_t>(uint32_t(vm.get_int(student,counters,pairedLesson.counter))+30u);
  const bool paired=vm.get_int(student,counters,lesson.counter)>=pairedLimit;
  if(paired)
    raise(pairedLesson);
  if(printLessonNotice(vm,symbol(paired ? lesson.pairedNotice : lesson.singleNotice)))
    (void)vm.pop_int();
  vm.push_reference(symbol("TRUE"));
  }

static constexpr struct RuneLesson {
  int32_t spell;
  const char* scroll;
  const char* recipe;
  } runeLessons[] = {
  {18,"ITSC_LIGHT","$27381"},
  {19,"ITSC_FIREBOLT","$27382"},
  {20,"ITSC_ICEBOLT","$27383"},
  {21,"ITSC_LIGHTHEAL","$27384"},
  {22,"ITSC_SUMGOBSKEL","$27385"},
  {23,"ITSC_INSTANTFIREBALL","$27386"},
  {24,"ITSC_ZAP","$27387"},
  {25,"ITSC_SUMWOLF","$27388"},
  {26,"ITSC_WINDFIST","$27389"},
  {27,"ITSC_SLEEP","$27390"},
  {28,"ITSC_MEDIUMHEAL","$27391"},
  {29,"ITSC_LIGHTNINGFLASH","$27392"},
  {30,"ITSC_CHARGEFIREBALL","$27393"},
  {31,"ITSC_SUMSKEL","$27394"},
  {32,"ITSC_FEAR","$27395"},
  {33,"ITSC_ICECUBE","$27396"},
  {34,"ITSC_THUNDERBALL","$27397"},
  {35,"ITSC_SUMGOL","$27398"},
  {36,"ITSC_HARMUNDEAD","$27399"},
  {37,"ITSC_PYROKINESIS","$27400"},
  {38,"ITSC_FIRESTORM","$27401"},
  {39,"ITSC_ICEWAVE","$27402"},
  {40,"ITSC_SUMDEMON","$27403"},
  {41,"ITSC_FULLHEAL","$27404"},
  {42,"ITSC_FIRERAIN","$27405"},
  {43,"ITSC_BREATHOFDEATH","$27406"},
  {44,"ITSC_MASSDEATH","$27407"},
  {45,"ITSC_ARMYOFDARKNESS","$27408"},
  {46,"ITSC_SHRINK","$27409"},
  {71,"ITSC_WHIRLWIND","LOG_TEXT_ADDON_TALENTRUNE_WHIRLWIND"},
  {72,"ITSC_WATERFIST","LOG_TEXT_ADDON_TALENTRUNE_WATERFIST"},
  {73,"ITSC_ICELANCE","LOG_TEXT_ADDON_TALENTRUNE_ICELANCE"},
  {75,"ITSC_GEYSER","LOG_TEXT_ADDON_TALENTRUNE_GEYSER"},
  {70,"ITSC_THUNDERSTORM","LOG_TEXT_ADDON_TALENTRUNE_THUNDERSTORM"},
  };

void teachRune(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& helper) {
  const auto symbol=[&](std::string_view name) { return vm.find_symbol_by_name(name); };
  auto args=vm.find_parameters_for_function(&helper);
  auto& teacher=args[0];
  auto& learner=args[1];
  auto& spell=args[2];
  RestoreContext restore{vm,vm.unsafe_get_gi()};
  vm.set_int(restore.caller,&spell,0,vm.pop_int());
  learner.set_instance(vm.pop_instance());
  teacher.set_instance(vm.pop_instance());
  auto* runes=symbol("NPC_TALENT_RUNES");
  vm.push_reference(&learner);
  vm.push_reference(runes);
  vm.push_reference(&spell);
  vm.unsafe_call_bl(symbol("B_GETLEARNCOSTTALENT"));
  const auto cost=vm.pop_int();
  vm.set_int(restore.caller,symbol("B_TEACHPLAYERTALENTRUNES.KOSTEN"),0,cost);

  const auto student=learner.get_instance();
  vm.unsafe_set_gi(student);
  auto* lp=symbol("C_NPC.LP");
  const auto balance=vm.get_int(student,lp,0);
  if(balance<cost) {
    refuseLesson(vm,symbol("PRINT_NOTENOUGHLEARNPOINTS"),&teacher,&learner,"$27379","FONT_SCREENSMALL");
    return;
    }
  vm.set_int(student,lp,0,std::bit_cast<int32_t>(uint32_t(balance)-uint32_t(cost)));
  auto* topic=symbol("TOPIC_TALENTRUNES");
  vm.push_reference(topic);
  vm.push_reference(symbol("LOG_NOTE"));
  vm.unsafe_call_external(symbol("LOG_CREATETOPIC"));
  const auto addJournalEntry=[&](const char* entry) {
    vm.push_reference(topic);
    vm.push_reference(symbol(entry));
    vm.unsafe_call_bl(symbol("B_LOGENTRY"));
    };
  addJournalEntry("$27380"); // Stock introduction to rune crafting.

  auto* trader=symbol("B_TEACHPLAYERTALENTRUNES.SCROLLTRADER");
  for(const auto* name:{"GORAX","ISGAROTH","ENGOR","ORLAN","CRONOS_ADW"}) {
    auto* candidate=symbol(name);
    vm.push_reference(candidate);
    vm.unsafe_call_external(symbol("NPC_ISDEAD"));
    if(vm.pop_int()!=symbol("FALSE")->get_int())
      continue;
    vm.push_int(int32_t(candidate->index()));
    vm.unsafe_call_external(symbol("HLP_GETNPC"));
    trader->set_instance(vm.pop_instance());
    break;
    }
  // If no merchant is available, the original uses the saved trader slot.
  const auto id=spell.get_int();
  const auto recipe=std::ranges::find(runeLessons,id,&RuneLesson::spell);
  if((0<=id && id<=14) || recipe!=std::end(runeLessons))
    vm.set_int(student,symbol("PLAYER_TALENT_RUNES"),uint16_t(id),symbol("TRUE")->get_int());
  if(recipe!=std::end(runeLessons)) {
    vm.push_reference(trader);
    vm.push_int(int32_t(symbol(recipe->scroll)->index()));
    vm.push_int(1);
    vm.unsafe_call_external(symbol("CREATEINVITEMS"));
    addJournalEntry(recipe->recipe);
    }

  const bool printed=printLessonNotice(vm,symbol("PRINT_LEARNRUNES"));
  vm.push_reference(&learner);
  vm.push_reference(runes);
  vm.push_int(1);
  vm.unsafe_call_external(symbol("NPC_SETTALENTSKILL"));
  if(printed)
    (void)vm.pop_int();
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
  const auto ambientData = DialogInfoData::loadAmbient(baseProgramIndex().dir.parent_path());
  if(!ambientData)
    return;
  std::array<Binding,std::size(selectors)> bindings;
  auto* infoClass = findSymbol("C_INFO");
  auto* npcMember = findSymbol("C_INFO.NPC");
  auto* instanceId = findSymbol("HLP_GETINSTANCEID");
  for(size_t i=0; i<std::size(selectors); ++i) {
    if(selectors[i].suffix.empty())
      continue;
    const auto name = std::string("B_ASSIGNAMBIENTINFOS_").append(selectors[i].suffix);
    const auto found = ambientData->find(name);
    if(found==ambientData->end())
      throw BaseStore::Error("missing stock ambient dialog binding: "+name);
    auto& binding = bindings[i];
    binding.savedActor = findSymbol(found->second.actor);
    if(binding.savedActor==nullptr || binding.savedActor->name()!=name+".SLF" || binding.savedActor->type()!=Type::INSTANCE)
      throw BaseStore::Error("invalid stock ambient dialog actor: "+name);
    for(auto id : found->second.topics) {
      auto* topic = findSymbol(id);
      if(topic==nullptr || topic->type()!=Type::INSTANCE || topic->parent()!=infoClass->index())
        throw BaseStore::Error("invalid stock ambient dialog topic: "+name);
      binding.topics.push_back(topic);
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
      storySlot("B_BUILDLEARNSTRING.KOSTEN")->set_int(machine.pop_int());
      // The script returns its global string slot, which a later call can overwrite before it is read.
      machine.push_reference(&buildLearnString(machine.pop_string()));
      return {};
      }
    },enabled);
  vm.override_function("B_GETLEARNCOSTATTRIBUTE",std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [this](auto& machine) -> daedalus::DaedalusNakedCall {
      const auto attribute = machine.pop_int();
      storySlot("B_GETLEARNCOSTATTRIBUTE.ATTRIBUT")->set_int(attribute);
      const auto learner = machine.pop_instance();
      storySlot("B_GETLEARNCOSTATTRIBUTE.OTH")->set_instance(learner);
      auto* result = storySlot("B_GETLEARNCOSTATTRIBUTE.KOSTEN");
      result->set_int(0);
      std::string_view slot;
      switch(attribute) {
        case ATR_STRENGTH:  slot = "REAL_STRENGTH";  break;
        case ATR_DEXTERITY: slot = "REAL_DEXTERITY"; break;
        case ATR_MANAMAX:   slot = "REAL_MANA_MAX";  break;
        default: machine.push_reference(result); return {};
        }
      pushTieredLearnCost(machine,*result,learner,storySlot("C_NPC.AIVAR"),uint16_t(storySlot(slot)->get_int()));
      return {};
      }
    },enabled);
  vm.override_function("B_GETLEARNCOSTTALENT",std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [this](auto& machine) -> daedalus::DaedalusNakedCall {
      const auto skill = machine.pop_int();
      storySlot("B_GETLEARNCOSTTALENT.SKILL")->set_int(skill);
      const auto talent = machine.pop_int();
      storySlot("B_GETLEARNCOSTTALENT.TALENT")->set_int(talent);
      const auto learner = machine.pop_instance();
      storySlot("B_GETLEARNCOSTTALENT.OTH")->set_instance(learner);
      auto* result = storySlot("B_GETLEARNCOSTTALENT.KOSTEN");
      result->set_int(0);
      if(TALENT_1H<=talent && talent<=TALENT_CROSSBOW) {
        static constexpr std::string_view slots[] = {
          "REAL_TALENT_1H","REAL_TALENT_2H","REAL_TALENT_BOW","REAL_TALENT_CROSSBOW"
          };
        const auto index = uint16_t(storySlot(slots[size_t(talent-TALENT_1H)])->get_int());
        pushTieredLearnCost(machine,*result,learner,storySlot("C_NPC.AIVAR"),index,skill);
        } else {
        result->set_int(fixedTalentLearnCost(talent,skill));
        machine.push_reference(result);
        }
      return {};
      }
    },enabled);
  auto* raise = findSymbol("B_RAISEATTRIBUTE");
  auto* counter = findSymbol("B_RAISEREALATTRIBUTELEARNCOUNTER");
  const auto canRaise = [this,enabled] { return enabled() && hasStockNpcThreeArguments(vm); };
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
  auto* fightLesson=findSymbol("B_TEACHFIGHTTALENTPERCENT");
  vm.override_function(fightLesson->name(),std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [fightLesson](auto& machine) -> daedalus::DaedalusNakedCall {
      teachFightTalent(machine,*fightLesson);
      return {};
      }
    },enabled);
  auto* runeLesson=findSymbol("B_TEACHPLAYERTALENTRUNES");
  vm.override_function(runeLesson->name(),std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [runeLesson](auto& machine) -> daedalus::DaedalusNakedCall {
      teachRune(machine,*runeLesson);
      return {};
      }
    },enabled);
  auto* raiseFight=findSymbol("B_RAISEFIGHTTALENT");
  vm.override_function(raiseFight->name(),std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [raiseFight](auto& machine) -> daedalus::DaedalusNakedCall {
      raiseFightTalent(machine,*raiseFight);
      return {};
      }
    },canRaise);
  auto* realFight=findSymbol("B_RAISEREALFIGHTTALENTPERCENT");
  vm.override_function(realFight->name(),std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
    [realFight](auto& machine) -> daedalus::DaedalusNakedCall {
      raiseRealFightTalent(machine,*realFight);
      return {};
      }
    },canRaise);
  auto* addFight=findSymbol("B_ADDFIGHTSKILL");
  if(npcDefinitions.helpers.helpers.contains(addFight->index())) {
    const auto student=[this] {
      const auto& frame=vm.stack_frame(2);
      const auto actor=frame.reference ? std::get<daedalus::DaedalusSymbol*>(frame.value)->get_instance() :
                                        std::get<std::shared_ptr<daedalus::DaedalusInstance>>(frame.value);
      return std::static_pointer_cast<daedalus::INpc>(actor);
      };
    vm.override_function(addFight->name(),std::function<daedalus::DaedalusNakedCall(daedalus::DaedalusVm&)>{
      [this,addFight,student](auto& machine) -> daedalus::DaedalusNakedCall {
        RestoreContext restore{machine,machine.unsafe_get_gi()};
        NpcHelperRuntime::invoke(machine,student(),addFight,npcDefinitions.helpers);
        return {};
        }
      },[canRaise,student] { return NpcHelperRuntime::enabled() && canRaise() && student()!=nullptr; });
    }
  }

auto GameScript::buildLearnString(std::string_view title) -> daedalus::DaedalusSymbol& {
  auto& savedTitle = *storySlot("B_BUILDLEARNSTRING.TEXT");
  savedTitle.set_string(title);
  const auto cost = storySlot("B_BUILDLEARNSTRING.KOSTEN")->get_int();
  const auto text = savedTitle.get_string() + storySlot("PRINT_KOSTEN")->get_string() +
                    std::to_string(cost) + storySlot("PRINT_LP")->get_string();
  // The formatting externals reuse one temporary slot, which the legacy save also stores.
  vm.push_string(text);
  auto& result = *storySlot("B_BUILDLEARNSTRING.CONCATTEXT");
  result.set_string(vm.pop_string());
  return result;
  }

bool GameScript::readDialogInfos() {
  const auto rows = DialogInfoData::load(baseProgramIndex().dir.parent_path());
  if(!rows)
    return false;

  const auto* cls = vm.find_symbol_by_name("C_INFO");
  dialogsInfo.reserve(rows->size());
  vm.enumerate_instances_by_class_name("C_INFO",[&](auto& symbol) {
    const auto fields = rows->find(symbol.index());
    if(fields==rows->end())
      throw std::runtime_error("dialog infos: missing instance "+symbol.name());
    auto info = vm.allocate_instance<daedalus::IInfo>(&symbol);
    for(const auto& field : fields->second) {
      auto* member = vm.find_symbol_by_index(field.member);
      if(member==nullptr || !member->is_member() || member->parent()!=cls->index())
        throw std::runtime_error("dialog infos: invalid member for "+symbol.name());
      if(const auto* lesson = std::get_if<DialogInfoData::Lesson>(&field.value)) {
        vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.SKILL")->set_int(lesson->skill);
        vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.TALENT")->set_int(lesson->talent);
        vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.OTH")->set_instance(vm.global_other()->get_instance());
        const auto cost = fixedTalentLearnCost(lesson->talent,lesson->skill);
        vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.KOSTEN")->set_int(cost);
        vm.find_symbol_by_name("B_BUILDLEARNSTRING.KOSTEN")->set_int(cost);
        member->set_string(buildLearnString(lesson->title).get_string(),0,info.get());
        }
      else if(const auto* title = std::get_if<std::string>(&field.value))
        member->set_string(*title,0,info.get());
      else
        member->set_int(std::get<int32_t>(field.value),0,info.get());
      }
    rememberStoryDialog(symbol.name(),*info);
    dialogsInfo.push_back(std::move(info));
    });
  if(dialogsInfo.size()!=rows->size())
    throw std::runtime_error("dialog infos: unexpected instance in stock records");
  return true;
  }

void GameScript::setNpcInfoKnown(const NpcRecord& npc, const daedalus::IInfo& info) {
  const auto actor = static_cast<const daedalus::INpc&>(npc).symbol_index();
  if(actor!=uint32_t(-1) && info.symbol_index()!=uint32_t(-1))
    dlgKnownInfos.emplace(actor,info.symbol_index());
  }

bool GameScript::markNpcInfoKnown(const Npc& npc, std::string_view name) {
  const auto* info = storyDialog(name);
  if(info==nullptr)
    return false;
  setNpcInfoKnown(*npc.handlePtr(),*info);
  return true;
  }

bool GameScript::doesNpcKnowInfo(const NpcRecord& npc, size_t infoInstance) const {
  const auto actor = static_cast<const daedalus::INpc&>(npc).symbol_index();
  return dlgKnownInfos.contains({actor,infoInstance});
  }

void GameScript::rememberStoryDialog(std::string_view name, daedalus::IInfo& info) {
  const auto [entry, inserted] = storyDialogs.emplace(name,&info);
  if(!inserted || !storyDialogsById.try_emplace(info.symbol_index(),entry->first,&info).second)
    throw std::runtime_error("duplicate story dialog: "+std::string(name));
  }

const daedalus::IInfo* GameScript::storyDialog(std::string_view name) const {
  if(const auto entry=storyDialogs.find(name); entry!=storyDialogs.end())
    return entry->second;
  std::string upper(name);
  for(auto& ch : upper)
    ch = char(std::toupper(static_cast<unsigned char>(ch)));
  const auto entry = storyDialogs.find(upper);
  return entry!=storyDialogs.end() ? entry->second : nullptr;
  }

std::string_view GameScript::storyDialogName(uint32_t id) const {
  const auto entry = storyDialogsById.find(id);
  return entry!=storyDialogsById.end() ? entry->second.first : std::string_view{};
  }

std::optional<uint32_t> GameScript::storyDialogFunction(std::string_view name, bool condition) const {
  const auto* info = storyDialog(name);
  if(info==nullptr)
    return std::nullopt;
  const auto function = condition ? info->condition : info->information;
  return function>0 ? std::optional<uint32_t>(uint32_t(function)) : std::nullopt;
  }

auto GameScript::createItem(Item& owner, size_t instance, uint32_t& kind) -> std::shared_ptr<ItemRecord> {
  auto item = std::make_shared<daedalus::IItem>();
  owner.hitem = item;
  item->user_ptr = &owner;
  kind = uint32_t(instance);
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
  if(!bindPreparedItem(item,symbol)) {
    if(auto* sym = vm.find_symbol_by_index(symbol))
      vm.allocate_instance(item,sym);
    else
      Tempest::Log::e("Loading unknown item from save: ",symbol);
    }
  kind = item->symbol_index();
  fin.readItem(*item);
  return item;
  }

void GameScript::attachItem(const std::shared_ptr<ItemRecord>& record, Item& owner) {
  std::static_pointer_cast<daedalus::IItem>(record)->user_ptr = &owner;
  }
