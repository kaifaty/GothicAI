// Live world contracts for startup, world entry and story routine switches.

void dragonIslandInit(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& hero=*world.player();
  const auto key=script.findSymbolIndex("ITKE_SHIP_LEVELCHANGE_MIS");
  auto* level=script.findSymbol("CURRENTLEVEL");
  auto* chapter=script.findSymbol("KAPITEL");
  auto* pedroAlias=script.findSymbol("PEDRO_DI");
  auto* pedro=world.findNpcByInstance(script.findSymbolIndex("NOV_600_PEDRO_DI"));
  const auto island=script.intSymbol("DRAGONISLAND_ZEN",0);
  const auto khorinis=script.intSymbol("NEWWORLD_ZEN",0);
  const auto initialCount=hero.itemCount(key);
  const auto initialChapter=chapter->get_int();
  const auto initialPedro=pedroAlias->get_instance();
  require(pedro && initialPedro==std::static_pointer_cast<daedalus::INpc>(pedro->handlePtr()) && level->get_int()==island,
          "Dragon Island entry must set the level and bind its Pedro");
  const auto* setting=std::getenv("OPENGOTHIC_PLACEMENT_RECORDS");
  const auto previous=setting ? std::optional<std::string>(setting) : std::nullopt;
  const auto entry=script.findSymbol("INIT_DRAGONISLAND")->address();
  const auto result=[&](bool native,uint32_t count) {
    if(native) setenv("OPENGOTHIC_PLACEMENT_RECORDS","1",1);
    else setenv("OPENGOTHIC_PLACEMENT_RECORDS","0",1);
    hero.delItem(key,uint32_t(hero.itemCount(key)));
    if(count>0) hero.addItem(key,count);
    const auto priorChapter=count>0 ? 6 : 0;
    level->set_int(khorinis);
    chapter->set_int(priorChapter);
    pedroAlias->set_instance(nullptr);

    std::vector<std::pair<std::string,int32_t>> calls;
    const auto observer=vm.add_call_observer([&](const auto& symbol) {
      if(symbol.name()=="B_INITMONSTERATTITUDES" || symbol.name()=="B_INITGUILDATTITUDES" || symbol.name()=="B_INITNPCGLOBALS")
        calls.emplace_back(symbol.name(),level->get_int());
      });
    vm.observe_externals([&](const auto& symbol) {
      if(symbol.name()=="NPC_HASITEMS" || symbol.name()=="NPC_REMOVEINVITEMS")
        calls.emplace_back(symbol.name(),level->get_int());
      });
    size_t rootInstructions=0;
    vm.observe_instructions([&](const auto&) { if(vm.pc()==entry) ++rootInstructions; });

    const auto pc=vm.pc();
    const auto context=vm.unsafe_get_gi();
    vm.push_int(0x12345678);
    script.runWorldInit("INIT_DRAGONISLAND");
    const auto sentinel=vm.pop_int();
    vm.observe_instructions({});
    vm.observe_externals({});
    vm.remove_call_observer(observer);

    require(rootInstructions==(native ? 0u : 1u) && sentinel==0x12345678 && vm.pc()==pc && vm.unsafe_get_gi()==context,
            "Dragon Island Init must choose the expected root and restore the VM context");
    require(level->get_int()==island && chapter->get_int()==(priorChapter==0 ? 1 : priorChapter) && pedroAlias->get_instance()==initialPedro &&
            hero.itemCount(key)==(count>0 ? count-1 : 0),
            "Dragon Island Init must rebind actors and consume one ship key only when present");
    return calls;
    };
  for(uint32_t count:{0,2}) {
    using Call=std::pair<std::string,int32_t>;
    std::vector<Call> expected={{"B_INITMONSTERATTITUDES",khorinis},{"B_INITGUILDATTITUDES",khorinis},
                                {"B_INITNPCGLOBALS",khorinis},{"NPC_HASITEMS",island}};
    if(count>0) expected.emplace_back("NPC_REMOVEINVITEMS",island);
    expected.emplace_back("B_INITNPCGLOBALS",island);
    require(result(false,count)==expected && result(true,count)==expected,
            "Dragon Island Init: the stock and native call order must agree across the level change");
    }

  hero.delItem(key,uint32_t(hero.itemCount(key)));
  if(initialCount>0) hero.addItem(key,initialCount);
  chapter->set_int(initialChapter);
  if(previous) setenv("OPENGOTHIC_PLACEMENT_RECORDS",previous->c_str(),1);
  else unsetenv("OPENGOTHIC_PLACEMENT_RECORDS");
  std::puts("Dragon Island Init: both alias passes, level, inventory and call order match stock");
  }

