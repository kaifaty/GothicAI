#include "gamescript.h"

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <stdexcept>
#include <variant>
#include <vector>

namespace {
struct LiteralPlacement {
  const daedalus::DaedalusSymbol* instance;
  daedalus::DaedalusSymbol* point;
  const daedalus::DaedalusSymbol* external;
  };

LiteralPlacement readLiteralPlacement(daedalus::DaedalusScript& vm,uint32_t& pc,std::string_view section) {
  using Op = daedalus::DaedalusOpcode;
  using Type = daedalus::DaedalusDataType;
  // The three literal operations each have a four-byte operand.
  if(pc>=vm.size() || vm.size()-pc<15)
    throw std::runtime_error("Stock placement: truncated call in "+std::string(section));
  const auto instance = vm.instruction_at(pc);
  const auto point = vm.instruction_at(pc+5);
  const auto insert = vm.instruction_at(pc+10);
  const auto* what = vm.find_symbol_by_index(uint32_t(instance.immediate));
  auto* where = vm.find_symbol_by_index(point.symbol);
  const auto* external = vm.find_symbol_by_index(insert.symbol);
  if(instance.op!=Op::PUSHI || !what || what->type()!=Type::INSTANCE || !what->is_const() ||
     point.op!=Op::PUSHV || !where || where->type()!=Type::STRING || !where->is_const() ||
     insert.op!=Op::BE || !external || (external->name()!="WLD_INSERTNPC" && external->name()!="WLD_INSERTITEM"))
    throw std::runtime_error("Stock placement: unexpected literal in "+std::string(section));
  pc+=15;
  return {what,where,external};
  }

bool runKhorinisNpcTrigger(daedalus::DaedalusVm& vm,const daedalus::DaedalusSymbol& trigger) {
  const auto symbol = [&vm](std::string_view name) { return vm.find_symbol_by_name(name); };
  const auto ref = [&vm,symbol](std::string_view name) { vm.push_reference(symbol(name)); };
  const auto actor = [&vm,symbol](std::string_view name) { vm.push_instance(symbol(name)->get_instance()); };
  const auto external = [&vm,symbol](std::string_view name) { vm.unsafe_call_external(symbol(name)); };
  const auto value = [symbol](std::string_view name) { return symbol(name)->get_int(); };
  if(trigger.name()=="B_MAGECAVETRIGGER") {
    const bool questActive = value("MIS_SCHNITZELJAGD")==value("LOG_RUNNING");
    actor("MAGICGOLEM");
    external("NPC_ISDEAD");
    const auto golemDead = vm.pop_int();
    if(!questActive || golemDead!=value("FALSE"))
      return true;
    actor("AGON");
    external("NPC_ISDEAD");
    const auto agonDead = vm.pop_int();
    actor("AGON");
    external("HLP_ISVALIDNPC");
    const auto agonValid = vm.pop_int();
    if(agonDead || !agonValid)
      return true;
    actor("AGON");
    ref("$51970"); // NW_TROLLAREA_PATH_02
    external("AI_TELEPORT");
    actor("AGON");
    ref("$51971"); // GOLEMLIVES
    external("NPC_EXCHANGEROUTINE");
    actor("AGON");
    external("AI_CONTINUEROUTINE");
    return true;
    }

  struct Spawn {
    std::string_view trigger, once;
    uint32_t offset, count;
    std::array<std::string_view,7> ornamentText;
    };
  static constexpr Spawn spawns[] = {
    {"EVT_CAVALORNSGOBBOS_FUNC","EVT_CAVALORNSGOBBOS_FUNC_ONETIME",16,3,{}},
    {"EVT_TROLL_GRAVE_01",{},0,4,{}},
    {"EVT_ORNAMENT_SWITCH_FOREST_01_FUNC","ORNAMENT_SWITCHED_FOREST",16,2,
     {"$52164","$52165","$52166","$52167","$52168","$52169","$52170"}},
    {"EVT_ORNAMENT_SWITCH_BIGFARM_01_FUNC","ORNAMENT_SWITCHED_BIGFARM",16,2,
     {"$52153","$52154","$52155","$52156","$52157","$52158","$52159"}},
    };
  const auto spawn = std::ranges::find(spawns,trigger.name(),&Spawn::trigger);
  if(spawn==std::end(spawns))
    return false;
  auto* once = spawn->once.empty() ? nullptr : symbol(spawn->once);
  if(once && once->get_int()!=value("FALSE"))
    return true;
  auto pc = trigger.address()+spawn->offset;
  daedalus::DaedalusSymbol* placedItem = nullptr;
  for(uint32_t i=0;i<spawn->count;++i) {
    const auto row = readLiteralPlacement(vm,pc,trigger.name());
    vm.push_int(int32_t(row.instance->index()));
    vm.push_reference(row.point);
    vm.unsafe_call_external(row.external);
    if(row.external->name()=="WLD_INSERTITEM")
      placedItem = vm.find_symbol_by_index(row.instance->index());
    }
  if(placedItem) {
    const auto& text = spawn->ornamentText;
    const auto effect = [&](std::string_view name,daedalus::DaedalusSymbol& target) {
      ref(name);
      vm.push_instance(target.get_instance());
      vm.push_instance(target.get_instance());
      vm.push_int(0);
      vm.push_int(0);
      vm.push_int(0);
      ref("FALSE");
      external("WLD_PLAYEFFECT");
      };
    effect(text[0],*vm.global_hero());
    effect(text[1],*placedItem);
    for(const auto sound : {text[2],text[3],text[4]}) {
      ref(sound);
      external("SND_PLAY");
      }
    effect(text[5],*placedItem);
    ref("TOPIC_ADDON_ORNAMENT");
    ref("LOG_MISSION");
    external("LOG_CREATETOPIC");
    ref("TOPIC_ADDON_ORNAMENT");
    ref("LOG_RUNNING");
    external("LOG_SETTOPICSTATUS");
    ref("TOPIC_ADDON_ORNAMENT");
    ref(text[6]);
    vm.unsafe_call_bl(symbol("B_LOGENTRY"));
    }
  if(once)
    vm.set_int(nullptr,once,0,value("TRUE"));
  return true;
  }

void insertLiteralNpcs(daedalus::DaedalusVm& vm,const daedalus::DaedalusSymbol& trigger,uint32_t offset,uint32_t count) {
  auto pc = trigger.address()+offset;
  for(uint32_t i=0;i<count;++i) {
    const auto spawn = readLiteralPlacement(vm,pc,trigger.name());
    if(spawn.external->name()!="WLD_INSERTNPC")
      throw std::runtime_error("Stock NPC spawn trigger: expected NPC placement");
    vm.push_int(int32_t(spawn.instance->index()));
    vm.push_reference(spawn.point);
    vm.unsafe_call_external(spawn.external);
    }
  }

bool runIslandSpawnTrigger(daedalus::DaedalusVm& vm,const daedalus::DaedalusSymbol& trigger) {
  struct Spawn {
    std::string_view name;
    int32_t          limit = 9;
    uint32_t         npcs = 1;
    };
  static constexpr Spawn rooms[] = {
    {"EVT_KEYMASTER_01",1}, {"EVT_KEYMASTER_02",1}, {"EVT_KEYMASTER_03",1}, {"EVT_KEYMASTER_04",1},
    {"EVT_LEFT_UP_01_SPAWNNEW",3,3},
    {"EVT_LEFT_DOWN_01_SKEL_01"}, {"EVT_LEFT_DOWN_01_SKEL_02"}, {"EVT_LEFT_DOWN_01_SKEL_03"},
    {"EVT_LEFT_ROOM_02_SKEL_01"}, {"EVT_LEFT_ROOM_02_SKEL_02"}, {"EVT_LEFT_ROOM_02_SKEL_03"},
    {"EVT_LEFT_ROOM_02_SKEL_04"}, {"EVT_LEFT_ROOM_02_SKEL_05"},
    {"EVT_RIGHT_DOWN_01_SKEL_01"}, {"EVT_RIGHT_DOWN_01_SKEL_02"}, {"EVT_RIGHT_DOWN_01_SKEL_03"},
    {"EVT_RIGHT_DOWN_01_SKEL_04"},
    {"EVT_RIGHT_ROOM_01_SKEL_01"}, {"EVT_RIGHT_ROOM_01_SKEL_02"},
    {"EVT_RIGHT_ROOM_02_SKEL_01"}, {"EVT_RIGHT_ROOM_02_SKEL_02"},
    {"EVT_RIGHT_UP_01_SKEL_01"}, {"EVT_RIGHT_UP_01_SKEL_02"},
    {"EVT_RIGHT_UP_02_SKEL_01"}, {"EVT_RIGHT_UP_02_SKEL_02"}, {"EVT_RIGHT_UP_02_SKEL_03"},
    };
  static constexpr std::string_view traps[] = {
    "EVT_UNDEADDRAGON_TRAP_01_FUNC", "EVT_UNDEADDRAGON_TRAP_02_FUNC", "EVT_UNDEADDRAGON_TRAP_03_FUNC",
    };
  const auto read = [&vm](daedalus::DaedalusSymbol* field) { return vm.get_int(nullptr,field,0); };
  const auto increment = [&vm,&read](daedalus::DaedalusSymbol* field) {
    vm.set_int(nullptr,field,0,std::bit_cast<int32_t>(uint32_t(read(field))+1u));
    };
  const auto& name = trigger.name();
  const auto room = std::ranges::find(rooms,name,&Spawn::name);
  if(room!=std::end(rooms)) {
    auto* attempts = vm.find_symbol_by_name(name+"_ANTIEXPLOID");
    if(read(attempts)<room->limit) {
      // Each room's initial counter comparison and jump occupy 16 bytes.
      insertLiteralNpcs(vm,trigger,16,room->npcs);
      increment(attempts);
      }
    return true;
    }
  if(std::ranges::find(traps,name)==std::end(traps))
    return false;

  auto* activations = vm.find_symbol_by_name(name+"_COUNT_NOMORE");
  if(read(activations)<10) {
    auto* count = vm.find_symbol_by_name(name+"_COUNT");
    const auto before = read(count);
    if(before!=0 && before<2) {
      // The two count tests precede the trap's one literal placement.
      insertLiteralNpcs(vm,trigger,44,1);
      }
    increment(count);
    if(read(count)>2)
      vm.set_int(nullptr,count,0,1);
    increment(activations);
    }
  return true;
  }

void loadKhorinisLateChapters(daedalus::DaedalusVm& vm) {
  static constexpr auto chapter4 = "B_ENTER_NEWWORLD_KAPITEL_4";
  static constexpr auto chapter5 = "B_ENTER_NEWWORLD_KAPITEL_5";
  struct Group {
    uint32_t offset, count;
    };
  // Offsets and lengths in the verified stock program. Only the immutable
  // insert calls are read here, while the conditions are evaluated at entry.
  static constexpr std::array groups4 = {Group{82,15}, Group{328,61}, Group{1281,6},
                                          Group{1552,90}, Group{2923,23}, Group{3330,1}};
  static constexpr std::array groups5 = {Group{160,1}, Group{191,18}, Group{482,2},
                                          Group{554,26}, Group{1000,9}, Group{1772,1}};
  const auto read = [&](const char* name,const std::array<Group,6>& groups) {
    const auto start = vm.find_symbol_by_name(name)->address();
    std::array<std::vector<LiteralPlacement>,groups4.size()> rows;
    for(size_t i=0;i<groups.size();++i) {
      auto pc = start+groups[i].offset;
      rows[i].reserve(groups[i].count);
      for(uint32_t j=0;j<groups[i].count;++j)
        rows[i].push_back(readLiteralPlacement(vm,pc,name));
      }
    return rows;
    };
  const auto symbol = [&vm](std::string_view name) { return vm.find_symbol_by_name(name); };
  const auto number = [symbol](std::string_view name) { return symbol(name)->get_int(); };
  const auto set = [&vm,symbol,number](std::string_view target,std::string_view value) {
    vm.set_int(nullptr,symbol(target),0,number(value));
    };
  const auto insert = [&vm](const std::vector<LiteralPlacement>& rows) {
    for(const auto& row:rows) {
      vm.push_int(int32_t(row.instance->index()));
      vm.push_reference(row.point);
      vm.unsafe_call_external(row.external);
      }
    };
  const auto guildIs = [&vm,symbol,number](std::string_view guild) {
    const auto hero = vm.global_hero()->get_instance();
    vm.unsafe_set_gi(hero);
    return vm.get_int(hero,symbol("C_NPC.GUILD"),0)==number(guild);
    };
  const auto call = [&vm,symbol](std::string_view name) {
    vm.unsafe_call_bl(symbol(name));
    };
  const auto routine = [&vm,symbol,call](std::string_view actor,std::string_view label) {
    vm.push_reference(symbol(actor));
    vm.push_reference(symbol(label));
    call("B_STARTOTHERROUTINE");
    };
  const auto salandrilAtAltar = [&vm,symbol](std::string_view altar) {
    vm.push_reference(symbol("SALANDRIL"));
    vm.push_reference(symbol(altar));
    vm.unsafe_call_external(symbol("NPC_GETDISTTOWP"));
    return vm.pop_int()<10000;
    };
  const auto giveObsessionBook = [&vm,symbol](std::string_view actor) {
    vm.push_reference(symbol(actor));
    vm.push_int(int32_t(symbol("ITWR_DEMENTOROBSESSIONBOOK_MIS")->index()));
    vm.push_int(1);
    vm.unsafe_call_external(symbol("CREATEINVITEMS"));
    };

  vm.override_function(chapter4,std::function<void()>{[&vm,symbol,number,set,insert,guildIs,call,routine,
                                                       salandrilAtAltar,giveObsessionBook,rows=read(chapter4,groups4)] {
    if(number("ENTERNW_KAPITEL4")==number("FALSE")) {
      if(salandrilAtAltar("$28240"))
        routine("SALANDRIL","$28241"); // Start
      routine("JORGEN","$28242"); // Kloster
      vm.push_int(int32_t(symbol("BDT_1050_LANDSTREICHER")->index()));
      call("B_KILLNPC");
      insert(rows[0]);
      if(guildIs("GIL_PAL"))
        insert(rows[1]);
      const bool paladin = guildIs("GIL_PAL");
      const bool dragonHunter = guildIs("GIL_DJG");
      if(paladin || dragonHunter) {
        insert(rows[2]);
        routine("VINO","$28325"); // BugsThere
        routine("LOBARTSBAUER1","$28326");
        routine("LOBARTSBAUER2","$28327");
        }
      const bool hunter = guildIs("GIL_DJG");
      const bool mage = guildIs("GIL_KDF");
      if(hunter || mage) {
        for(const auto* shrine:{"SHRINEISOBSESSED_NW_TROLLAREA_PATH_37", "SHRINEISOBSESSED_NW_FARM1_CONNECT_XARDAS",
                                "SHRINEISOBSESSED_NW_TROLLAREA_PATH_66", "SHRINEISOBSESSED_NW_TROLLAREA_PATH_04",
                                "SHRINEISOBSESSED_SAGITTA", "SHRINEISOBSESSED_NW_BIGMILL_MALAKSVERSTECK_02",
                                "SHRINEISOBSESSED_NW_FARM3_BIGWOOD_02"})
          set(shrine,"FALSE");
        }
      if(guildIs("GIL_DJG"))
        insert(rows[3]);
      if(guildIs("GIL_KDF")) {
        insert(rows[4]);
        giveObsessionBook("RANDOLPH");
        routine("RANDOLPH","$28441"); // Obsessed
        }
      set("ENTERNW_KAPITEL4","TRUE");
      }
    if(number("TALBIN_FOLLOWSTHROUGHPASS")==number("LOG_RUNNING")) {
      insert(rows[5]);
      set("TALBIN_FOLLOWSTHROUGHPASS","LOG_SUCCESS");
      }
    }},PlacementDefinitionRuntime::enabled);

  vm.override_function(chapter5,std::function<void()>{[&vm,symbol,number,set,insert,guildIs,call,routine,
                                                       salandrilAtAltar,giveObsessionBook,rows=read(chapter5,groups5)] {
    const auto sekobAlive = [&] {
      vm.push_reference(symbol("SEKOB"));
      vm.unsafe_call_external(symbol("NPC_ISDEAD"));
      return vm.pop_int()==number("FALSE");
      };
    if(number("ENTERNW_KAPITEL5")==number("FALSE")) {
      vm.push_int(int32_t(symbol("XARDAS")->index())); // The stock call takes this alias's symbol ID.
      call("B_REMOVENPC");
      routine("LESTER","$28443"); // XardasWeg
      if(salandrilAtAltar("$28444"))
        routine("SALANDRIL","$28445"); // Start
      if(sekobAlive()) {
        routine("ROSI","$28446"); // FleeFromSekob
        routine("TILL","$28447");
        set("ROSI_FLEEFROMSEKOB_KAP5","TRUE");
        }
      if(number("GORNDJG_IS_ALIVE")==number("TRUE"))
        insert(rows[0]);
      if(number("DJG_ANGAR_IS_ALIVE")==number("TRUE"))
        insert(rows[1]);
      if(guildIs("GIL_PAL"))
        insert(rows[2]);
      (void)guildIs("GIL_DJG"); // Empty stock branch still evaluates the member read.
      if(guildIs("GIL_KDF")) {
        insert(rows[3]);
        if(sekobAlive()) {
          giveObsessionBook("SEKOB");
          routine("SEKOB","$28495"); // Obsessed
          }
        }
      insert(rows[4]);
      static constexpr const char* guards[] = {
        "SCHIFFSWACHE_212", "SCHIFFSWACHE_213", "PAL_220_SCHIFFSWACHE", "PAL_221_SCHIFFSWACHE", "PAL_222_SCHIFFSWACHE",
        "PAL_223_SCHIFFSWACHE", "PAL_224_SCHIFFSWACHE", "PAL_225_SCHIFFSWACHE", "PAL_226_SCHIFFSWACHE",
        "PAL_227_SCHIFFSWACHE", "PAL_228_SCHIFFSWACHE",
        };
      auto* flags = symbol("C_NPC.FLAGS");
      for(const auto* guard:guards) {
        const auto actor = symbol(guard)->get_instance();
        vm.unsafe_set_gi(actor);
        vm.set_int(actor,flags,0,0);
        }
      static constexpr std::pair<const char*,const char*> shipRoutines[] = {
        {"PAL_220_SCHIFFSWACHE","$28505"}, {"PAL_221_SCHIFFSWACHE","$28506"}, {"PAL_222_SCHIFFSWACHE","$28507"},
        {"PAL_223_SCHIFFSWACHE","$28508"}, {"PAL_224_SCHIFFSWACHE","$28509"}, {"PAL_225_SCHIFFSWACHE","$28510"},
        {"PAL_226_SCHIFFSWACHE","$28511"}, {"PAL_227_SCHIFFSWACHE","$28512"}, {"PAL_228_SCHIFFSWACHE","$28513"},
        {"PAL_230_RITTER","$28514"}, {"PAL_231_RITTER","$28515"}, {"PAL_240_RITTER","$28516"}, {"PAL_241_RITTER","$28517"},
        };
      for(const auto& [actor,label]:shipRoutines)
        routine(actor,label); // ShipFree
      const bool paladin = guildIs("GIL_PAL");
      const bool militia = guildIs("GIL_MIL");
      const auto art = paladin || militia ? std::array{"$28518","$28519"} :
                       guildIs("GIL_KDF") ? std::array{"$28520","$28521"} : std::array{"$28522","$28523"};
      vm.push_reference(symbol("KAPWECHSEL_5"));
      vm.push_reference(symbol("KAPWECHSEL_5_TEXT"));
      for(const auto* name:art)
        vm.push_reference(symbol(name));
      vm.push_int(6000);
      vm.unsafe_call_external(symbol("INTRODUCECHAPTER"));
      set("ENTERNW_KAPITEL5","TRUE");
      }
    const bool gateGuardsPending = number("PAL_SCHIFFSWACHE_EXCHANGE_ONETIME")==number("FALSE");
    const bool gateOpen = number("MIS_OCGATEOPEN")==number("TRUE");
    if(gateGuardsPending && gateOpen) {
      routine("PAL_212_SCHIFFSWACHE","$28524"); // ShipFree
      routine("PAL_213_SCHIFFSWACHE","$28525");
      set("PAL_SCHIFFSWACHE_EXCHANGE_ONETIME","TRUE");
      set("MIS_SHIPISFREE","TRUE");
      }
    if(number("BIFF_FOLLOWSTHROUGHPASS")==number("LOG_RUNNING")) {
      insert(rows[5]);
      set("BIFF_FOLLOWSTHROUGHPASS","LOG_SUCCESS");
      }
    }},PlacementDefinitionRuntime::enabled);
  }

bool runDragonIslandCrewTrigger(daedalus::DaedalusVm& vm,const daedalus::DaedalusSymbol& trigger) {
  const bool orcAttack = trigger.name()=="EVT_ORKOBERST";
  if(!orcAttack && trigger.name()!="EVT_UNDEADDRAGONDEAD_ENDSITUATION")
    return false;

  struct RestoreContext {
    daedalus::DaedalusVm& vm;
    std::shared_ptr<daedalus::DaedalusInstance> caller;
    ~RestoreContext() { vm.unsafe_set_gi(std::move(caller)); }
    } restore{vm,vm.unsafe_get_gi()};
  const auto symbol = [&](std::string_view name) { return vm.find_symbol_by_name(name); };
  const auto no = symbol("FALSE")->get_int();
  const auto yes = symbol("TRUE")->get_int();
  auto* once = symbol(trigger.name()+"_ONETIME");
  const auto dead = [&](const char* actor) {
    vm.push_reference(symbol(actor));
    vm.unsafe_call_external(symbol("NPC_ISDEAD"));
    return vm.pop_int();
    };
  const auto routine = [&](const char* actor,const char* name) {
    vm.push_reference(symbol(actor));
    vm.push_reference(symbol(name));
    vm.unsafe_call_bl(symbol("B_STARTOTHERROUTINE"));
    };
  if(!orcAttack) {
    if(dead("DRAGON_UNDEAD")!=0 && once->get_int()==no) {
      symbol("ANGAR_DI_PARTY")->set_int(symbol("LOG_SUCCESS")->get_int());
      routine("ANGAR_DI","$52181"); // FireDragonIsland
      routine("LEE_DI","$52182"); // UNDEADDRAGONDEAD
      routine("DIEGO_DI","$52183");
      routine("MILTEN_DI","$52184");
      routine("GORN_DI","$52185");
      routine("LESTER_DI","$52186");
      once->set_int(yes);
      }
    return true;
    }

  if(once->get_int()==no) {
    auto* aivar = symbol("C_NPC.AIVAR");
    const auto enemyOverride = uint16_t(symbol("AIV_ENEMYOVERRIDE")->get_int());
    for(const char* name : {"ORKELITE_ANTIPALADINORKOBERST_DI","ORCELITE_DIOBERST1_REST",
                           "ORCELITE_DIOBERST2_REST","ORCELITE_DIOBERST3_REST"}) {
      const auto actor = symbol(name)->get_instance();
      vm.unsafe_set_gi(actor);
      vm.set_int(actor,aivar,enemyOverride,no);
      }

    // Offsets and lengths of the verified stock trigger's literal NPC placements.
    const auto insert = [&](uint32_t offset,uint32_t count) {
      auto pc = trigger.address()+offset;
      for(uint32_t i=0;i<count;++i) {
        const auto spawn = readLiteralPlacement(vm,pc,trigger.name());
        if(spawn.external->name()!="WLD_INSERTNPC")
          throw std::runtime_error("Stock orc attack trigger: expected NPC placement");
        vm.push_int(int32_t(spawn.instance->index()));
        vm.push_reference(spawn.point);
        vm.unsafe_call_external(spawn.external);
        }
      };
    insert(84,4); // On and near the ship.
    if(symbol("TORLOFISCAPTAIN")->get_int()==yes)
      insert(160,1);
    insert(175,2); // Island patrol.

    routine("BIFF_DI","$52137"); // OrkSturmDI
    routine("JACK_DI","$52138");
    routine("TORLOF_DI","$52139");
    routine("MARIO_DI","$52140");
    if(dead("MARIO_DI")==no)
      insert(286,4); // Mario's skeletons.
    else
      insert(351,4); // Undead orcs at the same points.

    symbol("ORKSTURMDI")->set_int(yes);
    vm.push_reference(symbol("TOPIC_HALLENVONIRDORATH"));
    vm.push_reference(symbol("$52149"));
    vm.unsafe_call_bl(symbol("B_LOGENTRY"));
    once->set_int(yes);
    }
  // Re-entering a spent trigger still clears the stock screen message.
  vm.push_reference(symbol("$52150"));
  vm.push_int(-1);
  vm.push_int(-1);
  vm.push_reference(symbol("FONT_SCREEN"));
  vm.push_int(0);
  if(vm.unsafe_call_external(symbol("PRINTSCREEN")))
    (void)vm.pop_int();
  return true;
  }

void runDragonIslandFirstEntry(daedalus::DaedalusVm& vm) {
  struct RestoreContext {
    daedalus::DaedalusVm& vm;
    std::shared_ptr<daedalus::DaedalusInstance> caller;
    ~RestoreContext() { vm.unsafe_set_gi(std::move(caller)); }
    } restore{vm,vm.unsafe_get_gi()};
  const auto symbol = [&](std::string_view name) { return vm.find_symbol_by_name(name); };
  const auto number = [&](const char* name) { return symbol(name)->get_int(); };
  const auto reference = [&](const char* name) { vm.push_reference(symbol(name)); };
  const auto instance = [&](const char* name) { vm.push_int(int32_t(symbol(name)->index())); };
  const auto external = [&](const char* name) { return vm.unsafe_call_external(symbol(name)); };
  const auto playerItems = [&](const char* name) {
    vm.push_reference(vm.global_hero());
    instance(name);
    external("NPC_HASITEMS");
    return vm.pop_int();
    };
  const auto itemAt = [&](const char* name,const char* point) {
    instance(name);
    reference(point);
    external("WLD_INSERTITEM");
    };
  const auto giveItem = [&](const char* actor,const char* name) {
    reference(actor);
    instance(name);
    vm.push_int(1);
    external("CREATEINVITEMS");
    };
  const auto playerField = [&](const char* field,uint16_t index = 0) {
    const auto hero = vm.global_hero()->get_instance();
    vm.unsafe_set_gi(hero);
    return vm.get_int(hero,symbol(field),index);
    };

  constexpr auto shipKey = "ITKE_SHIP_LEVELCHANGE_MIS";
  bool keyResult = false;
  if(playerItems(shipKey)!=0) {
    vm.push_reference(vm.global_hero());
    instance(shipKey);
    vm.push_int(1);
    keyResult = external("NPC_REMOVEINVITEMS");
    }
  if(playerField("C_NPC.ATTRIBUTE",ATR_DEXTERITY)<15)
    itemAt("ITPO_PERM_DEX","$51980"); // FP_ITEM_DI_ENTER_05

  const auto no = number("FALSE");
  auto* entered = symbol("ENTERDI_KAPITEL6");
  if(entered->get_int()==no) {
    if(playerField("C_NPC.GUILD")==number("GIL_PAL"))
      giveItem("ARCHOL","ITRU_PALDESTROYEVIL");
    itemAt("ITMI_FLASK","$51981"); // FP_ITEM_SHIP_12
    if(playerItems("ITMI_INNOSEYE_MIS")==no) {
      if(playerItems("ITMI_INNOSEYE_DISCHARGED_MIS")==no) {
        itemAt("ITSE_XARDASNOTFALLBEUTEL_MIS","$51982"); // FP_ITEM_SHIP_12
        symbol("SC_INNOSEYEVERGESSEN_DI")->set_int(number("TRUE"));
        reference("TOPIC_HALLENVONIRDORATH");
        reference("$51983"); // The stock Xardas dream entry.
        vm.unsafe_call_bl(symbol("B_LOGENTRY"));
        }
      itemAt("ITMI_FLASK","$51984"); // FP_ITEM_SHIP_06
      bool carryingHeart = false;
      for(const auto* heart : {"ITAT_SWAMPDRAGONHEART","ITAT_FIREDRAGONHEART","ITAT_ROCKDRAGONHEART","ITAT_ICEDRAGONHEART"}) {
        if(playerItems(heart)>=1)
          carryingHeart = true;
        }
      if(!carryingHeart)
        giveItem("ORKELITE_ANTIPALADINORKOBERST_DI","ITAT_ROCKDRAGONHEART");
      }

    constexpr auto crewTopic = "TOPIC_MYCREW";
    reference(crewTopic);
    reference("LOG_MISSION");
    external("LOG_CREATETOPIC");
    reference(crewTopic);
    reference("LOG_RUNNING");
    external("LOG_SETTOPICSTATUS");
    const auto journalEntry = [&](const char* message) {
      reference(crewTopic);
      reference(message);
      external("LOG_ADDENTRY");
      };
    static constexpr std::pair<const char*,const char*> captains[] = {
      {"JORGENISCAPTAIN","$51985"}, {"TORLOFISCAPTAIN","$51986"}, {"JACKISCAPTAIN","$51987"},
      };
    for(const auto& [flag,message] : captains)
      if(number(flag)==number("TRUE"))
        journalEntry(message);
    const auto success = number("LOG_SUCCESS");
    static constexpr struct CrewEntry {
      const char* flag;
      const char* message;
      const char* extra = nullptr;
      bool extraForMage = false;
      } crew[] = {
      {"LEE_ISONBOARD","$51988"}, {"MILTENNW_ISONBOARD","$51989","$51990",true},
      {"LESTER_ISONBOARD","$51991"}, {"MARIO_ISONBOARD","$51992"}, {"WOLF_ISONBOARD","$51993"},
      {"VATRAS_ISONBOARD","$51994","$51995",true}, {"BENNET_ISONBOARD","$51996"}, {"DIEGO_ISONBOARD","$51997"},
      {"GORN_ISONBOARD","$51998","$51999"}, {"LARES_ISONBOARD","$52000"}, {"BIFF_ISONBOARD","$52001"},
      {"ANGAR_ISONBOARD","$52002"}, {"GIRION_ISONBOARD","$52003"},
      };
    for(const auto& row : crew) {
      if(number(row.flag)!=success)
        continue;
      journalEntry(row.message);
      if(row.extra && (!row.extraForMage || playerField("C_NPC.GUILD")==number("GIL_KDF")))
        journalEntry(row.extra);
      }
    for(const auto* value : {"KAPWECHSEL_6","KAPWECHSEL_6_TEXT","$52004","$52005"})
      reference(value);
    vm.push_int(6000);
    external("INTRODUCECHAPTER");
    entered->set_int(number("TRUE"));
    }
  // The stock void trigger drops the key-removal result only when it returns.
  if(keyResult)
    (void)vm.pop_int();
  }

PlacementDefinitionRuntime::Definitions dragonIslandPlacement(daedalus::DaedalusScript& vm) {
  using Value = PlacementDefinitions::Value;
  struct Captain {
    std::string_view flag, npc;
    };
  static constexpr Captain captains[] = {
    {"JORGENISCAPTAIN", "VLK_4250_JORGEN_DI"},
    {"TORLOFISCAPTAIN", "SLD_801_TORLOF_DI"},
    {"JACKISCAPTAIN",   "VLK_444_JACK_DI"},
    };
  struct Crew {
    std::string_view flag, npc, partner = {};
    };
  static constexpr Crew crew[] = {
    {"LEE_ISONBOARD",      "SLD_800_LEE_DI"},
    {"MILTENNW_ISONBOARD", "PC_MAGE_DI",        "LESTER_ISONBOARD"},
    {"LESTER_ISONBOARD",   "PC_PSIONIC_DI",     "MILTENNW_ISONBOARD"},
    {"MARIO_ISONBOARD",    "NONE_101_MARIO_DI"},
    {"WOLF_ISONBOARD",     "SLD_811_WOLF_DI"},
    {"VATRAS_ISONBOARD",   "VLK_439_VATRAS_DI"},
    {"BENNET_ISONBOARD",   "SLD_809_BENNET_DI"},
    {"DIEGO_ISONBOARD",    "PC_THIEF_DI",       "LARES_ISONBOARD"},
    {"GORN_ISONBOARD",     "PC_FIGHTER_DI"},
    {"LARES_ISONBOARD",    "VLK_449_LARES_DI",  "DIEGO_ISONBOARD"},
    {"BIFF_ISONBOARD",     "DJG_713_BIFF_DI"},
    {"ANGAR_ISONBOARD",    "DJG_705_ANGAR_DI"},
    {"GIRION_ISONBOARD",   "PAL_207_GIRION_DI"},
    };
  PlacementDefinitions::Library library;
  auto& steps = library.placements["STARTUP_DRAGONISLAND"].steps;
  const auto number = [&](std::string_view symbol) { return vm.find_symbol_by_name(symbol)->get_int(); };
  const auto name = [](std::string_view value) { return Value{.kind=Value::Kind::Symbol,.text=std::string(value)}; };
  const auto string = [](std::string_view value) { return Value{.kind=Value::Kind::String,.text=std::string(value)}; };
  const auto integer = [](int32_t value) { return Value{.integer=value}; };
  const auto call = [&](const char* function, std::initializer_list<Value> args, bool deferred = false) {
    steps.emplace_back(PlacementDefinitions::Call{function,args,deferred});
    };
  const auto aboard = [&](std::string_view npc) { call("WLD_INSERTNPC",{name(npc),string("SHIP_DECK_01")}); };
  // Stock NPC constructors and the sitting routine switches do not write the crew flags.
  const auto success = number("LOG_SUCCESS");
  const auto yes = number("TRUE");
  for(const auto& row : captains)
    if(number(row.flag)==yes)
      aboard(row.npc);
  for(const auto& row : crew) {
    if(number(row.flag)!=success)
      continue;
    aboard(row.npc);
    if(!row.partner.empty() && number(row.partner)!=success)
      call("B_STARTOTHERROUTINE",{name(row.npc),string("SittingShipDI")},true);
    }
  aboard("NOV_600_PEDRO_DI");
  for(const auto* point : {"FP_ITEM_SHIP_03", "FP_ITEM_SHIP_07"})
    call("WLD_INSERTITEM",{name("ITMI_FLASK"),string(point)});
  // These authored sections consist only of literal placement calls. Read their
  // data from the stock package without executing any of their instructions.
  using Op = daedalus::DaedalusOpcode;
  for(const auto* section : {"STARTUP_NEWWORLD_PART_DRAGON_ISLAND_01", "STARTUP_NEWWORLD_PART_DRAGON_UNDEAD_01",
                             "STARTUP_NEWWORLD_PART_DRAGON_FINAL_01"}) {
    auto pc = vm.find_symbol_by_name(section)->address();
    for(;;) {
      if(pc>=vm.size())
        throw std::runtime_error("Dragon Island placement: missing return in "+std::string(section));
      if(vm.instruction_at(pc).op==Op::RSR)
        break;
      const auto placement = readLiteralPlacement(vm,pc,section);
      call(placement.external->name().c_str(),{name(placement.instance->name()),string(placement.point->get_string())});
      }
    }

  call("PLAYVIDEO",{string("SHIP.BIK")});
  const auto topic = string(vm.find_symbol_by_name("TOPIC_HALLENVONIRDORATH")->get_string());
  call("LOG_CREATETOPIC",{topic,integer(number("LOG_MISSION"))});
  call("LOG_SETTOPICSTATUS",{topic,integer(number("LOG_RUNNING"))});
  call("LOG_ADDENTRY",{topic,string(vm.find_symbol_by_name("$53061")->get_string())});
  call("B_KAPITELWECHSEL",{integer(6),integer(number("DRAGONISLAND_ZEN"))},true);
  return PlacementDefinitionRuntime::resolve(library,vm);
  }

void runTeleportStation(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& function) {
  const auto symbol = [&vm](std::string_view name) { return vm.find_symbol_by_name(name); };
  const auto number = [&](std::string_view name) { return symbol(name)->get_int(); };
  const auto set = [&](std::string_view name) { vm.set_int(vm.unsafe_get_gi(),symbol(name),0,number("TRUE")); };
  const auto reference = [&](const char* name) { vm.push_reference(symbol(name)); };
  const auto external = [&](const char* name) { vm.unsafe_call_external(symbol(name)); };
  const auto helper = [&](const char* name) { vm.unsafe_call_bl(symbol(name)); };
  const auto teleport = [&](const char* point) {
    vm.push_reference(vm.global_hero());
    reference(point);
    external("AI_TELEPORT");
    };
  const auto record = [&](const char* topic,const char* entry) {
    reference(topic);
    reference("LOG_MISSION");
    external("LOG_CREATETOPIC");
    reference(topic);
    reference("LOG_RUNNING");
    external("LOG_SETTOPICSTATUS");
    reference(topic);
    reference(entry);
    helper("B_LOGENTRY");
    };
  const auto experience = [&](const char* amount) {
    reference(amount);
    helper("B_GIVEPLAYERXP");
    };

  reference("$17837"); // spellFX_Teleport_RING
  vm.push_reference(vm.global_hero());
  vm.push_reference(vm.global_hero());
  for(int i=0;i<3;++i)
    vm.push_int(0);
  reference("FALSE");
  external("WLD_PLAYEFFECT");
  reference("$17838"); // MFX_TELEPORT_CAST
  external("SND_PLAY");
  vm.push_reference(vm.global_hero());
  external("NPC_CLEARAIQUEUE");
  set("SCUSED_TELEPORTER");

  struct Station {
    const char* source;
    const char* destination;
    const char* visited;
    const char* journalEntry;
    };
  const auto level = number("CURRENTLEVEL");
  if(level==number("NEWWORLD_ZEN")) {
    static constexpr Station stations[] = {
      {"$17839","$17840","SCUSED_NW_TELEPORTSTATION_CITY","$17841"},
      {"$17842","$17843","SCUSED_NW_TELEPORTSTATION_TAVERNE","$17844"},
      {"$17845","$17846","SCUSED_NW_TELEPORTSTATION_MAYA","$17847"},
      };
    bool travelled = false;
    for(const auto& station:stations) {
      vm.push_reference(vm.global_hero());
      reference(station.source);
      external("NPC_GETDISTTOWP");
      if(vm.pop_int()>=3000)
        continue;
      teleport(station.destination);
      if(number(station.visited)==number("FALSE"))
        record("TOPIC_ADDON_TELEPORTSNW",station.journalEntry);
      set(station.visited);
      travelled = true;
      break;
      }
    if(!travelled)
      teleport("$17848"); // MARKT
    bool award = number("SCUSED_ALLNWTELEPORSTSTONES")==number("FALSE");
    for(const auto& station:stations)
      award &= number(station.visited)==number("TRUE");
    if(award) {
      set("SCUSED_ALLNWTELEPORSTSTONES");
      experience("XP_ADDON_ALLNWTELEPORSTSTONES");
      }
    return;
    }
  if(level!=number("ADDONWORLD_ZEN"))
    return;

  static constexpr Station stations[] = {
    {"$17849","$17850","SCUSED_ADW_TELEPORTSTATION_PORTALTEMPEL","$17851"},
    {"$17852","$17853","SCUSED_ADW_TELEPORTSTATION_ADANOSTEMPEL","$17854"},
    {"$17855","$17856","SCUSED_ADW_TELEPORTSTATION_SOUTHEAST","$17857"},
    {"$17858","$17859","SCUSED_ADW_TELEPORTSTATION_SOUTHWEST","$17860"},
    {"$17861","$17862","SCUSED_ADW_TELEPORTSTATION_PIRATES","$17863"},
    {"$17870","$17871","SCUSED_ADW_TELEPORTSTATION_RAVENTELEPORT_OUT",nullptr},
    };
  for(const auto& station:stations) {
    // The stock checks query the nearest waypoint for each row, including misses.
    vm.push_reference(vm.global_hero());
    external("NPC_GETNEARESTWP");
    reference(station.source);
    external("HLP_STRCMP");
    if(!vm.pop_int())
      continue;
    teleport(station.destination);
    if(station.journalEntry && number(station.visited)==number("FALSE")) {
      record("TOPIC_ADDON_TELEPORTSADW",station.journalEntry);
      experience("XP_AMBIENT");
      }
    if(std::string_view(station.visited)=="SCUSED_ADW_TELEPORTSTATION_PIRATES" &&
       number("SCUSED_ADW_TELEPORTSTATION_PIRATES_JACKSMONSTER")==number("FALSE") &&
       number("MIS_KROKOJAGD")==number("LOG_SUCCESS")) {
      // Six literal inserts follow the stock pirate-camp quest guard.
      auto pc = function.address()+1197u;
      for(int i=0;i<6;++i) {
        const auto spawn = readLiteralPlacement(vm,pc,function.name());
        vm.push_int(int32_t(spawn.instance->index()));
        vm.push_reference(spawn.point);
        vm.unsafe_call_external(spawn.external);
        }
      set("SCUSED_ADW_TELEPORTSTATION_PIRATES_JACKSMONSTER");
      }
    set(station.visited);
    return;
    }
  teleport("$17872"); // ADW_ENTRANCE
  }

bool runAddonNpcTrigger(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& function) {
  const auto& name = function.name();
  if(name=="B_FUNCTION_TOUCH_AND_GET_KILLED") {
    // The script passes the SELF symbol ID, even when its instance is unset.
    vm.push_int(int32_t(vm.global_self()->index()));
    vm.unsafe_call_bl(vm.find_symbol_by_name("B_KILLNPC"));
    return true;
    }
  if(name=="EVT_ADDON_ADANOSDOOR_ENTRANCE_VOICESCRIPT") {
    vm.push_reference(vm.global_self());
    vm.push_reference(vm.global_self());
    vm.push_reference(vm.find_symbol_by_name("$52010")); // $ADDON_OPENADANOSTEMPLE
    vm.unsafe_call_bl(vm.find_symbol_by_name("B_SAY"));
    return true;
    }
  if(name=="EVT_TELEPORTSTATION_FUNC") {
    runTeleportStation(vm,function);
    return true;
    }
  const bool entrance = name=="ADW_ADANOSTEMPEL_STONEGRD_TRIGG_FUNC_01";
  const bool rhademes = name=="ADW_ADANOSTEMPEL_STONEGRD_TRIGG_FUNC_02";
  const bool portal = name=="ADW_PORTALTEMPEL_FOCUS_FUNC";
  const bool library = name=="SC_COMESINTO_CANYONLIBRARY_FUNC";
  if(!entrance && !rhademes && !portal && !library)
    return false;

  struct RestoreContext {
    daedalus::DaedalusVm& vm;
    std::shared_ptr<daedalus::DaedalusInstance> caller;
    ~RestoreContext() { vm.unsafe_set_gi(std::move(caller)); }
    } restore{vm,vm.unsafe_get_gi()};
  const auto symbol = [&vm](std::string_view name) { return vm.find_symbol_by_name(name); };
  const auto truth = symbol("TRUE")->get_int();
  const auto falsity = symbol("FALSE")->get_int();
  auto* once = symbol(name+"_ONETIME");
  const auto mark = [&](daedalus::DaedalusSymbol* flag) { vm.set_int(vm.unsafe_get_gi(),flag,0,truth); };
  const auto sound = [&](const char* literal) {
    vm.push_reference(symbol(literal));
    vm.unsafe_call_external(symbol("SND_PLAY"));
    };
  const auto awaken = [&](daedalus::DaedalusSymbol* npc) {
    vm.push_reference(npc);
    vm.unsafe_call_bl(symbol("B_AWAKE_STONEGUARDIAN"));
    };
  const auto alive = [&](daedalus::DaedalusSymbol* npc) {
    vm.push_reference(npc);
    vm.unsafe_call_external(symbol("NPC_ISDEAD"));
    return vm.pop_int()==falsity;
    };

  if(portal) {
    vm.push_reference(vm.global_hero());
    vm.push_int(int32_t(symbol("ITMI_FOCUS")->index()));
    vm.push_int(1);
    const bool inventoryResult = vm.unsafe_call_external(symbol("NPC_REMOVEINVITEMS"));
    auto* count = symbol("TRIGGEREDTELEPORTERADW");
    const auto activated = std::bit_cast<int32_t>(uint32_t(count->get_int())+1u);
    vm.set_int(restore.caller,count,0,activated);
    sound("$17873"); // MFX_TELEKINESIS_STARTINVEST
    if(count->get_int()>=5)
      mark(symbol("SC_ADW_ACTIVATEDALLTELEPORTSTONES"));
    vm.push_reference(vm.global_hero());
    vm.push_reference(symbol("$17874")); // ADW_PORTALTEMPEL_TELEPORTSTATION
    vm.unsafe_call_external(symbol("NPC_GETDISTTOWP"));
    if(vm.pop_int()<3000 && once->get_int()==falsity) {
      auto* statue = symbol("STONEGUARDIAN_NAILEDPORTALADW1");
      const auto actor = statue->get_instance();
      vm.unsafe_set_gi(actor);
      const bool sleeping = vm.get_int(actor,symbol("C_NPC.AIVAR"),61)==truth;
      const bool stillAlive = alive(statue);
      if(sleeping && stillAlive)
        sound("$17875"); // THRILLJINGLE_02
      awaken(statue);
      mark(once);
      }
    if(inventoryResult)
      (void)vm.pop_int(); // The original void trigger discards the removal result on return.
    return true;
    }

  if(once->get_int()!=falsity)
    return true;
  if(entrance) {
    static constexpr const char* statues[] = {"STONEGUARDIAN_ADANOSTEMPELENTRANCE_01","STONEGUARDIAN_ADANOSTEMPELENTRANCE_02",
                                               "STONEGUARDIAN_ADANOSTEMPELENTRANCE_03","STONEGUARDIAN_ADANOSTEMPELENTRANCE_04"};
    for(const auto* statue:statues)
      awaken(symbol(statue));
    bool anyoneAlive = false;
    for(auto i=std::rbegin(statues);i!=std::rend(statues);++i)
      anyoneAlive |= alive(symbol(*i));
    if(anyoneAlive)
      sound("$52007"); // THRILLJINGLE_02
    for(const auto* victim:{"BDT_10400_ADDON_DEADBANDIT","BDT_10401_ADDON_DEADBANDIT"}) {
      vm.push_int(int32_t(symbol(victim)->index()));
      vm.unsafe_call_bl(symbol("B_KILLNPC"));
      }
    }
  else if(rhademes) {
    for(const auto* statue:{"STONEGUARDIAN_RHADEMES_14A","STONEGUARDIAN_RHADEMES_14D","STONEGUARDIAN_RHADEMES_14F"})
      awaken(symbol(statue));
    mark(symbol("SC_TOOKRHADEMESTRAP"));
    }
  else {
    auto* beast = symbol("SHADOWBEAST_ADDON_FIRE_CANYONLIB");
    if(alive(beast)) {
      vm.push_reference(beast);
      vm.unsafe_call_external(symbol("AI_STANDUP"));
      const auto actor = beast->get_instance();
      vm.unsafe_set_gi(actor);
      vm.set_int(actor,symbol("C_NPC.AIVAR"),61,falsity);
      sound("$52190"); // THRILLJINGLE_03
      sound("$52191"); // GOL_Ambient_A2
      }
    }
  mark(once);
  return true;
  }

}

