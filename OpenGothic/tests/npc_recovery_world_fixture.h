// World contracts for death, unconsciousness, healing and equipment.

void dragonDeathBranches(Gothic& gothic, bool native) {
  auto& world = *gothic.world();
  auto& script = world.script();
  auto& vm = script.getVm();
  const auto symbol = [&](std::string_view name) { return script.findSymbol(name); };
  const auto* point = world.findWayPoint("NW_FARM4_BALTHASAR");
  require(point!=nullptr,"dragon death: need a waypoint for the disposable cast");
  const auto spawn = [&](std::string_view name) -> Npc& {
    auto* npc = world.addNpc(script.findSymbolIndex(name),point->position());
    require(npc!=nullptr,"dragon death: stock actor must be present");
    return *npc;
    };
  const auto handle = [](const Npc& npc) { return std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()); };
  auto& raven = spawn("BDT_1090_ADDON_RAVEN");
  auto& myxir = spawn("KDW_14030_ADDON_MYXIR_ADW");
  auto& biff = spawn("DJG_713_BIFF");
  auto& swamp = spawn("DRAGON_SWAMP");
  auto& undead = spawn("DRAGON_UNDEAD");
  require(!myxir.isDead() && !biff.isDead() && swamp.handle().guild==GIL_DRAGON,
          "dragon death: need living witnesses and the stock dragon guild");
  myxir.handle().flags = NpcFlag::IMMORTAL;
  symbol("MYXIR_ADW")->set_instance(handle(myxir));
  symbol("BIFF")->set_instance(handle(biff));
  for(auto name : {"RAVENISDEAD","SWAPDRAGNISDEAD","MIS_ALLDRAGONSDEAD","DJG_BIFFSURVIVEDLASTDRAGON","UNDEADDRAGONISDEAD"})
    symbol(name)->set_int(0);
  symbol("MIS_KILLEDDRAGONS")->set_int(3);
  symbol("DJG_BIFFPARTY")->set_int(1);
  world.player()->handle().guild = GIL_DJG;

  struct Videos {
    Gothic& gothic;
    std::vector<std::string> played;
    explicit Videos(Gothic& engine) : gothic(engine) { gothic.onVideo.bind(this,&Videos::record); }
    ~Videos() { gothic.onVideo.ubind(this,&Videos::record); }
    void record(std::string_view name,bool) { played.emplace_back(name); }
    } videos(gothic);
  const auto* configuredRecords = std::getenv("OPENGOTHIC_NPC_RECORDS");
  const auto previousRecords = configuredRecords ? std::optional<std::string>(configuredRecords) : std::nullopt;
  setenv("OPENGOTHIC_NPC_RECORDS",native ? "1" : "0",1);

  const auto* counter = symbol("B_DRAGONKILLCOUNTER");
  const auto* remove = symbol("B_REMOVENPC");
  bool counterVm = false, removeVm = false;
  std::vector<std::string> calls;
  vm.observe_instructions([&](const auto&) {
    counterVm |= vm.pc()==counter->address();
    removeVm |= vm.pc()==remove->address();
    });
  vm.observe_externals([&](const auto& fn) {
    const auto& name = fn.name();
    if(name=="PLAYVIDEOEX" || name=="AI_TELEPORT" || name=="NPC_CHANGEATTRIBUTE" ||
       name=="LOG_CREATETOPIC" || name=="LOG_SETTOPICSTATUS")
      calls.push_back(name);
    });
  const auto always = vm.add_call_observer([&](const auto& fn) {
    if(fn.name()=="B_REMOVENPC" || fn.name()=="B_STARTOTHERROUTINE" || fn.name()=="B_LOGENTRY")
      calls.push_back(fn.name());
    });
  const auto call = [&](Npc& actor) {
    const auto context = vm.unsafe_get_gi();
    vm.push_int(0x12345678);
    vm.push_instance(handle(actor));
    vm.unsafe_call_bl(counter);
    const auto result = vm.pop_int();
    require(vm.pop_int()==0x12345678 && vm.unsafe_get_gi()==context,
            "dragon death: every exit restores the caller stack and member context");
    return result;
    };
  const auto restore = [&] {
    if(previousRecords) setenv("OPENGOTHIC_NPC_RECORDS",previousRecords->c_str(),1);
    else unsetenv("OPENGOTHIC_NPC_RECORDS");
    vm.remove_call_observer(always);
    vm.observe_instructions({});
    vm.observe_externals({});
    };
  try {
    require(call(raven)==1 && symbol("RAVENISDEAD")->get_int()==1 && myxir.attribute(ATR_HITPOINTS)==0 &&
            myxir.handle().flags==NpcFlag::NONE && symbol("B_REMOVENPC.NPC")->get_instance()==handle(myxir) &&
            symbol("B_REMOVENPC.NPCINSTANCE")->get_int()==int32_t(symbol("MYXIR_ADW")->index()),
            "dragon death: Raven's movie result is returned and the live Myxir alias is removed");
    require(calls==std::vector<std::string>{"PLAYVIDEOEX","B_REMOVENPC","AI_TELEPORT","B_STARTOTHERROUTINE",
                                            "NPC_CHANGEATTRIBUTE","AI_TELEPORT"},
            "dragon death: Raven's movie, routine, damage and the two teleports keep the stock order");
    calls.clear();
    require(call(raven)==0 && calls.empty(),"dragon death: Raven's latch suppresses replay and removal");
    require(call(swamp)==0 && call(swamp)==0 && symbol("SWAPDRAGNISDEAD")->get_int()==1 &&
            symbol("MIS_KILLEDDRAGONS")->get_int()==4 && symbol("MIS_ALLDRAGONSDEAD")->get_int()==1 &&
            symbol("DJG_BIFFSURVIVEDLASTDRAGON")->get_int()==1 && calls.empty(),
            "dragon death: the final counted dragon is counted once and a living Biff is credited");

    require(call(undead)==1 && symbol("UNDEADDRAGONISDEAD")->get_int()==1 &&
            calls==std::vector<std::string>{"LOG_CREATETOPIC","LOG_SETTOPICSTATUS","B_LOGENTRY","AI_TELEPORT","PLAYVIDEOEX"} &&
            videos.played==std::vector<std::string>{symbol("$17823")->get_string(),symbol("$17826")->get_string()},
            "dragon death: the undead dragon records the journal and queues the exit before playing the hero's guild movie");
    const auto& log = script.questLog();
    bool foundTopic = false;
    for(size_t i=0;i<log.questCount();++i) {
      const auto& quest = log.quest(i);
      if(quest.name==symbol("TOPIC_BACKTOSHIP")->get_string())
        foundTopic = quest.section==QuestLog::Mission && quest.status==QuestLog::Status::Running &&
                     !quest.entry.empty() && quest.entry.back()==symbol("PRINT_DRAGKILLCOUNT")->get_string();
      }
    require(foundTopic && counterVm!=native && removeVm!=native,
            "dragon death: both executors reach the original journal result and the expected function bodies");
    } catch(...) {
    restore();
    throw;
    }
  restore();
  }

void tradeInventoryAfterDefeat(ActivityEventsWorld& test, bool native) {
  auto& world = test.world();
  auto& script = world.script();
  auto& vm = script.getVm();
  auto* matteo = activityNpc(world,"VLK_416_MATTEO");
  require(matteo && !matteo->isDead(),"trade inventory: need the living stock merchant");
  test.stop(*matteo);
  const auto actor = std::static_pointer_cast<daedalus::INpc>(matteo->handlePtr());
  const auto oldSelf = vm.global_self()->get_instance();
  const auto gold = script.findSymbolIndex("ITMI_GOLD");
  const auto junk = script.findSymbolIndex("ITAT_WOLFFUR");
  const auto before = matteo->itemCount(gold);
  script.findSymbol("KAPITEL")->set_int(1);
  auto* stocked = script.findSymbol("MATTEO_ITEMSGIVEN_CHAPTER_1");
  stocked->set_int(0);
  actor->aivar[49] = 1; // At the restock chapter boundary.
  actor->aivar[16] = 1; // The defeat does not award ambient goods again.
  matteo->addItem(junk,2);
  vm.global_self()->set_instance(actor);

  std::vector<std::string> calls;
  const auto observer = vm.add_call_observer([&](const auto& symbol) {
    const auto name = std::string_view(symbol.name());
    if(name=="B_CLEARRUNEINV" || name=="B_CLEARJUNKTRADEINV" || name=="B_GIVETRADEINV_MATTEO" ||
       name=="B_CLEARDEADTRADER" || name=="B_CREATEAMBIENTINV")
      calls.emplace_back(name);
    });
  const auto* trade = script.findSymbol("B_GIVETRADEINV");
  const auto* ambient = script.findSymbol("B_CREATEAMBIENTINV");
  const auto* restock = script.findSymbol("B_GIVETRADEINV_MATTEO");
  bool tradeVm=false, ambientVm=false, stockVm=false;
  vm.observe_instructions([&](const auto&) {
    tradeVm |= vm.pc()==trade->address();
    ambientVm |= vm.pc()==ambient->address();
    stockVm |= vm.pc()==restock->address();
    });
  const auto run = [&] {
    vm.push_int(0x12345678);
    vm.push_reference(vm.global_self());
    vm.unsafe_call_bl(trade);
    require(vm.pop_int()==0x12345678,"trade inventory: the legacy void frame drops all cleanup results");
    };
  run();
  require(calls==std::vector<std::string>{"B_CLEARRUNEINV","B_CLEARJUNKTRADEINV","B_GIVETRADEINV_MATTEO"} &&
          matteo->itemCount(junk)==0 && matteo->itemCount(gold)==before+100 && actor->aivar[49]==2 && stocked->get_int()==1 &&
          script.findSymbol("B_GIVETRADEINV.TRD_MATTEO")->get_instance()==actor &&
          script.findSymbol("B_GIVETRADEINV_MATTEO.SLF")->get_instance()==actor && tradeVm!=native && stockVm!=native,
          "trade inventory: stock junk cleanup and Matteo's real chapter restock run in order");
  calls.clear();
  require(matteo->startState(script.findFunction("ZS_UNCONSCIOUS"),""),"trade inventory: Matteo enters the knockout state");
  run();
  require(calls==std::vector<std::string>{"B_CLEARRUNEINV","B_CLEARDEADTRADER"} && matteo->itemCount(gold)==0 &&
          actor->aivar[49]==0 && stocked->get_int()==1,
          "trade inventory: a defeated merchant resets stock without re-granting ambient loot");
  calls.clear();
  actor->aivar[16]=0;
  run();
  const bool nativeAmbient=native && NpcHelperRuntime::enabled() &&
                           NpcDefinitionContract::definitions(script).helpers.helpers.contains(ambient->index());
  require(calls==std::vector<std::string>{"B_CLEARRUNEINV","B_CLEARDEADTRADER","B_CREATEAMBIENTINV"} &&
          ambientVm!=nativeAmbient && script.findSymbol("B_CREATEAMBIENTINV.SLF")->get_instance()==actor,
          "trade inventory: allow the original ambient inventory record for an unrewarded defeat");
  vm.observe_instructions({});
  vm.remove_call_observer(observer);
  vm.global_self()->set_instance(oldSelf);
  }