// First-world placement against the original script path.
void dragonIslandPlacement(Gothic& gothic) {
  const auto program=NpcSnapshot::load(baseProgramIndex().dir.parent_path());
  if(program.sha256!="a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c")
    return;
  gothic.setGame(std::make_unique<GameSession>("DRAGONISLAND.ZEN"));
  auto& world=*gothic.world();
  dragonIslandInit(world);
  auto& script=world.script();
  auto& vm=script.getVm();
  const auto baseline=placementSnapshot(world,true);
  const auto random=NpcDefinitionContract::random(script);
  const auto* startup=vm.find_symbol_by_name("STARTUP_DRAGONISLAND");
  const std::array addresses={startup->address(),vm.find_symbol_by_name("STARTUP_NEWWORLD_PART_DRAGON_ISLAND_01")->address(),
                              vm.find_symbol_by_name("STARTUP_NEWWORLD_PART_DRAGON_UNDEAD_01")->address(),
                              vm.find_symbol_by_name("STARTUP_NEWWORLD_PART_DRAGON_FINAL_01")->address()};
  const auto captured=[&](bool record,int crew) {
    placementRestore(world,baseline);
    NpcDefinitionContract::random(script)=random;
    const auto flag=[&](const char* name,int32_t value) { vm.find_symbol_by_name(name)->set_int(value); };
    for(const auto* name:{"LEE_ISONBOARD","MILTENNW_ISONBOARD","LESTER_ISONBOARD","MARIO_ISONBOARD","WOLF_ISONBOARD",
                          "VATRAS_ISONBOARD","BENNET_ISONBOARD","DIEGO_ISONBOARD","GORN_ISONBOARD","LARES_ISONBOARD",
                          "BIFF_ISONBOARD","ANGAR_ISONBOARD","GIRION_ISONBOARD"})
      flag(name,crew==3 ? 2 : 0);
    flag("JORGENISCAPTAIN",crew==0 ? 2 : crew!=2);
    flag("TORLOFISCAPTAIN",crew>=2);
    flag("JACKISCAPTAIN",crew>=2);
    flag("KAPITEL",5);
    flag("CURRENTLEVEL",vm.find_symbol_by_name("NEWWORLD_ZEN")->get_int());
    if(crew==1) {
      flag("MILTENNW_ISONBOARD",2);
      flag("DIEGO_ISONBOARD",2);
      }
    if(crew==2) {
      flag("LESTER_ISONBOARD",2);
      flag("LARES_ISONBOARD",2);
      }
    std::vector<std::string> calls;
    size_t entries=0;
    vm.observe_instructions([&](const auto&) { entries+=std::find(addresses.begin(),addresses.end(),vm.pc())!=addresses.end(); });
    vm.observe_externals([&](const daedalus::DaedalusSymbol& external) {
      auto line=external.name();
      const auto arguments=vm.find_parameters_for_function(&external);
      for(size_t i=0;i<arguments.size();++i) {
        line+=" ";
        line+=placementArgument(vm,arguments[i],uint16_t(arguments.size()-i-1),world);
        }
      calls.push_back(std::move(line));
      });
    script.startupFloor().begin();
    vm.push_int(0x12345678);
    const auto pc=vm.pc();
    try {
      if(record) script.runStartup(startup->name());
      else vm.call_function<void>(startup);
      }
    catch(...) {
      vm.observe_externals({});
      vm.observe_instructions({});
      script.startupFloor().end();
      throw;
      }
    const auto stack=vm.pop_int();
    vm.observe_externals({});
    vm.observe_instructions({});
    world.resetPositionToTA();
    script.startupFloor().end();
    require(stack==0x12345678 && vm.pc()==pc && entries==(record ? 0u : addresses.size()),
            "Dragon Island placement: the selected executor leaves the stack and PC intact");
    require((world.findNpcByInstance(script.findSymbolIndex("VLK_4250_JORGEN_DI"))!=nullptr)==(crew==1 || crew==3),
            "Dragon Island placement: only the exact Jorgen captain flag inserts his island instance");
    return std::tuple{calls,archiveEntries(placementSnapshot(world,false)),placementSymbols(script,false),script.randomState(nullptr)};
    };
  for(int crew:{0,1,2,3}) {
    const auto vmResult=captured(false,crew);
    require(captured(true,crew)==vmResult,
            ("Dragon Island placement: crew case "+std::to_string(crew)+" matches stock calls and saved world").c_str());
    }
  gothic.clearGame().reset();
  std::puts("Dragon Island placement: four captain and crew combinations match the original calls and world state");
  }