void GameScript::invokeWorldTrigger(std::string_view fn) {
  const auto* symbol = findSymbol(fn);
  if(symbol!=nullptr && nativeRoutineProgram && PlacementDefinitionRuntime::enabled()) {
    if(runKhorinisNpcTrigger(vm,*symbol))
      return;
    if(symbol->name()=="ENTER_OLDWORLD_FIRSTTIME_TRIGGER" || symbol->name()=="ENTER_ADDONWORLD_FIRSTTIME_TRIGGER_FUNC") {
      vm.unsafe_call_bl(symbol);
      return;
      }
    if(symbol->name()=="ENTER_DI_FIRSTTIME_TRIGGER") {
      runDragonIslandFirstEntry(vm);
      return;
      }
    if(runDragonIslandCrewTrigger(vm,*symbol))
      return;
    struct Room {
      std::string_view trigger;
      uint32_t         spawnCount;
      };
    static constexpr Room rooms[] = {{"EVT_CRYPT_01",7}, {"EVT_CRYPT_02",7},
                                     {"EVT_CRYPT_03",7}, {"EVT_CRYPT_FINAL",6}};
    const auto room = std::ranges::find(rooms,symbol->name(),&Room::trigger);
    if(room!=std::end(rooms)) {
      auto* once = findSymbol(symbol->name()+"_ONETIME");
      if(vm.get_int(nullptr,once,0)==findSymbol("FALSE")->get_int()) {
        // The stock comparison and conditional jump precede the literal spawn calls.
        insertLiteralNpcs(vm,*symbol,16,room->spawnCount);
        vm.set_int(nullptr,once,0,findSymbol("TRUE")->get_int());
        }
      return;
      }
    if(runIslandSpawnTrigger(vm,*symbol) || runAddonNpcTrigger(vm,*symbol))
      return;
    }
  vm.call_function(symbol);
  }

