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

    // Serialize the single entry through the same writer as a full save, and compare its legacy bytes.
    const auto saveChapter=[&](std::optional<int32_t> legacy) {
      std::vector<uint8_t> bytes;
      Tempest::MemWriter writer(bytes);
      {
        Serialize out(writer);
        out.setEntry("game/daedalus");
        out.write(uint32_t(1));
        if(legacy)
          out.write(uint32_t(daedalus::DaedalusDataType::INT),std::string("KAPITEL"),uint32_t(1),*legacy);
        else
          NpcDefinitionContract::saveChapter(script,out);
      }
      return bytes;
      };
    ownedChapter=5;
    const auto legacy=saveChapter(5);
    const auto current=saveChapter(std::nullopt);
    require(archiveEntries(legacy)==archiveEntries(current),"chapter must use the unchanged game/daedalus row");
    require(vm.export_program().symbols[chapter->index()].int_values==std::vector<int32_t>{5},
            "program export must see the live bound integer value");

    ownedChapter=0;
    Tempest::MemReader reader(legacy);
    Serialize in(reader);
    require(in.setEntry("game/daedalus"),"legacy archive must include a chapter value");
    script.loadVar(in);
    require(ownedChapter==5 && chapter->get_int()==5,"legacy chapter value must load into engine storage");
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

void shrinkSpellCast(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const auto previousEvents=events;
  auto* hero=script.findSymbol("PC_HERO");
  auto* sheep=script.findSymbol("SHEEP");
  auto* undeadLocal=script.findSymbol("C_NPCISUNDEAD.SLF");
  auto* perceptionLocal=script.findSymbol("B_CLEARPERCEPTIONS.SLF");
  auto* refreshLocal=script.findSymbol("B_REFRESHATINSERT.HER");
  const auto state=script.findFunction("ZS_MAGICSHRINK");
  require(hero && sheep && undeadLocal && perceptionLocal && refreshLocal && state.isValid(),
          "the cast fixture needs its stock symbols");
  const auto oldHero=hero->get_instance(), oldSheep=sheep->get_instance();
  const auto oldUndead=undeadLocal->get_instance(), oldPerception=perceptionLocal->get_instance();
  const auto oldRefresh=refreshLocal->get_instance();
  const auto* wp=world.findWayPoint("NW_FARM4_BALTHASAR");
  require(wp!=nullptr,"the cast fixture needs a spawn point");
  const auto target=world.addNpc(sheep->index(),wp->position());
  require(target!=nullptr,"the cast fixture needs a live target");
  const auto targetHandle=std::static_pointer_cast<daedalus::INpc>(target->handlePtr());
  const auto oldSelf=vm.global_self()->get_instance(), oldOther=vm.global_other()->get_instance();
  const auto oldGi=vm.unsafe_get_gi();
  const std::array locals{
    script.findSymbol("SPELL_PROCESSMANA.MANAINVESTED"),
    script.findSymbol("SPELL_PROCESSMANA.ACTIVESPELL"),
    script.findSymbol("SPELL_LOGIC_SHRINK.MANAINVESTED"),
    script.findSymbol("SPELL_PROCESSMANA_RELEASE.MANAINVESTED"),
    script.findSymbol("SPELL_PROCESSMANA_RELEASE.ACTIVESPELL")
    };
  std::array<int32_t,5> oldLocals{};
  for(size_t i=0;i<locals.size();++i)
    oldLocals[i]=locals[i]->get_int();

  const struct Case {
    int32_t guild, stage, species;
    NpcFlag flags;
    bool eligible;
    } cases[]={
    {GIL_SHEEP,0,0,NpcFlag::NONE,true},
    {GIL_SHEEP,0,0,NpcFlag::IMMORTAL|NpcFlag::FRIENDS,true},
    {GIL_SHEEP,0,0,NpcFlag::IMMORTAL,false},
    {GIL_SHEEP,1,0,NpcFlag::NONE,false},
    {GIL_SHEEP,0,42,NpcFlag::NONE,false},
    {GIL_SKELETON,0,0,NpcFlag::NONE,false},
    {GIL_BAU,0,0,NpcFlag::NONE,false}
    };
  using Observation=std::tuple<int32_t,int32_t,std::vector<std::string>,std::array<bool,PERC_Count>>;
  const auto run=[&](const char* itemName,int32_t cost,bool native) {
    events=native;
    auto* itemSymbol=script.findSymbol(itemName);
    require(itemSymbol!=nullptr,"the cast fixture needs the stock scroll or rune");
    const auto oldItem=itemSymbol->get_instance();
    auto* caster=world.addNpc(hero->index(),wp->position());
    require(caster!=nullptr,"the cast fixture needs a caster");
    auto* item=caster->addItem(itemSymbol->index(),1);
    require(item && caster->drawSpell(item->spellId()) && caster->activeWeapon()==item,
            "the caster must ready the normal shrink item");
    size_t instructions=0;
    vm.observe_instructions([&](const auto&) { ++instructions; });
    auto& mana=caster->handle().attribute[ATR_MANA];
    mana=cost-1;
    require(script.invokeMana(*caster,target,17)==SPL_SENDSTOP,"stock mana dispatch must deny one below the cost");
    mana=cost;
    itemSymbol->set_instance(nullptr);
    require(script.invokeMana(*caster,target,18)==SPL_SENDCAST &&
            itemSymbol->get_instance()==std::static_pointer_cast<daedalus::IItem>(item->handlePtr()),
            "stock mana dispatch must accept the cost and publish the active item instance");
    require(script.invokeManaRelease(*caster,target,19)==SPL_SENDSTOP &&
            locals[0]->get_int()==18 && locals[1]->get_int()==item->spellId() && locals[2]->get_int()==18 &&
            locals[3]->get_int()==19 && locals[4]->get_int()==item->spellId(),
            "mana dispatch must preserve its saved parameter and active-spell locals");

    std::vector<Observation> result;
    for(const auto& c:cases) {
      target->handle().guild=c.guild;
      target->handle().aivar[42]=c.stage;
      target->handle().aivar[43]=c.species;
      target->handle().flags=c.flags;
      target->clearAiQueue();
      target->aiPush(AiQueue::aiWait(912));
      for(int i=1;i<PERC_Count;++i)
        target->setPerceptionEnable(PercType(i),state.ptr);
      undeadLocal->set_instance(nullptr);
      perceptionLocal->set_instance(nullptr);
      mana=cost+1;
      const auto selected=caster->handle().aivar[20];
      script.invokeSpell(*caster,target,*item);
      const auto* next=target->aiQueueFront();
      require(next && (c.eligible ? next->act==AI_StartState && next->func==state && next->i0==0 && next->target==target :
                                    next->act==AI_Wait),
              "the normal cast must replace the queue only for an eligible target");
      require(mana==1 && caster->handle().aivar[20]==selected+1 && undeadLocal->get_instance()==targetHandle &&
              perceptionLocal->get_instance()==(c.eligible ? targetHandle : nullptr),
              "the cast must deduct mana, advance selection and preserve both helper locals");
      std::array<bool,PERC_Count> perceptions{};
      for(int i=1;i<PERC_Count;++i)
        perceptions[i]=target->hasPerc(PercType(i));
      result.emplace_back(mana,caster->handle().aivar[20],activityQueue(*target),perceptions);
      }
    vm.observe_instructions({});
    require((instructions==0)==native && vm.global_self()->get_instance()==oldSelf &&
            vm.global_other()->get_instance()==oldOther && vm.unsafe_get_gi()==oldGi,
            "the selected cast path must restore the calling script context");
    world.removeNpc(*caster);
    hero->set_instance(oldHero);
    itemSymbol->set_instance(oldItem);
    return result;
    };
  for(const auto [item,cost]:{std::pair("ITSC_SHRINK",5),std::pair("ITRU_SHRINK",300)}) {
    const auto stock=run(item,cost,false);
    require(run(item,cost,true)==stock,"the shrink cast must match stock queue, mana and perceptions");
    }
  world.removeNpc(*target);
  sheep->set_instance(oldSheep);
  undeadLocal->set_instance(oldUndead);
  perceptionLocal->set_instance(oldPerception);
  refreshLocal->set_instance(oldRefresh);
  for(size_t i=0;i<locals.size();++i)
    locals[i]->set_int(oldLocals[i]);
  events=previousEvents;
  std::puts("Shrink cast: stock mana, target eligibility, perceptions, queued state and saved locals match");
  }