void routineSwitchWorld(World& world) {
  const auto program=NpcSnapshot::load(baseProgramIndex().dir.parent_path());
  if(program.sha256!="a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c")
    return;

  auto& script=world.script();
  auto& vm=script.getVm();
  auto* xardas=script.findSymbol("NONE_100_XARDAS");
  auto* helper=script.findSymbol("B_STARTOTHERROUTINE");
  auto* actorLocal=script.findSymbol("B_STARTOTHERROUTINE.SLF");
  auto* routineLocal=script.findSymbol("B_STARTOTHERROUTINE.NEWROUTINE");
  auto* hero=world.player();
  const auto dead=script.findFunction("ZS_DEAD");
  const auto ordinary=script.findFunction("ZS_STAND_WP");
  require(xardas && helper && actorLocal && routineLocal && hero && dead.isValid() && ordinary.isValid(),
          "the routine switch fixture needs the stock actors, old schedule and saved locals");
  const auto oldXardas=xardas->get_instance(), oldActor=actorLocal->get_instance();
  const auto oldSelf=vm.global_self()->get_instance(), oldGi=vm.unsafe_get_gi();
  const std::string oldRoutine=routineLocal->get_string();
  const auto* configured=std::getenv("OPENGOTHIC_ROUTINE_RECORDS");
  const auto oldSwitch=configured ? std::optional<std::string>(configured) : std::nullopt;

  constexpr std::string_view waypoint="NW_CITY_PATH_HABOUR_03";
  Npc target(world,xardas->index(),waypoint);
  Npc sameKind(world,xardas->index(),waypoint);
  const auto handle=[](Npc* npc) {
    return npc ? std::static_pointer_cast<daedalus::INpc>(npc->handlePtr()) : nullptr;
    };
  const struct Case {
    Npc* self;
    Npc* target;
    bool dead, exchanged, continued;
    } cases[]={
    {&target,&target,false,true,false},
    {&sameKind,&target,false,true,false},
    {hero,&target,false,true,true},
    {hero,&target,true,false,false},
    {&target,&target,true,true,false},
    {hero,nullptr,false,false,false}
    };
  for(const auto& c:cases) {
    const auto run=[&](bool native) {
      if(native) unsetenv("OPENGOTHIC_ROUTINE_RECORDS");
      else setenv("OPENGOTHIC_ROUTINE_RECORDS","0",1);
      target.clearState(true);
      if(c.dead) require(target.startState(dead,""),"the test NPC must enter the dead state");
      target.clearAiQueue();
      target.clearRoutine();
      target.addRoutine(gtime(0,0),gtime(23,59),uint32_t(ordinary.ptr),waypoint);
      vm.global_self()->set_instance(handle(c.self));

      size_t instructions=0;
      std::vector<std::string> calls;
      vm.observe_instructions([&](const auto&) {
        if(vm.pc()==helper->address()) ++instructions;
        });
      vm.observe_externals([&](const auto& fn) {
        if(fn.name()=="AI_STANDUP" || fn.name()=="HLP_GETINSTANCEID" || fn.name()=="NPC_ISDEAD" ||
           fn.name()=="HLP_ISVALIDNPC" || fn.name()=="NPC_EXCHANGEROUTINE" || fn.name()=="AI_CONTINUEROUTINE")
          calls.push_back(fn.name());
        });
      std::string error;
      const auto result=storygraph::callFunction(vm,*helper,{storygraph::Value::instance(handle(c.target)),
                                                          storygraph::Value::str("RITUALINNOSEYE")},error);
      vm.observe_instructions({});
      vm.observe_externals({});
      require(result.has_value(),error.c_str());
      require((instructions==0)==native && actorLocal->get_instance()==handle(c.target) &&
              routineLocal->get_string()=="RITUALINNOSEYE" && vm.global_self()->get_instance()==handle(c.self) &&
              vm.unsafe_get_gi()==oldGi,"routine switches must retain the saved locals and restore the invoking context");

      std::vector<std::tuple<gtime,gtime,ScriptFn,std::string,const WayPoint*>> rows;
      target.forEachRoutine([&](const Npc::RoutineInfo& row) {
        rows.emplace_back(row.start,row.end,row.state,std::string(row.point),row.waypoint);
        });
      require(rows.size()==(c.exchanged ? 2u : 1u) &&
              std::ranges::count(calls,"NPC_EXCHANGEROUTINE")==int(c.exchanged) &&
              std::ranges::count(calls,"AI_CONTINUEROUTINE")==int(c.continued) &&
              !calls.empty() && calls.front()=="AI_STANDUP",
              "the stock switch must stand first, observe the instance rules and continue only another live actor");
      return std::tuple(calls,rows,activityQueue(target));
      };
    const auto stock=run(false);
    require(run(true)==stock,"routine switch must match stock calls, rows and queued actions");
    }
  xardas->set_instance(oldXardas);
  actorLocal->set_instance(oldActor);
  routineLocal->set_string(oldRoutine);
  vm.global_self()->set_instance(oldSelf);
  if(oldSwitch) setenv("OPENGOTHIC_ROUTINE_RECORDS",oldSwitch->c_str(),1);
  else unsetenv("OPENGOTHIC_ROUTINE_RECORDS");
  std::puts("Routine switch: saved aliases, same-instance rule, validity, schedule and queue match stock");
  }