void kardifStockWithSecretSign(ActivityEventsWorld& test, bool native) {
  auto& world = test.world();
  auto& script = world.script();
  auto& vm = script.getVm();
  auto* kardif = activityNpc(world,"VLK_431_KARDIF");
  auto* recipient = world.player();
  require(kardif && recipient,"trade inventory: need the stock tavern keeper and a recipient for the sign bonus");
  const auto oldSelf = vm.global_self()->get_instance();
  const auto merchant = std::static_pointer_cast<daedalus::INpc>(kardif->handlePtr());
  vm.global_self()->set_instance(std::static_pointer_cast<daedalus::INpc>(recipient->handlePtr()));
  script.findSymbol("KAPITEL")->set_int(2);
  script.findSymbol("KARDIF_ITEMSGIVEN_CHAPTER_1")->set_int(1);
  auto* stocked = script.findSymbol("KARDIF_ITEMSGIVEN_CHAPTER_2");
  auto* sign = script.findSymbol("KNOWS_SECRETSIGN");
  const auto id = script.findSymbolIndex("ITKE_LOCKPICK");
  const auto recipientBefore = recipient->itemCount(id), merchantBefore = kardif->itemCount(id);
  const auto* restock = script.findSymbol("B_GIVETRADEINV_KARDIF");
  bool ranVm = false;
  vm.observe_instructions([&](const auto&) { ranVm |= vm.pc()==restock->address(); });
  for(const bool known : {false,true}) {
    stocked->set_int(0);
    sign->set_int(known ? 1 : 0);
    vm.push_instance(merchant);
    vm.unsafe_call_bl(restock);
    require(stocked->get_int()==1 && kardif->itemCount(id)==merchantBefore &&
            recipient->itemCount(id)==recipientBefore+(known ? 20u : 0u),
            "trade inventory: Kardif completes the batch either way and gives the sign bonus to live SELF");
    }
  require(ranVm!=native,"trade inventory: Kardif uses the selected executor");
  vm.observe_instructions({});
  vm.global_self()->set_instance(oldSelf);
  }

void stockDeathTrophies(ActivityEventsWorld& test, bool native) {
  auto& world = test.world();
  auto& script = world.script();
  auto& vm = script.getVm();
  auto* wolf = activityNpc(world,"WOLF");
  require(wolf && wolf->handle().aivar[43]==script.findSymbol("ID_WOLF")->get_int(),
          "death trophies: need the stock wolf and species ID");
  const auto actor = std::static_pointer_cast<daedalus::INpc>(wolf->handlePtr());
  const auto oldSelf = vm.global_self()->get_instance();
  vm.global_self()->set_instance(actor);
  auto* learned = script.findSymbol("PLAYER_TALENT_TAKEANIMALTROPHY");
  for(uint16_t i=0;i<learned->count();++i)
    learned->set_int(i==0 || i==2 ? 1 : 0,i);
  actor->aivar[11] = 0;
  script.setSeed(73106);
  auto expectedRandom = NpcDefinitionContract::random(script);
  const auto green = int32_t(uint32_t(expectedRandom())%100u);
  const auto black = int32_t(uint32_t(expectedRandom())%100u);
  const auto orc = int32_t(uint32_t(expectedRandom())%10u);
  const auto teeth = script.findSymbolIndex("ITAT_TEETH");
  const auto fur = script.findSymbolIndex("ITAT_WOLFFUR");
  const auto beforeTeeth = wolf->itemCount(teeth), beforeFur = wolf->itemCount(fur);
  const auto* loot = script.findSymbol("B_GIVEDEATHINV");
  size_t bodies = 0;
  std::vector<std::string> calls;
  vm.observe_instructions([&](const auto&) { bodies+=vm.pc()==loot->address(); });
  vm.observe_externals([&](const auto& fn) {
    if(fn.name()=="CREATEINVITEMS" || fn.name()=="HLP_RANDOM")
      calls.push_back(fn.name());
    });
  const auto run = [&] {
    const auto gi = vm.unsafe_get_gi();
    vm.push_int(0x12345678);
    vm.push_reference(vm.global_self());
    vm.unsafe_call_bl(loot);
    require(vm.pop_int()==0x12345678 && vm.unsafe_get_gi()==gi,"death trophies: the call restores the stack and member context");
    };
  run();
  const auto lastItem = vm.global_item()->get_instance();
  require(wolf->itemCount(teeth)==beforeTeeth+2 && wolf->itemCount(fur)==beforeFur+1 && actor->aivar[11]==1 &&
          script.findSymbol("B_GIVEDEATHINV.SLF")->get_instance()==actor && lastItem && lastItem->symbol_index()==fur,
          "death trophies: learned teeth and fur use the original grant order, ITEM and saved latch");
  require(calls==std::vector<std::string>{"CREATEINVITEMS","CREATEINVITEMS","HLP_RANDOM","HLP_RANDOM","HLP_RANDOM"} &&
          script.findSymbol("GOBLINGREEN_RANDOMIZER")->get_int()==green &&
          script.findSymbol("GOBLINBLACK_RANDOMIZER")->get_int()==black &&
          script.findSymbol("ORC_RANDOMIZER")->get_int()==orc && NpcDefinitionContract::random(script)==expectedRandom,
          "death trophies: all three draws and global results are retained even for wolves");
  calls.clear();
  run();
  require(calls.empty() && bodies==(native ? 0u : 2u) && vm.global_item()->get_instance()==lastItem,
          "death trophies: the saved latch skips grants and draws on the second call");
  vm.observe_externals({});
  vm.observe_instructions({});
  vm.global_self()->set_instance(oldSelf);
  }