void stockSpellSummons(World& world) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  auto& hero=*world.player();
  auto& armyLevel=*script.findSymbol("SPELL_CAST_ARMYOFDARKNESS.SPELLLEVEL");
  auto& heroSymbol=*script.findSymbol("PC_HERO");
  const auto previousEvents=events;
  const auto previousLevel=armyLevel.get_int();
  const auto previousHero=heroSymbol.get_instance();
  const auto* point=world.findWayPoint("NW_FARM4_BALTHASAR");
  auto& perceptions=Contract::passivePerceptions(world);
  const auto passiveCount=perceptions.size();
  require(point!=nullptr,"spell summons: the fixture needs a stock spawn point");
  const struct Spell {
    const char* item;
    const char* cast;
    const char* creature;
    int32_t cost, selection;
    uint32_t count;
    bool player;
    } spells[] = {
    {"ITSC_SUMWOLF", "SPELL_CAST_SUMMONWOLF", "SUMMONED_WOLF",5,1,1,true},
    {"ITRU_SUMSKEL", "SPELL_CAST_SUMMONSKELETON", "SKELETON",60,1,1,false},
    {"ITRU_SUMMONGUARDIAN", "SPELL_CAST_SUMMONGUARDIAN", "STONEGUARDIAN",60,2,1,false},
    {"ITSC_ARMYOFDARKNESS", "SPELL_CAST_ARMYOFDARKNESS", "SUMMONED_SKELETON",5,1,6,true},
    };
  for(const auto& row:spells) {
    auto& itemSymbol=*script.findSymbol(row.item);
    auto& creature=*script.findSymbol(row.creature);
    const auto oldItem=itemSymbol.get_instance(), oldCreature=creature.get_instance();
    const auto oldCurrent=vm.global_item()->get_instance();
    const auto castPc=script.findSymbol(row.cast)->address();
    std::vector<Tempest::Vec3> stock;
    for(bool native:{false,true}) {
      auto* caster=world.addNpc(heroSymbol.index(),point->position());
      require(caster!=nullptr,"spell summons: the stock caster must spawn");
      caster->setProcessPolicy(row.player ? NpcProcessPolicy::Player : NpcProcessPolicy::AiNormal);
      require(caster->isPlayer()==row.player,"spell summons: the caster must have the intended engine role");
      auto* item=caster->addItem(itemSymbol.index(),1);
      require(item && caster->drawSpell(item->spellId()) && caster->activeWeapon()==item,
              "spell summons: the actual rune or scroll must be active");
      events=native;
      caster->handle().attribute[ATR_MANA]=3;
      caster->handle().aivar[20]=9;
      armyLevel.set_int(71);
      const auto before=world.npcCount();
      const auto callSelf=vm.global_self()->get_instance(), callOther=vm.global_other()->get_instance(), callGi=vm.unsafe_get_gi();
      size_t castEntries=0;
      vm.observe_instructions([&](const auto&) { castEntries+=vm.pc()==castPc; });
      vm.push_int(0x12345678);
      script.invokeSpell(*caster,row.player ? nullptr : &hero,*item);
      const auto marker=vm.pop_int();
      vm.observe_instructions({});
      require(castEntries==size_t(!native) && world.npcCount()==before+row.count && marker==0x12345678 &&
              caster->attribute(ATR_MANA)==3-row.cost && caster->handle().aivar[20]==9+row.selection &&
              armyLevel.get_int()==(row.count==6 ? 0 : 71),
              "spell summons: cast keeps the original mana, selection, level and number of spawns");
      require(vm.global_self()->get_instance()==callSelf && vm.global_other()->get_instance()==callOther && vm.unsafe_get_gi()==callGi,
              "spell summons: the callback restores its caller's actor context");
      for(uint32_t i=0;i<row.count;++i) {
        auto* spawned=world.npcById(before);
        require(spawned && spawned->instanceSymbol()==creature.index(),"spell summons: the engine spawns the stock creature for this caster");
        if(native)
          require(spawned->position()==stock[i],"spell summons: each creature is placed at the stock position in the original order");
        else
          stock.push_back(spawned->position());
        world.removeNpc(*spawned);
        }
      world.removeNpc(*caster);
      heroSymbol.set_instance(previousHero);
      creature.set_instance(oldCreature);
      itemSymbol.set_instance(oldItem);
      vm.global_item()->set_instance(oldCurrent);
      perceptions.resize(passiveCount);
      }
    }
  events=previousEvents;
  armyLevel.set_int(previousLevel);
  std::puts("Spell summons: hero, NPC, selection and Army of Darkness match the VM");
  }

