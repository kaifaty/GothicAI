#pragma once

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
      size_t bytecode=0,followCalls=0;
      vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==assessPc; });
      const auto count=vm.add_call_observer([&](const auto& sym) { followCalls+=sym.name()=="B_ASSESSFOLLOWPLAYER"; });
      const bool delivered=npc.perceptionProcess(hero,nullptr,npc.qDistTo(hero),PERC_ASSESSPLAYER);
      vm.remove_call_observer(count);
      vm.observe_instructions({});
      require(delivered && bytecode==(native ? 0u : 1u) && followCalls==(route==follow ? 1u : 0u),
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
  }
