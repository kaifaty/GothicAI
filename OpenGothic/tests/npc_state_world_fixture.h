// World contracts for NPC initialization, spells and quest effects.

void refreshAtInsert(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();

  auto* hero=world.player();
  auto* local=vm.find_symbol_by_name("B_REFRESHATINSERT.HER");
  auto* mace=vm.find_symbol_by_name("ITMW_1H_BAU_MACE");
  Npc* actor=nullptr;
  for(uint32_t index=0;index<world.npcCount() && actor==nullptr;++index) {
    auto* candidate=world.npcById(index);
    if(candidate && candidate!=hero && candidate->handle().attribute[ATR_HITPOINTS]>0 &&
       !candidate->currentMeleeWeapon() && !candidate->currentRangedWeapon())
      actor=candidate;
    }
  require(hero && local && mace && actor,"refresh fixture needs the hero and a live NPC without an equipped weapon");

  const auto heroHandle=std::static_pointer_cast<daedalus::INpc>(hero->handlePtr());
  const auto oldLocal=local->get_instance();
  const auto oldItem=vm.global_item()->get_instance();
  const auto oldMace=mace->get_instance();
  const auto oldSelf=vm.global_self()->get_instance();
  const auto oldGi=vm.unsafe_get_gi();
  const auto* configured=std::getenv("OPENGOTHIC_NPC_RECORDS");
  const auto oldRecords=configured ? std::optional<std::string>(configured) : std::nullopt;
  auto run=[&](Npc& npc,int32_t guild,int32_t hp,bool native) {
    auto& record=npc.handle();
    const auto oldHp=record.attribute[ATR_HITPOINTS], oldGuild=record.guild;
    const auto before=npc.itemCount(mace->index());
    const auto restore=[&] {
      vm.observe_instructions({});
      if(const auto count=npc.itemCount(mace->index()); count>before)
        npc.delItem(mace->index(),uint32_t(count-before));
      local->set_instance(oldLocal);
      vm.global_item()->set_instance(oldItem);
      mace->set_instance(oldMace);
      record.attribute[ATR_HITPOINTS]=oldHp;
      record.guild=oldGuild;
      if(oldRecords) setenv("OPENGOTHIC_NPC_RECORDS",oldRecords->c_str(),1);
      else unsetenv("OPENGOTHIC_NPC_RECORDS");
      };
    try {
      record.guild=guild;
      record.attribute[ATR_HITPOINTS]=hp;
      local->set_instance(nullptr);
      if(native) unsetenv("OPENGOTHIC_NPC_RECORDS");
      else setenv("OPENGOTHIC_NPC_RECORDS","0",1);
      size_t instructions=0;
      vm.observe_instructions([&](const auto&) { ++instructions; });
      script.invokeRefreshAtInsert(npc);
      vm.observe_instructions({});
      const auto added=npc.itemCount(mace->index())-before;
      const auto expectedItem=added ? std::static_pointer_cast<daedalus::IItem>(npc.getItem(mace->index())->handlePtr()) : oldItem;
      require((instructions==0)==native && vm.global_self()->get_instance()==oldSelf && vm.unsafe_get_gi()==oldGi,
              "refresh must use the selected executor and restore the calling context");
      require(vm.global_item()->get_instance()==expectedItem &&
              local->get_instance()==(hp>0 ? heroHandle : nullptr),
              "refresh must retain the saved hero lookup and publish the received item");
      const auto result=std::pair(record.attribute[ATR_HITPOINTS],added);
      restore();
      return result;
      }
    catch(...) { restore(); throw; }
    };

  const auto compare=[&](Npc& npc,int32_t guild,int32_t hp,int32_t expectedHp,size_t expectedMaces) {
    const auto stock=run(npc,guild,hp,false);
    const auto native=run(npc,guild,hp,true);
    require(native==stock && native==std::pair(expectedHp,expectedMaces),"NPC refresh must match stock health and inventory");
    };
  compare(*actor,GIL_BAU,1,actor->handle().attribute[ATR_HITPOINTSMAX],1);
  compare(*actor,GIL_VLK,1,actor->handle().attribute[ATR_HITPOINTSMAX],0);
  compare(*actor,GIL_WOLF,1,1,0);
  compare(*actor,GIL_BAU,0,0,0);
  compare(*hero,GIL_BAU,1,1,0);
  std::puts("NPC refresh at insert: stock health, weapon, hero/death exclusions and VM context match");
  }