void npcDeathLifecycle(Gothic& gothic) {
  using Contract=NpcDefinitionContract;
  constexpr auto* actor="BAU_951_HILDA";
  std::string program;
  loadBaseProgram(gothic.defaultGameDatFile(),Settings::getI("GAME","language"),&program);
  if(program!="a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c") {
    std::puts("SKIP death: the fixture is for the verified NotR program");
    return;
    }

  auto save=[&](const char* label) {
    std::vector<uint8_t> bytes;
    {
      Tempest::MemWriter writer(bytes);
      Serialize out(writer);
      Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
      std::memset(preview.data(),0,4);
      gothic.gameSession()->save(out,label,preview);
    }
    return bytes;
    };
  auto load=[&](const std::vector<uint8_t>& bytes) {
    gothic.clearGame().reset();
    Tempest::MemReader reader(bytes);
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
    };
  auto consequences=[](Npc& npc,const Npc& hero) {
    std::map<size_t,size_t> loot;
    for(auto item=npc.inventory().iterator(Inventory::T_Ransack);item.isValid();++item)
      loot[item->clsId()]+=item.count();
    return std::tuple{activityQueue(npc),loot,std::to_array(npc.handle().aivar),hero.experience()};
    };
  using Consequences=decltype(consequences(std::declval<Npc&>(),std::declval<const Npc&>()));

  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  {
    ActivityEventsWorld test(gothic);
    test.only({});
    test.step(24);
  }
  const auto alive=save("before death state");
  Consequences original;
  std::vector<uint8_t> corpse;
  for(bool native:{false,true}) {
    load(alive);
    ActivityEventsWorld test(gothic);
    auto& world=test.world();
    auto& script=world.script();
    auto& vm=script.getVm();
    auto& hero=*world.player();
    auto* npc=activityNpc(world,actor);
    require(npc && !npc->isDead() && !npc->isImmortal() && npc->attribute(ATR_HITPOINTS)>0,
            "death: the fixture needs a living Hilda");
    test.stop(*npc);
    npc->setSimTier(NpcSimTier::Full);
    npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
    script.setSeed(73106);
    Contract::activityEventsOn(script)=native;
    npc->handle().aivar[16]=0; // This defeat has not yet awarded experience.
    const auto light=script.findSymbolIndex("ITRU_LIGHT");
    const auto paladinLight=script.findSymbolIndex("ITRU_PALLIGHT");
    npc->addItem(light,5);
    npc->addItem(paladinLight,2);
    const auto beforeXp=hero.experience();
    const auto dead=script.findFunction("ZS_DEAD");
    const auto initPc=script.findSymbol("ZS_DEAD")->address();
    const auto loopPc=script.findSymbol("ZS_DEAD_LOOP")->address();
    const auto runePc=script.findSymbol("B_CLEARRUNEINV")->address();
    const auto razorPc=script.findSymbol("C_IAMCANYONRAZOR")->address();
    const auto dropPc=script.findSymbol("C_DROPUNCONSCIOUS")->address();
    size_t initVm=0,loopVm=0,runeVm=0,conditionalVm=0,deathLoot=0,dragonChecks=0;
    const auto calls=vm.add_call_observer([&](const auto& symbol) {
      deathLoot+=symbol.name()=="B_GIVEDEATHINV";
      dragonChecks+=symbol.name()=="B_DRAGONKILLCOUNTER";
      });
    vm.observe_instructions([&](const auto&) {
      initVm+=vm.pc()==initPc;
      loopVm+=vm.pc()==loopPc;
      runeVm+=vm.pc()==runePc;
      conditionalVm+=vm.pc()==razorPc || vm.pc()==dropPc;
      });
    npc->changeAttribute(ATR_HITPOINTS,-npc->attribute(ATR_HITPOINTS),false);
    npc->setOther(&hero);
    require(npc->isDead() && npc->isInState(dead) && !Contract::aiState(*npc).started,
            "death: the engine enters the original death state");
    vm.push_int(0x12345678);
    Contract::tickRoutine(*npc);
    Contract::tickRoutine(*npc);
    Contract::tickRoutine(*npc);
    require(vm.pop_int()==0x12345678,"death: the first Loop discards the dragon helper result and preserves the caller stack");
    vm.observe_instructions({});
    vm.remove_call_observer(calls);
    require(initVm==(native ? 0u : 1u) && loopVm==(native ? 0u : 2u) && deathLoot==1 && dragonChecks==1,
            "death: Init grants loot once and the first Loop alone checks the dragon counter");
    require(npc->stateOther()==&hero && hero.experience()>beforeXp && npc->handle().aivar[16]==1,
            "death: the credited player receives experience and remains the state's other");
    require(runeVm==(native ? 0u : 2u) && npc->itemCount(light)==3 && npc->itemCount(paladinLight)==0,
            "death: both cleanup calls preserve the stock cross-count for the two light runes");
    require(conditionalVm==(native ? 0u : 2u),
            "death: canyon identity and the rule for lethal damage use the selected executor");
    auto outcome=consequences(*npc,hero);
    if(!native) original=std::move(outcome);
    else {
      require(outcome==original,"death: the two executors agree on queue, loot, flags and experience");
      npc->setStateTime(750);
      corpse=save("inside death state");
      }
    tradeInventoryAfterDefeat(test,native);
    kardifStockWithSecretSign(test,native);
    dragonDeathBranches(gothic,native);
    stockDeathTrophies(test,native);
    }

  load(corpse);
  auto& world=*gothic.world();
  auto& script=world.script();
  {
    auto* canyon=script.findSymbol("CANYONRAZORBODYCOUNT");
    auto* swamp=script.findSymbol("SWAPDRAGNISDEAD");
    const auto oldCanyon=canyon->get_int(), oldSwamp=swamp->get_int();
    std::vector<uint8_t> legacy;
    Tempest::MemWriter writer(legacy);
    {
      Serialize out(writer);
      out.setEntry("game/daedalus");
      out.write(uint32_t(2));
      out.write(uint32_t(daedalus::DaedalusDataType::INT),canyon->name(),uint32_t(1),int32_t(7));
      out.write(uint32_t(daedalus::DaedalusDataType::INT),swamp->name(),uint32_t(1),int32_t(2));
    }
    Tempest::MemReader reader(legacy);
    Serialize in(reader);
    require(in.setEntry("game/daedalus"),"death progress: legacy save entry is present");
    script.loadVar(in);
    const auto& progress=Contract::deathProgress(script);
    require(progress.canyonRazors==7 && progress.dragonDeaths[0]==2 &&
            canyon->get_int()==progress.canyonRazors && swamp->get_int()==progress.dragonDeaths[0],
            "death progress: legacy save values are visible in engine storage and script aliases");
    canyon->set_int(oldCanyon);
    swamp->set_int(oldSwamp);
  }
  auto& hero=*world.player();
  auto* npc=activityNpc(world,actor);
  require(npc && npc->isDead() && Contract::aiState(*npc).started && npc->stateOther()==&hero &&
          npc->stateTime()==750 && consequences(*npc,hero)==original,
          "death: normal load restores the corpse, credited player, time, loot and queue");
  {
    ActivityStream stream(world,*npc);
    auto& vm=script.getVm();
    size_t repeated=0;
    const auto calls=vm.add_call_observer([&](const auto& symbol) {
      repeated+=symbol.name()=="B_GIVEPLAYERXP" || symbol.name()=="B_GIVEDEATHINV" || symbol.name()=="B_DRAGONKILLCOUNTER";
      });
    Contract::tickRoutine(*npc);
    vm.remove_call_observer(calls);
    require(activityCount(stream.events,"phase ZS_DEAD")==0 && activityCount(stream.events,"phase ZS_DEAD_LOOP")==1 &&
            stream.records==1 && repeated==0 && consequences(*npc,hero)==original,
            "death: a loaded corpse continues the native Loop without repeating rewards or the first check");
  }
  gothic.clearGame().reset();
  std::puts("Death lifecycle: native Init and Loop match Daedalus, including the ordinary corpse load");
  }

void stockItemRestoreCoverage(GameScript& script) {
  using Op=daedalus::DaedalusOpcode;
  using Type=daedalus::DaedalusDataType;
  const auto& vm=script.getVm();
  const auto& catalog=NpcDefinitionContract::items(script);
  const auto& effects=catalog.restoreEffects;
  const auto self=vm.find_symbol_by_name("SELF")->index();
  const auto change=vm.find_symbol_by_name("NPC_CHANGEATTRIBUTE")->index();
  const auto attributes=vm.find_symbol_by_name("C_NPC.ATTRIBUTE")->index();
  size_t health=0,mana=0,paired=0,full=0;
  for(const auto& fn:vm.symbols()) {
    if(!fn.name().starts_with("USE") || fn.type()!=Type::FUNCTION || fn.rtype()!=Type::VOID ||
       !fn.is_const() || fn.is_external() || fn.address()>=vm.size())
      continue;
    auto pc=fn.address();
    std::array<daedalus::DaedalusInstruction,9> ops{};
    for(auto& op:ops) {
      if(pc>=vm.size()) break;
      op=vm.instruction_at(pc);
      pc+=op.size;
      if(op.op==Op::RSR) break;
      }
    const auto attributeAt=[&](size_t from) {
      return ops[from].op==Op::GMOVI && ops[from].symbol==self && ops[from+1].symbol==attributes &&
             (ops[from+1].op==Op::PUSHV || ops[from+1].op==Op::PUSHVV);
      };
    if(attributeAt(0) && attributeAt(2) && ops[4].op==Op::MOVI && ops[5].op==Op::RSR) {
      const auto row=catalog.fullRestoreEffects.find(fn.index());
      require(row!=catalog.fullRestoreEffects.end() && row->second.attribute==ops[3].index &&
              row->second.maximum==ops[1].index,"stock restores: the full elixir copies the exact maximum slot");
      ++full;
      continue;
      }
    const auto changeAt=[&](size_t from) {
      return ops[from].op==Op::PUSHVI && ops[from].symbol==self && ops[from+1].op==Op::PUSHV &&
             (ops[from+2].op==Op::PUSHV || ops[from+2].op==Op::PUSHI) &&
             ops[from+3].op==Op::BE && ops[from+3].symbol==change;
      };
    const bool single=ops[4].op==Op::RSR;
    if(!changeAt(0) || (!single && (!changeAt(4) || ops[8].op!=Op::RSR)))
      continue;
    const auto effect=effects.find(fn.index());
    require(effect!=effects.end() && effect->second.second.has_value()!=single,
            ("stock restores: exact number of changes for "+fn.name()).c_str());
    const auto check=[&](size_t from, const ItemDefinitionRuntime::RestoreChange& step) {
      const auto* attr=vm.find_symbol_by_index(ops[from+1].symbol);
      require(attr && attr->is_const() && attr->type()==Type::INT && attr->count()==1,
              "stock restores: current attribute must be a scalar constant");
      const auto value=attr->get_int();
      require((value==ATR_HITPOINTS || value==ATR_MANA) && step.attribute==value,
              ("stock restores: exact attribute for "+fn.name()).c_str());
      const auto& operand=ops[from+2];
      if(operand.op==Op::PUSHI) {
        require(operand.immediate>0 && std::holds_alternative<int32_t>(step.amount) &&
                std::get<int32_t>(step.amount)==operand.immediate,"stock restores: retain the positive literal dose");
        } else {
        const auto* dose=vm.find_symbol_by_index(operand.symbol);
        require(dose && dose->is_const() && dose->type()==Type::INT && dose->count()==1 && dose->get_int()>0 &&
                std::holds_alternative<uint32_t>(step.amount) && std::get<uint32_t>(step.amount)==operand.symbol,
                "stock restores: retain the exact positive dose symbol");
        }
      };
    check(0,effect->second.first);
    if(effect->second.second) {
      check(4,*effect->second.second);
      ++paired;
      } else {
      health+=effect->second.first.attribute==ATR_HITPOINTS;
      mana+=effect->second.first.attribute==ATR_MANA;
      }
    }
  require(effects.size()==health+mana+paired && health==30 && mana==7 && paired==5 &&
          catalog.fullRestoreEffects.size()==full && full==2,
          "stock restores: cover all 37 single, five paired and two raw full restorations");
  require(catalog.permanentAttributeEffects.size()==7,"stock effects: cover seven permanent elixirs and plants");
  }