void GameScript::runWorldFirstEntryTrigger(bool oldWorld) {
  using Op = daedalus::DaedalusOpcode;
  using Type = daedalus::DaedalusDataType;
  auto* once = findSymbol(oldWorld ? "ENTER_OLDWORLD_FIRSTTIME_TRIGGER_ONETIME" :
                                     "ENTER_ADDONWORLD_FIRSTTIME_TRIGGER_ONETIME");
  const auto* yes = findSymbol("TRUE");
  if(vm.get_int(nullptr,once,0)==findSymbol("FALSE")->get_int()) {
    const auto* trigger = findSymbol(oldWorld ? "ENTER_OLDWORLD_FIRSTTIME_TRIGGER" :
                                               "ENTER_ADDONWORLD_FIRSTTIME_TRIGGER_FUNC");
    const auto kill = findSymbol("B_KILLNPC")->address();
    auto pc = trigger->address()+16u; // The guard precedes the fixed literal calls.
    for(uint32_t i=0;i<(oldWorld ? 18u : 19u);++i) {
      const auto instance = vm.instruction_at(pc);
      const auto call = vm.instruction_at(pc+instance.size);
      const auto* actor = instance.op==Op::PUSHI ? findSymbol(uint32_t(instance.immediate)) : nullptr;
      if(!actor || actor->type()!=Type::INSTANCE || call.op!=Op::BL || call.address!=kill)
        throw std::runtime_error("Unexpected stock first-entry actor in "+trigger->name());
      invokeKillNpc(instance.immediate);
      pc+=instance.size+call.size;
      }
    if(oldWorld) {
      vm.push_reference(findSymbol("$52006")); // DRAGONATTACK.BIK.
      vm.unsafe_call_external(findSymbol("PLAYVIDEO")); // The void trigger discards the result on return.
      }
    vm.set_int(nullptr,once,0,yes->get_int());
    }
  if(oldWorld && findSymbol("TSCHUESSBILGOT")->get_int()==yes->get_int()) {
    vm.push_int(int32_t(findSymbolIndex("BILGOT")));
    vm.unsafe_call_bl(findSymbol("B_REMOVENPC"));
    }
  }