void transformNpcRecords(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto* source=vm.find_symbol_by_name("NONE_100_XARDAS");
  require(source!=nullptr,"the transformation fixture needs a stock human");
  const auto originalSource=source->get_instance();
  const auto oldSelf=vm.global_self()->get_instance();
  const auto oldItem=vm.global_item()->get_instance();
  const auto oldGi=vm.unsafe_get_gi();
  const auto oldRandom=NpcDefinitionContract::random(script);
  const auto* configured=std::getenv("OPENGOTHIC_NPC_RECORDS");
  const auto oldRecords=configured ? std::optional<std::string>(configured) : std::nullopt;
  const auto restoreRecords=[&] {
    if(oldRecords) setenv("OPENGOTHIC_NPC_RECORDS",oldRecords->c_str(),1);
    else unsetenv("OPENGOTHIC_NPC_RECORDS");
    };

  // Cover creation of inventory, an animation overlay and forced equipment.
  for(const auto name:{"SHEEP","FIREWARAN","SKELETON"}) {
    auto* target=vm.find_symbol_by_name(name);
    require(target && NpcDefinitionContract::definitions(script).npcs.contains(target->index()),
            "each transformation fixture must have a stock record");
    const auto originalTarget=target->get_instance();
    const auto restore=[&] {
      vm.observe_instructions({});
      vm.observe_externals({});
      source->set_instance(originalSource);
      target->set_instance(originalTarget);
      vm.global_self()->set_instance(oldSelf);
      vm.global_item()->set_instance(oldItem);
      vm.unsafe_set_gi(oldGi);
      NpcDefinitionContract::random(script)=oldRandom;
      restoreRecords();
      };
    const auto run=[&](bool recorded) {
      try {
        Npc npc(world,source->index(),"NW_CITY_PATH_HABOUR_03");
        npc.handle().aivar[99]=789;
        npc.handle().level=37;
        const auto record=npc.handlePtr();
        const auto self=vm.global_self()->get_instance(), gi=vm.unsafe_get_gi();
        bool constructorRan=false;
        std::vector<std::string> externals;
        if(!recorded) setenv("OPENGOTHIC_NPC_RECORDS","0",1);
        vm.observe_instructions([&](const auto&) { constructorRan |= vm.pc()==target->address(); });
        vm.observe_externals([&](const auto& fn) { externals.push_back(fn.name()); });
        require(npc.transformInto(target->index()),"the stock NPC must transform");
        vm.observe_instructions({});
        vm.observe_externals({});
        require(constructorRan!=recorded && npc.handlePtr()==record &&
                static_cast<const daedalus::INpc&>(npc.handle()).symbol_index()==target->index() &&
                npc.handle().aivar[99]==789 && npc.handle().level==37,
                "the chosen executor must transform the existing NPC and retain untouched fields");
        require(vm.global_self()->get_instance()==self && vm.unsafe_get_gi()==gi,
                "transformation must restore the calling context");
        const auto transformed=npcState(npc);
        const auto item=itemState(script);
        npc.transformBack();
        require(static_cast<const daedalus::INpc&>(npc.handle()).symbol_index()==source->index(),
                "undo must restore the original NPC kind");
        const auto restored=npcState(npc);
        restore();
        return std::tuple(transformed,restored,item,externals);
        }
      catch(...) { restore(); throw; }
      };
    const auto stock=run(false);
    const auto recorded=run(true);
    require(stock==recorded,(std::string(name)+" transformation and undo must match the saved stock NPC and call order").c_str());
    }
  std::puts("NPC transformation records: inventory, animation, equipment, untouched fields and undo match stock");
  }

