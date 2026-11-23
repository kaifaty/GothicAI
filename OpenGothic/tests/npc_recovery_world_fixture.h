// World contracts for death, unconsciousness, healing and equipment.

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
    const auto beforeXp=hero.experience();
    const auto dead=script.findFunction("ZS_DEAD");
    const auto initPc=script.findSymbol("ZS_DEAD")->address();
    const auto loopPc=script.findSymbol("ZS_DEAD_LOOP")->address();
    size_t initVm=0,loopVm=0,deathLoot=0,dragonChecks=0;
    const auto calls=vm.add_call_observer([&](const auto& symbol) {
      deathLoot+=symbol.name()=="B_GIVEDEATHINV";
      dragonChecks+=symbol.name()=="B_DRAGONKILLCOUNTER";
      });
    vm.observe_instructions([&](const auto&) {
      initVm+=vm.pc()==initPc;
      loopVm+=vm.pc()==loopPc;
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
    auto outcome=consequences(*npc,hero);
    if(!native) original=std::move(outcome);
    else {
      require(outcome==original,"death: the two executors agree on queue, loot, flags and experience");
      npc->setStateTime(750);
      corpse=save("inside death state");
      }
    }

  load(corpse);
  auto& world=*gothic.world();
  auto& script=world.script();
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
  const auto& effects=NpcDefinitionContract::items(script).restoreEffects;
  const auto self=vm.find_symbol_by_name("SELF")->index();
  const auto change=vm.find_symbol_by_name("NPC_CHANGEATTRIBUTE")->index();
  size_t health=0,mana=0;
  for(const auto& fn:vm.symbols()) {
    if(!fn.name().starts_with("USE") || fn.type()!=Type::FUNCTION || fn.rtype()!=Type::VOID ||
       !fn.is_const() || fn.is_external() || fn.address()>=vm.size())
      continue;
    auto pc=fn.address();
    std::array<daedalus::DaedalusInstruction,5> ops;
    for(auto& op:ops) {
      if(pc>=vm.size()) break;
      op=vm.instruction_at(pc);
      pc+=op.size;
      }
    if(ops[0].op!=Op::PUSHVI || ops[0].symbol!=self || ops[1].op!=Op::PUSHV || ops[2].op!=Op::PUSHV ||
       ops[3].op!=Op::BE || ops[3].symbol!=change || ops[4].op!=Op::RSR)
      continue;
    const auto* attr=vm.find_symbol_by_index(ops[1].symbol);
    const auto* dose=vm.find_symbol_by_index(ops[2].symbol);
    require(attr && dose && attr->is_const() && dose->is_const() && attr->type()==Type::INT && dose->type()==Type::INT &&
            attr->count()==1 && dose->count()==1 && dose->get_int()>0,
            "stock restores: both arguments must be scalar constants and the dose positive");
    const auto value=attr->get_int();
    require(value==ATR_HITPOINTS || value==ATR_MANA,"stock restores: the callback restores a current attribute");
    const auto effect=effects.find(fn.index());
    require(effect!=effects.end() && effect->second.attribute==value && effect->second.amountSymbol==dose->index(),
            ("stock restores: exact callback and argument match for "+fn.name()).c_str());
    health+=value==ATR_HITPOINTS;
    mana+=value==ATR_MANA;
    }
  require(effects.size()==health+mana && health==29 && mana==7,
          "stock restores: cover exactly all 29 health and 7 mana callbacks and no others");
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
  size_t createBody=0,selectBody=0;
  vm.observe_instructions([&](const auto&) {
    createBody+=vm.pc()==create.address();
    selectBody+=vm.pc()==select.address();
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
  require(createBody==(native ? 0u : 4u) && selectBody==(native ? 0u : 5u),
          "equipment: the selected executor runs both helpers through their normal BL entry");
  vm.observe_instructions({});
  vm.global_self()->set_instance(previousSelf);
  vm.global_item()->set_instance(previousItem);
  hero.setPosition(oldHeroPosition);
  std::vector<size_t> inventory;
  for(auto item=actor.inventory().iterator(Inventory::T_Ransack);item.isValid();++item)
    inventory.push_back(item->clsId());
  return std::make_pair(std::move(inventory),activityQueue(actor));
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
    script.setSeed(73106);
    const auto beforeXp=hero.experience();
    const auto wakeAt=int64_t(script.findSymbol("HAI_TIME_UNCONSCIOUS")->get_int())*1000;
    auto& vm=script.getVm();
    size_t phaseCalls=0,rewards=0,minimalCalls=0,restoreCalls=0;
    const auto observer=vm.add_call_observer([&](const auto& symbol) {
      phaseCalls+=symbol.name().starts_with("ZS_UNCONSCIOUS");
      rewards+=symbol.name()=="B_GIVEPLAYERXP";
      minimalCalls+=symbol.name()=="PERCEPTION_SET_MINIMAL";
      restoreCalls+=symbol.name().starts_with("USEITPO_HEALTH_") || symbol.name()=="USEITPO_MANA_01" || symbol.name()=="USE_CHEESE";
      });

    require(Settings::physicsFlags().meleeHealInterrupt,"health potions: the fixture uses deferred potion effects");
    const auto attributes=std::to_array(npc->handle().attribute);
    const auto heroHealth=hero.attribute(ATR_HITPOINTS);
    const auto previousSelf=vm.global_self()->get_instance();
    vm.global_self()->set_instance(std::static_pointer_cast<daedalus::INpc>(hero.handlePtr()));
    const auto self=vm.global_self()->get_instance(),other=vm.global_other()->get_instance(),
               victim=vm.global_victim()->get_instance(),itemGlobal=vm.global_item()->get_instance();
    const std::array potionUse={std::tuple{"ITPO_HEALTH_01",ATR_HITPOINTS,20,70},
                                std::tuple{"ITPO_HEALTH_02",ATR_HITPOINTS,20,90},
                                std::tuple{"ITPO_HEALTH_03",ATR_HITPOINTS,20,120},
                                std::tuple{"ITPO_HEALTH_03",ATR_HITPOINTS,125,200},
                                std::tuple{"ITPO_MANA_01",ATR_MANA,20,70}};
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
      if(!native && after==200) save(pendingPotion,"VM-era save with a potion in use");
      Contract::finishItemUseAnimation(*npc);
      require(Contract::pendingItemUse(*npc)==0 && npc->attribute(attribute)==after &&
              npc->itemCount(id)==initialItems && hero.attribute(ATR_HITPOINTS)==heroHealth &&
              vm.global_self()->get_instance()==self && vm.global_other()->get_instance()==other &&
              vm.global_victim()->get_instance()==victim && vm.global_item()->get_instance()==itemGlobal,
              "item restores: the tag restores only the drinker once, consumes one item and restores the VM context");
      }
    require(Settings::physicsFlags().foodOverTime,"item restores: the fixture uses gradual food effects");
    npc->handle().attribute[ATR_HITPOINTS]=20;
    const auto [cheese,initialCheese]=startUse("ITFO_CHEESE");
    Contract::finishItemUseAnimation(*npc);
    const auto& pool=Contract::foodPool(*npc);
    require(npc->attribute(ATR_HITPOINTS)==20 && pool.hp==15 && pool.mana==0 && pool.remaining==Healing::FoodDuration &&
            npc->itemCount(cheese)==initialCheese,"item restores: food adds its dose to the existing gradual pool");
    require(restoreCalls==(native ? 0u : potionUse.size()+1),"item restores: only fallback executes the callback bodies");
    vm.global_self()->set_instance(previousSelf);
    std::copy(attributes.begin(),attributes.end(),npc->handle().attribute);
    npc->changeAttribute(ATR_HITPOINTS,-npc->attribute(ATR_HITPOINTS),true);
    npc->setOther(&hero);
    require(npc->isUnconscious() && !npc->isDead() && npc->attribute(ATR_HITPOINTS)==1,
            "unconscious: nonlethal HP loss enters the engine knockout path");
    {
      ActivityStream stream(world,*npc);
      Contract::tickRoutine(*npc);
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
  require(potionScript.findSymbol("B_SELECTSPELL.DK_RND")->get_int()==int32_t(std::mt19937(73106)()%10),
          "equipment: normal load keeps the original saved spell-selection draw");
  auto* drinker=activityNpc(potionWorld,actor);
  const auto potion=potionScript.findSymbolIndex("ITPO_HEALTH_03");
  require(drinker && Contract::pendingItemUse(*drinker)==potion && drinker->attribute(ATR_HITPOINTS)==125 &&
          drinker->itemCount(potion)>0,"health potions: load restores a pending VM-era drink without healing");
  const auto before=potionWorld.player()->attribute(ATR_HITPOINTS);
  const auto count=drinker->itemCount(potion);
  auto& potionVm=potionScript.getVm();
  size_t loadedPotionCalls=0;
  const auto potionObserver=potionVm.add_call_observer([&](const auto& symbol) {
    loadedPotionCalls+=symbol.name()=="USEITPO_HEALTH_03";
    });
  Contract::finishItemUseAnimation(*drinker);
  potionVm.remove_call_observer(potionObserver);
  require(Contract::pendingItemUse(*drinker)==0 && drinker->attribute(ATR_HITPOINTS)==200 &&
          drinker->itemCount(potion)==count-1 && potionWorld.player()->attribute(ATR_HITPOINTS)==before && loadedPotionCalls==0,
          "health potions: the saved drink finishes with the same capped effect and one consumption");
  gothic.clearGame().reset();
  std::puts("Health potions: queued doses, animation delivery and VM-era save match both executors");
  }