void GameScript::runStartup(std::string_view fn) {
  const auto* function = vm.find_symbol_by_name(fn);
  if(nativeRoutineProgram && PlacementDefinitionRuntime::enabled() && function && function->name()=="STARTUP_DRAGONISLAND") {
    PlacementDefinitionRuntime::execute(vm,function,dragonIslandPlacement(vm));
    return;
    }
  PlacementDefinitionRuntime::execute(vm,function,placementDefinitions);
  }

void GameScript::runWorldInit(std::string_view fn) {
  const auto* function = vm.find_symbol_by_name(fn);
  const bool native = nativeRoutineProgram && PlacementDefinitionRuntime::enabled() && function;
  if(native && function->name()=="INIT_GLOBAL") {
    vm.unsafe_call_bl(function);
    return;
    }
  if(native && (function->name()=="INIT_NEWWORLD" || function->name()=="INIT_OLDWORLD")) {
    const bool oldWorld = function->name()=="INIT_OLDWORLD";
    if(oldWorld) {
      for(auto name : {"INIT_SUB_OLDCAMP","INIT_SUB_DEMONTOWER","INIT_SUB_SURFACE"})
        vm.unsafe_call_bl(findSymbol(name));
      }
    for(auto name : {"B_INITMONSTERATTITUDES","B_INITGUILDATTITUDES","B_INITNPCGLOBALS"})
      vm.unsafe_call_bl(findSymbol(name));
    vm.unsafe_call_bl(findSymbol(oldWorld ? "B_ENTER_OLDWORLD" : "B_ENTER_NEWWORLD"));

    auto* yes = findSymbol("TRUE");
    auto* no = findSymbol("FALSE");
    auto* world = findSymbol(oldWorld ? "OLDWORLD_ZEN" : "NEWWORLD_ZEN");
    const auto advance = [&](int32_t chapter,std::string_view latchName,std::string_view readyName) {
      auto* latch = findSymbol(latchName);
      const bool pending = latch->get_int()==no->get_int();
      const bool ready = findSymbol(readyName)->get_int()==yes->get_int();
      if(pending && ready) {
        vm.push_int(chapter);
        vm.push_reference(world);
        vm.unsafe_call_bl(findSymbol("B_KAPITELWECHSEL"));
        latch->set_int(yes->get_int());
        }
      };
    if(oldWorld) {
      advance(4,"B_CHAPTER4_ONETIME","MIS_READYFORCHAPTER4");
      return;
      }

    auto* thievesCleared = findSymbol("ANDRE_DIEBESGILDE_AUFGERAEUMT");
    const bool pending = thievesCleared->get_int()!=yes->get_int();
    const bool betrayed = findSymbol("DIEBESGILDE_VERRATEN")->get_int()!=0;
    if(pending && betrayed) {
      const auto alive = [&](std::string_view alias) {
        vm.push_reference(findSymbol(alias));
        vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
        return vm.pop_int()==0;
        };
      // The original checks all three aliases before deciding to clear the guild.
      const bool ramirez = alive("RAMIREZ");
      const bool jesper = alive("JESPER");
      const bool cassia = alive("CASSIA");
      if(ramirez || jesper || cassia) {
        for(auto name : {"VLK_447_CASSIA","VLK_446_JESPER","VLK_445_RAMIREZ"})
          invokeKillNpc(int32_t(findSymbolIndex(name)));
        thievesCleared->set_int(yes->get_int());
        }
      }
    for(auto name : {"INIT_SUB_NEWWORLD_PART_CITY_01","INIT_SUB_NEWWORLD_PART_FARM_01",
                     "INIT_SUB_NEWWORLD_PART_XARDAS_01","INIT_SUB_NEWWORLD_PART_MONASTERY_01",
                     "INIT_SUB_NEWWORLD_PART_GREATPEASANT_01","INIT_SUB_NEWWORLD_PART_TROLLAREA_01",
                     "INIT_SUB_NEWWORLD_PART_FOREST_01","INIT_SUB_NEWWORLD_PART_PASS_TO_OW_01"})
      vm.unsafe_call_bl(findSymbol(name));
    advance(3,"B_CHAPTER3_ONETIME","MIS_READYFORCHAPTER3");
    advance(5,"B_CHAPTER5_ONETIME","MIS_ALLDRAGONSDEAD");
    return;
    }
  if(native && function->name()=="INIT_ADDONWORLD") {
    for(const auto* stage:{"INIT_SUB_ADDON_PART_BANDITSCAMP_01", "INIT_SUB_ADDON_PART_PIRATESCAMP_01",
                           "INIT_SUB_ADDON_PART_ENTRANCE_01", "INIT_SUB_ADDON_PART_GOLDMINE_01",
                           "INIT_SUB_ADDON_PART_CANYON_01", "INIT_SUB_ADDON_PART_VALLEY_01",
                           "INIT_SUB_ADDON_PART_ADANOSTEMPLE_01", "B_INITMONSTERATTITUDES",
                           "B_INITGUILDATTITUDES", "B_INITNPCGLOBALS", "B_ENTER_ADDONWORLD"})
      vm.unsafe_call_bl(findSymbol(stage));
    return;
    }
  if(!native || function->name()!="INIT_DRAGONISLAND") {
    vm.call_function(function);
    return;
    }

  const auto* npcGlobals = findSymbol("B_INITNPCGLOBALS");
  vm.unsafe_call_bl(findSymbol("B_INITMONSTERATTITUDES"));
  vm.unsafe_call_bl(findSymbol("B_INITGUILDATTITUDES"));
  vm.unsafe_call_bl(npcGlobals);
  currentStoryWorld=findSymbol("DRAGONISLAND_ZEN")->get_int();

  const auto key = int32_t(findSymbolIndex("ITKE_SHIP_LEVELCHANGE_MIS"));
  vm.push_reference(vm.global_hero());
  vm.push_int(key);
  vm.unsafe_call_external(findSymbol("NPC_HASITEMS"));
  const bool hasKey = vm.pop_int()!=0;
  if(hasKey) {
    vm.push_reference(vm.global_hero());
    vm.push_int(key);
    vm.push_int(1);
    vm.unsafe_call_external(findSymbol("NPC_REMOVEINVITEMS"));
    }
  vm.unsafe_call_bl(npcGlobals);
  // The stock void root discards the removal result on return.
  if(hasKey)
    (void)vm.pop_int();
  }

