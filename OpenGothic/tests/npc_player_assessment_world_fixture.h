#pragma once

void talkTopicAssignments(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  const auto before=events;
  auto& actor=npc.handle();
  auto& player=hero.handle();
  const auto originalActor=std::tuple(actor.guild,actor.type,actor.voice,npc.attitude());
  const auto npcVars=std::to_array(actor.aivar),heroVars=std::to_array(player.aivar);
  const auto npcQueue=Contract::queue(npc),heroQueue=Contract::queue(hero);
  auto& talkFix=*vm.find_symbol_by_name("ZSTALKBUGFIX");
  const auto savedFix=talkFix.get_int();
  enum Site { FakeBandit, AmbientInfos, City, OldCamp, Guide, Potion, Tough, ToughNews, Ambient, Crime, Absolution, AmbientNews };
  constexpr std::array helpers={"C_PLAYERISFAKEBANDIT","B_ASSIGNAMBIENTINFOS","C_NPCBELONGSTOCITY","C_NPCBELONGSTOOLDCAMP",
      "B_ASSIGNCITYGUIDE","B_ADDON_GIVEPOTION","C_NPCISTOUGHGUY","B_ASSIGNTOUGHGUYNEWS","C_NPCHASAMBIENTNEWS",
      "B_GETPLAYERCRIME","B_GETCURRENTABSOLUTIONLEVEL","B_ASSIGNAMBIENTNEWS"};
  std::array<daedalus::DaedalusSymbol*,helpers.size()> slots;
  std::array<std::shared_ptr<daedalus::DaedalusInstance>,helpers.size()> savedSlots;
  std::array<uint32_t,helpers.size()> addresses;
  for(size_t i=0;i<helpers.size();++i) {
    addresses[i]=vm.find_symbol_by_name(helpers[i])->address();
    slots[i]=vm.find_symbol_by_name(std::string(helpers[i])+".SLF");
    savedSlots[i]=slots[i]->get_instance();
    }
  constexpr std::array topics={"DIA_CITYGUIDE","DIA_ADDON_GIVEPOTION","DIA_TOUGHGUY_NEWS","DIA_AMBIENT_NEWS"};
  std::array<std::shared_ptr<daedalus::IInfo>,topics.size()> infos;
  std::ranges::transform(topics,infos.begin(),[&](const auto* name) {
    return std::static_pointer_cast<daedalus::IInfo>(vm.find_symbol_by_name(name)->get_instance());
    });
  std::array<int32_t,topics.size()> savedOwners;
  std::ranges::transform(infos,savedOwners.begin(),[](const auto& info) { return info->npc; });
  auto& disguiseOther=*vm.find_symbol_by_name("C_PLAYERISFAKEBANDIT.OTH");
  const auto savedDisguiseOther=disguiseOther.get_instance();
  auto& disguiseArmor=*vm.find_symbol_by_name("C_PLAYERISFAKEBANDIT.ITM");
  const auto savedDisguiseArmor=disguiseArmor.get_instance();
  const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto init=script.findFunction("ZS_TALK");
  constexpr std::array ambientTopics={"DIA_MIL_6_EXIT","DIA_MIL_6_JOIN","DIA_MIL_6_PEOPLE","DIA_MIL_6_LOCATION","DIA_MIL_6_STANDARD",
      "DIA_OCVLK_6_EXIT","DIA_OCVLK_6_PEOPLE","DIA_OCVLK_6_LOCATION","DIA_OCVLK_6_STANDARD"};
  std::array<std::shared_ptr<daedalus::IInfo>,ambientTopics.size()> ambientInfos;
  std::ranges::transform(ambientTopics,ambientInfos.begin(),[&](const auto* name) {
    return std::static_pointer_cast<daedalus::IInfo>(vm.find_symbol_by_name(name)->get_instance());
    });
  std::array<int32_t,ambientTopics.size()> savedAmbientOwners;
  std::ranges::transform(ambientInfos,savedAmbientOwners.begin(),[](const auto& info) { return info->npc; });
  const std::array ambientSlots{vm.find_symbol_by_name("B_ASSIGNAMBIENTINFOS_MIL_6.SLF"),
                                vm.find_symbol_by_name("B_ASSIGNAMBIENTINFOS_OCVLK_6.SLF")};
  const std::array savedAmbientSlots{ambientSlots[0]->get_instance(),ambientSlots[1]->get_instance()};
  actor.guild=GIL_MIL;
  actor.voice=6;
  for(bool oldCamp:{false,true}) {
    actor.type=oldCamp ? NpcType::G2_OC_AMBIENT : NpcType::G2_AMBIENT;
    npc.setAttitude(oldCamp ? ATT_ANGRY : ATT_FRIENDLY);
    for(bool native:{false,true}) {
      if(native && (!before || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
        continue;
      events=native;
      actor.aivar[25]=actor.aivar[58]=int(oldCamp);
      actor.aivar[57]=1;
      actor.aivar[15]=int(!oldCamp);
      actor.aivar[73]=1; // The hostile actor ignores armor, so the stock armor slot is untouched.
      actor.aivar[1]=script.intSymbol("CRIME_THEFT",-1);
      actor.aivar[2]=int32_t(world.time().day());
      actor.aivar[46]=script.intSymbol("ABSOLUTIONLEVEL_CITY",-1);
      player.aivar[4]=0;
      Contract::queue(npc).clear();
      Contract::queue(hero).clear();
      for(auto* slot:slots) slot->set_instance(nullptr);
      disguiseOther.set_instance(nullptr);
      for(const auto& info:infos) info->npc=-1;
      for(const auto& info:ambientInfos) info->npc=-1;
      for(auto* slot:ambientSlots) slot->set_instance(nullptr);
      std::vector<Site> called;
      vm.observe_instructions([&](const auto&) {
        if(const auto it=std::ranges::find(addresses,vm.pc()); it!=addresses.end())
          called.push_back(Site(it-addresses.begin()));
        });
      script.invokeState(&npc,&hero,nullptr,init);
      vm.observe_instructions({});
      const auto expected=oldCamp ? std::vector<Site>{FakeBandit,AmbientInfos,City,OldCamp,Tough,Ambient} :
          std::vector<Site>{AmbientInfos,City,OldCamp,Guide,Potion,Tough,ToughNews,Ambient,Crime,Absolution,OldCamp,City,OldCamp,AmbientNews};
      require(called==(native ? std::vector<Site>{} : expected),
              "talk topics: the assigned topics and nested crime callbacks keep their order");
      for(size_t i=0;i<slots.size();++i) {
        const bool used=std::ranges::find(expected,Site(i))!=expected.end();
        require(slots[i]->get_instance()==(used ? self : nullptr),"talk topics: stock saved formals keep their recipients");
        }
      for(const auto& info:infos)
        require(info->npc==(oldCamp ? -1 : int32_t(npc.instanceSymbol())),"talk topics: stock info recipients agree");
      const auto speaker=int32_t(npc.instanceSymbol());
      for(size_t i=0;i<ambientInfos.size();++i)
        require(ambientInfos[i]->npc==((i>=5)==oldCamp ? speaker : -1),
                "talk topics: the ambient assignment uses the actor's type and voice");
      require(ambientSlots[size_t(oldCamp)]->get_instance()==self && ambientSlots[size_t(!oldCamp)]->get_instance()==nullptr,
              "talk topics: only the selected voice helper saves the actor");
      require(disguiseOther.get_instance()==(oldCamp ? other : nullptr) && disguiseArmor.get_instance()==savedDisguiseArmor,
              "talk topics: disguise ignores armor in the stock actor's context");
      }
    }
  for(size_t i=0;i<slots.size();++i) slots[i]->set_instance(savedSlots[i]);
  for(size_t i=0;i<infos.size();++i) infos[i]->npc=savedOwners[i];
  for(size_t i=0;i<ambientInfos.size();++i) ambientInfos[i]->npc=savedAmbientOwners[i];
  for(size_t i=0;i<ambientSlots.size();++i) ambientSlots[i]->set_instance(savedAmbientSlots[i]);
  disguiseOther.set_instance(savedDisguiseOther);
  disguiseArmor.set_instance(savedDisguiseArmor);
  const auto [guild,type,voice,attitude]=originalActor;
  actor.guild=guild;
  actor.type=type;
  actor.voice=voice;
  npc.setAttitude(attitude);
  npc.startFaceAnim("S_NEUTRAL",1.f,uint64_t(-1)); // Restore the face after the hostile talk trial.
  std::copy(npcVars.begin(),npcVars.end(),actor.aivar);
  std::copy(heroVars.begin(),heroVars.end(),player.aivar);
  Contract::queue(npc)=npcQueue;
  Contract::queue(hero)=heroQueue;
  talkFix.set_int(savedFix);
  events=before;
  }

void specialDamagePerceptions(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const auto wasEnabled=enabled;
  const auto symbol=[&](std::string_view name) { return vm.find_symbol_by_name(name); };
  auto& body=npc.handle();
  auto& player=hero.handle();
  const auto savedNpcVars=std::to_array(body.aivar);
  const auto savedNpcHp=body.attribute[ATR_HITPOINTS],savedNpcMax=body.attribute[ATR_HITPOINTSMAX];
  const auto savedHeroHp=player.attribute[ATR_HITPOINTS],savedHeroMax=player.attribute[ATR_HITPOINTSMAX];
  const auto savedNpcFlags=body.flags,savedHeroFlags=player.flags;
  const auto savedState=Contract::aiState(npc).funcIni;
  const auto savedPerception=Contract::perceptions(npc)[PERC_ASSESSDAMAGE];
  const auto savedNpcQueue=Contract::queue(npc),savedHeroQueue=Contract::queue(hero);
  const auto savedRandom=Contract::random(script);
  const auto savedItem=vm.global_item()->get_instance();
  const auto attacker=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto defender=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto ghostPc=symbol("B_GHOSTSPECIALDAMAGE")->address();
  const auto beliarPc=symbol("B_BELIARSWEAPONSPECIALDAMAGE")->address();
  const auto readyPc=symbol("C_SCHASREADIEDBELIARSWEAPON")->address();
  std::array pointers={symbol("NONE_ADDON_111_QUARHODRON"),symbol("RAVEN"),
      symbol("B_GHOSTSPECIALDAMAGE.SLF"),symbol("B_GHOSTSPECIALDAMAGE.OTH"),symbol("B_GHOSTSPECIALDAMAGE.QUARHO"),
      symbol("B_GHOSTSPECIALDAMAGE.RHADEM"),symbol("B_GHOSTSPECIALDAMAGE.HER"),
      symbol("B_BELIARSWEAPONSPECIALDAMAGE.SLF"),symbol("B_BELIARSWEAPONSPECIALDAMAGE.OTH"),
      symbol("C_SCHASREADIEDBELIARSWEAPON.READYWEAP"),symbol("C_ISITEMBELIARSWEAPON.WEAP")};
  std::array<std::shared_ptr<daedalus::DaedalusInstance>,pointers.size()> savedPointers;
  std::ranges::transform(pointers,savedPointers.begin(),[](auto* slot) { return slot->get_instance(); });
  std::array numbers={symbol("GHOSTATTACKWARN"),symbol("BELIARDAMAGECHANCE"),symbol("RAVENBLITZ"),
                      symbol("B_BELIARSWEAPONSPECIALDAMAGE.RAVENRANDY"),symbol("B_BELIARSWEAPONSPECIALDAMAGE.DAMAGERANDY")};
  std::array<int32_t,numbers.size()> savedNumbers;
  std::ranges::transform(numbers,savedNumbers.begin(),[](auto* slot) { return slot->get_int(); });
  const auto* rage=Gothic::inst().loadVisualFx("spellFX_BELIARSRAGE");
  const auto* collide=Gothic::inst().loadVisualFx("spellFX_BELIARSRAGE_COLLIDE");
  const auto fx=[&](const Npc& actor) {
    std::string result;
    for(const auto& effect:Contract::effects(actor)) {
      if(effect.view.handle()==rage) result+='R';
      if(effect.view.handle()==collide) result+='C';
      }
    return result;
    };
  require(rage && collide && fx(npc).empty() && fx(hero).empty(),
          "special damage: both original effects are available and the fixture starts clear");
  auto& weaponSymbol=*symbol("ITMW_BELIARWEAPON_1H_01");
  const auto savedWeaponAlias=weaponSymbol.get_instance();
  const auto weapon=weaponSymbol.index();
  const bool temporaryWeapon=hero.itemCount(weapon)==0;
  if(temporaryWeapon)
    hero.addItem(weapon,1);
  hero.setToFightMode(weapon);
  require(hero.activeWeapon() && hero.activeWeapon()->clsId()==weapon,"special damage: the real Claw is ready");
  const auto claw=std::static_pointer_cast<daedalus::IItem>(hero.activeWeapon()->handlePtr());
  const auto damage=script.findFunction("B_ASSESSDAMAGE");
  enum class Site { GhostWarn, GhostKill, Claw, Glow, Reflect, Raven, RavenReset };
  for(const auto site:{Site::GhostWarn,Site::GhostKill,Site::Claw,Site::Glow,Site::Reflect,Site::Raven,Site::RavenReset}) {
    const bool ghost=site==Site::GhostWarn || site==Site::GhostKill;
    const bool raven=site==Site::Raven || site==Site::RavenReset;
    const bool checkClaw=!ghost && !raven;
    for(bool native:{false,true}) {
      enabled=native;
      Contract::queue(npc).clear();
      Contract::queue(hero).clear();
      body.flags=player.flags=NpcFlag::NONE;
      body.attribute[ATR_HITPOINTSMAX]=player.attribute[ATR_HITPOINTSMAX]=500;
      body.attribute[ATR_HITPOINTS]=player.attribute[ATR_HITPOINTS]=400;
      body.aivar[15]=1;
      body.aivar[43]=site==Site::Reflect ? script.intSymbol("ID_DRAGON_UNDEAD",-1) : 0;
      Contract::aiState(npc).funcIni=script.findFunction("ZS_ATTACK");
      npc.setPerceptionEnable(PERC_ASSESSDAMAGE,damage.ptr);
      pointers[0]->set_instance(ghost ? defender : nullptr);
      pointers[1]->set_instance(raven ? defender : nullptr);
      for(size_t i=2;i<pointers.size();++i)
        pointers[i]->set_instance(nullptr);
      numbers[0]->set_int(site==Site::GhostKill ? 2 : 1);
      numbers[1]->set_int(site==Site::Glow ? 0 : 99);
      numbers[2]->set_int(site==Site::RavenReset ? 3 : 0);
      numbers[3]->set_int(-1);
      numbers[4]->set_int(-1);
      vm.global_item()->set_instance(nullptr);
      weaponSymbol.set_instance(nullptr);
      script.setSeed(73106);
      auto random=Contract::random(script);
      const int expectedDamageRoll=checkClaw ? int(random()%100) : -1;
      const int expectedRavenRoll=site==Site::RavenReset ? int(random()%3) : -1;
      size_t ghostCode=0,beliarCode=0,readyCode=0;
      vm.observe_instructions([&](const auto&) {
        ghostCode+=vm.pc()==ghostPc;
        beliarCode+=vm.pc()==beliarPc;
        readyCode+=vm.pc()==readyPc;
        });
      require(npc.passPerception(hero,&npc,PERC_ASSESSDAMAGE),"special damage: deliver the stock damage callback");
      vm.observe_instructions({});
      require(ghostCode==size_t(!native && ghost) && beliarCode==size_t(!native && !ghost) &&
              readyCode==2u*size_t(!native && checkClaw) && numbers[3]->get_int()==expectedRavenRoll &&
              numbers[4]->get_int()==expectedDamageRoll && Contract::random(script)==random,
              "special damage: routing, both weapon checks and the original RNG draws agree");
      const auto heroHp=site==Site::GhostWarn ? 200 : site==Site::GhostKill ? 0 : site==Site::Reflect ? 300 : site==Site::Raven ? 350 : 400;
      require(body.attribute[ATR_HITPOINTS]==(site==Site::Claw ? 300 : 400) && player.attribute[ATR_HITPOINTS]==heroHp &&
              numbers[0]->get_int()==(site==Site::GhostKill ? 3 : ghost ? 2 : 1) &&
              numbers[2]->get_int()==(site==Site::Raven ? 1 : 0),
              "special damage: warnings, ordinary damage, reflection and Raven's cadence preserve the original health");
      require(fx(npc)==(raven ? "C" : site==Site::Claw ? "R" : "") &&
              fx(hero)==(ghost || site==Site::Raven ? "R" : site==Site::Claw ? "CC" : site==Site::Reflect ? "RCC" : site==Site::Glow ? "C" : ""),
              "special damage: the original actors receive the same visual effects in order");
      if(ghost) {
        require(pointers[2]->get_instance()==defender && pointers[3]->get_instance()==attacker &&
                pointers[4]->get_instance()==defender && pointers[6]->get_instance()==attacker,
                "special damage: direct arguments and quest aliases keep their original roles");
        if(site==Site::GhostKill)
          require(hero.aiQueueFront() && hero.aiQueueFront()->act==AI_PlayAnim && hero.aiQueueFront()->s0=="T_DEAD",
                  "special damage: the third warning queues the stock death clip");
        } else {
        require(pointers[7]->get_instance()==defender && pointers[8]->get_instance()==attacker,
                "special damage: the Claw helper keeps the original arguments");
        if(checkClaw)
          require(pointers[9]->get_instance()==claw && pointers[10]->get_instance()==claw &&
                  weaponSymbol.get_instance()==claw && vm.global_item()->get_instance()==nullptr,
                  "special damage: finding the readied Claw sets its instance symbol and saved slots, leaving ITEM clear");
        }
      for(auto* actor:{&npc,&hero}) {
        while(!fx(*actor).empty()) {
          actor->stopEffect(*rage);
          actor->stopEffect(*collide);
          }
        }
      }
    }
  enabled=wasEnabled;
  hero.closeWeapon(true);
  if(temporaryWeapon)
    hero.delItem(weapon,1);
  body.flags=savedNpcFlags;
  player.flags=savedHeroFlags;
  body.attribute[ATR_HITPOINTS]=savedNpcHp;
  body.attribute[ATR_HITPOINTSMAX]=savedNpcMax;
  player.attribute[ATR_HITPOINTS]=savedHeroHp;
  player.attribute[ATR_HITPOINTSMAX]=savedHeroMax;
  std::copy(savedNpcVars.begin(),savedNpcVars.end(),body.aivar);
  Contract::aiState(npc).funcIni=savedState;
  Contract::perceptions(npc)[PERC_ASSESSDAMAGE]=savedPerception;
  Contract::queue(npc)=savedNpcQueue;
  Contract::queue(hero)=savedHeroQueue;
  Contract::random(script)=savedRandom;
  for(size_t i=0;i<pointers.size();++i) pointers[i]->set_instance(savedPointers[i]);
  for(size_t i=0;i<numbers.size();++i) numbers[i]->set_int(savedNumbers[i]);
  weaponSymbol.set_instance(savedWeaponAlias);
  vm.global_item()->set_instance(savedItem);
  std::puts("Special damage: ghost warnings, the Claw and Raven agree with the VM");
  }

void productionDialogEnd(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  auto& production=*vm.find_symbol_by_name("PLAYER_MOBSI_PRODUCTION");
  auto& count=*vm.find_symbol_by_name("TRUEMMER_COUNT");
  auto& savedHero=*vm.find_symbol_by_name("GOLDHACKEN_S1.HER");
  const auto previousMode=production.get_int();
  const auto previousCount=count.get_int();
  const auto previousHero=savedHero.get_instance();
  const auto previousOrder=Contract::outputOrder(script);
  const auto previousInvincible=hero.handle().aivar[4];
  const auto previousQueue=Contract::queue(hero);
  const auto previousContext=vm.unsafe_get_gi();
  const auto context=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto player=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  auto* close=vm.find_symbol_by_name("B_ENDPRODUCTIONDIALOG");
  StoryRuntime story(nullptr);
  story.ensureLifter();
  const auto program=storygraph::liftProgram(vm,story.bytecode(),script.findFunction("PC_GOLDHACKEN_END_INFO").ptr);
  require(program.error.empty(),program.error.c_str());
  StoryRuntime::Bind binding(vm,&hero,&hero);
  vm.unsafe_set_gi(context);
  for(bool native:{false,true}) {
    if(native && (!wasEnabled || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
      continue;
    enabled=native;
    Contract::queue(hero).clear();
    hero.handle().aivar[4]=0;
    production.set_int(script.intSymbol("MOBSI_NONE",-1));
    script.useInteractive(hero.handlePtr(),"GOLDHACKEN_S1");
    const auto* open=hero.aiQueueFront();
    require(production.get_int()==script.intSymbol("MOBSI_GOLDHACKEN",-1) && hero.handle().aivar[4]==1 && open &&
            open->act==AI_ProcessInfo && open->target==&hero,"production dialog: the stock work callback opens the hero's menu");
    count.set_int(7);
    vm.push_int(173);
    size_t bytecode=0;
    vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==close->address(); });
    const auto result=story.interpreter(vm).run(program);
    vm.observe_instructions({});
    require(result.ok,result.error.c_str());
    auto queue=Contract::queue(hero);
    require(queue.size()==2 && queue.pop().act==AI_ProcessInfo,"production dialog: opening precedes the close command");
    const auto stop=queue.pop();
    require(stop.act==AI_StopProcessInfo && stop.i0==0 && Contract::outputOrder(script)==1 && count.get_int()==0 &&
            production.get_int()==script.intSymbol("MOBSI_NONE",-1) && hero.handle().aivar[4]==0,
            "production dialog: the real menu choice closes the menu, clears protection and preserves command order");
    require(bytecode==size_t(!native) && vm.global_self()->get_instance()==player && vm.global_other()->get_instance()==player &&
            vm.unsafe_get_gi()==context && vm.pop_int()==173,"production dialog: the chosen executor preserves the VM context");
    }
  enabled=wasEnabled;
  production.set_int(previousMode);
  count.set_int(previousCount);
  savedHero.set_instance(previousHero);
  hero.handle().aivar[4]=previousInvincible;
  Contract::queue(hero)=previousQueue;
  Contract::outputOrder(script)=previousOrder;
  vm.unsafe_set_gi(previousContext);
  std::puts("Production dialog: the hero's stock work menu closes through the chosen executor");
  }

void itemUseRoutes(World& world,Npc& npc,Npc& hero,bool seated) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  auto* use=vm.find_symbol_by_name("B_USEITEM");
  const auto stopPc=vm.find_symbol_by_name("B_STOPLOOKAT")->address();
  const auto lookPc=vm.find_symbol_by_name("B_LOOKATNPC")->address();
  const auto actor=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto target=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  auto& itemSlot=*vm.find_symbol_by_name("B_USEITEM.ITMINSTANCE");
  const auto previousItem=itemSlot.get_int();
  const auto item=int32_t(script.findSymbolIndex("ITFO_APPLE"));
  auto& state=*vm.find_symbol_by_name("C_BODYSTATECONTAINS.BODYSTATE");
  const auto previousState=state.get_int();
  std::array slots={vm.find_symbol_by_name("B_USEITEM.SLF"),vm.find_symbol_by_name("B_USEITEM.TARGET"),
                    vm.find_symbol_by_name("B_STOPLOOKAT.SLF"),vm.find_symbol_by_name("B_STOPLOOKAT.TARGET"),
                    vm.find_symbol_by_name("B_LOOKATNPC.SLF"),vm.find_symbol_by_name("B_LOOKATNPC.OTH"),
                    vm.find_symbol_by_name("C_BODYSTATECONTAINS.SLF")};
  std::array<std::shared_ptr<daedalus::DaedalusInstance>,slots.size()> previousSlots;
  std::ranges::transform(slots,previousSlots.begin(),[](auto* slot) { return slot->get_instance(); });
  const auto previousQueue=Contract::queue(npc);
  auto* previousLook=npc.lookAtTarget();
  const auto previousContext=vm.unsafe_get_gi();
  StoryRuntime::Bind binding(vm,&hero,&npc);
  vm.unsafe_set_gi(target);
  for(bool looking:{false,true}) {
    const bool resumesLook=looking && !seated;
    for(bool native:{false,true}) {
      enabled=native;
      Contract::queue(npc).clear();
      npc.aiPush(looking ? AiQueue::aiLookAtNpc(&hero) : AiQueue::aiStopLookAt());
      Contract::nextAiAction(npc);
      require(npc.lookAtTarget()==(looking ? &hero : nullptr),"use item: the current look is set in the engine before calling the helper");
      for(auto* slot:slots)
        slot->set_instance(nullptr);
      slots[1]->set_instance(target);
      itemSlot.set_int(-1);
      state.set_int(-1);
      vm.push_int(173);
      vm.push_instance(actor);
      vm.push_int(item);
      size_t useBytecode=0,stopBytecode=0,lookBytecode=0;
      vm.observe_instructions([&](const auto&) {
        useBytecode+=vm.pc()==use->address();
        stopBytecode+=vm.pc()==stopPc;
        lookBytecode+=vm.pc()==lookPc;
        });
      vm.unsafe_call_bl(use);
      vm.observe_instructions({});
      require(useBytecode==size_t(!native) && stopBytecode==2u*size_t(!native && resumesLook) &&
              lookBytecode==size_t(!native && resumesLook) && vm.pop_int()==173 &&
              vm.global_self()->get_instance()==target && vm.global_other()->get_instance()==actor &&
              vm.unsafe_get_gi()==target,"use item: original and native paths retain the caller's stack and actor context");
      require(itemSlot.get_int()==item && slots[0]->get_instance()==actor && slots[6]->get_instance()==actor &&
              state.get_int()==BS_SIT && slots[1]->get_instance()==(seated || looking ? target : nullptr),
              "use item: the seated early return preserves the previous target and records the original formals");
      if(!resumesLook)
        require(!slots[2]->get_instance() && !slots[3]->get_instance() && !slots[4]->get_instance() && !slots[5]->get_instance(),
                "use item: missing and seated looks leave both helper arguments alone");
      auto queue=Contract::queue(npc);
      require(queue.size()==(seated ? 0u : resumesLook ? 4u : 1u),"use item: only standing NPCs use the item");
      if(!seated) {
        if(resumesLook)
          require(queue.pop().act==AI_StopLookAt,"use item: stop looking before using the item");
        const auto useAction=queue.pop();
        require(useAction.act==AI_UseItem && useAction.i0==item,"use item: queue the same item instance");
        if(resumesLook) {
          require(queue.pop().act==AI_StopLookAt,"use item: the resumed look adds its own stop command");
          const auto look=queue.pop();
          require(look.act==AI_LookAtNpc && look.target==&hero && slots[2]->get_instance()==actor &&
                  slots[3]->get_instance()==target && slots[4]->get_instance()==actor && slots[5]->get_instance()==target,
                  "use item: restore the original look and its saved arguments");
          }
        }
      }
    }
  Contract::queue(npc).clear();
  npc.aiPush(previousLook ? AiQueue::aiLookAtNpc(previousLook) : AiQueue::aiStopLookAt());
  Contract::nextAiAction(npc);
  Contract::queue(npc)=previousQueue;
  enabled=wasEnabled;
  vm.unsafe_set_gi(previousContext);
  itemSlot.set_int(previousItem);
  state.set_int(previousState);
  for(size_t i=0;i<slots.size();++i)
    slots[i]->set_instance(previousSlots[i]);
  }

void fakeGuildAssessment(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto priorSelf=vm.global_self()->get_instance();
  auto& npcState=Contract::aiState(npc).funcIni;
  auto& heroState=Contract::aiState(hero).funcIni;
  const auto priorNpcState=npcState;
  const auto priorHeroState=heroState;
  const auto priorNpcGuild=npc.handle().guild;
  const auto priorHeroGuild=hero.handle().guild;
  const auto priorIgnoreArmor=npc.handle().aivar[73];
  const auto priorIgnoreDisguise=npc.handle().aivar[70];
  const auto priorArmor=hero.currentArmor() ? hero.currentArmor()->clsId() : size_t(-1);
  const auto guard=script.findFunction("ZS_GUARD_PASSAGE");
  auto& banditQuest=*vm.find_symbol_by_name("PLAYER_HASTALKEDTOBANDITCAMP");
  auto& level=*vm.find_symbol_by_name("CURRENTLEVEL");
  const auto priorQuest=banditQuest.get_int();
  const auto priorLevel=level.get_int();
  const auto addonLevel=script.intSymbol("ADDONWORLD_ZEN",-1);
  auto* check=vm.find_symbol_by_name("C_PLAYERHASFAKEGUILD");
  auto& savedSelf=*vm.find_symbol_by_name("C_PLAYERHASFAKEGUILD.SLF");
  auto& savedOther=*vm.find_symbol_by_name("C_PLAYERHASFAKEGUILD.OTH");
  auto& savedArmor=*vm.find_symbol_by_name("C_PLAYERHASFAKEGUILD.ITM");
  auto& comment=*vm.find_symbol_by_name("C_PLAYERHASFAKEGUILD.NSC_COMMENTRANGERARMOR");
  auto& banditArmor=*vm.find_symbol_by_name("C_PLAYERISFAKEBANDIT.ITM");
  const auto priorSavedSelf=savedSelf.get_instance();
  const auto priorSavedOther=savedOther.get_instance();
  const auto priorSavedArmor=savedArmor.get_instance();
  const auto priorComment=comment.get_int();
  const auto priorBanditArmor=banditArmor.get_instance();
  std::vector<size_t> temporaryArmor;
  const auto equip=[&](std::string_view name) {
    if(auto* old=hero.currentArmor()) hero.unequipItem(old->clsId());
    if(name.empty()) return;
    const auto id=script.findSymbolIndex(name);
    if(hero.itemCount(id)==0) {
      hero.addItem(id,1);
      temporaryArmor.push_back(id);
      }
    hero.useItem(id,Item::NSLOT,true);
    require(hero.currentArmor() && hero.currentArmor()->clsId()==id,"fake guild: the real item is equipped");
    };

  enum class Guard { None, Global, Argument };
  struct Site {
    int32_t npcGuild;
    int32_t heroGuild;
    std::string_view armor;
    int expected;
    size_t ignore=0;
    Guard guard=Guard::None;
    bool banditQuest=false;
    };
  for(const Site site: {
      Site{GIL_NONE,GIL_NONE,"",0},
      Site{GIL_NONE,GIL_NONE,"ITAR_BAU_M",0},
      Site{GIL_NONE,GIL_NONE,"ITAR_SLD_L",1},
      Site{GIL_DJG,GIL_DJG,"ITAR_SLD_L",0},
      Site{GIL_NOV,GIL_NOV,"ITAR_SLD_L",1},
      Site{GIL_NOV,GIL_DJG,"ITAR_SLD_L",0},
      Site{GIL_NONE,GIL_NONE,"ITAR_RANGER_ADDON",1},
      Site{GIL_NONE,GIL_NONE,"ITAR_RANGER_ADDON",0,73},
      Site{GIL_NONE,GIL_NONE,"ITAR_RANGER_ADDON",0,70},
      Site{GIL_NONE,GIL_NONE,"ITAR_RANGER_ADDON",0,0,Guard::Global},
      Site{GIL_NONE,GIL_NONE,"ITAR_RANGER_ADDON",1,0,Guard::Argument},
      Site{GIL_BDT,GIL_BDT,"",1,0,Guard::None,true},
      Site{GIL_BDT,GIL_BDT,"ITAR_BDT_M",0,0,Guard::None,true}}) {
    equip(site.armor);
    npc.handle().guild=site.npcGuild;
    hero.handle().guild=site.heroGuild;
    npc.handle().aivar[73]=int32_t(site.ignore==73);
    npc.handle().aivar[70]=int32_t(site.ignore==70);
    npcState=site.guard==Guard::Argument ? guard : priorNpcState;
    heroState=site.guard==Guard::Global ? guard : priorHeroState;
    vm.global_self()->set_instance(site.guard==Guard::None ? self : other);
    banditQuest.set_int(site.banditQuest ? 1 : priorQuest);
    level.set_int(site.banditQuest ? addonLevel : priorLevel);
    const auto armor=hero.currentArmor() ? std::static_pointer_cast<daedalus::IItem>(hero.currentArmor()->handlePtr()) : nullptr;
    for(bool native:{false,true}) {
      enabled=native;
      savedSelf.set_instance(nullptr);
      savedOther.set_instance(nullptr);
      savedArmor.set_instance(nullptr);
      comment.set_int(0);
      size_t bytecode=0;
      vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==check->address(); });
      vm.push_instance(self);
      vm.push_instance(other);
      vm.unsafe_call_bl(check);
      const auto result=vm.pop_int();
      vm.observe_instructions({});
      require(result==site.expected && bytecode==(native ? 0u : 1u),
              "fake guild: the real armor and guard or story state use the same stock decision");
      require(savedSelf.get_instance()==self && savedOther.get_instance()==other && savedArmor.get_instance()==armor &&
              comment.get_int()==1,"fake guild: all four original scratch values are recorded, including early exits");
      }
    }
  enabled=wasEnabled;
  npcState=priorNpcState;
  heroState=priorHeroState;
  npc.handle().guild=priorNpcGuild;
  hero.handle().guild=priorHeroGuild;
  npc.handle().aivar[73]=priorIgnoreArmor;
  npc.handle().aivar[70]=priorIgnoreDisguise;
  vm.global_self()->set_instance(priorSelf);
  savedSelf.set_instance(priorSavedSelf);
  savedOther.set_instance(priorSavedOther);
  savedArmor.set_instance(priorSavedArmor);
  comment.set_int(priorComment);
  banditArmor.set_instance(priorBanditArmor);
  banditQuest.set_int(priorQuest);
  level.set_int(priorLevel);
  equip("");
  for(const auto id:temporaryArmor) hero.delItem(id,1);
  if(priorArmor!=size_t(-1)) hero.useItem(priorArmor,Item::NSLOT,true);
  std::puts("Fake guild: armor, guard context and story exceptions match the VM");
  }