void stockSpellCastAccounting(World& world) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  const auto previousEvents=events;
  auto* hero=script.findSymbol("PC_HERO");
  auto& target=*world.player();
  const auto oldHero=hero->get_instance();
  const auto oldItem=vm.global_item()->get_instance();
  const auto targetHp=target.attribute(ATR_HITPOINTS);
  auto& level=*script.findSymbol("SPELL_CAST_ZAP.SPELLLEVEL");
  const auto oldLevel=level.get_int();
  auto& invested=*script.findSymbol("SPELL_PROCESSMANA.MANAINVESTED");
  auto& activeSpell=*script.findSymbol("SPELL_PROCESSMANA.ACTIVESPELL");
  const auto oldInvested=invested.get_int(),oldActiveSpell=activeSpell.get_int();
  const auto* point=world.findWayPoint("NW_FARM4_BALTHASAR");
  require(point!=nullptr,"cast accounting: need a live stock caster spawn point");

  const struct Cast {
    const char* item;
    int32_t cost;
    const char* admission=nullptr;
    bool leveled=false;
    } cases[] = {
    {"ITSC_ICEBOLT",5,"SPELL_LOGIC_ICEBOLT.MANAINVESTED"},
    {"ITRU_ICEBOLT",10,"SPELL_LOGIC_ICEBOLT.MANAINVESTED"},
    {"ITSC_ZAP",5,"SPELL_LOGIC_ZAP.MANAINVESTED",true},
    {"ITSC_SLEEP",0},
    };
  for(const auto& row:cases) {
    auto& symbol=*script.findSymbol(row.item);
    const auto savedItem=symbol.get_instance();
    for(bool native:{false,true}) {
      auto* caster=world.addNpc(hero->index(),point->position());
      require(caster!=nullptr,"cast accounting: the stock caster must spawn");
      auto* item=caster->addItem(symbol.index(),1);
      require(item && caster->drawSpell(item->spellId()) && caster->activeWeapon()==item,
              "cast accounting: the actual test item must be active");
      events=native;
      size_t instructions=0;
      if(row.admission!=nullptr) {
        auto& local=*script.findSymbol(row.admission);
        const auto oldLocal=local.get_int();
        for(bool enough:{false,true}) {
          const auto available=row.cost-int32_t(!enough);
          caster->handle().attribute[ATR_MANA]=available;
          symbol.set_instance(nullptr);
          invested.set_int(-1);
          activeSpell.set_int(-1);
          local.set_int(-1);
          instructions=0;
          vm.observe_instructions([&](const auto&) { ++instructions; });
          const auto result=script.invokeMana(*caster,&target,17);
          vm.observe_instructions({});
          require(result==(enough ? SPL_SENDCAST : SPL_SENDSTOP) && (instructions==0)==native &&
                  caster->attribute(ATR_MANA)==available && invested.get_int()==17 &&
                  activeSpell.get_int()==item->spellId() && local.get_int()==17 &&
                  symbol.get_instance()==std::static_pointer_cast<daedalus::IItem>(item->handlePtr()),
                  "spell admission: preserve both thresholds, saved locals and the active item");
          }
        local.set_int(oldLocal);
        }
      caster->handle().attribute[ATR_MANA]=3;
      caster->handle().aivar[20]=9;
      const auto casterQueue=activityQueue(*caster),targetQueue=activityQueue(target);
      const auto callSelf=vm.global_self()->get_instance(),callOther=vm.global_other()->get_instance();
      const auto callItem=vm.global_item()->get_instance(),callGi=vm.unsafe_get_gi();
      level.set_int(71);
      instructions=0;
      vm.observe_instructions([&](const auto&) { ++instructions; });
      vm.push_int(0x12345678);
      script.invokeSpell(*caster,&target,*item);
      const auto stack=vm.pop_int();
      vm.observe_instructions({});
      require(stack==0x12345678 && (instructions==0)==native && caster->attribute(ATR_MANA)==3-row.cost &&
              caster->handle().aivar[20]==10 && level.get_int()==(row.leveled ? 0 : 71),
              "cast accounting: selected executor keeps the raw mana debit, counter and optional level");
      require(vm.global_self()->get_instance()==callSelf && vm.global_other()->get_instance()==callOther &&
              vm.global_item()->get_instance()==callItem && vm.unsafe_get_gi()==callGi &&
              target.attribute(ATR_HITPOINTS)==targetHp && activityQueue(*caster)==casterQueue && activityQueue(target)==targetQueue,
              "cast accounting: the numeric callback preserves the actor context and queues");
      world.removeNpc(*caster);
      hero->set_instance(oldHero);
      symbol.set_instance(savedItem);
      vm.global_item()->set_instance(oldItem);
      }
    }
  level.set_int(oldLevel);
  invested.set_int(oldInvested);
  activeSpell.set_int(oldActiveSpell);
  events=previousEvents;
  std::puts("Spell accounting: mana admission, rune, scroll, level and counter-only callbacks match both executors");
  stockSpellSummons(world);
  }

void stockSpellCollisionParity(World& world) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  const bool priorEvents=events;
  auto& hero=*world.player();
  auto* sheep=script.findSymbol("SHEEP");
  auto* collision=script.findSymbol("C_CANNPCCOLLIDEWITHSPELL");
  auto& spellType=*script.findSymbol("C_CANNPCCOLLIDEWITHSPELL.SPELLTYPE");
  const auto* point=world.findWayPoint("NW_FARM4_BALTHASAR");
  require(sheep && collision && point,"spell collision: the stock scene must provide the actor and predicate");
  const auto priorSheep=sheep->get_instance();
  const auto priorSpellType=spellType.get_int();
  const auto priorHero=vm.global_hero()->get_instance();
  const auto priorHeroGuild=hero.handle().guild;
  const auto priorSelf=vm.global_self()->get_instance(), priorOther=vm.global_other()->get_instance();
  const auto priorGi=vm.unsafe_get_gi();
  auto& bodySlot=*script.findSymbol("C_BODYSTATECONTAINS.SLF");
  auto& bodyState=*script.findSymbol("C_BODYSTATECONTAINS.BODYSTATE");
  auto& downSlot=*script.findSymbol("C_NPCISDOWN.SLF");
  auto& undeadSlot=*script.findSymbol("C_NPCISUNDEAD.SLF");
  const auto priorBodySlot=bodySlot.get_instance(), priorDown=downSlot.get_instance(), priorUndead=undeadSlot.get_instance();
  const auto priorBodyState=bodyState.get_int();
  auto* actor=world.addNpc(sheep->index(),point->position());
  require(actor && !actor->isDown(),"spell collision: a live actor must exist");
  const auto target=std::static_pointer_cast<daedalus::INpc>(actor->handlePtr());
  vm.global_hero()->set_instance(std::static_pointer_cast<daedalus::INpc>(hero.handlePtr()));
  for(bool native:{false,true}) {
    events=native;
    const auto check=[&](Npc& victim, Npc& caster, std::string_view spell, CollideMask expected) {
      const auto id=script.intSymbol(spell,-1);
      const auto callSelf=vm.global_self()->get_instance(), callOther=vm.global_other()->get_instance();
      const auto callGi=vm.unsafe_get_gi();
      size_t entries=0;
      vm.observe_instructions([&](const auto&) { entries+=vm.pc()==collision->address(); });
      vm.push_int(0x12345678);
      const auto result=script.canNpcCollideWithSpell(victim,&caster,id);
      const auto marker=vm.pop_int();
      vm.observe_instructions({});
      require(result==expected && entries==size_t(!native) && spellType.get_int()==id && marker==0x12345678,
              "spell collision: the public entry preserves the mask, original parameter and VM stack");
      require(vm.global_self()->get_instance()==callSelf && vm.global_other()->get_instance()==callOther && vm.unsafe_get_gi()==callGi,
              "spell collision: the public entry restores the two actor aliases and VM context");
      };
    target->guild=GIL_FIREGOLEM;
    target->aivar[43]=0;
    check(*actor,hero,"SPL_ICELANCE",COLL_APPLYDOUBLEDAMAGE);
    require(bodySlot.get_instance()==target && downSlot.get_instance()==target && bodyState.get_int()==BS_SWIM,
            "spell collision: the original water and down checks still update their saved arguments");
    check(*actor,hero,"SPL_FIREBOLT",COLL_APPLYHALVEDAMAGE);
    check(*actor,hero,"SPL_ZAP",CollideMask(COLL_APPLYDAMAGE | COLL_DONTKILL));
    target->guild=GIL_SKELETON;
    target->attribute[ATR_HITPOINTSMAX]=1000;
    check(*actor,hero,"SPL_DESTROYUNDEAD",COLL_DOEVERYTHING);
    target->attribute[ATR_HITPOINTSMAX]=1001;
    check(*actor,hero,"SPL_DESTROYUNDEAD",COLL_DONOTHING);
    require(undeadSlot.get_instance()==target,"spell collision: a rejected undead target still updates the original argument");
    target->guild=GIL_DMT;
    hero.handle().guild=GIL_DMT;
    check(*actor,hero,"SPL_FIRESTORM",COLL_DONOTHING);
    hero.handle().guild=GIL_NONE;
    check(hero,*actor,"SPL_LIGHTNINGFLASH",COLL_APPLYHALVEDAMAGE);
    hero.handle().guild=priorHeroGuild;
    }
  world.removeNpc(*actor);
  sheep->set_instance(priorSheep);
  vm.global_hero()->set_instance(priorHero);
  vm.global_self()->set_instance(priorSelf);
  vm.global_other()->set_instance(priorOther);
  vm.unsafe_set_gi(priorGi);
  spellType.set_int(priorSpellType);
  bodySlot.set_instance(priorBodySlot);
  bodyState.set_int(priorBodyState);
  downSlot.set_instance(priorDown);
  undeadSlot.set_instance(priorUndead);
  events=priorEvents;
  std::puts("Spell collision: masks, saved arguments and world entry match the VM");
  }