void GameScript::loadNewWorldChapterEntries() {
  const auto place=[this](std::string_view external,std::string_view instance,std::string_view point) {
    vm.push_int(int32_t(findSymbolIndex(instance)));
    vm.push_reference(findSymbol(point));
    vm.unsafe_call_external(findSymbol(external));
    };
  const auto remove=[this](std::string_view instance) {
    vm.push_int(int32_t(findSymbolIndex(instance)));
    vm.unsafe_call_bl(findSymbol("B_REMOVENPC"));
    };
  const auto number=[this](std::string_view name) { return findSymbol(name)->get_int(); };
  vm.override_function("B_ENTER_NEWWORLD_KAPITEL_1",std::function<void()>{[this,place,remove,number] {
    constexpr std::string_view myxir="KDW_140300_ADDON_MYXIR_CITY";
    vm.push_reference(findSymbol(myxir));
    vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
    const bool myxirDead=vm.pop_int()!=0;
    const bool ravenDead=number("RAVENISDEAD")==number("TRUE");
    if(myxirDead && ravenDead)
      place("WLD_INSERTNPC",myxir,"$28058"); // CITY1

    if(number("ENTERED_ADDONWORLD")==number("TRUE")) {
      for(auto instance:{"KDW_1400_ADDON_SATURAS_NW","KDW_1401_ADDON_CRONOS_NW","KDW_1402_ADDON_NEFARIUS_NW",
                         "KDW_1403_ADDON_MYXIR_NW","KDW_1404_ADDON_RIORDIAN_NW","KDW_1405_ADDON_MERDARION_NW",
                         "PIR_1301_ADDON_SKIP_NW"})
        remove(instance);
      auto* portalKey=findSymbol("SC_GOTPORTALTEMPELWALKTHROUGHKEY");
      if(portalKey->get_int()==number("FALSE")) {
        place("WLD_INSERTNPC","STONEGUARDIAN_MERDARIONSSCHLUESSEL","$28059");
        portalKey->set_int(number("TRUE"));
        }
      }

    auto* escaped=findSymbol("SKLAVEN_WEG");
    const bool alreadyReturned=escaped->get_int()!=number("FALSE");
    const bool haveEscaped=number("SKLAVEN_FLUCHT")==number("TRUE");
    if(!alreadyReturned && haveEscaped) {
      static constexpr std::pair<std::string_view,std::string_view> homes[]={
        {"STRF_1123_ADDON_PATRICK_NW","$28060"},
        {"STRF_1124_ADDON_MONTY_NW",  "$28061"},
        {"STRF_1125_ADDON_TONAK_NW",  "$28062"},
        {"STRF_1126_ADDON_TELBOR_NW", "$28063"},
        {"STRF_1127_ADDON_PARDOS_NW", "$28064"},
        };
      for(const auto& [instance,waypoint]:homes)
        place("WLD_INSERTNPC",instance,waypoint);
      escaped->set_int(number("TRUE"));
      findSymbol("MISSINGPEOPLERETURNEDHOME")->set_int(number("TRUE"));
      }
    }},PlacementDefinitionRuntime::enabled);

  constexpr auto chapterTwo="B_ENTER_NEWWORLD_KAPITEL_2";
  // The verified callback starts its block of 50 literal NPC rows after the two tests.
  auto pc=findSymbol(chapterTwo)->address()+42;
  std::array<LiteralPlacement,50> population{};
  for(auto& row:population) {
    row=readLiteralPlacement(vm,pc,chapterTwo);
    if(row.external->name()!="WLD_INSERTNPC")
      throw std::runtime_error("Unexpected NPC placement in "+std::string(chapterTwo));
    }
  vm.override_function(chapterTwo,std::function<void()>{[this,place,remove,number,population] {
    if(number("GREGISBACK")==number("TRUE"))
      remove("PIR_1300_ADDON_GREG_NW");
    auto* entered=findSymbol("ENTERNW_KAPITEL2");
    if(entered->get_int()!=number("FALSE"))
      return;
    for(const auto& row:population) {
      vm.push_int(int32_t(row.instance->index()));
      vm.push_reference(row.point);
      vm.unsafe_call_external(row.external);
      }

    const auto heroGuild=[this] {
      const auto hero=vm.global_hero()->get_instance();
      vm.unsafe_set_gi(hero);
      return vm.get_int(hero,findSymbol("C_NPC.GUILD"),0);
      };
    const auto guildMatches=[&](std::string_view first,std::string_view second) {
      const bool matchesFirst=heroGuild()==number(first);
      const bool matchesSecond=heroGuild()==number(second);
      return matchesFirst || matchesSecond;
      };
    if(guildMatches("GIL_PAL","GIL_MIL"))
      place("WLD_INSERTITEM","ITAM_DEX_01","$28115");
    else if(guildMatches("GIL_DJG","GIL_SLD"))
      place("WLD_INSERTITEM","ITAM_HP_01","$28116");
    else
      place("WLD_INSERTITEM","ITAM_HP_MANA_01","$28117");
    if(heroGuild()==number("GIL_KDF")) {
      vm.push_reference(findSymbol("AGON"));
      vm.push_reference(findSymbol("$28118")); // StillAlive
      vm.unsafe_call_bl(findSymbol("B_STARTOTHERROUTINE"));
      }
    entered->set_int(number("TRUE"));
    }},PlacementDefinitionRuntime::enabled);
  }