void guildGreetingRoutes(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto npcHandle=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto heroHandle=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto priorSelf=vm.global_self()->get_instance();
  const auto priorOther=vm.global_other()->get_instance();
  const auto priorGuild=npc.handle().guild;
  const auto priorHeroGuild=hero.handle().guild;
  const auto priorRandom=Contract::random(script);
  const auto order=Contract::outputOrder(script);
  auto* greeting=vm.find_symbol_by_name("B_SAY_GUILDGREETINGS");
  const auto bartokPc=vm.find_symbol_by_name("B_BARTOK_SHITANORC")->address();
  const auto cookPc=vm.find_symbol_by_name("B_KOCH_HACKEBEIL")->address();
  auto& bartok=*vm.find_symbol_by_name("BARTOK");
  auto& cook=*vm.find_symbol_by_name("KOCH");
  auto& quest=*vm.find_symbol_by_name("BARTOK_ORKGESAGT");
  auto& savedSelf=*vm.find_symbol_by_name("B_SAY_GUILDGREETINGS.SLF");
  auto& savedOther=*vm.find_symbol_by_name("B_SAY_GUILDGREETINGS.OTH");
  auto& variation=*vm.find_symbol_by_name("B_SAY_GUILDGREETINGS.ZUFALL");
  const auto priorBartok=bartok.get_instance();
  const auto priorCook=cook.get_instance();
  const auto priorQuest=quest.get_int();
  const auto priorSavedSelf=savedSelf.get_instance();
  const auto priorSavedOther=savedOther.get_instance();
  const auto priorVariation=variation.get_int();
  vm.global_self()->set_instance(npcHandle);
  vm.global_other()->set_instance(heroHandle);

  enum class Hint { None, Bartok, Cook };
  struct Site {
    int32_t speakerGuild;
    int32_t listenerGuild;
    std::string_view line;
    Hint hint=Hint::None;
    int quest=0;
    };
  for(const Site site:{
      Site{GIL_MIL,GIL_PAL,"$MILGREETINGS"},
      Site{GIL_PAL,GIL_KDF,"$PALGREETINGS"},
      Site{GIL_KDF,GIL_NOV,"$PALGREETINGS"},
      Site{GIL_KDF,GIL_MIL,""},
      Site{GIL_VLK,GIL_VLK,"DIA_Bartok_Angekommen_04_02",Hint::Bartok,1},
      Site{GIL_VLK,GIL_VLK,"",Hint::Bartok,2},
      Site{GIL_NOV,GIL_VLK,"Hackebeil_01_00",Hint::Cook}}) {
    const bool named=site.hint!=Hint::None;
    const bool ambient=named && !site.line.empty();
    const bool overlay=!named && !site.line.empty();
    const auto self=named ? heroHandle : npcHandle;
    const auto other=named ? npcHandle : heroHandle;
    self->guild=site.speakerGuild;
    other->guild=site.listenerGuild;
    bartok.set_instance(site.hint==Hint::Bartok ? self : nullptr);
    cook.set_instance(site.hint==Hint::Cook ? self : nullptr);
    quest.set_int(site.quest);
    for(bool native:{false,true}) {
      enabled=native;
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(173));
      Contract::outputOrder(script)=order;
      script.setSeed(73106);
      auto expectedRandom=Contract::random(script);
      const int expectedRoll=site.line.empty() ? int(expectedRandom()%100) : -1;
      savedSelf.set_instance(nullptr);
      savedOther.set_instance(nullptr);
      variation.set_int(-1);
      size_t greetingBytecode=0,ambientBytecode=0;
      vm.observe_instructions([&](const auto&) {
        greetingBytecode+=vm.pc()==greeting->address();
        ambientBytecode+=vm.pc()==bartokPc || vm.pc()==cookPc;
        });
      vm.push_instance(self);
      vm.push_instance(other);
      vm.unsafe_call_bl(greeting);
      vm.observe_instructions({});
      require(greetingBytecode==size_t(!native) && ambientBytecode==size_t(!native && ambient) &&
              savedSelf.get_instance()==self && savedOther.get_instance()==other &&
              variation.get_int()==expectedRoll && Contract::random(script)==expectedRandom,
              "guild greeting: exact script dispatch, saved formals and weather draw");
      auto queue=Contract::queue(npc);
      const auto& overlays=Contract::overlayQueue(npc);
      require(queue.size()==1u+size_t(ambient) && queue.pop().act==AI_Wait &&
              overlays.size()==size_t(overlay) && Contract::outputOrder(script)==order+int(!site.line.empty()),
              "guild greeting: speech uses the original output queues and order");
      const auto* speech=ambient ? queue.front() : overlays.front();
      if(speech)
        require(speech->act==(ambient ? AI_Output : AI_OutputSvmOverlay) &&
                speech->s0==site.line && speech->target==&hero && speech->i0==order,
                "guild greeting: faction lines use the formals and story lines use the current context");
      }
    }
  npc.clearAiQueue();
  enabled=wasEnabled;
  npc.handle().guild=priorGuild;
  hero.handle().guild=priorHeroGuild;
  vm.global_self()->set_instance(priorSelf);
  vm.global_other()->set_instance(priorOther);
  bartok.set_instance(priorBartok);
  cook.set_instance(priorCook);
  quest.set_int(priorQuest);
  savedSelf.set_instance(priorSavedSelf);
  savedOther.set_instance(priorSavedOther);
  variation.set_int(priorVariation);
  Contract::random(script)=priorRandom;
  Contract::outputOrder(script)=order;
  std::puts("Guild greetings: faction lines, ambient quest lines and weather draws match the VM");
  }