auto stockEquipmentHelpers(ActivityEventsWorld& test, Npc& hero, const Npc& reserved, bool native) {
  using Contract=NpcDefinitionContract;
  auto& script=test.world().script();
  auto& vm=script.getVm();
  auto* spare=test.extra();
  auto& actor=*(spare==&reserved ? test.extra() : spare);
  test.stop(actor);
  actor.setSimTier(NpcSimTier::Full);
  actor.setProcessPolicy(NpcProcessPolicy::AiNormal);
  actor.closeWeapon(true);
  actor.clearInventory();
  actor.handle().guild=GIL_BAU;
  actor.handle().type=NpcType::G2_AMBIENT;
  actor.handle().aivar[43]=0; // AIV_MM_REAL_ID
  actor.handle().aivar[51]=0; // AIV_MAGICUSER
  const auto self=std::static_pointer_cast<daedalus::INpc>(actor.handlePtr());
  const auto other=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto previousSelf=vm.global_self()->get_instance();
  const auto previousItem=vm.global_item()->get_instance();
  vm.global_self()->set_instance(self);
  auto& create=*script.findSymbol("B_CREATEAMMO");
  auto& select=*script.findSymbol("B_SELECTWEAPON");
  auto& spellChoice=*script.findSymbol("B_SELECTSPELL");
  auto& ready=*script.findSymbol("B_READYSPELL");
  auto& kill=*script.findSymbol("C_NPCHASATTACKREASONTOKILL");
  auto& evil=*script.findSymbol("C_NPCISEVIL");
  auto& undead=*script.findSymbol("C_NPCISUNDEAD");
  size_t createBody=0,selectBody=0,spellChoiceBody=0,readyBody=0,killBody=0,evilBody=0,undeadBody=0;
  vm.observe_instructions([&](const auto&) {
    createBody+=vm.pc()==create.address();
    selectBody+=vm.pc()==select.address();
    spellChoiceBody+=vm.pc()==spellChoice.address();
    readyBody+=vm.pc()==ready.address();
    killBody+=vm.pc()==kill.address();
    evilBody+=vm.pc()==evil.address();
    undeadBody+=vm.pc()==undead.address();
    });
  const auto locals=[&](std::string_view name) { return script.findSymbol(name)->get_instance(); };
  auto refill=[&] {
    vm.push_instance(self);
    vm.unsafe_call_bl(&create);
    require(locals("B_CREATEAMMO.SLF")==self,"equipment: ammo commits its saved NPC argument");
    };
  const auto arrows=script.findSymbolIndex("ITRW_ARROW"),bolts=script.findSymbolIndex("ITRW_BOLT");
  const auto bow=script.findSymbolIndex("ITRW_BOW_L_01"),crossbow=script.findSymbolIndex("ITRW_CROSSBOW_L_01");
  const auto sword=script.findSymbolIndex("ITMW_1H_BAU_AXE");
  script.setSeed(73106);
  auto random=Contract::random(script);
  actor.setToBowMode(bow);
  actor.closeWeapon(true);
  actor.addItem(arrows,9);
  const auto bowRef=std::static_pointer_cast<daedalus::IItem>(actor.getItem(bow)->handlePtr());
  refill();
  require(actor.itemCount(arrows)==19 && locals("B_CREATEAMMO.RANGEDWEAPON")==bowRef &&
          vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(actor.getItem(arrows)->handlePtr()),
          "equipment: an equipped bow below ten gains ten arrows and sets ITEM");
  actor.unequipItem(bow);
  refill();
  require(actor.itemCount(arrows)==19 && locals("B_CREATEAMMO.RANGEDWEAPON")==bowRef,
          "equipment: without a ranged weapon the previous saved weapon stays in place");
  actor.setToBowMode(crossbow);
  actor.addItem(bolts,10);
  auto* readied=actor.activeWeapon();
  require(readied && readied->clsId()==crossbow && actor.weaponState()==WeaponState::CBow,
          "equipment: the threshold check needs a drawn crossbow");
  const auto crossbowRef=std::static_pointer_cast<daedalus::IItem>(readied->handlePtr());
  auto& crossbowSymbol=*script.findSymbol(crossbow);
  const auto previousAmmo=std::static_pointer_cast<daedalus::IItem>(actor.getItem(arrows)->handlePtr());
  crossbowSymbol.set_instance(nullptr);
  vm.global_item()->set_instance(previousAmmo);
  refill();
  require(actor.itemCount(bolts)==10 && locals("B_CREATEAMMO.RANGEDWEAPON")==crossbowRef,
          "equipment: exactly ten bolts stay unchanged and the readied weapon is saved");
  require(crossbowSymbol.get_instance()==crossbowRef && vm.global_item()->get_instance()==previousAmmo,
          "equipment: reading the crossbow publishes its instance and preserves ITEM");
  require(Contract::random(script)==random,"equipment: ammo checks do not draw RNG");
  actor.delItem(bolts,1);
  refill();
  require(actor.itemCount(bolts)==19 &&
          vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(actor.getItem(bolts)->handlePtr()),
          "equipment: a readied crossbow below ten gains ten bolts and sets ITEM to the ammo");

  const auto oldHeroPosition=hero.position();
  actor.setPosition({1000,2000,3000});
  actor.handle().aivar[9]=script.findSymbol("AR_KILL")->get_int();
  auto choose=[&](int distance) {
    const auto heroCenterOffset=hero.centerPosition()-hero.position();
    hero.setPosition(actor.centerPosition()-heroCenterOffset+Tempest::Vec3(float(distance)+0.25f,0,0));
    require(int(std::sqrt(actor.qDistTo(hero)))==distance,"equipment: match the script's truncated distance");
    actor.clearAiQueue();
    vm.push_instance(self);
    vm.push_instance(other);
    vm.unsafe_call_bl(&select);
    require(locals("B_SELECTWEAPON.SLF")==self && locals("B_SELECTWEAPON.OTH")==other &&
            locals("C_BODYSTATECONTAINS.SLF")==self && script.findSymbol("C_BODYSTATECONTAINS.BODYSTATE")->get_int()==BS_FALL,
            "equipment: select preserves the original NPC and final body-check arguments");
    require(locals("B_SELECTSPELL.SLF")==self && locals("B_SELECTSPELL.OTH")==other,
            "equipment: spell choice saves both original NPC arguments");
    return Contract::queue(actor);
    };
  actor.setToFightMode(sword);
  actor.setToBowMode(crossbow);
  auto close=choose(900);
  require(close.size()==2 && close.pop().act==AI_RemoveWeapon && close.pop().act==AI_DrawWeaponMelee &&
          choose(901).size()==0,"equipment: a drawn ranged weapon switches at 900 cm and stays drawn at 901 cm");
  actor.setToFightMode(sword);
  require(choose(1000).size()==0,"equipment: melee stays drawn at 1000 cm");
  auto distant=choose(1001);
  require(distant.size()==2 && distant.pop().act==AI_RemoveWeapon && distant.pop().act==AI_DrawWeaponRange &&
          Contract::random(script)==random,"equipment: a lethal reason readies ranged beyond 1000 cm without RNG");

  actor.closeWeapon(true);
  actor.setAnim(Npc::Anim::Idle);
  actor.handle().guild=GIL_DMT;
  actor.handle().aivar[51]=script.findSymbol("MAGIC_ALWAYS")->get_int();
  actor.handle().aivar[20]=0;
  const auto expectedRoll=int32_t(random()%10);
  choose(1500);
  require(Contract::random(script)==random && script.findSymbol("B_SELECTSPELL.DK_RND")->get_int()==expectedRoll &&
          actor.handle().aivar[20]==expectedRoll,
          "equipment: the spell-selection branch preserves the original seeded draw and saved local");
  require(createBody==(native ? 0u : 4u) && selectBody==(native ? 0u : 5u) && spellChoiceBody==(native ? 0u : 5u),
          "equipment: the stock weapon path runs ammo, weapon and spell choice in the selected executor");
  std::vector<size_t> inventory;
  for(auto item=actor.inventory().iterator(Inventory::T_Ransack);item.isValid();++item)
    inventory.push_back(item->clsId());
  const auto darkMageQueue=activityQueue(actor);
  require(native ? readyBody==0 : readyBody>0,"equipment: the spell selector reaches the selected ready-spell executor");

  actor.handle().guild=GIL_SKELETON_MAGE;
  actor.handle().aivar[51]=0;
  actor.handle().aivar[20]=6;
  actor.clearAiQueue();
  const auto previousChoiceBody=spellChoiceBody;
  vm.push_int(0x12345678);
  vm.push_instance(self);
  vm.push_instance(other);
  vm.unsafe_call_bl(&spellChoice);
  const auto choice=vm.pop_int(),stack=vm.pop_int();
  auto summon=Contract::queue(actor);
  require(choice==1 && stack==0x12345678 && spellChoiceBody-previousChoiceBody==(native ? 0u : 1u) &&
          actor.handle().aivar[20]==1 && summon.size()==1,
          "equipment: a direct skeleton spell choice wraps the saved ordinal through the selected executor");
  const auto summons=summon.pop();
  require(summons.act==AI_DrawSpell && summons.i0==script.findSymbol("SPL_SUMMONSKELETON")->get_int() &&
          summons.i1==script.findSymbol("SPL_COST_SUMMONSKELETON")->get_int() && Contract::random(script)==random,
          "equipment: the wrapped skeleton choice prepares the stock summon without another RNG draw");
  const auto heroGuild=hero.handle().guild,tactic=actor.handle().fight_tactic;
  hero.handle().guild=GIL_UNDEADORC;
  actor.handle().guild=GIL_PAL;
  actor.handle().fight_tactic=0;
  auto holyBolt=choose(601);
  require(holyBolt.size()==1 && locals("C_NPCISEVIL.SLF")==other && locals("C_NPCISUNDEAD.SLF")==other &&
          evilBody==(native ? 0u : 1u) && undeadBody==(native ? 0u : 1u),
          "equipment: paladin spell choice classifies the target through both stock predicates");
  const auto boltCommand=holyBolt.pop();
  require(boltCommand.act==AI_DrawSpell && boltCommand.i0==script.findSymbol("SPL_PALHOLYBOLT")->get_int(),
          "equipment: an undead orc qualifies for the paladin's holy bolt");
  hero.handle().guild=heroGuild;
  actor.handle().fight_tactic=tactic;
  actor.handle().guild=GIL_DMT;
  actor.handle().aivar[51]=script.findSymbol("MAGIC_ALWAYS")->get_int();
  actor.handle().aivar[20]=expectedRoll;

  const auto previousReadyBody=readyBody,previousKillBody=killBody;
  const auto fire=script.findSymbol("SPL_FIREBOLT")->get_int();
  const auto ice=script.findSymbol("SPL_ICEBOLT")->get_int();
  const auto useSpell=[&](int32_t spell,int32_t cost) {
    actor.clearAiQueue();
    vm.push_instance(self);
    vm.push_int(spell);
    vm.push_int(cost);
    vm.unsafe_call_bl(&ready);
    require(locals("B_READYSPELL.SLF")==self && script.findSymbol("B_READYSPELL.SPELL")->get_int()==spell &&
            script.findSymbol("B_READYSPELL.MANA")->get_int()==cost,"equipment: ready spell saves all three parameters");
    return Contract::queue(actor);
    };
  auto& attributes=actor.handle().attribute;
  attributes[ATR_MANA]=3;
  attributes[ATR_MANAMAX]=5;
  auto first=useSpell(fire,20);
  require(attributes[ATR_MANA]==23 && attributes[ATR_MANAMAX]==25 && first.size()==1,
          "equipment: apply the full cost addends to both short mana attributes before queuing");
  const auto draw=first.pop();
  require(draw.act==AI_DrawSpell && draw.i0==fire && draw.i1==20,"equipment: ready command keeps spell and cost");

  const auto scrollId=script.findSymbolIndex("ITSC_FIREBOLT");
  auto* scroll=actor.addItem(scrollId,1);
  require(scroll && actor.drawSpell(fire) && actor.activeWeapon()==scroll,"equipment: the actor has the stock scroll drawn");
  const auto scrollRef=std::static_pointer_cast<daedalus::IItem>(scroll->handlePtr());
  auto& scrollSymbol=*script.findSymbol(scrollId);
  vm.global_item()->set_instance(previousAmmo);
  const auto publishesScroll=[&] {
    return scrollSymbol.get_instance()==scrollRef && vm.global_item()->get_instance()==previousAmmo;
    };
  scrollSymbol.set_instance(nullptr);
  require(useSpell(int32_t(scrollId),26).size()==0 && attributes[ATR_MANA]==49 && attributes[ATR_MANAMAX]==51 && publishesScroll(),
          "equipment: prepare mana before publishing the drawn scroll on its own symbol");
  scrollSymbol.set_instance(nullptr);
  require(useSpell(fire,20).size()==0 && publishesScroll(),
          "equipment: the already active spell is published without queuing it twice");
  scrollSymbol.set_instance(nullptr);
  auto next=useSpell(ice,20);
  require(next.size()==2 && next.pop().act==AI_RemoveWeapon,"equipment: remove an active spell before switching");
  const auto drawNext=next.pop();
  require(drawNext.act==AI_DrawSpell && drawNext.i0==ice && drawNext.i1==20 && publishesScroll(),
          "equipment: a spell switch publishes the active scroll and queues the replacement");

  for(const auto* reason:{"AR_NONE","AR_KILL","AR_GUILDENEMY","AR_GUARDCALLEDTOKILL","AR_GUARDSTOPSINTRUDER","AR_HUMANMURDEREDHUMAN"}) {
    actor.handle().aivar[9]=script.findSymbol(reason)->get_int();
    vm.push_instance(self);
    vm.unsafe_call_bl(&kill);
    require(vm.pop_int()==int(std::string_view(reason)!="AR_NONE") && locals("C_NPCHASATTACKREASONTOKILL.SLF")==self,
            "equipment: all five lethal reasons match the original and save the actor");
    }
  require(readyBody-previousReadyBody==(native ? 0u : 4u) && killBody-previousKillBody==(native ? 0u : 6u),
          "equipment: both new helpers use the native or reference executor for their BL entry");
  actor.closeWeapon(true);
  actor.clearAiQueue();
  vm.observe_instructions({});
  vm.global_self()->set_instance(previousSelf);
  vm.global_item()->set_instance(previousItem);
  hero.setPosition(oldHeroPosition);
  return std::make_pair(std::move(inventory),darkMageQueue);
  }