void GameScript::loadNativeWorldEntry() {
  if(!nativeRoutineProgram)
    return;

  findSymbol("CURRENTLEVEL")->bind_global_int(currentStoryWorld);
  vm.override_function("ENTER_OLDWORLD_FIRSTTIME_TRIGGER",std::function<void()>{[this] {
    runWorldFirstEntryTrigger(true);
    }},PlacementDefinitionRuntime::enabled);
  vm.override_function("ENTER_ADDONWORLD_FIRSTTIME_TRIGGER_FUNC",std::function<void()>{[this] {
    runWorldFirstEntryTrigger(false);
    }},PlacementDefinitionRuntime::enabled);
  static constexpr std::pair<std::string_view,int32_t GhostProgress::*> ghostFlags[]={
    {"GHOST_SCKNOWSHOW2GETINADANOSTEMPEL",&GhostProgress::knowsTempleEntrance},
    {"SC_SUMMONEDANCIENTGHOST",&GhostProgress::summonedAncientGhost},
    {"B_REMOVEQUARHODRON_ONETIME",&GhostProgress::removedQuarhodron},
    {"ADW_ADANOSTEMPEL_STONEGRD_TRIGG_FUNC_01_ONETIME",&GhostProgress::stoneGuardianTrigger},
    {"SC_TOOKRHADEMESTRAP",&GhostProgress::tookRhademesTrap},
    {"SC_TALKEDTORHADEMAFTER",&GhostProgress::talkedToRhademesAfter},
    {"B_REMOVERHADEMES_ONETIME",&GhostProgress::removedRhademes},
    {"GHOSTATTACKWARN",&GhostProgress::attackWarn},
    };
  for(const auto& [name,field]:ghostFlags)
    findSymbol(name)->bind_global_int(ghostProgress.*field);

  loadKhorinisChapter3();
  static constexpr std::pair<const char*,ChapterWorld> worlds[]={
    {"B_ENTER_NEWWORLD",ChapterWorld::New}, {"B_ENTER_OLDWORLD",ChapterWorld::Old}, {"B_ENTER_ADDONWORLD",ChapterWorld::Addon},
    };
  for(const auto& [name,region]:worlds)
    vm.override_function(name,std::function<void()>{[this,region] { enterChapterWorld(region); }},
                         PlacementDefinitionRuntime::enabled);

  loadNewWorldChapterEntries();

  static constexpr std::pair<const char*,const char*> latches[]={
    {"B_ENTER_NEWWORLD_KAPITEL_6","ENTERNW_KAPITEL6"},
    {"B_ENTER_OLDWORLD_KAPITEL_1","ENTEROW_KAPITEL1"},
    {"B_ENTER_OLDWORLD_KAPITEL_2","ENTEROW_KAPITEL2"},
    {"B_ENTER_OLDWORLD_KAPITEL_6","ENTEROW_KAPITEL6"},
    {"B_ENTER_ADDONWORLD_KAPITEL_2","ENTERADW_KAPITEL2"},
    {"B_ENTER_ADDONWORLD_KAPITEL_3","ENTERADW_KAPITEL3"},
    {"B_ENTER_ADDONWORLD_KAPITEL_5","ENTERADW_KAPITEL5"},
    {"B_ENTER_ADDONWORLD_KAPITEL_6","ENTERADW_KAPITEL6"},
    };
  static_assert(std::size(latches)==std::tuple_size_v<decltype(worldEntryLatches)>);
  for(size_t i=0;i<std::size(latches);++i) {
    const auto& [name,flag]=latches[i];
    findSymbol(flag)->bind_global_int(worldEntryLatches[i]);
    vm.override_function(name,std::function<void()>{[this,i] {
      if(worldEntryLatches[i]==findSymbol("FALSE")->get_int())
        worldEntryLatches[i]=findSymbol("TRUE")->get_int();
      }},PlacementDefinitionRuntime::enabled);
    }
  for(const auto* stage:{"B_INITGUILDATTITUDES","INIT_SUB_DEMONTOWER","INIT_SUB_NEWWORLD_PART_XARDAS_01",
                         "INIT_SUB_NEWWORLD_PART_TROLLAREA_01","INIT_SUB_NEWWORLD_PART_FOREST_01",
                         "INIT_SUB_NEWWORLD_PART_PASS_TO_OW_01",
                         "INIT_SUB_ADDON_PART_PIRATESCAMP_01", "INIT_SUB_ADDON_PART_ENTRANCE_01",
                         "INIT_SUB_ADDON_PART_GOLDMINE_01", "INIT_SUB_ADDON_PART_CANYON_01",
                         "INIT_SUB_ADDON_PART_VALLEY_01", "INIT_SUB_ADDON_PART_ADANOSTEMPLE_01"})
    vm.override_function(stage,std::function<void()>{[] {}},PlacementDefinitionRuntime::enabled);

  using Op = daedalus::DaedalusOpcode;
  using Type = daedalus::DaedalusDataType;
  using Symbol = daedalus::DaedalusSymbol;
  vm.override_function("INIT_GLOBAL",std::function<void()>{[this] {
    // The void script frame discards GAME_INITGERMAN's integer result.
    vm.unsafe_call_external(findSymbol("GAME_INITGERMAN"));
    }},PlacementDefinitionRuntime::enabled);

  auto* yes = findSymbol("TRUE");
  auto* no = findSymbol("FALSE");
  auto* enteredAddon4 = findSymbol("ENTERADW_KAPITEL4");
  const auto* insertNpc = findSymbol("WLD_INSERTNPC");
  auto pc = findSymbol("B_ENTER_ADDONWORLD_KAPITEL_4")->address();
  const auto takeAddon4 = [&] {
    const auto instruction = vm.instruction_at(pc);
    pc += instruction.size;
    return instruction;
    };
  const auto expectAddon4 = [](bool match) {
    if(!match)
      throw std::runtime_error("Unexpected Addon World chapter 4 placement");
    };
  const auto compareAddon4 = takeAddon4();
  const auto latchAddon4 = takeAddon4();
  expectAddon4(compareAddon4.op==Op::PUSHV && compareAddon4.symbol==no->index() &&
               latchAddon4.op==Op::PUSHV && latchAddon4.symbol==enteredAddon4->index() &&
               takeAddon4().op==Op::EQ);
  const auto skipAddon4 = takeAddon4();
  expectAddon4(skipAddon4.op==Op::BZ);

  struct NpcPlacement {
    int32_t instance;
    Symbol* point;
    };
  std::array<NpcPlacement,137> addon4Spawns;
  for(auto& spawn : addon4Spawns) {
    const auto instance = takeAddon4();
    const auto point = takeAddon4();
    const auto call = takeAddon4();
    const auto* actor = instance.op==Op::PUSHI ? findSymbol(uint32_t(instance.immediate)) : nullptr;
    auto* waypoint = point.op==Op::PUSHV ? findSymbol(point.symbol) : nullptr;
    expectAddon4(actor && actor->type()==Type::INSTANCE && actor->is_const() &&
                 waypoint && waypoint->type()==Type::STRING && waypoint->is_const() &&
                 !waypoint->is_member() && waypoint->count()==1 &&
                 call.op==Op::BE && call.symbol==insertNpc->index());
    spawn = {instance.immediate,waypoint};
    }
  const auto valueAddon4 = takeAddon4();
  const auto updateAddon4 = takeAddon4();
  expectAddon4(valueAddon4.op==Op::PUSHV && valueAddon4.symbol==yes->index() &&
               updateAddon4.op==Op::PUSHV && updateAddon4.symbol==enteredAddon4->index() &&
               takeAddon4().op==Op::MOVI && pc==skipAddon4.address && takeAddon4().op==Op::RSR);
  vm.override_function("B_ENTER_ADDONWORLD_KAPITEL_4",std::function<void()>{[this,yes,no,enteredAddon4,insertNpc,addon4Spawns] {
    if(enteredAddon4->get_int()!=no->get_int())
      return;
    for(const auto& spawn : addon4Spawns) {
      vm.push_int(spawn.instance);
      vm.push_reference(spawn.point);
      vm.unsafe_call_external(insertNpc);
      }
    enteredAddon4->set_int(yes->get_int());
    }},PlacementDefinitionRuntime::enabled);

  const auto removeActor = [this](std::string_view actor) {
    vm.push_int(int32_t(findSymbolIndex(actor)));
    vm.unsafe_call_bl(findSymbol("B_REMOVENPC"));
    };
  vm.override_function("B_ENTER_ADDONWORLD_KAPITEL_1",std::function<void()>{[this,yes,no,removeActor] {
    if(findSymbol("RAVENISDEAD")->get_int()==yes->get_int())
      removeActor("KDW_14030_ADDON_MYXIR_ADW");
    auto* slavesRemoved = findSymbol("REMOVESKLAVEN");
    if(slavesRemoved->get_int()==no->get_int() && findSymbol("SKLAVEN_WEG")->get_int()==yes->get_int()) {
      for(const auto* actor : {"STRF_1118_ADDON_PATRICK","STRF_1119_ADDON_MONTY","STRF_1120_ADDON_TONAK",
                              "STRF_1121_ADDON_TELBOR","STRF_1122_ADDON_PARDOS"})
        removeActor(actor);
      slavesRemoved->set_int(yes->get_int());
      }
    }},PlacementDefinitionRuntime::enabled);

  vm.override_function("B_ENTER_OLDWORLD_KAPITEL_5",std::function<void()>{[this,yes,no,removeActor] {
    auto* entered = findSymbol("ENTEROW_KAPITEL5");
    if(entered->get_int()==no->get_int()) {
      const auto grant = [this](std::string_view actor,std::string_view item) {
        vm.push_reference(findSymbol(actor));
        vm.push_int(int32_t(findSymbolIndex(item)));
        vm.push_int(1);
        vm.unsafe_call_external(findSymbol("CREATEINVITEMS"));
        };
      auto* guard = findSymbol("VLK_4143_HAUPTTORWACHE");
      const auto actor = guard->get_instance();
      vm.unsafe_set_gi(actor);
      vm.set_int(actor,findSymbol("C_NPC.FLAGS"),0,0);
      grant(guard->name(),"ITKE_OC_MAINGATE_MIS");

      vm.push_reference(findSymbol("BRUTUS"));
      vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
      if(vm.pop_int()==no->get_int())
        grant("VLK_4100_BRUTUS","ITWR_DEMENTOROBSESSIONBOOK_MIS");
      if(findSymbol("TSCHUESSBILGOT")->get_int()==yes->get_int())
        removeActor("VLK_4120_BILGOT");
      entered->set_int(yes->get_int());
      }
    if(findSymbol("BIFF_FOLLOWSTHROUGHPASS")->get_int()==findSymbol("LOG_SUCCESS")->get_int())
      removeActor("DJG_713_BIFF");
    }},PlacementDefinitionRuntime::enabled);

  struct Room {
    Symbol* label;
    Symbol* guild;
    };
  struct RoomChoice {
    Symbol* expected;
    Symbol* flag;
    Room equal, otherwise;
    };
  struct MobRoutine {
    int32_t hour, minute;
    Symbol* name;
    int32_t state;
    };
  const auto* assign = findSymbol("WLD_ASSIGNROOMTOGUILD");
  const auto* setMobRoutine = findSymbol("WLD_SETMOBROUTINE");
  const auto assignRoom = [this,assign](const Room& room) {
    vm.push_reference(room.label);
    if(room.guild)
      vm.push_reference(room.guild);
    else
      vm.push_int(-1);
    vm.unsafe_call_external(assign);
    };
  static constexpr struct Region {
    const char* name;
    size_t roomCount, choiceCount = 0, mobCount = 0;
    } roomSections[] = {
    {"INIT_SUB_ADDON_PART_BANDITSCAMP_01",6}, {"INIT_SUB_NEWWORLD_PART_MONASTERY_01",13}, {"INIT_SUB_OLDCAMP",25},
    {"INIT_SUB_NEWWORLD_PART_CITY_01",44,4,3}, {"INIT_SUB_NEWWORLD_PART_GREATPEASANT_01",13,2},
    };
  for(const auto& region : roomSections) {
    auto pc = findSymbol(region.name)->address();
    const auto malformed = [&] { throw std::runtime_error("Unexpected room initializer in "+std::string(region.name)); };
    const auto take = [&] {
      const auto op = vm.instruction_at(pc);
      pc += op.size;
      return op;
      };
    const auto reference = [&](const daedalus::DaedalusInstruction& op) {
      return op.op==Op::PUSHV ? findSymbol(op.symbol) : nullptr;
      };
    const auto constant = [](const Symbol* symbol,Type type) {
      return symbol && symbol->type()==type && symbol->is_const() && !symbol->is_member() && symbol->count()==1;
      };
    const auto readRoom = [&](Symbol* room) {
      const auto owner = take();
      auto* guild = reference(owner);
      if(!constant(room,Type::STRING))
        malformed();
      if(guild) {
        if(!constant(guild,Type::INT))
          malformed();
        } else if(owner.op!=Op::PUSHI || owner.immediate!=1 || take().op!=Op::NEGATE) {
        malformed();
        }
      const auto call = take();
      if(call.op!=Op::BE || call.symbol!=assign->index())
        malformed();
      return Room{room,guild};
      };

    std::vector<MobRoutine> mobs;
    for(size_t i=0;i<region.mobCount;++i) {
      const auto hour = take();
      const auto minute = take();
      auto* name = reference(take());
      const auto state = take();
      const auto call = take();
      if(hour.op!=Op::PUSHI || minute.op!=Op::PUSHI || !constant(name,Type::STRING) ||
         state.op!=Op::PUSHI || call.op!=Op::BE || call.symbol!=setMobRoutine->index())
        malformed();
      mobs.push_back({hour.immediate,minute.immediate,name,state.immediate});
      }
    std::vector<std::variant<Room,RoomChoice>> rooms;
    rooms.reserve(region.roomCount);
    size_t choices = 0;
    for(size_t i=0;i<region.roomCount;++i) {
      auto* first = reference(take());
      if(constant(first,Type::STRING)) {
        rooms.emplace_back(readRoom(first));
        continue;
        }

      auto* flag = reference(take());
      if(!constant(first,Type::INT) || !flag || flag->type()!=Type::INT || flag->is_member() ||
         flag->is_const() || flag->count()!=1 || take().op!=Op::EQ)
        malformed();
      const auto branch = take();
      const auto equal = readRoom(reference(take()));
      const auto skip = take();
      if(branch.op!=Op::BZ || skip.op!=Op::B || pc!=branch.address)
        malformed();
      const auto otherwise = readRoom(reference(take()));
      if(pc!=skip.address)
        malformed();
      rooms.emplace_back(RoomChoice{first,flag,equal,otherwise});
      ++choices;
      }
    if(choices!=region.choiceCount || take().op!=Op::RSR)
      malformed();
    vm.override_function(region.name,std::function<void()>{[this,assignRoom,setMobRoutine,mobs=std::move(mobs),rooms=std::move(rooms)] {
      for(const auto& mob : mobs) {
        vm.push_int(mob.hour);
        vm.push_int(mob.minute);
        vm.push_reference(mob.name);
        vm.push_int(mob.state);
        vm.unsafe_call_external(setMobRoutine);
        }
      for(const auto& room : rooms) {
        if(const auto* fixed = std::get_if<Room>(&room)) {
          assignRoom(*fixed);
          } else {
          const auto& choice = std::get<RoomChoice>(room);
          assignRoom(choice.flag->get_int()==choice.expected->get_int() ? choice.equal : choice.otherwise);
          }
        }
      }},PlacementDefinitionRuntime::enabled);
    }
  vm.override_function("INIT_SUB_NEWWORLD_PART_FARM_01",std::function<void()>{[this,assignRoom] {
    auto* publicRoom = findSymbol("GIL_PUBLIC");
    assignRoom({findSymbol("$54786"),publicRoom});
    assignRoom({findSymbol("$54787"),publicRoom});
    if(findSymbol("LOBART_KLEIDUNG_VERKAUFT")->get_int()==findSymbol("TRUE")->get_int())
      assignRoom({findSymbol("$54788"),findSymbol("GIL_NONE")});
    else
      assignRoom({findSymbol("$54789"),publicRoom});
    }},PlacementDefinitionRuntime::enabled);
  vm.override_function("INIT_SUB_SURFACE",std::function<void()>{[this,assignRoom] {
    auto* seekers = findSymbol("GIL_DMT");
    assignRoom({findSymbol("$54426"),seekers});
    assignRoom({findSymbol("$54427"),seekers});
    vm.unsafe_call_bl(findSymbol("B_ENTER_OLDWORLD"));
    }},PlacementDefinitionRuntime::enabled);

  loadKhorinisLateChapters(vm);
  vm.override_function("B_KAPITELWECHSEL",std::function<void(int32_t,int32_t)>{
    [this](int32_t chapter,int32_t level) { changeChapter(chapter,level); }},PlacementDefinitionRuntime::enabled);
  loadOldWorldChapterEntries();
  }