void stockPickpocket(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto symbol=[&](std::string_view name) -> daedalus::DaedalusSymbol& { return *vm.find_symbol_by_name(name); };
  auto& required=symbol("THEFTDEXGLOB");
  auto& reward=symbol("THEFTGOLDGLOB");
  auto& possessed=symbol("NPCOBSESSEDBYDMT");
  auto& victims=symbol("VICTIMCOUNT");
  auto& nextLevel=symbol("VICTIMLEVEL");
  auto& level=symbol("THIEFLEVEL");
  auto& dexParameter=symbol("C_BEKLAUEN.THEFTDEX");
  auto& goldParameter=symbol("C_BEKLAUEN.THEFTGOLD");
  std::array slots={&required,&reward,&possessed,&victims,&nextLevel,&level,&dexParameter,&goldParameter};
  std::array<int32_t,slots.size()> savedSlots;
  std::ranges::transform(slots,savedSlots.begin(),[](auto* slot) { return slot->get_int(); });
  const auto savedContext=vm.unsafe_get_gi();
  const auto savedItem=vm.global_item()->get_instance();
  const auto savedOrder=Contract::outputOrder(script);
  const auto savedNpcVars=std::to_array(npc.handle().aivar);
  const auto savedHeroVars=std::to_array(hero.handle().aivar);
  const auto savedQueue=Contract::queue(npc);
  const auto savedTarget=npc.target();
  const auto savedState=Contract::aiState(npc).funcIni;
  const auto savedTalent=hero.talentSkill(TALENT_PICKPOCKET);
  const auto savedDexterity=hero.handle().attribute[ATR_DEXTERITY];
  const auto savedXp=hero.handle().exp;
  const auto savedXpLimit=hero.handle().exp_next;
  const auto savedHeroLevel=hero.handle().level;
  const auto savedCrimes=Contract::crimeCounters(script);
  const auto savedQuests=script.questLog();
  const auto info=std::dynamic_pointer_cast<daedalus::IInfo>(symbol("DIA_HILDA_PICKPOCKET").get_instance());
  require(info!=nullptr,"pickpocket: Hilda's info is initialized");
  const auto savedChoices=info->choices;
  const auto gold=script.findSymbolIndex("ITMI_GOLD");
  const auto savedHeroGold=hero.itemCount(gold);
  const auto savedNpcGold=npc.itemCount(gold);
  const auto setGold=[&](Npc& owner,size_t amount) {
    const auto before=owner.itemCount(gold);
    if(before<amount) owner.addItem(gold,uint32_t(amount-before));
    if(before>amount) owner.delItem(gold,uint32_t(before-amount));
    };
  const auto restore=[&] {
    Contract::queue(npc)=savedQueue;
    npc.setTarget(savedTarget);
    Contract::aiState(npc).funcIni=savedState;
    std::copy(savedNpcVars.begin(),savedNpcVars.end(),npc.handle().aivar);
    std::copy(savedHeroVars.begin(),savedHeroVars.end(),hero.handle().aivar);
    hero.setTalentSkill(TALENT_PICKPOCKET,savedTalent);
    hero.handle().attribute[ATR_DEXTERITY]=savedDexterity;
    hero.handle().exp=savedXp;
    hero.handle().exp_next=savedXpLimit;
    hero.handle().level=savedHeroLevel;
    Contract::crimeCounters(script)=savedCrimes;
    Contract::questLog(script)=savedQuests;
    info->choices=savedChoices;
    setGold(npc,savedNpcGold);
    setGold(hero,savedHeroGold);
    for(size_t i=0;i<slots.size();++i)
      slots[i]->set_int(savedSlots[i]);
    vm.global_item()->set_instance(savedItem);
    vm.unsafe_set_gi(savedContext);
    Contract::outputOrder(script)=savedOrder;
    };
  StoryRuntime story(nullptr);
  story.ensureLifter();
  StoryRuntime::Bind binding(vm,&npc,&hero);
  auto& interpreter=story.interpreter(vm);
  constexpr auto conditionName="DIA_HILDA_PICKPOCKET_CONDITION";
  constexpr auto attemptName="DIA_HILDA_PICKPOCKET_DOIT";
  const auto condition=storygraph::liftProgram(vm,story.bytecode(),symbol(conditionName).index());
  const auto attempt=storygraph::liftProgram(vm,story.bytecode(),symbol(attemptName).index());
  require(condition.error.empty() && attempt.error.empty(),"pickpocket: the real Hilda dialogue must be available");
  const auto talk=script.findFunction("ZS_TALK");
  const auto check=[&] {
    const auto result=interpreter.run(condition);
    require(result.ok,result.error.c_str());
    return result.returned.i;
    };
  const auto questStates=[&] {
    std::vector<std::pair<std::string,QuestLog::Status>> ret;
    const auto& log=script.questLog();
    for(size_t i=0;i<log.questCount();++i)
      ret.emplace_back(log.quest(i).name,log.quest(i).status);
    return ret;
    };
  for(bool success:{false,true}) {
    std::vector<std::string> original;
    auto originalQuests=questStates();
    auto originalCrimes=savedCrimes;
    for(bool native:{false,true}) {
      restore();
      enabled=native;
      Contract::queue(npc).clear();
      Contract::aiState(npc).funcIni={};
      hero.handle().attribute[ATR_DEXTERITY]=26;
      hero.setTalentSkill(TALENT_PICKPOCKET,1);
      npc.handle().aivar[6]=0;
      possessed.set_int(0);
      required.set_int(-1);
      reward.set_int(-1);
      size_t bytecode=0;
      const std::array entries={symbol("C_BEKLAUEN").address(),symbol("B_BEKLAUEN").address(),
                                symbol("B_GIVETHIEFXP").address(),symbol("B_RESETTHIEFLEVEL").address()};
      vm.observe_instructions([&](const auto&) { bytecode+=std::ranges::find(entries,vm.pc())!=entries.end(); });
      require(check()==1 && required.get_int()==-1 && reward.get_int()==-1,
              "pickpocket: the menu preview leaves the previous offer intact outside ZS_TALK");
      Contract::aiState(npc).funcIni=talk;
      possessed.set_int(1);
      require(check()==0 && required.get_int()==-1 && reward.get_int()==-1 &&
              dexParameter.get_int()==26 && goldParameter.get_int()==35,
              "pickpocket: possession suppresses the offer and retains both original input slots");
      possessed.set_int(0);
      vm.push_int(20);
      vm.push_int(7);
      vm.unsafe_call_bl(&symbol("C_BEKLAUEN"));
      require(vm.pop_int()==1 && required.get_int()==10 && reward.get_int()==7,
              "pickpocket: a low-dexterity offer uses the original threshold of ten");
      hero.handle().attribute[ATR_DEXTERITY]=success ? 26 : 16;
      require(check()==1 && required.get_int()==26 && reward.get_int()==35,
              "pickpocket: the real Hilda offer is visible ten points below its execution requirement");
      victims.set_int(success ? 1 : 2);
      nextLevel.set_int(0);
      level.set_int(success ? 0 : 1);
      setGold(npc,50);
      hero.handle().exp=100;
      hero.handle().exp_next=500;
      hero.handle().level=1;
      const auto result=interpreter.run(attempt);
      vm.observe_instructions({});
      require(result.ok,result.error.c_str());
      require(bytecode==(native ? 0u : 6u) && vm.unsafe_get_gi()==savedContext && possessed.get_int()==0,
              "pickpocket: the selected executor and original story context are retained from condition to choice");
      auto queue=Contract::queue(npc);
      require(queue.size()==2 && level.get_int()==1,"pickpocket: both outcomes preserve the correct thief level and command count");
      if(success) {
        require(npc.itemCount(gold)==15 && hero.itemCount(gold)==savedHeroGold+35 &&
                npc.handle().aivar[6]==1 && hero.handle().exp==160 && victims.get_int()==2 && nextLevel.get_int()==3 &&
                queue.pop().act==AI_PrintScreen && queue.pop().act==AI_PrintScreen,
                "pickpocket: the real successful choice pays gold, marks the victim and awards the promoted experience");
        } else {
        require(npc.itemCount(gold)==50 && hero.itemCount(gold)==savedHeroGold &&
                npc.handle().aivar[6]==0 && hero.handle().exp==100 && victims.get_int()==1 && nextLevel.get_int()==0 &&
                queue.pop().act==AI_StopProcessInfo,"pickpocket: failure removes one victim and stops the dialogue before the attack");
        const auto attack=queue.pop();
        require(attack.act==AI_StartState && attack.func==script.findFunction("ZS_ATTACK") && attack.target==&hero &&
                npc.handle().aivar[9]==script.intSymbol("AR_THEFT",-1) && npc.handle().aivar[23]==1,
                "pickpocket: failure queues the original theft attack and warning delay");
        }
      if(native) {
        require(original==activityQueue(npc) && originalQuests==questStates() && originalCrimes==Contract::crimeCounters(script),
                "pickpocket: both outcomes keep the original localized queue, quests and crime memory");
        } else {
        original=activityQueue(npc);
        originalQuests=questStates();
        originalCrimes=Contract::crimeCounters(script);
        }
      }
    }
  restore();
  enabled=wasEnabled;
  std::puts("Pickpocket: Hilda's original offer, reward and failed theft match both executors");
  }