void shrinkSpellStates(World& world) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  const auto previousEvents=events;
  auto* sheep=script.findSymbol("SHEEP");
  const auto* wp=world.findWayPoint("NW_FARM4_BALTHASAR");
  const auto shrinking=script.findFunction("ZS_MAGICSHRINK");
  const auto shrunk=script.findFunction("ZS_MAGICSHRUNK");
  require(sheep && wp && world.player() && shrinking.isValid() && shrunk.isValid(),
          "the shrink fixture needs a monster, its states and the hero");
  const auto previousSheep=sheep->get_instance();
  size_t stateCalls=0;
  const auto observer=vm.add_call_observer([&](const auto& function) {
    if(function.name().starts_with("ZS_MAGICSHR")) ++stateCalls;
    });
  std::vector<uint8_t> previousSave;
  const auto run=[&](bool native) {
    events=native;
    auto* npc=world.addNpc(sheep->index(),wp->position());
    require(npc!=nullptr,"the shrink fixture must insert a live sheep");
    npc->setScale(1.1f,1.2f,1.3f);
    auto& record=npc->handle();
    record.attribute[ATR_HITPOINTS]=319;
    record.attribute[ATR_HITPOINTSMAX]=493;
    record.attribute[ATR_STRENGTH]=167;
    for(int i=0;i<PROT_MAX;++i)
      record.protection[i]=10*i+9;

    const auto snapshot=[&] {
      npc->updateTransform();
      return std::tuple(npc->transform().at(1,1),npcRecordFields(*npc),activityQueue(*npc),npc->target());
      };
    std::vector<decltype(snapshot())> steps;
    stateCalls=0;
    require(npc->startState(shrinking,""),"the monster must start shrinking");
    Contract::tickRoutine(*npc);
    steps.push_back(snapshot());
    require(std::abs(std::get<0>(steps.back())-1.2f)<0.001f,"Init must preserve the pre-spell scale");
    for(int stage=0;stage<=14;++stage) {
      npc->clearAiQueue();
      Contract::tickRoutine(*npc);
      steps.push_back(snapshot());
      if(stage==4) {
        if(!native)
          previousSave=savedNpc(*npc);
        else {
          npc->setScale(1.f,1.f,1.f);
          npc->handle().aivar[42]=-1;
          loadSavedNpc(*npc,previousSave);
          require(npc->isInState(shrinking) && Contract::aiState(*npc).started && snapshot()==steps.back(),
                  "loading a VM save must restore the active shrink phase, scale and wait command");
          }
        }
      }
    const auto* next=npc->aiQueueFront();
    require(next && next->act==AI_StartState && next->func==shrunk && next->i0==1,
            "the completed shrink must queue the permanent shrunken state");
    Contract::nextAiAction(*npc);
    require(npc->isInState(shrunk),"the NPC must enter the shrunken state through the queue");
    Contract::tickRoutine(*npc);
    require(npc->target()==world.player(),"the shrunken monster must target the hero");
    steps.push_back(snapshot());
    Contract::tickRoutine(*npc);
    steps.push_back(snapshot());
    npc->clearState(false);
    steps.push_back(snapshot());
    require(std::abs(std::get<0>(steps.back())-0.3f)<0.001f && (stateCalls==0)==native,
            "the selected executor must keep the final scale after End");
    world.removeNpc(*npc);
    sheep->set_instance(previousSheep);
    return steps;
    };
  const auto stock=run(false);
  require(run(true)==stock,"the shrink states must match VM scale, attributes, target and commands");
  vm.remove_call_observer(observer);
  events=previousEvents;
  std::puts("Shrink spell states: staged effects, old save and permanent shrunken state match stock");
  shrinkSpellCast(world);
  }

