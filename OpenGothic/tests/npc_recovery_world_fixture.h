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
  auto& spellChoice=*script.findSymbol("B_SELECTSPELL");
  auto& ready=*script.findSymbol("B_READYSPELL");
  auto& kill=*script.findSymbol("C_NPCHASATTACKREASONTOKILL");
  size_t createBody=0,selectBody=0,spellChoiceBody=0,readyBody=0,killBody=0;
  vm.observe_instructions([&](const auto&) {
    createBody+=vm.pc()==create.address();
    selectBody+=vm.pc()==select.address();
    spellChoiceBody+=vm.pc()==spellChoice.address();
    readyBody+=vm.pc()==ready.address();
    killBody+=vm.pc()==kill.address();
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