void playerAssessmentHelpers(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const auto savedEvents=enabled;
  const auto symbol=[&](std::string_view name) -> daedalus::DaedalusSymbol& { return *script.findSymbol(name); };
  const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto savedSelf=vm.global_self()->get_instance(), savedOther=vm.global_other()->get_instance();
  const auto savedGi=vm.unsafe_get_gi();
  const auto savedType=self->type;
  const auto savedGuild=self->guild;
  const auto savedVoice=self->voice;
  const auto savedAttitude=npc.attitude();
  const auto savedNpcPosition=npc.position(), savedHeroPosition=hero.position();
  const auto guardState=script.findFunction("ZS_GUARD_PASSAGE");
  auto& globalState=Contract::aiState(hero).funcIni;
  const auto savedGlobalState=globalState;
  auto& sneak=symbol("C_WANTTOREACTTOSNEAKER");
  auto& assign=symbol("B_ASSIGNDEMENTORTALK");
  auto& ambient=symbol("B_ASSIGNAMBIENTINFOS");
  auto& sneakSelf=symbol("C_WANTTOREACTTOSNEAKER.SLF");
  auto& sneakOther=symbol("C_WANTTOREACTTOSNEAKER.OTH");
  auto& campSlot=symbol("C_NPCBELONGSTOBL.SLF");
  auto& gateSlot=symbol("C_NPCISGATEGUARD.SLF");
  auto& assignSlot=symbol("B_ASSIGNDEMENTORTALK.SLF");
  auto& ambientSlot=symbol("B_ASSIGNAMBIENTINFOS.SLF");
  const std::array slots{&sneakSelf,&sneakOther,&campSlot,&gateSlot,&assignSlot,&ambientSlot};
  std::array<std::shared_ptr<daedalus::DaedalusInstance>,slots.size()> savedSlots;
  for(size_t i=0;i<slots.size();++i)
    savedSlots[i]=slots[i]->get_instance();
  const auto exit=std::dynamic_pointer_cast<daedalus::IInfo>(symbol("DIA_AMBIENTDEMENTOR_EXIT").get_instance());
  const auto dialogue=std::dynamic_pointer_cast<daedalus::IInfo>(symbol("DIA_AMBIENTDEMENTOR").get_instance());
  require(exit && dialogue,"player assessment: the stock Dementor dialogues are initialized");
  const auto savedExit=exit->npc, savedDialogue=dialogue->npc;
  vm.global_self()->set_instance(other);
  vm.global_other()->set_instance(self);
  npc.setPosition({0,0,0});
  const auto contextsPreserved=[&] {
    return vm.global_self()->get_instance()==other && vm.global_other()->get_instance()==self && vm.unsafe_get_gi()==savedGi;
    };
  const struct Sneak {
    int distance;
    uint32_t guild;
    NpcType type;
    Attitude attitude;
    bool guard, reacts, reachesGuard;
    } cases[] = {
    {700,GIL_BAU,NpcType::G2_AMBIENT,ATT_NEUTRAL,false,true,true},
    {701,GIL_BAU,NpcType::G2_AMBIENT,ATT_NEUTRAL,false,false,false},
    {700,GIL_BDT,NpcType::G2_BL_AMBIENT,ATT_NEUTRAL,false,true,true},
    {700,GIL_BDT,NpcType::G2_AMBIENT,ATT_NEUTRAL,false,false,false},
    {700,GIL_STRF,NpcType::G2_AMBIENT,ATT_NEUTRAL,false,false,false},
    {700,GIL_DMT,NpcType::G2_AMBIENT,ATT_NEUTRAL,false,false,false},
    {700,GIL_BAU,NpcType::G2_FRIEND,ATT_HOSTILE,false,false,false},
    {700,GIL_BAU,NpcType::G2_AMBIENT,ATT_FRIENDLY,false,false,false},
    {700,GIL_BAU,NpcType::G2_AMBIENT,ATT_NEUTRAL,true,false,true},
    };
  for(bool native:{false,true}) {
    enabled=native;
    for(const auto& c:cases) {
      self->guild=c.guild;
      self->type=c.type;
      npc.setAttitude(c.attitude);
      hero.setPosition({float(c.distance),0,0});
      globalState=c.guard ? guardState : ScriptFn{};
      sneakSelf.set_instance(nullptr);
      sneakOther.set_instance(nullptr);
      campSlot.set_instance(nullptr);
      gateSlot.set_instance(nullptr);
      size_t bytecode=0;
      vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==sneak.address(); });
      vm.push_instance(self);
      vm.push_instance(other);
      vm.unsafe_call_bl(&sneak);
      const auto reaction=vm.pop_int();
      vm.observe_instructions({});
      require(reaction==int(c.reacts) && bytecode==size_t(!native) && sneakSelf.get_instance()==self && sneakOther.get_instance()==other &&
              campSlot.get_instance()==(c.distance<=700 ? self : nullptr) && gateSlot.get_instance()==(c.reachesGuard ? other : nullptr) &&
              contextsPreserved(),"sneaker: stock distance, type, guild, attitude, and global guard check agree in both executors");
      }
    self->guild=GIL_DMT;
    self->voice=19;
    for(bool eligible:{false,true}) {
      self->type=eligible ? NpcType::G2_AMBIENT : NpcType::G2_OC_AMBIENT;
      exit->npc=-21;
      dialogue->npc=-23;
      assignSlot.set_instance(nullptr);
      ambientSlot.set_instance(nullptr);
      size_t assignEntries=0,ambientEntries=0;
      std::vector<const daedalus::DaedalusInstance*> writes;
      vm.observe_instructions([&](const auto&) {
        assignEntries+=vm.pc()==assign.address();
        ambientEntries+=vm.pc()==ambient.address();
        });
      vm.observe_member_writes([&](const auto& member,uint16_t,const auto* info) {
        if(member.name()=="C_INFO.NPC")
          writes.push_back(info);
        });
      vm.push_instance(self);
      vm.unsafe_call_bl(&ambient);
      vm.observe_member_writes({});
      vm.observe_instructions({});
      const auto id=int32_t(npc.instanceSymbol());
      const std::vector<const daedalus::DaedalusInstance*> expected=eligible ?
           std::vector<const daedalus::DaedalusInstance*>{exit.get(),dialogue.get()} : std::vector<const daedalus::DaedalusInstance*>{};
      require(assignEntries==size_t(!native) && ambientEntries==size_t(!native) &&
              assignSlot.get_instance()==self && ambientSlot.get_instance()==self &&
              exit->npc==(eligible ? id : -21) && dialogue->npc==(eligible ? id : -23) && writes==expected && contextsPreserved(),
              "Dementor talk: the ambient wrapper preserves the borrowed two dialogues and their write order");
      }
    }
  for(size_t i=0;i<slots.size();++i)
    slots[i]->set_instance(savedSlots[i]);
  vm.global_self()->set_instance(savedSelf);
  vm.global_other()->set_instance(savedOther);
  globalState=savedGlobalState;
  self->type=savedType;
  self->guild=savedGuild;
  self->voice=savedVoice;
  npc.setAttitude(savedAttitude);
  npc.setPosition(savedNpcPosition);
  hero.setPosition(savedHeroPosition);
  npc.updateTransform();
  hero.updateTransform();
  exit->npc=savedExit;
  dialogue->npc=savedDialogue;
  enabled=savedEvents;
  std::puts("Player assessment helpers: sneaking gates and ambient Dementor binding match the VM");
  }