void GameScript::loadOldWorldChapterEntries() {
  constexpr std::string_view old3 = "B_ENTER_OLDWORLD_KAPITEL_3";
  constexpr std::string_view old4 = "B_ENTER_OLDWORLD_KAPITEL_4";
  const auto batch = [this](std::string_view root,uint32_t offset,size_t count) {
    std::vector<LiteralPlacement> rows;
    rows.reserve(count);
    auto pc = findSymbol(root)->address()+offset;
    for(size_t i=0;i<count;++i)
      rows.push_back(readLiteralPlacement(vm,pc,root));
    return rows;
    };
  const auto place = [this](const LiteralPlacement& row) {
    vm.push_int(int32_t(row.instance->index()));
    vm.push_reference(row.point);
    vm.unsafe_call_external(row.external);
    };
  const auto remove = [this](std::string_view npc) {
    vm.push_int(int32_t(findSymbolIndex(npc)));
    vm.unsafe_call_bl(findSymbol("B_REMOVENPC"));
    };
  const auto isMage = [this] {
    const auto hero = vm.global_hero()->get_instance();
    vm.unsafe_set_gi(hero);
    return vm.get_int(hero,findSymbol("C_NPC.GUILD"),0)==findSymbol("GIL_KDF")->get_int();
    };
  const auto push = [this](std::string_view name) { vm.push_reference(findSymbol(name)); };
  const auto external = [this](std::string_view name) { vm.unsafe_call_external(findSymbol(name)); };
  const auto number = [this](std::string_view name) { return findSymbol(name)->get_int(); };
  const auto setFlag = [this,number](std::string_view name,std::string_view value) {
    vm.set_int(vm.unsafe_get_gi(),findSymbol(name),0,number(value));
    };

  // Fixed placement ranges in the verified world script, relative to each chapter root.
  const auto chapter3Placements = batch(old3,46,44);
  const auto mageItem = batch(old3,727,1).front();
  const auto otherItem = batch(old3,747,1).front();
  vm.override_function(old3,std::function<void()>{[place,remove,isMage,number,setFlag,chapter3Placements,mageItem,otherItem] {
    if(number("ENTEROW_KAPITEL3")!=number("FALSE"))
      return;
    for(const auto* companion : {"PC_MAGE_OW","PC_FIGHTER_OW","PC_THIEFOW"})
      remove(companion);
    for(const auto& row : chapter3Placements)
      place(row);
    place(isMage() ? mageItem : otherItem);
    setFlag("ENTEROW_KAPITEL3","TRUE");
    }},PlacementDefinitionRuntime::enabled);

  auto pc = findSymbol(old4)->address()+291;
  std::array<LiteralPlacement,11> deadHunters{};
  using Op = daedalus::DaedalusOpcode;
  for(auto& row : deadHunters) {
    row = readLiteralPlacement(vm,pc,old4);
    const auto instance = vm.instruction_at(pc);
    const auto kill = vm.instruction_at(pc+5);
    if(instance.op!=Op::PUSHI || instance.immediate!=int32_t(row.instance->index()) ||
       kill.op!=Op::BL || kill.address!=findSymbol("B_KILLNPC")->address())
      throw std::runtime_error("Stock placement: unexpected Old World chapter four insert and kill pair");
    pc+=10;
    }
  const auto camp = batch(old4,581,5);
  const std::array optionalCamp={batch(old4,672,1).front(),batch(old4,703,1).front(),
                                 batch(old4,734,1).front(),batch(old4,765,1).front()};
  const auto valley = batch(old4,780,235);
  const auto talbinSnapper = batch(old4,4417,1).front();
  vm.override_function(old4,std::function<void()>{[this,place,remove,isMage,push,external,number,setFlag,
                                                  deadHunters,camp,optionalCamp,valley,talbinSnapper] {
    const auto alive = [this,push,external,number](std::string_view npc) {
      push(npc);
      external("NPC_ISDEAD");
      return vm.pop_int()==number("FALSE");
      };
    const auto routine = [this,push](std::string_view npc,std::string_view name) {
      push(npc);
      push(name);
      vm.unsafe_call_bl(findSymbol("B_STARTOTHERROUTINE"));
      };
    const auto giveItem = [this,push,external](std::string_view npc,std::string_view item) {
      push(npc);
      vm.push_int(int32_t(findSymbolIndex(item)));
      vm.push_int(1);
      external("CREATEINVITEMS");
      };

    if(number("ENTEROW_KAPITEL4")==number("FALSE")) {
      if(alive("ENGROM")) {
        routine("ENGROM","$27796");
        giveItem("ENGROM","ITAT_TALBINSLURKERSKIN");
        if(isMage())
          giveItem("ENGROM","ITWR_DEMENTOROBSESSIONBOOK_MIS");
        else
          invokeKillNpc(int32_t(findSymbolIndex("ENGROM")));
        setFlag("ENGROMISGONE","TRUE");
        }
      for(const auto* npc : {"STRF_1115_GEPPERT","STRF_1116_KERVO","VLK_4106_DOBAR","VLK_4107_PARLAF"})
        remove(npc);
      if(alive("SENGRATH")) {
        routine("SENGRATH","$27797");
        const auto crossbow = int32_t(findSymbolIndex("ITRW_MIL_CROSSBOW"));
        push("SENGRATH");
        vm.push_int(crossbow);
        external("NPC_HASITEMS");
        if(vm.pop_int()!=0) {
          push("SENGRATH");
          vm.push_int(crossbow);
          external("NPC_REMOVEINVITEM");
          }
        giveItem("SENGRATH","ITRW_SENGRATHSARMBRUST_MIS");
        setFlag("SENGRATH_MISSING","TRUE");
        invokeKillNpc(int32_t(findSymbolIndex("SENGRATH")));
        }
      for(const auto& row : deadHunters) {
        place(row);
        invokeKillNpc(int32_t(row.instance->index()));
        }
      push("BRUTUS");
      push("$27809");
      external("NPC_EXCHANGEROUTINE");
      for(const auto& row : camp)
        place(row);
      constexpr const char* companions[]={"SLD_BULLCO_IS_ALIVE","SLD_ROD_IS_ALIVE","SLD_CIPHER_IS_ALIVE","SLD_GORN_IS_ALIVE"};
      for(size_t i=0;i<optionalCamp.size();++i)
        if(number(companions[i])==number("TRUE"))
          place(optionalCamp[i]);
      for(const auto& row : valley)
        place(row);

      push("TOPIC_DRAGONHUNTER");
      push("LOG_MISSION");
      external("LOG_CREATETOPIC");
      push("TOPIC_DRAGONHUNTER");
      push("LOG_RUNNING");
      external("LOG_SETTOPICSTATUS");
      push("TOPIC_DRAGONHUNTER");
      push("$28054");
      vm.unsafe_call_bl(findSymbol("B_LOGENTRY"));
      for(const auto* title : {"KAPWECHSEL_4","KAPWECHSEL_4_TEXT","$28055","$28056"})
        push(title);
      vm.push_int(6000);
      external("INTRODUCECHAPTER");
      setFlag("ENTEROW_KAPITEL4","TRUE");
      }

    if(number("TALBIN_FOLLOWSTHROUGHPASS")==number("LOG_OBSOLETE")) {
      invokeKillNpc(int32_t(findSymbolIndex("VLK_4130_TALBIN")));
      place(talbinSnapper);
      setFlag("TALBIN_FOLLOWSTHROUGHPASS","LOG_FAILED");
      }
    else if(number("TALBIN_FOLLOWSTHROUGHPASS")==number("LOG_SUCCESS")) {
      remove("VLK_4130_TALBIN");
      setFlag("TALBIN_FOLLOWSTHROUGHPASS","LOG_FAILED");
      }
    }},PlacementDefinitionRuntime::enabled);
  }