void magicHurtStates(World& world) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  const auto previousEvents=events;
  auto* actor=script.findSymbol("VLK_408_JORA");
  auto* helper=script.findSymbol("B_MAGICHURTNPC");
  auto* sourceLocal=script.findSymbol("B_MAGICHURTNPC.SLF");
  auto* targetLocal=script.findSymbol("B_MAGICHURTNPC.OTH");
  auto* damageLocal=script.findSymbol("B_MAGICHURTNPC.DAMAGE");
  auto* hero=world.player();
  const auto* wp=world.findWayPoint("NW_CITY_PATH_HABOUR_03");
  require(actor && helper && sourceLocal && targetLocal && damageLocal && hero && wp,"the magic damage fixture needs stock bindings");
  const auto previousActor=actor->get_instance();
  const auto previousSource=sourceLocal->get_instance(), previousTarget=targetLocal->get_instance();
  const auto previousDamage=damageLocal->get_int();
  auto& heroRecord=hero->handle();
  const auto previousExp=heroRecord.exp, previousNext=heroRecord.exp_next, previousLevel=heroRecord.level;

  const struct Case {
    const char* name;
    const char* stop;
    const char* restart;
    int32_t damage;
    bool exactSecond, shortBurn;
    } cases[]={
    {"ZS_MAGICBURN","B_STOPMAGICBURN","B_RESTARTBURN",1,true,false},
    {"ZS_MAGICBURNSHORT","B_STOPMAGICBURNSHORT","B_RESTARTBURNSHORT",1,true,true},
    {"ZS_PYRO","B_STOPPYRO",nullptr,20,false,false},
    {"ZS_ZAPPED","B_STOPZAPPED",nullptr,2,false,false}
    };
  for(const auto& c:cases) {
    const auto init=script.findFunction(c.name);
    const auto loop=script.findFunction(std::string(c.name)+"_LOOP");
    const auto end=script.findFunction(std::string(c.name)+"_END");
    require(init.isValid() && loop.isValid() && end.isValid(),"the magic damage fixture needs three stock phases");
    const auto* stop=script.findSymbol(c.stop);
    const auto* restart=c.restart ? script.findSymbol(c.restart) : nullptr;
    require(stop && (!c.restart || restart),"the magic damage fixture needs its stock perceptions");
    const std::array addresses{script.findSymbol(init.ptr)->address(),script.findSymbol(loop.ptr)->address(),
                               script.findSymbol(end.ptr)->address(),helper->address(),stop->address(),
                               restart ? restart->address() : stop->address()};
    std::vector<uint8_t> oldSave;
    const auto run=[&](bool native) {
      events=native;
      auto* npc=world.addNpc(actor->index(),wp->position());
      require(npc!=nullptr,"the magic damage fixture needs a disposable NPC");
      npc->handle().attribute[ATR_HITPOINTS]=100;
      npc->handle().attribute[ATR_HITPOINTSMAX]=100;
      npc->handle().flags=NpcFlag::NONE;
      npc->handle().aivar[16]=0;
      npc->handle().level=3;
      npc->setLastHitBy(nullptr);

      size_t instructions=0;
      vm.observe_instructions([&](const auto&) {
        if(std::ranges::find(addresses,vm.pc())!=addresses.end()) ++instructions;
        });
      require(npc->startState(init,""),"the NPC must enter the magic damage state");
      npc->setOther(hero);
      Contract::tickRoutine(*npc);
      require(npc->hasPerc(PERC_ASSESSSTOPMAGIC),"Init must subscribe to the stock stop perception");

      const auto snapshot=[&](int result) {
        return std::tuple(result,npc->handle().attribute[ATR_HITPOINTS],npc->stateTime(),
                          npc->hasPerc(PERC_ASSESSMAGIC),npc->hasPerc(PERC_ASSESSSTOPMAGIC),activityQueue(*npc));
        };
      std::vector<decltype(snapshot(0))> observations{snapshot(0)};
      npc->setStateTime(999);
      if(!native)
        oldSave=savedNpc(*npc);
      else {
        npc->setStateTime(0);
        loadSavedNpc(*npc,oldSave);
        require(npc->stateTime()==999 && npc->isInState(init) && Contract::aiState(*npc).started,
                "an old save must resume damage timing with Init already executed");
        }

      for(const int32_t time:{999,1000,1999,2000}) {
        npc->handle().attribute[ATR_HITPOINTS]=100;
        npc->setStateTime(time);
        const auto result=script.invokeState(npc,hero,nullptr,loop);
        const bool due=time>=1000 && (!c.exactSecond || time<2000);
        require(npc->attribute(ATR_HITPOINTS)==100-(due ? c.damage : 0) &&
                npc->stateTime()==uint64_t(due ? 0 : time) && npc->lastHitBy()==nullptr &&
                result==(c.shortBurn && due ? LOOP_END : LOOP_CONTINUE),
                "each hit must keep the original integer timing, attribution and loop result");
        if(due)
          require(sourceLocal->get_instance()==std::static_pointer_cast<daedalus::INpc>(hero->handlePtr()) &&
                  targetLocal->get_instance()==std::static_pointer_cast<daedalus::INpc>(npc->handlePtr()) &&
                  damageLocal->get_int()==c.damage,"the original damage locals must survive the call");
        observations.push_back(snapshot(result));
        }
      observations.push_back(snapshot(script.invokeState(npc,hero,nullptr,end)));

      if(std::string_view(c.name)=="ZS_PYRO") {
        heroRecord.level=1;
        heroRecord.exp=100;
        heroRecord.exp_next=10000;
        npc->handle().attribute[ATR_HITPOINTS]=c.damage;
        npc->setStateTime(1000);
        script.invokeState(npc,hero,nullptr,loop);
        require(npc->isDead() && npc->handle().aivar[16]==1 && heroRecord.exp==130 && npc->lastHitBy()==nullptr,
                "the lethal tick must use SELF level, preserve attribution and credit victory once");
        npc->setStateTime(1000);
        script.invokeState(npc,hero,nullptr,loop);
        require(heroRecord.exp==130,"a victim already credited must not award XP again");
        heroRecord.exp=previousExp;
        heroRecord.exp_next=previousNext;
        heroRecord.level=previousLevel;
        }
      if(restart) {
        const auto freeze=script.findFunction("ZS_MAGICFREEZE");
        for(const auto* spell : {"SPL_WHIRLWIND",c.shortBurn ? "SPL_FIRESTORM" : "SPL_FIREBOLT","SPL_ICEWAVE"}) {
          npc->clearAiQueue();
          npc->aiPush(AiQueue::aiWait(173));
          npc->setPerceptionEnable(PERC_ASSESSMAGIC,restart->index());
          npc->setStateTime(17000);
          Contract::lastHitSpell(*npc)=script.findSymbol(spell)->get_int();
          script.invokeState(npc,hero,nullptr,ScriptFn(restart->index()));
          const auto* next=npc->aiQueueFront();
          const bool ice=std::string_view(spell)=="SPL_ICEWAVE";
          const bool other=std::string_view(spell)=="SPL_WHIRLWIND";
          require(npc->stateTime()==(other || ice ? 17000u : 0u) && next &&
                  next->act==(ice ? AI_StartState : AI_Wait) &&
                  (!ice || (next->func==freeze && !npc->hasPerc(PERC_ASSESSMAGIC))),
                  "burn restart preserves the queue, resets its clock, or replaces the effect with freeze");
          observations.push_back(snapshot(0));
          }
        }
      npc->handle().guild=GIL_WOLF;
      npc->setTempAttitude(ATT_NEUTRAL);
      npc->setPerceptionDisable(PERC_ASSESSMAGIC);
      script.invokeState(npc,hero,nullptr,ScriptFn(stop->index()));
      auto commands=Contract::queue(*npc);
      require(commands.size()==2 && commands.pop().act==AI_StandUp && commands.pop().act==AI_ContinueRoutine &&
              npc->hasPerc(PERC_ASSESSMAGIC) && npc->tempAttitude()==ATT_HOSTILE,
              "cancelled periodic damage restores perception, stands up and resumes the nonhuman routine");
      observations.push_back(snapshot(0));
      vm.observe_instructions({});
      require((instructions==0)==native,"the selected executor must run the expected magic damage code");
      world.removeNpc(*npc);
      actor->set_instance(previousActor);
      return observations;
      };
    const auto stock=run(false);
    require(run(true)==stock,"magic damage phases must match VM on state, queue, perceptions and timing");
    }
  sourceLocal->set_instance(previousSource);
  targetLocal->set_instance(previousTarget);
  damageLocal->set_int(previousDamage);
  events=previousEvents;
  std::puts("Magic damage states: stock ticks, queue, old save, attribution and one-time XP match");
  }