std::vector<std::string> placementFirstWorld(Gothic& gothic) {
  const auto* configured=std::getenv("OPENGOTHIC_PLACEMENT_RECORDS");
  const bool configuredBefore=configured!=nullptr;
  const std::string previous=configured ? configured : "";
  auto state=[&] {
    auto& world=*gothic.world();
    auto result=placementSymbols(world.script(),false);
    result["NPC count"]=std::to_string(world.npcCount());
    uint32_t items=0;
    while(world.itmById(items)) ++items;
    result["item count"]=std::to_string(items);
    result["random generator"]=world.script().randomState(nullptr);
    auto entries=archiveEntries(placementSnapshot(world,false));
    const auto item=itemState(world.script());
    for(const auto& [name,payload]:item) entries.emplace("global-ITEM/"+name,payload);
    for(const auto& [name,payload]:entries)
      result["save/"+name]=std::to_string(payload.size())+":"+
          Sha256::hash({reinterpret_cast<const char*>(payload.data()),payload.size()});
    return result;
    };
  std::vector<std::string> differences;
  try {
    setenv("OPENGOTHIC_PLACEMENT_RECORDS","0",1);
    gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
    require(NpcDefinitionContract::placements(gothic.world()->script()).placements.empty(),
            "disabled first-world placement must have no game definitions");
    const auto expected=state();
    const auto expectedLocals=placementSymbols(gothic.world()->script(),true);
    gothic.clearGame().reset();
    unsetenv("OPENGOTHIC_PLACEMENT_RECORDS");
    gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
    require(!NpcDefinitionContract::placements(gothic.world()->script()).placements.empty(),
            "default first-world placement must load game definitions");
    const auto actual=state();
    for(const auto& [field,value]:expected) {
      const auto found=actual.find(field);
      if(found==actual.end() || value!=found->second)
        differences.push_back(field+" expected="+value+" actual="+(found==actual.end() ? "<absent>" : found->second));
      }
    for(const auto& [field,value]:actual)
      if(!expected.contains(field)) differences.push_back(field+" expected=<absent> actual="+value);
    const auto actualLocals=placementSymbols(gothic.world()->script(),true);
    for(const auto& [name,value]:expectedLocals) {
      const auto found=actualLocals.find(name);
      if(found==actualLocals.end() || value!=found->second)
        differences.push_back("VM local "+name+" expected="+value+" actual="+(found==actualLocals.end() ? "<absent>" : found->second));
      }
    for(const auto& [name,value]:actualLocals)
      if(!expectedLocals.contains(name)) differences.push_back("VM local "+name+" expected=<absent> actual="+value);
    const auto* startup=gothic.world()->script().getVm().find_symbol_by_name("STARTUP_NEWWORLD");
    require(startup && NpcDefinitionContract::placements(gothic.world()->script()).placements.contains(startup->index()),
            "default NEWWORLD startup must replay a captured placement record");
    routineSwitchWorld(*gothic.world());
    std::printf("First-world NEWWORLD placement: %s NPCs, %s items; %zu saved/global state entries, %zu differences; STARTUP_NEWWORLD=%s\n",
                actual.at("NPC count").c_str(),actual.at("item count").c_str(),expected.size(),differences.size(),
                startup && NpcDefinitionContract::placements(gothic.world()->script()).placements.contains(startup->index()) ? "record" : "VM");
    }
  catch(...) {
    if(configuredBefore) setenv("OPENGOTHIC_PLACEMENT_RECORDS",previous.c_str(),1);
    else unsetenv("OPENGOTHIC_PLACEMENT_RECORDS");
    throw;
    }
  if(configuredBefore) setenv("OPENGOTHIC_PLACEMENT_RECORDS",previous.c_str(),1);
  else unsetenv("OPENGOTHIC_PLACEMENT_RECORDS");
  for(const auto& value:differences) std::fprintf(stderr,"[placement-world-difference] %s\n",value.c_str());
  return differences;
  }