auto stockArmorSetEffects(World& world, bool native) {
  auto& script=world.script();
  auto& hero=*world.player();
  auto* mika=activityNpc(world,"MIL_337_MIKA");
  const auto armor=script.findSymbolIndex("ITAR_MIL_L");
  const auto belt=script.findSymbolIndex("ITBE_ADDON_MIL_01");
  auto& armorFlag=*script.findSymbol("MILARMOR_EQUIPPED");
  auto& beltFlag=*script.findSymbol("MIL01_EQUIPPED");
  const auto bonus=script.findSymbol("BA_BONUS01")->get_int();
  require(mika && mika->currentArmor() && mika->currentArmor()->clsId()==armor && !hero.currentArmor() &&
          armorFlag.get_int()==0 && beltFlag.get_int()==0,"armor sets: the stock guard wears the armor and the hero starts without the set");
  const auto npcProtection=std::to_array(mika->handle().protection);
  size_t armorCalls=0;
  auto& vm=script.getVm();
  const auto observer=vm.add_call_observer([&](const auto& fn) {
    armorCalls+=fn.name()=="EQUIP_ITAR_MIL_L" || fn.name()=="UNEQUIP_ITAR_MIL_L";
    });
  require(hero.addItem(belt,1) && hero.addItem(armor,1),"armor sets: add the actual stock equipment");
  hero.useItem(belt);
  require(beltFlag.get_int()==1 && hero.getItem(belt)->isEquipped(),"armor sets: the matching belt must be worn through inventory");
  const auto beltOnly=std::to_array(hero.handle().protection);
  mika->unequipItem(armor);
  mika->useItem(armor);
  require(mika->currentArmor() && mika->currentArmor()->clsId()==armor && armorFlag.get_int()==0 &&
          std::to_array(mika->handle().protection)==npcProtection,
          "armor sets: the guard keeps ordinary armor protection with the hero's belt active");
  hero.useItem(armor);
  const auto withSet=std::to_array(hero.handle().protection);
  const auto* worn=hero.currentArmor();
  require(worn && worn->clsId()==armor && armorFlag.get_int()==1,"armor sets: the hero's armor must set the saved flag");
  for(size_t i=0;i<withSet.size();++i) {
    const bool affected=i==PROT_EDGE || i==PROT_BLUNT || i==PROT_POINT || i==PROT_MAGIC || i==PROT_FIRE;
    require(withSet[i]==beltOnly[i]+worn->handle().protection[i]+(affected ? bonus : 0),
            "armor sets: add base armor and the five matching belt bonuses in the original fields");
    }
  hero.unequipItem(armor);
  require(!hero.currentArmor() && armorFlag.get_int()==0 && std::to_array(hero.handle().protection)==beltOnly,
          "armor sets: removing the armor subtracts both contributions and clears the flag");
  hero.useItem(armor);
  vm.remove_call_observer(observer);
  require(armorCalls==(native ? 0u : 5u),"armor sets: Mika and the hero must use the selected callback executor");
  return std::pair{withSet,beltOnly};
  }

bool stockItemFightSkillEffects(World& world, bool native) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& hero=*world.player();
  auto* jora=activityNpc(world,"VLK_408_JORA");
  const auto alrikSword=script.findSymbolIndex("ITMW_ALRIKSSWORD_MIS");
  const auto sword=script.findSymbolIndex("ITMW_1H_VLK_SWORD");
  const auto club=script.findSymbolIndex("ITMW_ADDON_KEULE_1H_01");
  const auto staff=script.findSymbolIndex("ITMW_ADDON_STAB01");
  require(jora && jora->currentMeleeWeapon() && jora->currentMeleeWeapon()->clsId()==alrikSword,
          "weapon skill: the actual merchant must already wear Alrik's sword");
  if(const auto* prior=hero.currentMeleeWeapon())
    hero.unequipItem(prior->clsId());
  auto equip=[&](Npc& actor, size_t item) {
    require(actor.itemCount(item)!=0 || actor.addItem(item,1),"weapon skill: add the actual stock item");
    actor.useItem(item,Item::NSLOT,true);
    require(actor.currentMeleeWeapon() && actor.currentMeleeWeapon()->clsId()==item,
            "weapon skill: equip through the inventory slot");
    };
  auto setSkill=[](Npc& actor, Talent talent, int32_t chance, int32_t level) {
    actor.handle().hitchance[talent]=chance;
    actor.setTalentSkill(talent,level);
    };
  auto hasSkill=[](const Npc& actor, Talent talent, int32_t chance, int32_t level) {
    return actor.hitChance(talent)==chance && actor.talentSkill(talent)==level;
    };
  const auto& helpers=NpcDefinitionContract::definitions(script).helpers.helpers;
  const bool typed=native && NpcHelperRuntime::enabled() &&
                   helpers.contains(script.findSymbol("B_ADDFIGHTSKILL")->index());
  size_t weapons=0,helper=0,stages=0;
  const auto observer=vm.add_call_observer([&](const auto& fn) {
    weapons+=fn.name().starts_with("EQUIP_1H_") || fn.name().starts_with("UNEQUIP_1H_") ||
             fn.name().starts_with("EQUIP_2H_") || fn.name().starts_with("UNEQUIP_2H_") ||
             fn.name().ends_with("EQUIP_ALRIKSSWORD");
    helper+=fn.name()=="B_ADDFIGHTSKILL";
    });
  vm.observe_externals([&](const auto& fn) { stages+=fn.name()=="NPC_SETTALENTSKILL"; });
  setSkill(*jora,TALENT_1H,60,2);
  jora->unequipItem(alrikSword);
  require(hasSkill(*jora,TALENT_1H,50,1),"weapon skill: Alrik's sword removes the merchant's master rank");
  equip(*jora,sword);
  jora->unequipItem(sword);
  require(hasSkill(*jora,TALENT_1H,50,1) && script.findSymbol("B_ADDFIGHTSKILL.PERCENT")->get_int()==-10,
          "weapon skill: generic blades leave an NPC's skill and the helper arguments untouched");
  equip(*jora,alrikSword);
  require(hasSkill(*jora,TALENT_1H,60,2),"weapon skill: Alrik's sword restores the NPC's master rank");

  setSkill(hero,TALENT_1H,32,1);
  equip(hero,club);
  require(hasSkill(hero,TALENT_1H,22,0),"weapon skill: the stock club's reversed binding subtracts while worn");
  hero.unequipItem(club);
  require(hasSkill(hero,TALENT_1H,32,1),"weapon skill: removing the stock club restores the original rank");

  const auto oldTwoHand=std::pair{hero.hitChance(TALENT_2H),hero.talentSkill(TALENT_2H)};
  setSkill(hero,TALENT_1H,-1,0);
  setSkill(hero,TALENT_2H,20,2);
  equip(hero,staff);
  require(hasSkill(hero,TALENT_2H,23,2),"weapon skill: the two-handed novice check still reads the one-handed percentage");
  hero.unequipItem(staff);
  setSkill(hero,TALENT_2H,oldTwoHand.first,oldTwoHand.second);
  setSkill(hero,TALENT_1H,25,0);
  const auto context=vm.unsafe_get_gi();
  equip(hero,sword);
  vm.observe_externals({});
  vm.remove_call_observer(observer);
  const size_t expectedCallbacks=typed ? 0u : native ? 7u : 9u;
  require(hasSkill(hero,TALENT_1H,30,1) && weapons==expectedCallbacks && helper==(typed ? 0u : 7u) &&
          stages==10 && vm.unsafe_get_gi()==context,
          "weapon skill: the hero reaches the fighter threshold through the selected executor");
  return typed;
  }