std::vector<std::vector<std::string>> spellVictimPhasesWorld(Gothic& gothic, bool events) {
  using Contract=NpcDefinitionContract;
  std::string program;
  loadBaseProgram(gothic.defaultGameDatFile(),Settings::getI("GAME","language"),&program);
  if(program!="a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c")
    return {};

  ActivityEventsWorld test(gothic);
  auto& world=test.world();
  auto& script=world.script();
  auto& victim=*test.extra();
  auto& caster=*test.extra();
  auto& hero=*world.player();
  const auto oldHeroPosition=hero.position();
  const auto heroHp=hero.attribute(ATR_HITPOINTS);
  victim.setPosition({1000,2000,3000});
  caster.setPosition({1000,2000,3500});
  victim.setTrueGuild(GIL_WOLF); // Exercise Stop's nonhuman branch without starting a new fight.
  victim.handle().guild=GIL_WOLF;
  victim.setSimTier(NpcSimTier::Full);
  victim.setProcessPolicy(NpcProcessPolicy::AiNormal);
  victim.setAnim(Npc::Anim::Idle);

  static constexpr struct {
    std::string_view state, restart, spell;
    int damage, duration, first;
    size_t marker;
    } cases[] = {
    {"ZS_INFLATE","B_RESTARTINFLATE","SPL_INFLATE",5,19,5,77},
    {"ZS_SUCKENERGY","B_RESTARTSUCKENERGY","SPL_SUCKENERGY",100,9,0,79},
    {"ZS_SWARM","B_RESTARTSWARM","SPL_SWARM",80,6,2,78},
    };
  std::vector<std::vector<std::string>> actions;
  for(const auto& row:cases) {
    const auto state=script.findFunction(row.state);
    const auto restart=script.findFunction(row.restart);
    const bool drain=row.state=="ZS_SUCKENERGY";
    test.stop(victim);
    victim.setFatness(0);
    victim.setTempAttitude(ATT_NEUTRAL);
    victim.handle().attribute[ATR_HITPOINTSMAX]=500;
    victim.handle().attribute[ATR_HITPOINTS]=500;
    caster.handle().attribute[ATR_HITPOINTSMAX]=500;
    caster.handle().attribute[ATR_HITPOINTS]=50;
    hero.setPosition(victim.position()+Tempest::Vec3(400,0,0));
    require(victim.startState(state,"",gtime::endOfTime(),true),"spell victim: start the actual AI state");
    victim.setOther(&caster);
    ActivityStream stream(world,victim);
    // This fixture counts phases. Native damage consumes external returns before the next VM instruction.
    script.getVm().observe_externals({});
    script.getVm().observe_instructions({});
    Contract::tickRoutine(victim);
    require(Contract::perception(victim,PERC_ASSESSMAGIC)==restart && victim.handle().aivar[row.marker]==int(drain),
            "spell victim: Init binds the matching restart and seeds the tick marker");
    if(drain)
      require(script.findSymbol("TEMP_SUCKENERGY_DISTTOPLAYER")->get_int()==400,
              "spell victim: drain records the shared HERO distance");
    actions.push_back(activityQueue(victim));
    victim.clearAiQueue();
    victim.setStateTime(row.first*1000);
    Contract::tickRoutine(victim);
    const auto casterHp=drain ? 150 : 50;
    require(victim.attribute(ATR_HITPOINTS)==500-row.damage && caster.attribute(ATR_HITPOINTS)==casterHp &&
            hero.attribute(ATR_HITPOINTS)==heroHp && victim.handle().aivar[row.marker]==row.first &&
            victim.fatness()==(row.first==5 ? 9.f : 0.f),
            "spell victim: the first unseen second applies the stock damage, caster transfer and deformation");
    actions.push_back(activityQueue(victim));
    const auto prior=actions.back();
    loadSavedNpc(victim,savedNpc(victim));
    Contract::tickRoutine(victim);
    require(victim.attribute(ATR_HITPOINTS)==500-row.damage && caster.attribute(ATR_HITPOINTS)==casterHp &&
            activityQueue(victim)==prior,"spell victim: a loaded second does not replay damage or animation");

    victim.clearAiQueue();
    victim.setStateTime(4000);
    Contract::lastHitSpell(victim)=0;
    script.invokeState(&victim,&caster,nullptr,restart);
    require(victim.stateTime()==4000 && victim.isAiQueueEmpty(),"spell victim: an unrelated spell does not reset the state");
    Contract::lastHitSpell(victim)=script.findSymbol(row.spell)->get_int();
    script.invokeState(&victim,&caster,nullptr,restart);
    require(victim.stateTime()==0 && victim.isAiQueueEmpty()!=drain,
            "spell victim: a matching spell resets the timer and only drain queues a new animation");
    actions.push_back(activityQueue(victim));

    victim.handle().attribute[ATR_HITPOINTS]=row.damage;
    if(drain)
      hero.setPosition(victim.position()+Tempest::Vec3(301,0,0));
    victim.setStateTime(row.duration*1000);
    Contract::tickRoutine(victim);
    require(victim.isInState(state) && victim.attribute(ATR_HITPOINTS)==1 && caster.attribute(ATR_HITPOINTS)==casterHp,
            "spell victim: the final second applies partial damage without transferring life");
    if(drain)
      hero.setPosition(victim.position()+Tempest::Vec3(300,0,0));
    else
      victim.setStateTime((row.duration+1)*1000);
    Contract::tickRoutine(victim);
    auto queue=Contract::queue(victim);
    require(!victim.isInState(state) && victim.tempAttitude()==ATT_HOSTILE && queue.size()==1 && queue.pop().act==AI_StandUp &&
            Contract::perception(victim,PERC_ASSESSMAGIC)==script.findFunction("B_ASSESSMAGIC") &&
            stream.records==(events ? 8u : 0u),
            "spell victim: timeout or exact drain distance ends the state through the selected executor");
    actions.push_back(activityQueue(victim));
    }
  hero.setPosition(oldHeroPosition);
  test.stop(victim);
  return actions;
  }