void GameScript::loadKhorinisChapter3() {
  constexpr auto section = "B_ENTER_NEWWORLD_KAPITEL_3";
  const auto start = findSymbol(section)->address();
  // Offsets identify strictly literal runs in this verified stock function.
  enum PlacementGroup { Diego, Gorn, FirstSeeker, Bugs, SekobFarm, ChapterPopulation, Mercenaries, BlessedStones, VinoRitual };
  static constexpr std::pair<uint32_t,size_t> runs[] = {
    {116,1}, {161,1}, {227,1}, {293,3}, {479,4}, {625,61}, {1824,8}, {1965,2}, {2092,4},
    };
  std::array<std::vector<LiteralPlacement>,std::size(runs)> placements;
  for(size_t i=0;i<placements.size();++i) {
    auto [offset,count] = runs[i];
    auto pc = start+offset;
    placements[i].reserve(count);
    for(size_t row=0;row<count;++row)
      placements[i].push_back(readLiteralPlacement(vm,pc,section));
    }

  vm.override_function(section,std::function<void()>{[this,placements=std::move(placements)] {
    const auto number = [this](std::string_view symbol) { return findSymbol(symbol)->get_int(); };
    auto* entered = findSymbol("ENTERNW_KAPITEL3");
    if(entered->get_int()!=number("FALSE"))
      return;

    const auto assignTrue = [&](std::string_view flag) { findSymbol(flag)->set_int(number("TRUE")); };
    const auto dead = [this](std::string_view actor) {
      vm.push_reference(findSymbol(actor));
      vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
      return vm.pop_int();
      };
    const auto member = [this](std::string_view actor) {
      auto context = findSymbol(actor)->get_instance();
      vm.unsafe_set_gi(context);
      return context;
      };
    const auto guild = [&](std::string_view name) {
      const auto hero = member("HERO");
      return vm.get_int(hero,findSymbol("C_NPC.GUILD"),0)==number(name);
      };
    const auto guildEither = [&](std::string_view first,std::string_view second) {
      const bool a = guild(first);
      const bool b = guild(second);
      return a || b;
      };
    const auto place = [&](PlacementGroup group) {
      for(const auto& row : placements[group]) {
        vm.push_int(int32_t(row.instance->index()));
        vm.push_reference(row.point);
        vm.unsafe_call_external(row.external);
        }
      };
    const auto routine = [this](std::string_view actor,std::string_view name) {
      vm.push_reference(findSymbol(actor));
      vm.push_reference(findSymbol(name));
      vm.unsafe_call_bl(findSymbol("B_STARTOTHERROUTINE"));
      };
    const auto lifecycle = [this](std::string_view action,std::string_view actor) {
      vm.push_int(int32_t(findSymbolIndex(actor)));
      vm.unsafe_call_bl(findSymbol(action));
      };
    const auto grant = [this](std::string_view actor,std::string_view item) {
      vm.push_reference(findSymbol(actor));
      vm.push_int(int32_t(findSymbolIndex(item)));
      vm.push_int(1);
      vm.unsafe_call_external(findSymbol("CREATEINVITEMS"));
      };

    if(dead("SALANDRIL")==number("FALSE")) {
      const auto salandril = member("SALANDRIL");
      vm.set_int(salandril,findSymbol("C_NPC.AIVAR"),uint16_t(number("AIV_TOUGHGUY")),number("TRUE"));
      }
    auto* flags = findSymbol("C_NPC.FLAGS");
    vm.set_int(member("CORNELIUS"),flags,0,0);
    if(dead("HODGES")==0)
      routine("HODGES","$28119");
    if(dead("DIEGONW")!=0) {
      place(Diego);
      routine("DIEGONW","$28121");
      }
    if(dead("GORNNW_VOR_DJG")!=0) {
      place(Gorn);
      routine("GORNNW_VOR_DJG","$28123");
      }
    if(dead("LARES")==number("FALSE"))
      routine("LARES","$28124");
    place(FirstSeeker);
    lifecycle("B_KILLNPC","PAL_297_RITTER");
    lifecycle("B_KILLNPC","PAL_298_RITTER");
    if(guild("GIL_KDF"))
      lifecycle("B_KILLNPC","ULF");
    place(Bugs);

    if(dead("SEKOB")==number("FALSE")) {
      for(const auto& [actor,point] : {std::pair{"SEKOB","$28129"}, {"ROSI","$28130"}, {"TILL","$28131"},
                                      {"BALTHASAR","$28132"}, {"BAU_933_REGA","$28133"}, {"BAU_934_BABERA","$28134"},
                                      {"BAU_937_BAUER","$28135"}, {"BAU_938_BAUER","$28136"}})
        routine(actor,point);
      place(SekobFarm);
      vm.set_int(member("SEKOB"),flags,0,number("NPC_FLAG_IMMORTAL"));
      }
    for(const auto& [actor,point] : {std::pair{"LESTER","$28141"}, {"BENNET","$28142"},
                                    {"SERGIO","$28143"}, {"PECK","$28144"}})
      routine(actor,point);
    lifecycle("B_REMOVENPC","PAL_203_LOTHAR");
    place(ChapterPopulation);

    constexpr auto obsessionBook = "ITWR_DEMENTOROBSESSIONBOOK_MIS";
    if(dead("MALAK")==number("FALSE")) {
      routine("MALAK","$28206");
      assignTrue("MALAK_ISALIVE_KAP3");
      for(const auto& [actor,point] : {std::pair{"BAU_962_BAUER","$28207"}, {"BAU_964_BAUER","$28208"},
                                      {"BAU_965_BAUER","$28209"}, {"BAU_966_BAUER","$28210"}, {"BAU_967_BAUER","$28211"},
                                      {"BAU_968_BAUER","$28212"}, {"BAU_969_BAUER","$28213"}})
        routine(actor,point);
      if(guild("GIL_KDF"))
        grant("MALAK",obsessionBook);
      }
    if(guildEither("GIL_PAL","GIL_MIL"))
      routine("HILDA","$28214");
    if(guildEither("GIL_DJG","GIL_SLD"))
      place(Mercenaries);
    if(guild("GIL_KDF")) {
      place(BlessedStones);
      routine("HILDA","$28225");
      if(dead("VINO")==number("FALSE")) {
        routine("VINO","$28226");
        grant("VINO",obsessionBook);
        assignTrue("VINO_ISALIVE_KAP3");
        routine("LOBART","$28227");
        place(VinoRitual);
        lifecycle("B_KILLNPC","YGIANT_BUG_VINORITUAL1");
        lifecycle("B_KILLNPC","YGIANT_BUG_VINORITUAL2");
        }
      if(dead("BROMOR")==number("FALSE"))
        grant("BROMOR",obsessionBook);
      }

    const bool cantharAlive = dead("CANTHAR")==number("FALSE");
    const bool unpaid = number("CANTHAR_PAY")==number("FALSE");
    const bool blackmailKnown = number("MIS_CANTHARS_KOMPROBRIEF")!=number("FALSE");
    const bool blackmailUnresolved = number("MIS_CANTHARS_KOMPROBRIEF")!=number("LOG_SUCCESS");
    if(cantharAlive && unpaid && blackmailKnown && blackmailUnresolved) {
      lifecycle("B_REMOVENPC","SARAH");
      routine("CANTHAR","$28232");
      vm.push_reference(findSymbol("CANTHAR"));
      vm.push_reference(findSymbol("$28233"));
      vm.unsafe_call_external(findSymbol("AI_TELEPORT"));
      assignTrue("CANTHAR_SPERRE");
      assignTrue("CANTHAR_WIEDERRAUS");
      }
    grant("LESTER","ITMW_1H_BAU_AXE");
    grant("EHNIM","ITMI_MOLERATLUBRIC_MIS");
    for(const auto* shrine : {"SHRINEISOBSESSED_NW_TROLLAREA_PATH_37","SHRINEISOBSESSED_NW_FARM1_CONNECT_XARDAS",
                              "SHRINEISOBSESSED_NW_TROLLAREA_PATH_66","SHRINEISOBSESSED_NW_TROLLAREA_PATH_04",
                              "SHRINEISOBSESSED_SAGITTA","SHRINEISOBSESSED_NW_BIGMILL_MALAKSVERSTECK_02",
                              "SHRINEISOBSESSED_NW_FARM3_BIGWOOD_02"})
      assignTrue(shrine);

    const auto introduce = [this](std::string_view image,std::string_view sound) {
      for(const auto* symbol : {"KAPWECHSEL_3","KAPWECHSEL_3_TEXT"})
        vm.push_reference(findSymbol(symbol));
      vm.push_reference(findSymbol(image));
      vm.push_reference(findSymbol(sound));
      vm.push_int(6000);
      vm.unsafe_call_external(findSymbol("INTRODUCECHAPTER"));
      };
    if(guildEither("GIL_PAL","GIL_MIL"))
      introduce("$28234","$28235");
    else if(guild("GIL_KDF"))
      introduce("$28236","$28237");
    else
      introduce("$28238","$28239");
    entered->set_int(number("TRUE"));
    }},PlacementDefinitionRuntime::enabled);
  }

void GameScript::enterChapterWorld(ChapterWorld region) {
  struct Entry {
    const char* level;
    std::array<const char*,6> chapters;
    };
  static constexpr Entry entries[]={
    {"NEWWORLD_ZEN",{"B_ENTER_NEWWORLD_KAPITEL_1", "B_ENTER_NEWWORLD_KAPITEL_2", "B_ENTER_NEWWORLD_KAPITEL_3",
                    "B_ENTER_NEWWORLD_KAPITEL_4", "B_ENTER_NEWWORLD_KAPITEL_5", "B_ENTER_NEWWORLD_KAPITEL_6"}},
    {"OLDWORLD_ZEN",{"B_ENTER_OLDWORLD_KAPITEL_1", "B_ENTER_OLDWORLD_KAPITEL_2", "B_ENTER_OLDWORLD_KAPITEL_3",
                    "B_ENTER_OLDWORLD_KAPITEL_4", "B_ENTER_OLDWORLD_KAPITEL_5", "B_ENTER_OLDWORLD_KAPITEL_6"}},
    {"ADDONWORLD_ZEN",{"B_ENTER_ADDONWORLD_KAPITEL_1", "B_ENTER_ADDONWORLD_KAPITEL_2", "B_ENTER_ADDONWORLD_KAPITEL_3",
                      "B_ENTER_ADDONWORLD_KAPITEL_4", "B_ENTER_ADDONWORLD_KAPITEL_5", "B_ENTER_ADDONWORLD_KAPITEL_6"}},
    };
  const auto& entry=entries[size_t(region)];
  const auto* npcGlobals=findSymbol("B_INITNPCGLOBALS");
  vm.unsafe_call_bl(npcGlobals);
  for(size_t i=0;i<entry.chapters.size();++i) {
    if(storyChapter>=int32_t(i)+1)
      vm.unsafe_call_bl(findSymbol(entry.chapters[i]));
    }
  const auto number=[&](std::string_view name) { return findSymbol(name)->get_int(); };
  currentStoryWorld=number(entry.level);
  vm.unsafe_call_bl(npcGlobals);
  if(region!=ChapterWorld::Old)
    return;

  const auto isDead=[&](std::string_view alias) {
    vm.push_reference(findSymbol(alias));
    vm.unsafe_call_external(findSymbol("NPC_ISDEAD"));
    return vm.pop_int();
    };
  const bool biffAlive=isDead("BIFF")==number("FALSE");
  const bool biffParty=number("DJG_BIFFPARTY")==number("TRUE");
  if(biffAlive && biffParty) {
    vm.push_reference(findSymbol("BIFF"));
    vm.push_int(number("DJG_BIFF_HALBEHALBE")==number("TRUE") ? 500 : 300);
    vm.unsafe_call_external(findSymbol("NPC_SETREFUSETALK"));
    }
  const bool rescueRunning=number("MIS_RESCUEBILGOT")==number("LOG_RUNNING");
  const bool bilgotDead=isDead("BILGOT")==number("TRUE");
  if(rescueRunning && bilgotDead)
    findSymbol("MIS_RESCUEBILGOT")->set_int(number("LOG_FAILED"));
  }

void GameScript::changeChapter(int32_t chapter, int32_t level) {
  auto* savedLevel=findSymbol("B_KAPITELWECHSEL.AKTUELLES_LEVEL_ZEN");
  savedLevel->set_int(level);
  findSymbol("B_KAPITELWECHSEL.NEUES_KAPITEL")->set_int(chapter);
  storyChapter=chapter;

  if(chapter==2) {
    for(const auto* name:{"SEKOB","BENGAR"}) {
      const auto actor=findSymbol(name)->get_instance();
      vm.unsafe_set_gi(actor);
      if(actor)
        findSymbol("C_NPC.FLAGS")->set_int(0,0,actor.get());
      }
    }
  if(chapter==1 || chapter==2) {
    static constexpr std::array introductions={
      std::array{"KAPWECHSEL_1","KAPWECHSEL_1_TEXT","$28527","$28528"},
      std::array{"KAPWECHSEL_2","KAPWECHSEL_2_TEXT","$28529","$28530"},
      };
    for(const auto* name:introductions[size_t(chapter)-1])
      vm.push_reference(findSymbol(name));
    vm.push_int(6000);
    vm.unsafe_call_external(findSymbol("INTRODUCECHAPTER"));
    }
  else if(chapter>=4 && chapter<=6) {
    findSymbol("PLAYER_TALENT_ALCHEMY")->set_int(findSymbol("TRUE")->get_int(),12);
    }

  vm.unsafe_call_bl(findSymbol("B_CHECKLOG"));
  // Nested story calls can update the parameter symbols and the chapter.
  if(savedLevel->get_int()==findSymbol("OLDWORLD_ZEN")->get_int())
    vm.unsafe_call_bl(findSymbol("B_ENTER_OLDWORLD"));
  if(savedLevel->get_int()==findSymbol("NEWWORLD_ZEN")->get_int())
    vm.unsafe_call_bl(findSymbol("B_ENTER_NEWWORLD"));

  static constexpr std::array ambientXp={
    "XP_AMBIENTKAP1","XP_AMBIENTKAP2","XP_AMBIENTKAP3","XP_AMBIENTKAP4","XP_AMBIENTKAP5","XP_AMBIENTKAP6",
    };
  if(storyChapter>=1 && storyChapter<=int32_t(ambientXp.size()))
    findSymbol("XP_AMBIENT")->set_int(findSymbol(ambientXp[size_t(storyChapter)-1])->get_int());
  }