void initializePerceptionRanges(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  const auto previous=script.percRanges();
  auto custom=previous;
  custom.range[PERC_ASSESSPLAYER]=725;
  custom.range[PERC_ASSESSQUIETSOUND]=42;
  const auto load=[&](uint32_t count,const PerDist& ranges) {
    std::vector<uint8_t> bytes;
    {
      Tempest::MemWriter writer(bytes);
      Serialize out(writer);
      out.setEntry("game/perc");
      out.write(count);
      for(uint32_t i=0;i<count;++i)
        out.write(ranges.range[i]);
      }
    Tempest::MemReader reader(bytes);
    Serialize in(reader);
    require(in.setEntry("game/perc"),"the perception fixture must have a save entry");
    script.loadPerc(in);
    };
  const auto* configured=std::getenv("OPENGOTHIC_NPC_RECORDS");
  const auto oldRecords=configured ? std::optional<std::string>(configured) : std::nullopt;
  const auto restore=[&] {
    vm.observe_instructions({});
    if(oldRecords) setenv("OPENGOTHIC_NPC_RECORDS",oldRecords->c_str(),1);
    else unsetenv("OPENGOTHIC_NPC_RECORDS");
    load(PERC_Count,previous);
    };
  const auto run=[&](bool native,bool oldSave) {
    if(native) unsetenv("OPENGOTHIC_NPC_RECORDS");
    else setenv("OPENGOTHIC_NPC_RECORDS","0",1);
    size_t instructions=0;
    vm.observe_instructions([&](const auto&) { ++instructions; });
    load(PERC_Count,custom);
    require(instructions==0 && script.percRanges().range[PERC_ASSESSQUIETSOUND]==42,
            "a matching save count must restore stored ranges directly");
    if(oldSave) load(PERC_Count-1,custom);
    else script.initPerceptions();
    vm.observe_instructions({});
    require((instructions==0)==native,"perception setup must use the selected executor");
    return script.percRanges();
    };
  try {
    for(const bool oldSave:{false,true}) {
      const auto stock=run(false,oldSave), native=run(true,oldSave);
      require(stock.range[PERC_ASSESSPLAYER]==725 && stock.range[PERC_ASSESSQUIETSOUND]==1000,
              "initialization must preserve unlisted ranges and replace a listed one");
      require(std::equal(std::begin(stock.range),std::end(stock.range),std::begin(native.range)),
              "native initialization and old save fallback must match all stock ranges");
      }
    restore();
    }
  catch(...) { restore(); throw; }
  std::puts("NPC perceptions: startup defaults, matching saves and old save fallback match stock");
  }

void bindNpcGlobals(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto* function=script.findSymbol("B_INITNPCGLOBALS");
  auto* chapter=script.findSymbol("KAPITEL");
  require(function && chapter && world.player(),"the alias fixture needs the stock helper and the player");
  auto& ownedChapter=NpcDefinitionContract::storyChapter(script);
  const auto previousChapter=ownedChapter;
  const auto previousContext=vm.unsafe_get_gi();
  const auto previousSelf=vm.global_self()->get_instance();
  const auto* configured=std::getenv("OPENGOTHIC_NPC_RECORDS");
  const auto oldRecords=configured ? std::optional<std::string>(configured) : std::nullopt;
  std::map<uint32_t,std::shared_ptr<daedalus::DaedalusInstance>> previousBindings;
  const auto restore=[&] {
    vm.observe_writes({});
    vm.observe_instructions({});
    ownedChapter=previousChapter;
    for(const auto& [symbol,instance]:previousBindings)
      vm.find_symbol_by_index(symbol)->set_instance(instance);
    if(oldRecords) setenv("OPENGOTHIC_NPC_RECORDS",oldRecords->c_str(),1);
    else unsetenv("OPENGOTHIC_NPC_RECORDS");
    };
  try {
    setenv("OPENGOTHIC_NPC_RECORDS","0",1);
    ownedChapter=0;
    size_t instructions=0;
    vm.observe_instructions([&](const auto&) { ++instructions; });
    vm.observe_writes([&](const auto& symbol) {
      if(symbol.type()==daedalus::DaedalusDataType::INSTANCE)
        previousBindings.try_emplace(symbol.index(),vm.find_symbol_by_index(symbol.index())->get_instance());
      });
    vm.unsafe_call_bl(function);
    vm.observe_writes({});
    require(ownedChapter==1 && chapter->get_int()==1 && instructions>0 && previousBindings.size()==310,
            "the VM must fill all stock aliases and default the chapter");

    const auto marker=std::static_pointer_cast<daedalus::INpc>(world.player()->handlePtr());
    std::map<uint32_t,std::shared_ptr<daedalus::DaedalusInstance>> expected;
    for(const auto& saved:previousBindings) {
      auto* binding=vm.find_symbol_by_index(saved.first);
      expected.emplace(saved.first,binding->get_instance());
      binding->set_instance(marker);
      }
    unsetenv("OPENGOTHIC_NPC_RECORDS");
    instructions=0;
    chapter->set_int(0);
    vm.unsafe_call_bl(function);
    require(instructions==0 && ownedChapter==1 && chapter->get_int()==1 && vm.unsafe_get_gi()==previousContext &&
            vm.global_self()->get_instance()==previousSelf,
            "native binding must match the chapter and context with zero script instructions");
    for(const auto& [symbol,instance]:expected)
      require(vm.find_symbol_by_index(symbol)->get_instance()==instance,
              "every NPC alias must resolve to the same live handle or null");
    chapter->set_int(3);
    vm.unsafe_call_bl(function);
    require(instructions==0 && ownedChapter==3 && chapter->get_int()==3,"native binding must preserve an established chapter");

    // Serialize the single entry through the same writer as a full save, and compare the encoded row.
    const auto saveChapter=[&](std::optional<int32_t> expected) {
      std::vector<uint8_t> bytes;
      Tempest::MemWriter writer(bytes);
      {
        Serialize out(writer);
        out.setEntry(ScriptVars::StateEntry);
        out.write(uint32_t(1));
        if(expected)
          out.write(chapter->index(),uint8_t(1),uint32_t(1),*expected);
        else
          NpcDefinitionContract::saveChapter(script,out);
      }
      return bytes;
      };
    ownedChapter=5;
    const auto expectedArchive=saveChapter(5);
    const auto current=saveChapter(std::nullopt);
    require(archiveEntries(expectedArchive)==archiveEntries(current),"chapter must keep its typed state row");
    require(vm.export_program().symbols[chapter->index()].int_values==std::vector<int32_t>{5},
            "program export must see the live bound integer value");

    ownedChapter=0;
    Tempest::MemReader reader(expectedArchive);
    Serialize in(reader);
    require(in.setEntry(ScriptVars::StateEntry),"save state must include a chapter value");
    script.loadVar(in);
    require(ownedChapter==5 && chapter->get_int()==5,"saved chapter value must load into engine storage");
    setenv("OPENGOTHIC_NPC_RECORDS","0",1);
    vm.unsafe_call_bl(function);
    require(ownedChapter==5,"legacy bytecode must read the saved chapter from engine storage");
    restore();
    }
  catch(...) { restore(); throw; }
  std::puts("NPC global bindings: all 310 aliases match stock; engine chapter agrees with VM reads, writes and save bytes");
  }