void assessMagicPerception(World& world) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  const auto previousEvents=events;
  auto* actor=script.findSymbol("VLK_408_JORA");
  auto* clearLocal=script.findSymbol("B_CLEARPERCEPTIONS.SLF");
  auto* callback=script.findSymbol("B_ASSESSMAGIC");
  auto* caster=world.player();
  const auto* wp=world.findWayPoint("NW_CITY_PATH_HABOUR_03");
  require(actor && clearLocal && callback && caster && wp,"the magic perception fixture needs its stock actors and callback");
  const auto originalActor=actor->get_instance(), originalClear=clearLocal->get_instance();
  auto* npc=world.addNpc(actor->index(),wp->position());
  require(npc!=nullptr,"the magic perception fixture needs a live victim");
  const auto victim=std::static_pointer_cast<daedalus::INpc>(npc->handlePtr());
  const auto oldSelf=vm.global_self()->get_instance(), oldOther=vm.global_other()->get_instance();
  const auto oldGi=vm.unsafe_get_gi();

  const struct Case {
    const char* spell;
    int32_t guild;
    std::string_view state;
    bool clear, fear;
    } cases[]={
    {"SPL_WHIRLWIND",GIL_VLK,"ZS_WHIRLWIND",true,false},
    {"SPL_SUCKENERGY",GIL_VLK,"ZS_SUCKENERGY",true,false},
    {"SPL_GREENTENTACLE",GIL_VLK,"ZS_GREENTENTACLE",true,false},
    {"SPL_SWARM",GIL_VLK,"ZS_SWARM",true,false},
    {"SPL_ICEWAVE",GIL_VLK,"ZS_MAGICFREEZE",true,false},
    {"SPL_ICECUBE",GIL_VLK,"ZS_MAGICFREEZE",true,false},
    {"SPL_CHARGEZAP",GIL_VLK,"ZS_SHORTZAPPED",true,false},
    {"SPL_FEAR",GIL_VLK,"ZS_MAGICFLEE",true,true},
    {"SPL_FEAR",GIL_WOLF,"ZS_MM_FLEE",true,true},
    {"SPL_FEAR",GIL_PAL,"",true,true},
    {"SPL_FEAR",GIL_KDF,"",true,true},
    {"SPL_FEAR",GIL_TROLL,"",true,true},
    {"SPL_FEAR",GIL_SEPERATOR_HUM,"",true,true},
    {"SPL_FIRERAIN",GIL_VLK,"ZS_MAGICBURNSHORT",false,false},
    {"SPL_FIREBOLT",GIL_VLK,"",false,false},
    {"SPL_PALLIGHT",GIL_VLK,"",false,false}
    };
  for(const auto& c:cases) {
    auto* spell=script.findSymbol(c.spell);
    require(spell!=nullptr,"the magic perception fixture needs each original spell ID");
    const auto spellId=spell->get_int();
    const auto run=[&](bool native) {
      events=native;
      npc->handle().guild=c.guild;
      Contract::lastHitSpell(*npc)=spellId;
      npc->setTarget(nullptr);
      npc->clearAiQueue();
      const auto wait=AiQueue::aiWait(713);
      npc->aiPush(AiQueue::AiAction(wait));
      for(int i=1;i<PERC_Count;++i)
        npc->setPerceptionEnable(PercType(i),callback->index());
      clearLocal->set_instance(nullptr);

      size_t instructions=0, fightSounds=0;
      vm.observe_instructions([&](const auto&) { ++instructions; });
      vm.observe_externals([&](const auto& fn) {
        if(fn.name()=="NPC_SENDPASSIVEPERC") {
          ++fightSounds;
          const auto* next=npc->aiQueueFront();
          require(next && next->act==wait.act && npc->hasPerc(PERC_ASSESSMAGIC),
                  "fight sound must be sent before changing the queue or perceptions");
          }
        });
      require(npc->perceptionProcess(*caster,nullptr,0,PERC_ASSESSMAGIC),
              "the real magic perception must dispatch its configured callback");
      vm.observe_instructions({});
      vm.observe_externals({});
      require((instructions==0)==native && vm.global_self()->get_instance()==oldSelf &&
              vm.global_other()->get_instance()==oldOther && vm.unsafe_get_gi()==oldGi,
              "the selected perception executor must restore the script context");
      require(fightSounds==(script.spellDesc(spellId).spell_type==SPELL_BAD ? 1u : 0u),
              "bad spells must report one fight sound before the state reaction");
      require(npc->target()==(c.fear ? caster : nullptr) && clearLocal->get_instance()==(c.clear ? victim : nullptr),
              "the original fear target and saved perception local must be kept");

      const auto* next=npc->aiQueueFront();
      if(!c.state.empty())
        require(next && next->act==AI_StartState && next->func==script.findFunction(c.state) &&
                next->i0==0 && next->target==caster && next->s0.empty(),"spell perception must queue the exact original state");
      else if(c.fear)
        require(next==nullptr,"immune targets must clear their old queue without starting a state");
      else
        require(next && next->act==wait.act && next->i0==wait.i0,"an unrelated spell must leave the old queue");

      std::array<bool,PERC_Count> enabled{};
      for(int i=1;i<PERC_Count;++i)
        enabled[size_t(i)]=npc->hasPerc(PercType(i));
      require(enabled[PERC_ASSESSITEM] && enabled[PERC_ASSESSMAGIC]!=c.clear,
              "only the stock perception subset may be cleared");
      return std::pair(activityQueue(*npc),enabled);
      };
    const auto stock=run(false);
    require(run(true)==stock,"the stock and native magic perception must match the queue and all subscriptions");
    }
  world.removeNpc(*npc);
  actor->set_instance(originalActor);
  clearLocal->set_instance(originalClear);
  events=previousEvents;
  std::puts("Magic perception: root dispatch, fear exceptions, fight sound, queue and perception cleanup match");
  }

void playerXpRule(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const auto previousEvents=events;
  auto* xp=script.findSymbol("B_GIVEPLAYERXP");
  auto* amount=script.findSymbol("B_GIVEPLAYERXP.ADD_XP");
  auto* text=script.findSymbol("B_GIVEPLAYERXP.CONCATTEXT");
  const auto& symbols=vm.symbols();
  const auto temporary=std::ranges::find_if(symbols,[](const auto& symbol) {
    return symbol.name()=="$PHOENIX_FAKE_STRINGS";
    });
  require(xp && amount && text && world.player() && temporary!=symbols.end(),"the XP fixture needs its stock bindings");
  auto* scratch=vm.find_symbol_by_index(temporary->index());
  const auto strings=[&] {
    std::vector<std::string> value;
    for(uint16_t i=0;i<scratch->count();++i)
      value.emplace_back(scratch->get_string(i));
    return value;
    };
  const auto oldStrings=strings();
  const auto restoreStrings=[&] {
    for(uint16_t i=0;i<oldStrings.size();++i)
      scratch->set_string(oldStrings[i],i);
    };
  const auto oldAmount=amount->get_int();
  const std::string oldText=text->get_string();
  auto& record=world.player()->handle();
  const auto fields=[&] {
    return std::tuple(record.exp,record.exp_next,record.level,record.attribute[ATR_HITPOINTS],
                      record.attribute[ATR_HITPOINTSMAX],record.lp);
    };
  const auto oldFields=fields();
  const auto oldSelf=vm.global_self()->get_instance(), oldHero=vm.global_hero()->get_instance();
  const auto oldGi=vm.unsafe_get_gi();

  const auto integer=[&](uint16_t depth) {
    const auto& frame=vm.stack_frame(depth);
    if(frame.reference)
      return std::get<daedalus::DaedalusSymbol*>(frame.value)->get_int(frame.index,frame.context.get());
    return std::get<int32_t>(frame.value);
    };
  const auto string=[&](uint16_t depth) {
    const auto& frame=vm.stack_frame(depth);
    return std::get<daedalus::DaedalusSymbol*>(frame.value)->get_string(frame.index,frame.context.get());
    };
  using Notice=std::tuple<std::string,std::string,int32_t,int32_t,std::string,int32_t>;

  const struct Case {
    int32_t level, experience, threshold, award, result, next, resultLevel;
    } cases[]={
    {0,100,9000,25,125,500,0},
    {0,499,9000,1,500,1500,1},
    {2,1000,3000,10000,11000,5000,3},
    {2,INT32_MAX-2,INT32_MAX,10,INT32_MIN+7,INT32_MAX,2},
    {1,INT32_MAX-3,INT32_MAX-3,1,INT32_MAX-2,INT32_MIN+1496,2}
    };
  for(const auto& c:cases) {
    const auto run=[&](bool native) {
      events=native;
      restoreStrings();
      record.level=c.level;
      record.exp=c.experience;
      record.exp_next=c.threshold;
      record.attribute[ATR_HITPOINTS]=78;
      record.attribute[ATR_HITPOINTSMAX]=123;
      record.lp=16;
      size_t instructions=0;
      std::vector<Notice> notices;
      const auto observer=vm.add_call_observer([&](const auto& symbol) {
        if(symbol.name()=="B_CHECKLOG")
          notices.emplace_back(symbol.name(),"",0,0,"",0);
        });
      vm.observe_instructions([&](const auto&) { if(vm.pc()==xp->address()) ++instructions; });
      vm.observe_externals([&](const auto& symbol) {
        if(symbol.name()=="PRINTSCREEN")
          notices.emplace_back(symbol.name(),string(4),integer(3),integer(2),string(1),integer(0));
        if(symbol.name()=="SND_PLAY")
          notices.emplace_back(symbol.name(),string(0),0,0,"",0);
        });
      std::string error;
      const auto result=storygraph::callFunction(vm,*xp,{storygraph::Value::integer(c.award)},error);
      require(result.has_value(),error.c_str());
      vm.remove_call_observer(observer);
      vm.observe_instructions({});
      vm.observe_externals({});
      const bool levelUp=c.level!=c.resultLevel;
      require(record.exp==c.result && record.exp_next==c.next && record.level==c.resultLevel &&
              record.attribute[ATR_HITPOINTS]==(levelUp ? 90 : 78) &&
              record.attribute[ATR_HITPOINTSMAX]==(levelUp ? 135 : 123) && record.lp==(levelUp ? 26 : 16),
              "XP must retain the original single-level threshold, wrap and stat changes");
      require((instructions==0)==native && amount->get_int()==c.award &&
              vm.global_self()->get_instance()==oldSelf && vm.global_hero()->get_instance()==oldHero && vm.unsafe_get_gi()==oldGi,
              "the XP helper must save the amount and restore its calling context");
      return std::tuple(fields(),std::string(text->get_string()),strings(),notices);
      };
    const auto stock=run(false);
    require(run(true)==stock,"the new XP rule must match saved fields, string scratch and exact notification arguments");
    }
  std::tie(record.exp,record.exp_next,record.level,record.attribute[ATR_HITPOINTS],
           record.attribute[ATR_HITPOINTSMAX],record.lp)=oldFields;
  amount->set_int(oldAmount);
  text->set_string(oldText);
  restoreStrings();
  events=previousEvents;
  std::puts("Player XP: stock wrap, single level-up, saved text, notices and quest hook match");
  }