void playerAssessmentRoutes(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto assess=script.findFunction("B_ASSESSPLAYER");
  const auto follow=script.findFunction("B_ASSESSFOLLOWPLAYER");
  const auto observe=script.findFunction("ZS_OBSERVEPLAYER");
  const auto assessPc=vm.find_symbol_by_name("B_ASSESSPLAYER")->address();
  const auto followPc=vm.find_symbol_by_name("B_ASSESSFOLLOWPLAYER")->address();
  auto& comment=*vm.find_symbol_by_name("PLAYER_DRAWWEAPONCOMMENT");
  const auto savedComment=comment.get_int();
  const auto savedFlag=npc.handle().aivar[19];
  const auto savedAttitude=npc.attitude();
  const auto savedInvincible=hero.handle().aivar[4];
  npc.setAttitude(ATT_NEUTRAL);
  hero.handle().aivar[4]=0;
  require(npc.canSeeNpc(hero,false) && npc.portalName()==hero.portalName() &&
          world.guildOfRoom(npc.portalName())==int32_t(GIL_PUBLIC),
          "player assessment: the fixture has a visible guest in the same public room");
  for(const auto route:{assess,follow}) {
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      enabled=native;
      comment.set_int(1);
      npc.handle().aivar[19]=1;
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(173));
      npc.setPerceptionEnable(PERC_ASSESSPLAYER,route.ptr);
      size_t assessEntries=0,followEntries=0;
      vm.observe_instructions([&](const auto&) {
        assessEntries+=vm.pc()==assessPc;
        followEntries+=vm.pc()==followPc;
        });
      const bool delivered=npc.perceptionProcess(hero,nullptr,npc.qDistTo(hero),PERC_ASSESSPLAYER);
      vm.observe_instructions({});
      require(delivered && assessEntries==(native ? 0u : 1u) && followEntries==(route==follow && !native ? 1u : 0u),
              "player assessment: direct and follower perceptions use the chosen executor");
      const auto* next=npc.aiQueueFront();
      require(next && next->act==AI_StartState && next->func==observe && next->target==&hero && next->i0==1 &&
              comment.get_int()==1 && !npc.hasPerc(PERC_ASSESSPLAYER),
              "player assessment: room entry wins before weapon handling and replaces the old queue");
      auto queue=activityQueue(npc);
      if(native) require(original==queue,"player assessment: both routes reproduce the original room queue");
      else original=std::move(queue);
      }
    }
  enabled=wasEnabled;
  comment.set_int(savedComment);
  npc.handle().aivar[19]=savedFlag;
  npc.setAttitude(savedAttitude);
  hero.handle().aivar[4]=savedInvincible;
  std::puts("Player assessment: direct and follower routes preserve room priority");
  fakeGuildAssessment(world,npc,hero);
  guildGreetingRoutes(world,npc,hero);
  stockPickpocket(world,npc,hero);
  playerAssessmentHelpers(world,npc,hero);
  }