void initializeMonsterAttitudes(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& matrix=NpcDefinitionContract::guildAttitudes(script);
  const auto previousMatrix=matrix;
  const auto* function=script.findSymbol("B_INITMONSTERATTITUDES");
  auto* from=script.findSymbol("B_SETMONSTERATTITUDE.FROMGUILD");
  auto* attitude=script.findSymbol("B_SETMONSTERATTITUDE.ATTITUDE");
  auto* to=script.findSymbol("B_SETMONSTERATTITUDE.TOGUILD");
  const auto summonedZombie=script.intSymbol("GIL_SUMMONEDZOMBIE",-1);
  require(function && from && attitude && to && summonedZombie>=0,"monster attitudes: the stock initializer and saved parameters must exist");
  const auto savedFrom=from->get_int(), savedAttitude=attitude->get_int(), savedTo=to->get_int();
  const auto* configured=std::getenv("OPENGOTHIC_NPC_RECORDS");
  const auto savedRecords=configured ? std::optional<std::string>(configured) : std::nullopt;
  size_t setters=0;
  const auto observer=vm.add_call_observer([&](const auto& symbol) { setters+=symbol.name()=="B_SETMONSTERATTITUDE"; });
  const auto restore=[&] {
    vm.remove_call_observer(observer);
    vm.observe_instructions({});
    matrix=previousMatrix;
    from->set_int(savedFrom);
    attitude->set_int(savedAttitude);
    to->set_int(savedTo);
    if(savedRecords) setenv("OPENGOTHIC_NPC_RECORDS",savedRecords->c_str(),1);
    else unsetenv("OPENGOTHIC_NPC_RECORDS");
    };
  try {
    std::vector<int32_t> original;
    for(bool native:{false,true}) {
      if(native) unsetenv("OPENGOTHIC_NPC_RECORDS");
      else setenv("OPENGOTHIC_NPC_RECORDS","0",1);
      std::fill(matrix.begin(),matrix.end(),-1);
      setters=0;
      size_t instructions=0;
      vm.observe_instructions([&](const auto&) { ++instructions; });
      vm.unsafe_call_bl(function);
      vm.observe_instructions({});
      require((instructions==0)==native && setters==(native ? 0u : 1932u),
              "monster attitudes: the native call replaces 1932 original setter calls");
      const auto untouched=std::count(matrix.begin(),matrix.end(),-1);
      require(untouched>0 && size_t(untouched)<matrix.size(),"monster attitudes: initialization leaves unrelated guild pairs unchanged");
      require(from->get_int()==summonedZombie && attitude->get_int()==ATT_NEUTRAL && to->get_int()==summonedZombie,
              "monster attitudes: the saved parameter fields match the final original call");
      if(native)
        require(matrix==original,"monster attitudes: every directional guild pair matches the original");
      else
        original=matrix;
      }
    restore();
    }
  catch(...) { restore(); throw; }
  std::puts("Monster attitudes: the whole ordered guild matrix matches the VM including untouched entries");
  }