void npcUnconsciousLifecycle(Gothic& gothic) {
  using Contract=NpcDefinitionContract;
  constexpr auto* actor="BAU_951_HILDA";
  std::string program;
  loadBaseProgram(gothic.defaultGameDatFile(),Settings::getI("GAME","language"),&program);
  if(program!="a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c") {
    std::puts("SKIP unconscious: the fixture is for the verified NotR program");
    return;
    }

  auto recoveryQueue=[](const Npc& npc,ScriptFn heal,const Npc& hero) {
    auto queue=Contract::queue(npc);
    require(queue.size()>=4 && queue.pop().act==AI_StandUp,
            "unconscious: recovery starts with standing up");
    auto next=queue.pop();
    if(next.act==AI_TakeItem)
      next=queue.pop();
    require(next.act==AI_EquipMelee && queue.pop().act==AI_EquipRange && queue.size()==1,
            "unconscious: recovery retrieves any weapon before equipping melee and ranged");
    const auto start=queue.pop();
    require(start.act==AI_StartState && start.func==heal && start.target==&hero,
            "unconscious: recovery queues self-healing with the original other");
    return activityQueue(npc);
    };
  auto save=[&](std::vector<uint8_t>& bytes,std::string_view label) {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,label,preview);
    };
  const auto potionQueue=[](const Npc& npc,uint32_t potion) {
    auto queue=Contract::queue(npc);
    require(queue.size()==1,"healing: a single potion use is queued");
    const auto use=queue.pop();
    require(use.act==AI_UseItem && uint32_t(use.i0)==potion && npc.itemCount(potion)>0,
            "healing: enqueue the strongest available potion through the item-use pipeline");
    };
  std::vector<std::string> originalInit,originalEnd;
  std::vector<uint8_t> knockedOut,healing,pendingPotion;
  std::pair<std::vector<size_t>,std::vector<std::string>> stockEquipment;
  std::pair<std::array<int32_t,PROT_MAX>,std::array<int32_t,PROT_MAX>> stockArmor;
  bool typedWeaponEffects=false;
  int32_t originalXp=0;
  for(bool native:{false,true}) {
    gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
    ActivityEventsWorld test(gothic);
    auto& world=test.world();
    auto& script=world.script();
    auto& hero=*world.player();
    auto* npc=activityNpc(world,actor);
    require(npc && npc->attribute(ATR_HITPOINTS)>0 && (npc->position()-hero.position()).length()>1000,
            "unconscious: the fixture needs a living Hilda away from the hero");
    test.stop(*npc);
    npc->setSimTier(NpcSimTier::Full);
    npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
    npc->setAttitude(ATT_NEUTRAL);
    npc->setOther(&hero);
    npc->handle().aivar[16]=0; // AIV_VictoryXPGiven
    Contract::activityEventsOn(script)=native;
    if(native) stockItemRestoreCoverage(script);
    const auto equipment=stockEquipmentHelpers(test,hero,*npc,native);
    if(native) require(equipment==stockEquipment,"equipment: spell preparation matches stock inventory contents and queue order");
    else stockEquipment=equipment;
    const auto armor=stockArmorSetEffects(world,native);
    if(native) require(armor==stockArmor,"armor sets: native protection matches the stock callbacks");
    else stockArmor=armor;
    typedWeaponEffects=stockItemFightSkillEffects(world,native);
    script.setSeed(73106);
    const auto beforeXp=hero.experience();
    const auto wakeAt=int64_t(script.findSymbol("HAI_TIME_UNCONSCIOUS")->get_int())*1000;
    auto& vm=script.getVm();
    size_t phaseCalls=0,rewards=0,minimalCalls=0,restoreCalls=0;
    const auto observer=vm.add_call_observer([&](const auto& symbol) {
      phaseCalls+=symbol.name().starts_with("ZS_UNCONSCIOUS");
      rewards+=symbol.name()=="B_GIVEPLAYERXP";
      minimalCalls+=symbol.name()=="PERCEPTION_SET_MINIMAL";
      restoreCalls+=symbol.name().starts_with("USEITPO_HEALTH_") || symbol.name()=="USEITPO_MANA_01" ||
                    symbol.name()=="USEITPO_MANA_04" || symbol.name()=="USE_MEATBUGFLESH" ||
                    symbol.name()=="USE_CHEESE" || symbol.name()=="USE_BEER" || symbol.name()=="USE_BLUEPLANT" ||
                    symbol.name()=="USEITPO_PERM_DEX" || symbol.name()=="USEITPO_LITTLEMANA" ||
                    symbol.name()=="USE_APPLE" || symbol.name()=="USE_MUSHROOM_01" ||
                    symbol.name()=="USE_MEATSOUP" || symbol.name()=="USEITPO_GEIST_01" ||
                    symbol.name()=="USEITPO_MEGADRINK" || symbol.name()=="USELOUHAMMER";
      });

    require(Settings::physicsFlags().meleeHealInterrupt,"health potions: the fixture uses deferred potion effects");
    const auto attributes=std::to_array(npc->handle().attribute);
    const auto heroHealth=hero.attribute(ATR_HITPOINTS);
    const auto previousSelf=vm.global_self()->get_instance();
    vm.global_self()->set_instance(std::static_pointer_cast<daedalus::INpc>(hero.handlePtr()));
    const auto self=vm.global_self()->get_instance(),other=vm.global_other()->get_instance(),
               victim=vm.global_victim()->get_instance(),itemGlobal=vm.global_item()->get_instance();
    auto* apples=script.findSymbol("APPLE_BONUS");
    auto* mushrooms=script.findSymbol("DUNKELPILZ_BONUS");
    const auto oldApples=apples->get_int(),oldMushrooms=mushrooms->get_int();
    apples->set_int(24);
    mushrooms->set_int(49);
    const std::array potionUse={std::tuple{"ITPO_HEALTH_01",ATR_HITPOINTS,20,70},
                                std::tuple{"ITPO_HEALTH_02",ATR_HITPOINTS,20,90},
                                std::tuple{"ITPO_HEALTH_03",ATR_HITPOINTS,20,120},
                                std::tuple{"ITPO_HEALTH_03",ATR_HITPOINTS,125,200},
                                std::tuple{"ITPO_MANA_01",ATR_MANA,20,70},
                                std::tuple{"ITPO_HEALTH_ADDON_04",ATR_HITPOINTS,37,200},
                                std::tuple{"ITPO_MANA_ADDON_04",ATR_MANA,220,200}};
    npc->handle().attribute[ATR_HITPOINTSMAX]=200;
    npc->handle().attribute[ATR_MANAMAX]=200;
    auto startUse=[&](std::string_view item) {
      const auto id=script.findSymbolIndex(item);
      const auto initialItems=npc->itemCount(id);
      require(npc->addItem(id,1)!=nullptr,"item restores: add a real stock item to the NPC");
      npc->setAnim(Npc::Anim::Idle);
      npc->aiPush(AiQueue::aiUseItem(int32_t(id)));
      Contract::nextAiAction(*npc);
      require(npc->isAiQueueEmpty() && Contract::pendingItemUse(*npc)==id && npc->itemCount(id)==initialItems+1,
              "item restores: the queue waits for the animation event");
      return std::pair{id,initialItems};
      };
    for(const auto& [item,attribute,before,after]:potionUse) {
      npc->handle().attribute[attribute]=before;
      const auto [id,initialItems]=startUse(item);
      require(npc->attribute(attribute)==before,"item restores: the potion has no effect before the tag");
      if(!native && std::string_view(item)=="ITPO_HEALTH_03" && before==125)
        save(pendingPotion,"VM-era save with a potion in use");
      Contract::finishItemUseAnimation(*npc);
      require(Contract::pendingItemUse(*npc)==0 && npc->attribute(attribute)==after &&
              npc->itemCount(id)==initialItems && hero.attribute(ATR_HITPOINTS)==heroHealth &&
              vm.global_self()->get_instance()==self && vm.global_other()->get_instance()==other &&
              vm.global_victim()->get_instance()==victim && vm.global_item()->get_instance()==itemGlobal,
              "item restores: the tag restores only the drinker once, consumes one item and restores the VM context");
      }
    const auto drinker=std::static_pointer_cast<daedalus::INpc>(npc->handlePtr());
    const auto oldAcrobat=npc->talentSkill(TALENT_ACROBAT);
    const auto dexTraining=npc->handle().aivar[82],manaTraining=npc->handle().aivar[83];
    npc->setTalentSkill(TALENT_ACROBAT,0);
    npc->handle().attribute[ATR_DEXTERITY]=87;
    npc->handle().attribute[ATR_MANA]=199;
    const auto permanentUse=std::array{std::tuple{"ITPO_PERM_DEX",ATR_DEXTERITY,87},
                                       std::tuple{"ITPO_PERM_LITTLEMANA",ATR_MANAMAX,200}};
    for(const auto& [item,attribute,before]:permanentUse) {
      const auto [id,initialItems]=startUse(item);
      require(npc->attribute(attribute)==before,"permanent items: the attribute changes at the animation tag");
      Contract::finishItemUseAnimation(*npc);
      require(npc->attribute(attribute)==before+3 && npc->itemCount(id)==initialItems &&
              script.findSymbol("B_RAISEATTRIBUTE.ATTRIB")->get_int()==attribute &&
              script.findSymbol("B_RAISEREALATTRIBUTELEARNCOUNTER.OTH")->get_instance()==drinker,
              "permanent items: consume once and store the same arguments as the VM");
      }
    require(npc->talentSkill(TALENT_ACROBAT)==1 && npc->attribute(ATR_MANA)==202 &&
            npc->handle().aivar[82]==dexTraining+3 && npc->handle().aivar[83]==manaTraining+3 &&
            vm.global_self()->get_instance()==self,
            "permanent items: unlock acrobatics at 90, raise maximum before refill and count permanent training");
    npc->setTalentSkill(TALENT_ACROBAT,oldAcrobat);
    npc->handle().aivar[82]=dexTraining;
    npc->handle().aivar[83]=manaTraining;
    require(Settings::physicsFlags().foodOverTime,"item restores: the fixture uses gradual food effects");
    npc->handle().attribute[ATR_HITPOINTS]=20;
    npc->handle().attribute[ATR_MANA]=20;
    const std::array foodUse={std::tuple{"ITFO_CHEESE",15,0},std::tuple{"ITFO_BEER",3,1},
                              std::tuple{"ITPL_BLUEPLANT",5,5},std::tuple{"ITAT_MEATBUGFLESH",10,0},
                              std::tuple{"ITFO_APPLE",3,0},std::tuple{"ITPL_MUSHROOM_01",5,0}};
    int hp=0,mana=0;
    for(const auto& [food,healthDose,manaDose]:foodUse) {
      const auto [id,initialItems]=startUse(food);
      Contract::finishItemUseAnimation(*npc);
      hp+=healthDose;
      mana+=manaDose;
      const auto& pool=Contract::foodPool(*npc);
      require(npc->attribute(ATR_HITPOINTS)==20 && npc->attribute(ATR_MANA)==20 &&
              pool.hp==hp && pool.mana==mana && pool.remaining==Healing::FoodDuration &&
              npc->itemCount(id)==initialItems,"item restores: food adds both ordered doses to the gradual pool and consumes once");
      }
    require(apples->get_int()==24 && mushrooms->get_int()==49,
            "cumulative food: a nonplayer receives only the ordinary food refill");
    const auto heroAttributes=std::to_array(hero.handle().attribute);
    const auto heroStrengthTraining=hero.handle().aivar[81],heroManaTraining=hero.handle().aivar[83];
    hero.handle().attribute[ATR_HITPOINTSMAX]=200;
    hero.handle().attribute[ATR_MANAMAX]=200;
    hero.handle().attribute[ATR_HITPOINTS]=20;
    hero.handle().attribute[ATR_MANA]=20;
    script.invokeItem(&hero,script.findFunction("USE_APPLE"));
    script.invokeItem(&hero,script.findFunction("USE_MUSHROOM_01"));
    require(apples->get_int()==0 && mushrooms->get_int()==0 &&
            hero.attribute(ATR_HITPOINTS)==28 && hero.attribute(ATR_MANA)==25 &&
            hero.attribute(ATR_STRENGTH)==heroAttributes[ATR_STRENGTH]+1 && hero.attribute(ATR_MANAMAX)==205 &&
            hero.handle().aivar[81]==heroStrengthTraining+1 && hero.handle().aivar[83]==heroManaTraining+5,
            "cumulative food: the hero's thresholds award strength and mana once and reset the saved counters");
    std::copy(heroAttributes.begin(),heroAttributes.end(),hero.handle().attribute);
    hero.handle().aivar[81]=heroStrengthTraining;
    hero.handle().aivar[83]=heroManaTraining;
    apples->set_int(oldApples);
    mushrooms->set_int(oldMushrooms);
    const auto strength=npc->attribute(ATR_STRENGTH);
    const auto strengthTraining=npc->handle().aivar[81];
    script.invokeItem(npc,script.findFunction("USE_MEATSOUP"));
    require(npc->attribute(ATR_STRENGTH)==strength+1 && npc->handle().aivar[81]==strengthTraining+1 &&
            script.findSymbol("USE_MEATSOUP.CONCATTEXT")->get_string()==
              script.findSymbol("NAME_BONUS_STR")->get_string()+"1",
            "special food: meat soup records the localized message and raises the NPC's real strength");
    npc->handle().attribute[ATR_DEXTERITY]=strength+1;
    script.invokeItem(npc,script.findFunction("USEITPO_MEGADRINK"));
    require(npc->attribute(ATR_STRENGTH)==strength+16 && npc->attribute(ATR_DEXTERITY)==strength+1 &&
            npc->attribute(ATR_MANA)==18 && npc->handle().aivar[81]==strengthTraining+16,
            "special potion: a megadrink raises strength on a tie and spends exactly two current mana");
    npc->handle().aivar[81]=strengthTraining;

    auto* hammer=script.findSymbol("HAMMER_ONCE");
    const auto oldHammer=hammer->get_int(),maxMana=npc->attribute(ATR_MANAMAX);
    const auto trainingMana=npc->handle().aivar[83];
    hammer->set_int(0);
    for(int use=0; use<2; ++use)
      script.invokeItem(npc,script.findFunction("USELOUHAMMER"));
    require(hammer->get_int()==1 && npc->attribute(ATR_MANAMAX)==maxMana+1 &&
            npc->handle().aivar[83]==trainingMana+1,
            "special food: only the first use raises mana and writes the saved flag");
    npc->handle().aivar[83]=trainingMana;
    hammer->set_int(oldHammer);

    const auto isStarted=Contract::aiState(*npc).started;
    script.invokeItem(npc,script.findFunction("USEITPO_GEIST_01"));
    require(npc->attribute(ATR_HITPOINTS)==-980 && Contract::aiState(*npc).started==isStarted,
            "special potion: the first spirit potion writes raw negative HP without an immediate health transition");
    require(restoreCalls==(native ? 0u : potionUse.size()+permanentUse.size()+foodUse.size()+7),
            "item restores: only fallback executes the callback bodies");
    vm.global_self()->set_instance(previousSelf);
    std::copy(attributes.begin(),attributes.end(),npc->handle().attribute);
    npc->changeAttribute(ATR_HITPOINTS,-npc->attribute(ATR_HITPOINTS),true);
    npc->setOther(&hero);
    require(npc->isUnconscious() && !npc->isDead() && npc->attribute(ATR_HITPOINTS)==1,
            "unconscious: nonlethal HP loss enters the engine knockout path");
    {
      auto* checkedBody=script.findSymbol("C_BODYSTATECONTAINS.BODYSTATE");
      auto* checkedActor=script.findSymbol("C_BODYSTATECONTAINS.SLF");
      checkedBody->set_int(BS_STAND);
      checkedActor->set_instance(std::static_pointer_cast<daedalus::INpc>(hero.handlePtr()));
      ActivityStream stream(world,*npc);
      Contract::tickRoutine(*npc);
      require(checkedBody->get_int()==BS_SWIM &&
              checkedActor->get_instance()==std::static_pointer_cast<daedalus::INpc>(npc->handlePtr()),
              "unconscious: Init leaves the last body check arguments in the saved VM symbols");
      require(npc->hasPerc(PERC_ASSESSMAGIC) && npc->handle().aivar[0]==1 && npc->handle().aivar[16]==1 &&
              npc->handle().aivar[66]==1 && rewards==1 && hero.experience()>beforeXp,
              "unconscious: Init restores magic perception, records a loss and awards victory XP once");
      const auto init=activityQueue(*npc);
      const auto gained=hero.experience()-beforeXp;
      if(native) require(init==originalInit && gained==originalXp,"unconscious: Init queue and experience match Daedalus");
      else { originalInit=init; originalXp=gained; }

      npc->clearAiQueue(); // Isolate the recovery queue from the knockdown commands.
      npc->handle().aivar[10]=1; // AIV_Ransacked
      npc->setStateTime(wakeAt-1);
      Contract::tickRoutine(*npc);
      require(npc->isUnconscious() && npc->isAiQueueEmpty() && npc->handle().aivar[10]==1,
              "unconscious: the last millisecond before the deadline still waits");
      if(native) save(knockedOut,"unconscious before recovery");
      npc->setStateTime(wakeAt);
      Contract::tickRoutine(*npc);
      require(!npc->isUnconscious() && npc->handle().aivar[10]==0 && rewards==1 &&
              phaseCalls==(native ? 0u : 4u) && stream.records==(native ? 4u : 0u) &&
              activityCount(stream.events,"phase ZS_UNCONSCIOUS_LOOP")==2,
              "unconscious: the deadline ends the state once through the selected executor");
      const auto end=recoveryQueue(*npc,script.findFunction("ZS_HEALSELF"),hero);
      if(native) require(end==originalEnd,"unconscious: End queue matches Daedalus");
      else originalEnd=end;
      }

    const auto heal=script.findFunction("ZS_HEALSELF");
    const std::array potions={script.findSymbolIndex("ITPO_HEALTH_03"),
                              script.findSymbolIndex("ITPO_HEALTH_02"),script.findSymbolIndex("ITPO_HEALTH_01")};
    {
      ActivityStream stream(world,*npc);
      for(const auto item:potions) {
        npc->delItem(item,uint32_t(npc->itemCount(item)));
        npc->addItem(item,1);
        }
      npc->handle().senses=0;
      npc->handle().senses_range=0;
      require(npc->startState(heal,"",gtime::endOfTime(),true),"healing: enter the recovery state");
      npc->setPerceptionEnable(PERC_ASSESSTALK,script.findFunction("B_ASSESSTALK").ptr);
      Contract::tickRoutine(*npc);
      require(npc->handle().senses==3 && npc->handle().senses_range==2000 && npc->hasPerc(PERC_ASSESSMAGIC) &&
              npc->hasPerc(PERC_ASSESSDAMAGE) && npc->hasPerc(PERC_ASSESSMURDER) && npc->hasPerc(PERC_ASSESSTHEFT) &&
              npc->hasPerc(PERC_ASSESSUSEMOB) && npc->hasPerc(PERC_ASSESSENTERROOM) && npc->hasPerc(PERC_ASSESSTALK),
              "healing: Init enables the six minimal perceptions and leaves other callbacks alone");
      for(const auto item:potions) {
        Contract::tickRoutine(*npc);
        require(npc->isInState(heal),"healing: keep the state active while a potion is available");
        potionQueue(*npc,item);
        if(native && item==potions.front()) save(healing,"healing with a queued potion");
        npc->clearAiQueue();
        npc->delItem(item,1);
        }
      npc->aiPush(AiQueue::aiWait(500));
      Contract::tickRoutine(*npc);
      require(!npc->isInState(heal) && npc->isAiQueueEmpty(),"healing: no potion ends the state and clears the queue");

      npc->changeAttribute(ATR_HITPOINTS,npc->attribute(ATR_HITPOINTSMAX),true);
      npc->addItem(potions.front(),1);
      require(npc->startState(heal,"",gtime::endOfTime(),true),"healing: enter the state at full health");
      Contract::tickRoutine(*npc);
      npc->aiPush(AiQueue::aiWait(500));
      Contract::tickRoutine(*npc);
      require(!npc->isInState(heal) && npc->isAiQueueEmpty() && npc->itemCount(potions.front())==1 &&
              activityCount(stream.events,"phase ZS_HEALSELF_END")==2 && stream.records==(native ? 9u : 0u) &&
              minimalCalls==(native ? 0u : 2u),
              "healing: full health clears the queue, preserves the potion and uses the selected executor");
      }
    vm.remove_call_observer(observer);
    gothic.clearGame().reset();
    }

  Tempest::MemReader reader(knockedOut);
  Serialize in(reader);
  gothic.setGame(std::make_unique<GameSession>(in));
  auto& world=*gothic.world();
  auto& script=world.script();
  auto& hero=*world.player();
  auto* npc=activityNpc(world,actor);
  const auto wakeAt=int64_t(script.findSymbol("HAI_TIME_UNCONSCIOUS")->get_int())*1000;
  require(npc && npc->isUnconscious() && Contract::aiState(*npc).started && npc->stateOther()==&hero &&
          npc->stateTime()==uint64_t(wakeAt-1) && npc->handle().aivar[10]==1 && npc->handle().aivar[16]==1,
          "unconscious: ordinary load keeps the down state, time, culprit and flags");
  const auto restoredXp=hero.experience();
  {
    ActivityStream stream(world,*npc);
    npc->setStateTime(wakeAt);
    Contract::tickRoutine(*npc);
    require(!npc->isUnconscious() && npc->handle().aivar[10]==0 && hero.experience()==restoredXp &&
            stream.records==2 && activityCount(stream.events,"phase ZS_UNCONSCIOUS")==0 &&
            recoveryQueue(*npc,script.findFunction("ZS_HEALSELF"),hero)==originalEnd,
            "unconscious: ordinary load resumes recovery without replaying Init or rewarding again");
  }
  gothic.clearGame().reset();
  std::puts("Unconscious lifecycle: native Init, deadline and recovery match Daedalus across ordinary load");

  Tempest::MemReader healingReader(healing);
  Serialize healingIn(healingReader);
  gothic.setGame(std::make_unique<GameSession>(healingIn));
  auto& healingWorld=*gothic.world();
  auto& healingScript=healingWorld.script();
  auto* recovering=activityNpc(healingWorld,actor);
  const auto strongest=healingScript.findSymbolIndex("ITPO_HEALTH_03");
  require(recovering && recovering->isInState(healingScript.findFunction("ZS_HEALSELF")) &&
          Contract::aiState(*recovering).started && recovering->attribute(ATR_HITPOINTS)==1,
          "healing: ordinary load restores the active state and the wounded NPC");
  potionQueue(*recovering,strongest);
  recovering->clearAiQueue();
  {
    ActivityStream stream(healingWorld,*recovering);
    Contract::tickRoutine(*recovering);
    potionQueue(*recovering,strongest);
    require(stream.records==1 && activityCount(stream.events,"phase ZS_HEALSELF_LOOP")==1,
            "healing: ordinary load resumes the native Loop");
  }
  gothic.clearGame().reset();
  std::puts("Healing lifecycle: native preferences, exits and ordinary load match Daedalus");

  Tempest::MemReader potionReader(pendingPotion);
  Serialize potionIn(potionReader);
  gothic.setGame(std::make_unique<GameSession>(potionIn));
  auto& potionWorld=*gothic.world();
  auto& potionScript=potionWorld.script();
  auto& potionVm=potionScript.getVm();
  size_t loadedPotionCalls=0,loadedArmorCalls=0,loadedWeaponCalls=0;
  const auto potionObserver=potionVm.add_call_observer([&](const auto& symbol) {
    loadedPotionCalls+=symbol.name()=="USEITPO_HEALTH_03";
    loadedArmorCalls+=symbol.name()=="UNEQUIP_ITAR_MIL_L";
    loadedWeaponCalls+=symbol.name()=="UNEQUIP_1H_05";
    });
  auto& armoredHero=*potionWorld.player();
  const auto armor=potionScript.findSymbolIndex("ITAR_MIL_L");
  require(armoredHero.currentArmor() && armoredHero.currentArmor()->clsId()==armor &&
          potionScript.findSymbol("MILARMOR_EQUIPPED")->get_int()==1 &&
          potionScript.findSymbol("MIL01_EQUIPPED")->get_int()==1 &&
          std::to_array(armoredHero.handle().protection)==stockArmor.first,
          "armor sets: the VM-era save restores the equipped item, flags and protection together");
  armoredHero.unequipItem(armor);
  require(!armoredHero.currentArmor() && potionScript.findSymbol("MILARMOR_EQUIPPED")->get_int()==0 &&
          std::to_array(armoredHero.handle().protection)==stockArmor.second && loadedArmorCalls==0,
          "armor sets: native unequip removes the saved bonus exactly once");
  const auto sword=potionScript.findSymbolIndex("ITMW_1H_VLK_SWORD");
  require(armoredHero.currentMeleeWeapon() && armoredHero.currentMeleeWeapon()->clsId()==sword &&
          armoredHero.hitChance(TALENT_1H)==30 && armoredHero.talentSkill(TALENT_1H)==1 &&
          potionScript.findSymbol("B_ADDFIGHTSKILL.SLF")->get_instance()==
            std::static_pointer_cast<daedalus::INpc>(armoredHero.handlePtr()) &&
          potionScript.findSymbol("B_ADDFIGHTSKILL.PERCENT")->get_int()==5 &&
          potionScript.findSymbol("B_ADDFIGHTSKILL.TALENT")->get_int()==TALENT_1H,
          "weapon skill: loading the VM-era save must keep the weapon, rank, and helper arguments");
  armoredHero.unequipItem(sword);
  require(!armoredHero.currentMeleeWeapon() && armoredHero.hitChance(TALENT_1H)==25 &&
          armoredHero.talentSkill(TALENT_1H)==0 && loadedWeaponCalls==(typedWeaponEffects ? 0u : 1u),
          "weapon skill: removal must subtract the saved bonus once and return to novice");
  require(potionScript.findSymbol("B_SELECTSPELL.DK_RND")->get_int()==int32_t(std::mt19937(73106)()%10),
          "equipment: normal load keeps the original saved spell-selection draw");
  require(potionScript.findSymbol("B_READYSPELL.SPELL")->get_int()==potionScript.findSymbol("SPL_ICEBOLT")->get_int() &&
          potionScript.findSymbol("B_READYSPELL.MANA")->get_int()==20,
          "equipment: normal load keeps the last spell and cost written by ready spell");
  require(potionScript.findSymbol("APPLE_BONUS")->get_int()==24 &&
          potionScript.findSymbol("DUNKELPILZ_BONUS")->get_int()==49,
          "cumulative food: ordinary load restores both VM-era progress counters");
  auto* drinker=activityNpc(potionWorld,actor);
  const auto potion=potionScript.findSymbolIndex("ITPO_HEALTH_03");
  require(drinker && Contract::pendingItemUse(*drinker)==potion && drinker->attribute(ATR_HITPOINTS)==125 &&
          drinker->itemCount(potion)>0,"health potions: load restores a pending VM-era drink without healing");
  const auto before=potionWorld.player()->attribute(ATR_HITPOINTS);
  const auto count=drinker->itemCount(potion);
  Contract::finishItemUseAnimation(*drinker);
  potionVm.remove_call_observer(potionObserver);
  require(Contract::pendingItemUse(*drinker)==0 && drinker->attribute(ATR_HITPOINTS)==200 &&
          drinker->itemCount(potion)==count-1 && potionWorld.player()->attribute(ATR_HITPOINTS)==before && loadedPotionCalls==0,
          "health potions: the saved drink finishes with the same capped effect and one consumption");
  gothic.clearGame().reset();
  std::puts("Health potions: queued doses, animation delivery and VM-era save match both executors");
  }

void npcRecoveryAfterActivityChecks(Gothic& gothic) {
  npcDeathLifecycle(gothic);
  npcUnconsciousLifecycle(gothic);
  }