void questClosureRule(World& world) {
  using Status=QuestLog::Status;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const auto previousEvents=events;
  auto* check=script.findSymbol("B_CHECKLOG");
  auto* close=script.findSymbol("B_CLOSETOPIC");
  auto* setStatus=script.findSymbol("LOG_SETTOPICSTATUS");
  auto* topicLocal=script.findSymbol("B_CLOSETOPIC.TOPIC");
  require(check && close && setStatus && topicLocal && world.player(),"the journal fixture needs the stock rule and hero");
  const auto oldTopic=std::string(topicLocal->get_string());
  auto& hero=world.player()->handle();
  const auto oldGuild=hero.guild;
  const auto oldGi=vm.unsafe_get_gi();
  const std::array topics{script.findSymbol("TOPIC_ADDON_FORTUNO"),script.findSymbol("TOPIC_RESCUEBENNET"),
                          script.findSymbol("TOPIC_ADDON_BUDDLER"),script.findSymbol("TOPIC_BALTHASARSSCHAFE")};
  const std::array inputs{script.findSymbol("KAPITEL"),script.findSymbol("SC_KNOWSFORTUNOINFOS"),
                          script.findSymbol("MIS_RESCUEBENNET"),script.findSymbol("MIS_SEND_BUDDLER"),
                          script.findSymbol("MIS_BALTHASAR_BENGARSWEIDE")};
  const std::array locals{script.findSymbol("B_CLOSETOPIC.MISSIONVAR"),script.findSymbol("B_CLOSETOPIC.COMMONVAR"),
                          script.findSymbol("B_CLOSETOPIC.ENDCHAPTER")};
  std::array<int32_t,inputs.size()> previousInputs{};
  std::array<int32_t,locals.size()> previousLocals{};
  for(size_t i=0;i<inputs.size();++i) previousInputs[i]=inputs[i]->get_int();
  for(size_t i=0;i<locals.size();++i) previousLocals[i]=locals[i]->get_int();

  const auto save=[&] {
    std::vector<uint8_t> bytes;
    Tempest::MemWriter writer(bytes);
    {
      Serialize out(writer);
      out.setEntry("game/quests");
      script.saveQuests(out);
      }
    return bytes;
    };
  const auto before=save();
  const auto load=[&] {
    Tempest::MemReader reader(before);
    Serialize in(reader);
    require(in.setEntry("game/quests"),"the journal fixture must have a saved quest section");
    script.loadQuests(in);
    };
  const auto status=[&](size_t topic) {
    const auto& journal=script.questLog();
    for(size_t i=0;i<journal.questCount();++i)
      if(journal.quest(i).name==topics[topic]->get_string())
        return journal.quest(i).status;
    throw std::runtime_error("Quest fixture topic is absent from the journal");
    };

  const std::array roots{check->address(),close->address(),script.findSymbol("B_CLOSETOPICS_MATTES")->address(),
                         script.findSymbol("B_CLOSETOPICS_HOSHI")->address(),script.findSymbol("B_CLOSETOPICS_YOLY")->address()};
  const auto run=[&](bool native) {
    events=native;
    load();
    for(auto* topic:topics) {
      vm.push_reference(topic);
      vm.push_int(int32_t(Status::Running));
      vm.unsafe_call_external(setStatus);
      }
    std::vector<std::pair<std::string,int32_t>> logCalls;
    size_t instructions=0;
    vm.observe_instructions([&](const auto&) {
      if(std::ranges::find(roots,vm.pc())!=roots.end()) ++instructions;
      });
    vm.observe_externals([&](const auto& external) {
      if(external.name()!="LOG_SETTOPICSTATUS")
        return;
      const auto& topic=vm.stack_frame(1);
      const auto& value=vm.stack_frame(0);
      const auto code=value.reference ?
                      std::get<daedalus::DaedalusSymbol*>(value.value)->get_int(value.index,value.context.get()) :
                      std::get<int32_t>(value.value);
      logCalls.emplace_back(std::get<daedalus::DaedalusSymbol*>(topic.value)->get_string(topic.index,topic.context.get()),code);
      });
    vm.push_reference(topics[0]);
    vm.push_int(int32_t(Status::Failed));
    vm.push_int(1);
    vm.push_int(6);
    vm.unsafe_call_bl(close);
    require(status(0)==Status::Success,"a true common flag must win over a failed mission");

    inputs[0]->set_int(4);
    inputs[1]->set_int(1);
    inputs[2]->set_int(int32_t(Status::Failed));
    inputs[3]->set_int(0);
    inputs[4]->set_int(int32_t(Status::Success));
    hero.guild=int32_t(GIL_SLD);
    vm.unsafe_call_bl(check);
    require(status(0)==Status::Success && status(1)==Status::Failed && status(2)==Status::Obsolete &&
            status(3)==Status::Success,"chapter cutoff and the mercenary exception must match stock");
    const auto atCutoff=archiveEntries(save());

    inputs[2]->set_int(int32_t(Status::Obsolete));
    hero.guild=int32_t(GIL_MIL);
    vm.unsafe_call_bl(check);
    require(status(1)==Status::Obsolete && status(3)==Status::Obsolete,
            "the next check must use live mission and guild values");
    vm.observe_instructions({});
    vm.observe_externals({});
    require((instructions==0)==native && vm.unsafe_get_gi()==oldGi,"quest closure must restore the context and select the expected path");
    return std::tuple(atCutoff,archiveEntries(save()),logCalls,std::string(topicLocal->get_string()),
                      locals[0]->get_int(),locals[1]->get_int(),locals[2]->get_int());
    };
  const auto stock=run(false);
  require(run(true)==stock,"all ordered quest updates and the serialized journal must match stock");
  load();
  hero.guild=oldGuild;
  for(size_t i=0;i<inputs.size();++i) inputs[i]->set_int(previousInputs[i]);
  for(size_t i=0;i<locals.size();++i) locals[i]->set_int(previousLocals[i]);
  topicLocal->set_string(oldTopic);
  events=previousEvents;
  std::puts("Quest closure: stock priority, chapter, guild, live flags, saved journal and ordered updates match");
  }
