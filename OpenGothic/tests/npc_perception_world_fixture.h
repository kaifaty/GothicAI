// World contracts for NPC talk, combat and room perceptions.

#include "npc_player_assessment_world_fixture.h"

void assessTalkEntry(World& world,Npc& npc,Npc& player) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& effects=Contract::activityEventsOn(script);
  const bool savedEffects=effects;
  const auto savedNpcQueue=Contract::queue(npc);
  const auto savedPlayerQueue=Contract::queue(player);
  const auto savedVars=std::to_array(npc.handle().aivar);
  const auto savedPerceptions=std::to_array(Contract::perceptions(npc));
  const auto savedState=Contract::aiState(npc);
  const auto savedDeadline=Contract::refusalDeadline(npc);
  const auto savedAttitude=npc.attitude();
  const auto savedOutputOrder=Contract::outputOrder(script);
  auto* savedOther=npc.stateOther();
  const auto savedVictim=vm.global_victim()->get_instance();
  const auto* callback=vm.find_symbol_by_name("B_ASSESSTALK");
  const auto refusal=vm.find_symbol_by_name("C_REFUSETALK")->address();
  const auto started=script.findFunction("ZS_TALK");
  const auto restore=[&] {
    Contract::queue(npc)=savedNpcQueue;
    Contract::queue(player)=savedPlayerQueue;
    std::copy(savedVars.begin(),savedVars.end(),npc.handle().aivar);
    std::copy(savedPerceptions.begin(),savedPerceptions.end(),Contract::perceptions(npc));
    Contract::aiState(npc)=savedState;
    Contract::refusalDeadline(npc)=savedDeadline;
    Contract::outputOrder(script)=savedOutputOrder;
    npc.setAttitude(savedAttitude);
    npc.setOther(savedOther);
    vm.global_victim()->set_instance(savedVictim);
    effects=savedEffects;
    };
  struct Site {
    bool nested, guardRefusal;
    };
  for(const auto site : {Site{false,false},Site{true,false},Site{false,true}}) {
    std::vector<std::string> originalNpc,originalPlayer;
    for(bool native : {false,true}) {
      if(native && (!savedEffects || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
        continue;
      restore();
      effects=native;
      Contract::queue(npc).clear();
      Contract::queue(player).clear();
      npc.setAttitude(ATT_NEUTRAL);
      npc.handle().aivar[1]=0;
      npc.handle().aivar[3]=1;
      npc.handle().aivar[12]=0;
      npc.setPerceptionEnable(PERC_ASSESSTALK,callback->index());
      Contract::aiState(npc).funcIni = site.guardRefusal ? script.findFunction("ZS_GUARD_PASSAGE") : ScriptFn();
      Contract::aiState(npc).started = true;
      Contract::refusalDeadline(npc)=world.tickCount()+1000;
      vm.global_victim()->set_instance(nullptr);
      size_t entryBytecode=0,refusalBytecode=0;
      vm.observe_instructions([&](const auto&) {
        entryBytecode+=vm.pc()==callback->address();
        refusalBytecode+=vm.pc()==refusal;
        });
      if(site.nested) {
        StoryRuntime::Bind context(vm,&npc,&player);
        vm.unsafe_call_bl(callback);
        } else {
        require(npc.perceptionProcess(player,nullptr,0,PERC_ASSESSTALK),
                "talk entry must arrive through the NPC's registered perception");
        }
      vm.observe_instructions({});
      require(entryBytecode==size_t(!native) && refusalBytecode==size_t(!native),
              "the selected executor must handle the entry and refusal rule");
      if(site.guardRefusal) {
        const auto* line=npc.aiQueueFront();
        require(line && line->act==AI_OutputSvm && line->target==&player && line->s0=="$NOTNOW" && line->i0==savedOutputOrder &&
                npc.hasPerc(PERC_ASSESSTALK),
                "an unpassed gate guard on cooldown must decline without disabling talk perception");
        } else {
        auto queued=Contract::queue(npc);
        bool opensDialog=false;
        while(queued.size()>0) {
          const auto action=queued.pop();
          opensDialog|=action.act==AI_StartState && action.func==started && action.target==&player;
          }
        const auto* stand=player.aiQueueFront();
        require(opensDialog && stand && stand->act==AI_StandUpQuick && !npc.hasPerc(PERC_ASSESSTALK),
                "an accepted approach must stand the player and queue ZS_TALK");
        }
      if(native) {
        require(activityQueue(npc)==originalNpc && activityQueue(player)==originalPlayer,
                "talk entry must queue the same actions as the original callback");
        } else {
        originalNpc=activityQueue(npc);
        originalPlayer=activityQueue(player);
        }
      }
    }
  restore();
  }

void stockLearnString(GameScript& script) {
  auto& vm=script.getVm();
  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const bool saved=events;
  auto* builder=vm.find_symbol_by_name("B_BUILDLEARNSTRING");
  auto* result=vm.find_symbol_by_name("B_BUILDLEARNSTRING.CONCATTEXT");
  auto* topic=vm.find_symbol_by_name("DIA_BARTOK_TEACHSNEAK");
  const auto topicBefore=topic->get_instance();
  const auto context=vm.unsafe_get_gi();
  const auto self=vm.global_self()->get_instance();
  const auto other=vm.global_other()->get_instance();
  std::pair<std::string,std::string> original;
  for(bool native:{false,true}) {
    if(native && (!saved || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
      continue;
    events=native;
    size_t instructions=0;
    vm.observe_instructions([&](const auto&) { instructions+=vm.pc()==builder->address(); });
    vm.push_int(77);
    vm.push_string("learning fixture");
    vm.push_int(7);
    vm.unsafe_call_bl(builder);
    const auto first=result->get_string();
    vm.push_reference(result);
    vm.push_int(-2);
    vm.unsafe_call_bl(builder);
    for(int i=0;i<2;++i) {
      require(vm.top_is_reference(),"learn string: each return must remain a reference");
      const auto [symbol,index,instance]=vm.pop_reference();
      require(symbol==result && index==0 && instance==context,
              "learn string: both queued returns must observe the shared saved string with the caller's context");
      }
    require(vm.pop_int()==77 && vm.find_symbol_by_name("B_BUILDLEARNSTRING.KOSTEN")->get_int()==-2 &&
            vm.find_symbol_by_name("B_BUILDLEARNSTRING.TEXT")->get_string()==first,
            "learn string: consume the old result as the next title and leave earlier operands intact");
    const auto second=result->get_string();
    const auto restored=vm.init_instance<daedalus::IInfo>(topic);
    topic->set_instance(topicBefore);
    vm.observe_instructions({});
    require(instructions==(native ? 0u : 3u) && vm.unsafe_get_gi()==context &&
            vm.global_self()->get_instance()==self && vm.global_other()->get_instance()==other,
            "learn string: direct BL and a stock nested BL must use the selected executor and preserve caller context");
    const auto descriptions=std::pair{second,restored->description};
    if(native)
      require(descriptions==original,"learn string: direct and nested descriptions must match the raw VM bytes");
    else
      original=descriptions;
    }
  events=saved;
  }

void stockLearnCostAttribute(GameScript& script, Npc& npc, Npc& player) {
  auto& vm=script.getVm();
  auto& enabled=NpcDefinitionContract::activityEventsOn(script);
  const bool saved=enabled;
  const auto caller=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto learner=std::static_pointer_cast<daedalus::INpc>(player.handlePtr());
  const auto priorContext=vm.unsafe_get_gi();
  const auto priorOther=vm.global_other()->get_instance();
  const auto priorAivar=std::to_array(learner->aivar);
  const auto priorAttributes=std::to_array(learner->attribute);
  auto* helper=vm.find_symbol_by_name("B_GETLEARNCOSTATTRIBUTE");
  auto* cost=vm.find_symbol_by_name("B_GETLEARNCOSTATTRIBUTE.KOSTEN");
  auto* argument=vm.find_symbol_by_name("B_GETLEARNCOSTATTRIBUTE.ATTRIBUT");
  auto* target=vm.find_symbol_by_name("B_GETLEARNCOSTATTRIBUTE.OTH");
  const auto priorCost=cost->get_int();
  const auto priorArgument=argument->get_int();
  const auto priorTarget=target->get_instance();
  vm.unsafe_set_gi(caller);
  vm.global_other()->set_instance(learner);
  constexpr std::pair<int,int> tiers[]={{29,1},{30,2},{59,2},{60,3},{89,3},{90,4},{119,4},{120,5}};
  const auto pushCall=[&](Attribute attribute) {
    vm.push_reference(vm.global_other());
    vm.push_int(attribute);
    vm.unsafe_call_bl(helper);
    };
  for(bool native:{false,true}) {
    if(native && (!saved || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
      continue;
    enabled=native;
    size_t instructions=0;
    vm.observe_instructions([&](const auto&) { instructions+=vm.pc()==helper->address(); });
    for(const auto& [attribute,slot]:{std::pair{ATR_STRENGTH,81u},std::pair{ATR_DEXTERITY,82u},std::pair{ATR_MANAMAX,83u}}) {
      learner->attribute[attribute]=130;
      for(const auto& [level,tier]:tiers) {
        learner->aivar[slot]=level;
        pushCall(attribute);
        require(vm.pop_int()==tier && argument->get_int()==attribute && target->get_instance()==learner,
                "attribute training: price tiers must use trained levels without temporary bonuses");
        }
      }
    vm.push_string("first caller temporary");
    vm.push_string("second caller temporary");
    pushCall(ATR_STRENGTH);
    pushCall(ATR_HITPOINTS);
    for(const auto& context:{caller,learner}) {
      require(vm.top_is_reference(),"attribute training: the return must refer to a saved variable");
      const auto [ref,index,instance]=vm.pop_reference();
      require(ref==cost && index==0 && instance==context && cost->get_int()==0,
              "attribute training: queued returns must share the latest value and retain their own context");
      }
    require(vm.pop_string()=="second caller temporary" && vm.pop_string()=="first caller temporary",
            "attribute training: leave both caller temporaries untouched");
    std::string error;
    const auto priced=storygraph::callFunction(vm,*helper,{storygraph::Value::instance(learner),
                                                          storygraph::Value::integer(ATR_MANAMAX)},error);
    vm.observe_instructions({});
    require(priced && priced->i==5 && instructions==(native ? 0u : 27u) && vm.unsafe_get_gi()==caller,
            "attribute training: the typed StoryGraph caller must use the selected executor and restore caller context");
    }
  vm.global_other()->set_instance(priorOther);
  vm.unsafe_set_gi(priorContext);
  std::ranges::copy(priorAivar,learner->aivar);
  std::ranges::copy(priorAttributes,learner->attribute);
  cost->set_int(priorCost);
  argument->set_int(priorArgument);
  target->set_instance(priorTarget);
  enabled=saved;
  }

void stockLearnCostTalent(GameScript& script, Npc& npc, Npc& player) {
  auto& vm=script.getVm();
  auto& enabled=NpcDefinitionContract::activityEventsOn(script);
  const bool saved=enabled;
  const auto caller=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto learner=std::static_pointer_cast<daedalus::INpc>(player.handlePtr());
  const auto originalContext=vm.unsafe_get_gi();
  const auto trainedBow=learner->aivar[86];
  const auto bowChance=learner->hitchance[TALENT_BOW];
  learner->aivar[86]=60;
  learner->hitchance[TALENT_BOW]=120;
  vm.unsafe_set_gi(caller);
  auto* helper=vm.find_symbol_by_name("B_GETLEARNCOSTTALENT");
  auto* result=vm.find_symbol_by_name("B_GETLEARNCOSTTALENT.KOSTEN");
  static constexpr struct { Talent talent; int32_t skill, price; } cases[] = {
    {TALENT_MAGE,6,5}, {TALENT_MAGE,7,0}, {TALENT_PICKLOCK,1,10},
    {TALENT_SMITH,9,4}, {TALENT_ALCHEMY,12,0}, {TALENT_ALCHEMY,14,8},
    {TALENT_TAKEANIMALTROPHY,9,3}, {TALENT_FOREIGNLANGUAGE,2,15},
    {TALENT_WISPDETECTOR,4,4}, {TALENT_RUNES,46,20}, {TALENT_RUNES,-1,5},
    {TALENT_BOW,5,15},
    };
  const auto pushCall=[&](Talent talent, int32_t skill) {
    vm.push_instance(learner);
    vm.push_int(talent);
    vm.push_int(skill);
    vm.unsafe_call_bl(helper);
    };
  for(bool native:{false,true}) {
    if(native && (!saved || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
      continue;
    enabled=native;
    size_t instructions=0;
    vm.observe_instructions([&](const auto&) { instructions+=vm.pc()==helper->address(); });
    for(const auto& [talent,skill,price]:cases) {
      pushCall(talent,skill);
      require(vm.pop_int()==price,"talent training: exact stock price including defaults and weapon bonuses");
      }
    pushCall(TALENT_BOW,2);
    pushCall(TALENT_SMITH,9);
    for(const auto& context:{caller,learner}) {
      require(vm.top_is_reference(),"talent training: both delayed results must remain references");
      const auto [symbol,index,instance]=vm.pop_reference();
      require(symbol==result && index==0 && instance==context && result->get_int()==4,
              "talent training: reference context is per call and the saved result is shared");
      }
    std::string error;
    const auto cost=storygraph::callFunction(vm,*helper,{storygraph::Value::instance(learner),
                                             storygraph::Value::integer(TALENT_BOW),storygraph::Value::integer(2)},error);
    vm.observe_instructions({});
    require(cost && cost->i==6 && vm.unsafe_get_gi()==caller &&
            instructions==(native ? 0u : std::size(cases)+3),
            "talent training: the StoryGraph preflight must use the selected executor with trained skill");
    }
  vm.unsafe_set_gi(originalContext);
  learner->aivar[86]=trainedBow;
  learner->hitchance[TALENT_BOW]=bowChance;
  enabled=saved;
  }

void stockDialogMetadata(World& world, Npc& npc, Npc& player) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  stockLearnString(script);
  stockLearnCostAttribute(script,npc,player);
  stockLearnCostTalent(script,npc,player);
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  const bool savedEvents=events;
  const auto infoClass=script.findSymbolIndex("C_INFO");
  const auto previousSelf=vm.global_self()->get_instance();
  const auto previousOther=vm.global_other()->get_instance();
  const auto previousContext=vm.unsafe_get_gi();
  const auto learner=std::static_pointer_cast<daedalus::INpc>(player.handlePtr());
  auto* savedLearner=script.findSymbol("B_GETLEARNCOSTTALENT.OTH");
  static constexpr std::array integerSlots={"B_GETLEARNCOSTTALENT.SKILL","B_GETLEARNCOSTTALENT.TALENT",
                                             "B_GETLEARNCOSTTALENT.KOSTEN","B_BUILDLEARNSTRING.KOSTEN"};
  static constexpr std::array textSlots={"B_BUILDLEARNSTRING.TEXT","B_BUILDLEARNSTRING.CONCATTEXT"};
  std::array<int32_t,integerSlots.size()> originalIntegers{};
  std::array<std::string,textSlots.size()> originalText;
  using Info=std::tuple<uint32_t,int32_t,int32_t,int32_t,int32_t,int32_t,std::string,int32_t,int32_t>;
  using Choice=std::tuple<uint32_t,uint32_t,int32_t,std::string,bool>;
  std::vector<Info> original;
  std::vector<Choice> originalChoices;
  for(bool native:{false,true}) {
    if(native && (!savedEvents || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
      continue;
    events=native;
    vm.global_other()->set_instance(learner);
    savedLearner->set_instance(nullptr);
    for(auto name:integerSlots)
      script.findSymbol(name)->set_int(-1);
    for(auto name:textSlots)
      script.findSymbol(name)->set_string("");
    size_t constructors=0;
    const auto observer=vm.add_call_observer([&](const auto& function) {
      constructors+=function.parent()==infoClass && function.type()==daedalus::DaedalusDataType::INSTANCE;
      });
    script.initDialogs();
    vm.remove_call_observer(observer);
    require(vm.global_self()->get_instance()==previousSelf && vm.global_other()->get_instance()==learner &&
            vm.unsafe_get_gi()==previousContext && savedLearner->get_instance()==learner,
            "dialog metadata: initialization must restore the original VM context");
    vm.global_other()->set_instance(previousOther);
    std::array<int32_t,integerSlots.size()> integers;
    std::array<std::string,textSlots.size()> text;
    std::ranges::transform(integerSlots,integers.begin(),[&](auto name) { return script.findSymbol(name)->get_int(); });
    std::ranges::transform(textSlots,text.begin(),[&](auto name) { return script.findSymbol(name)->get_string(); });

    std::vector<Info> metadata;
    for(const auto& info:Contract::dialogInfos(script)) {
      const auto id=info->symbol_index();
      require(vm.find_symbol_by_index(id)->get_instance().get()==info.get() && info->choices.empty(),
              "dialog metadata: bind each instance to its original saved symbol and reset its choices");
      metadata.emplace_back(id,info->npc,info->nr,info->important,info->condition,info->information,
                            info->description,info->trade,info->permanent);
      }
    std::vector<Choice> choices;
    for(const auto& choice:script.dialogChoices(player.handlePtr(),npc.handlePtr(),{},true))
      if(choice.handle!=nullptr)
        choices.emplace_back(choice.handle->symbol_index(),choice.scriptFn,choice.sort,choice.title,choice.isTrade);
    require(metadata.size()==4139 && !choices.empty(),"dialog metadata: the stock catalogue and an ordinary dialog must be live");
    if(native) {
      require(metadata==original && choices==originalChoices,
              "dialog metadata: preserve all typed fields, localized bytes, catalogue order and live choice filtering");
      require(constructors==0 && integers==originalIntegers && text==originalText,
              "dialog metadata: preserve the saved teacher helper fields without executing any stock constructor");
      } else {
      require(constructors==metadata.size(),"dialog metadata: the compatibility mode runs every constructor");
      original=std::move(metadata);
      originalChoices=std::move(choices);
      originalIntegers=integers;
      originalText=std::move(text);
      }
    }
  events=savedEvents;
  }

void talkPhases(Gothic& gothic) {
  using Contract=NpcDefinitionContract;
  auto& world=*gothic.world();
  auto& script=world.script();
  auto& vm=script.getVm();
  auto* actor=activityNpc(world,"NONE_100_XARDAS");
  require(actor!=nullptr && world.player()!=nullptr,"talk needs Xardas and the player");
  auto& npc=*actor;
  auto& player=*world.player();
  stockDialogMetadata(world,npc,player);
  productionDialogEnd(world,npc,player);
  auto& effects=Contract::activityEventsOn(script);
  const bool savedEffects=effects;
  const auto savedNpcQueue=Contract::queue(npc);
  const auto savedPlayerQueue=Contract::queue(player);
  const auto savedNpcVars=std::to_array(npc.handle().aivar);
  const auto savedPlayerVars=std::to_array(player.handle().aivar);
  const auto savedDeadline=Contract::refusalDeadline(player);
  const auto savedDialogCheck=gothic.isNpcInDialogFn;
  auto& talkFix=*vm.find_symbol_by_name("ZSTALKBUGFIX");
  const auto savedFix=talkFix.get_int();
  const auto phases=activityPhases(script,"ZS_TALK");
  const std::array<uint32_t,3> phaseAddresses={
    vm.find_symbol_by_index(uint32_t(phases.init))->address(),
    vm.find_symbol_by_index(uint32_t(phases.loop))->address(),
    vm.find_symbol_by_index(uint32_t(phases.end))->address(),
    };
  const auto restore=[&] {
    Contract::queue(npc)=savedNpcQueue;
    Contract::queue(player)=savedPlayerQueue;
    std::copy(savedNpcVars.begin(),savedNpcVars.end(),npc.handle().aivar);
    std::copy(savedPlayerVars.begin(),savedPlayerVars.end(),player.handle().aivar);
    Contract::refusalDeadline(player)=savedDeadline;
    effects=savedEffects;
    talkFix.set_int(savedFix);
    gothic.isNpcInDialogFn=savedDialogCheck;
    };
  std::vector<std::string> originalNpc,originalPlayer;
  const auto manager=std::make_shared<std::pair<bool,size_t>>();
  for(bool native:{false,true}) {
    if(native && (!savedEffects || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
      continue;
    restore();
    effects=native;
    Contract::queue(npc).clear();
    Contract::queue(player).clear();
    *manager={false,0};
    gothic.isNpcInDialogFn=[manager](const Npc* query) {
      if(query==nullptr)
        ++manager->second;
      return manager->first;
      };

    size_t bytecode=0;
    vm.observe_instructions([&](const auto&) {
      bytecode+=std::ranges::find(phaseAddresses,vm.pc())!=phaseAddresses.end();
      });
    const auto invoke=[&](size_t phase) { return script.invokeState(&npc,&player,nullptr,uint32_t(phase)); };
    npc.handle().aivar[4]=0;
    player.handle().aivar[4]=1;
    talkFix.set_int(17);
    invoke(phases.init);
    require(Contract::queue(npc).size()==0 && Contract::queue(player).size()==0 &&
            npc.handle().aivar[4]==0 && talkFix.get_int()==17,
            "talk Init must leave a player already protected by another dialog alone");

    npc.handle().aivar[3]=1;
    npc.handle().aivar[5]=0;
    player.handle().aivar[4]=0;
    invoke(phases.init);
    auto queued=Contract::queue(npc);
    bool opensDialog=false;
    while(queued.size()>0) {
      const auto action=queued.pop();
      opensDialog|=action.act==AI_ProcessInfo && action.target==&player;
      }
    require(opensDialog && npc.handle().aivar[4]==1 && player.handle().aivar[4]==1 && talkFix.get_int()==0,
            "talk Init must protect both participants and queue the player dialog");

    require(invoke(phases.loop)==LOOP_CONTINUE && talkFix.get_int()==1,
            "talk must wait one Loop before the manager can finish it");
    manager->first=true;
    require(invoke(phases.loop)==LOOP_CONTINUE && npc.handle().aivar[4]==1 && player.handle().aivar[4]==1,
            "an active dialog manager keeps both invincibility flags");
    manager->first=false;
    require(invoke(phases.loop)==LOOP_END && npc.handle().aivar[4]==0 && player.handle().aivar[4]==0 &&
            npc.handle().aivar[3]==0 && npc.handle().aivar[5]==1 && manager->second==3,
            "talk completion must clear protection and update the talk flags in the final Loop");

    invoke(phases.end);
    vm.observe_instructions({});
    require(Contract::refusalDeadline(player)==world.tickCount()+20000 &&
            bytecode==(native ? 0u : 6u),"talk End must set the player cooldown and all phases use the requested executor");
    if(native) {
      require(activityQueue(npc)==originalNpc && activityQueue(player)==originalPlayer,
              "talk stock and native queues must match for both participants");
      } else {
      originalNpc=activityQueue(npc);
      originalPlayer=activityQueue(player);
      }
    }
  restore();
  assessTalkEntry(world,npc,player);
  }

void stockQuestJournalEntry(GameScript& script, const std::string& topic) {
  auto& vm=script.getVm();
  auto& session=*Gothic::inst().gameSession();
  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const auto originalEvents=events;
  const auto originalTime=session.time();
  auto* savedTopic=vm.find_symbol_by_name("B_LOGENTRY.TOPIC");
  auto* savedEntry=vm.find_symbol_by_name("B_LOGENTRY.ENTRY");
  const auto originalTopic=savedTopic->get_string();
  const auto originalEntry=savedEntry->get_string();
  const auto* addEntry=vm.find_symbol_by_name("B_LOGENTRY");
  const auto* getDay=vm.find_symbol_by_name("B_GETDAYPLUS");
  const std::string entry="stock journal helper";
  for(const bool native:{false,true}) {
    events=native;
    std::vector<std::string> feedback;
    unsigned bytecode=0;
    vm.observe_instructions([&](const auto&) { ++bytecode; });
    vm.observe_externals([&](const auto& fn) {
      if(fn.name()=="PRINTSCREEN" || fn.name()=="SND_PLAY")
        feedback.push_back(fn.name());
      });
    vm.push_string(topic);
    vm.push_string(entry);
    vm.unsafe_call_bl(addEntry);
    for(const auto& [hour,minute,expected]:std::array{std::array{21,59,5},std::array{22,0,6},
                                                     std::array{23,58,6},std::array{23,59,5}}) {
      session.setTime(gtime(5,hour,minute));
      vm.unsafe_call_bl(getDay);
      require(vm.pop_int()==expected,"quest deadlines must preserve the stock late-evening boundary");
      }
    vm.observe_instructions({});
    vm.observe_externals({});
    require((bytecode==0)==native && savedTopic->get_string()==topic && savedEntry->get_string()==entry &&
            feedback==std::vector<std::string>{"PRINTSCREEN","SND_PLAY"},
            "stock journal entry and deadline helpers must preserve saved arguments and ordered feedback");
    }
  savedTopic->set_string(originalTopic);
  savedEntry->set_string(originalEntry);
  session.setTime(originalTime);
  events=originalEvents;
  }

auto storyJournalBeforeSave(GameScript& script) {
  const auto journalStart=script.questLog().questCount();
  const std::string note="StoryGraph contract note";
  const std::string mission="StoryGraph contract mission";
  {
    using namespace storygraph::ex;
    const std::string absent="StoryGraph contract absent";
    storygraph::Program program;
    const auto step=[&](std::string fn, const std::string& topic, storygraph::ExprP value) {
      auto& action=program.steps.emplace_back();
      action.kind=storygraph::Step::Call;
      action.call=call(std::move(fn),{str(topic),std::move(value)});
      };
    step("LOG_ADDENTRY",absent,str("ignored"));
    step("LOG_SETTOPICSTATUS",absent,integer(int32_t(QuestLog::Status::Obsolete)));
    step("LOG_CREATETOPIC",absent,integer(-1));
    step("LOG_CREATETOPIC",note,integer(QuestLog::Note));
    step("LOG_SETTOPICSTATUS",note,integer(int32_t(QuestLog::Status::Success)));
    step("LOG_ADDENTRY",note,str("first"));
    step("LOG_CREATETOPIC",note,integer(QuestLog::Mission));
    step("LOG_SETTOPICSTATUS",note,integer(256));
    step("LOG_ADDENTRY",note,str("second"));
    step("LOG_SETTOPICSTATUS",mission,integer(int32_t(QuestLog::Status::Failed)));
    step("LOG_CREATETOPIC",mission,integer(QuestLog::Note));
    auto& vm=script.getVm();
    size_t journalCalls=0;
    vm.observe_externals([&](const auto& fn) { journalCalls+=fn.name().starts_with("LOG_"); });
    StoryRuntime story(nullptr);
    const auto result=story.interpreter(vm).run(program);
    vm.observe_externals({});
    require(result.ok,result.error.c_str());
    require(journalCalls==0,"live StoryGraph journal effects must bypass VM externals");
  }
  stockQuestJournalEntry(script,note);
  const auto requireJournal=[journalStart,note,mission](const QuestLog& journal) {
    require(journal.questCount()==journalStart+2,"StoryGraph journal must ignore absent and invalid effects");
    const auto& first=journal.quest(journalStart);
    const auto& second=journal.quest(journalStart+1);
    require(first.name==note && first.section==QuestLog::Note && first.status==QuestLog::Status::Success &&
            first.entry==std::vector<std::string>{"first","second","stock journal helper","stock journal helper"},
            "StoryGraph journal must preserve note section, valid status and entry order");
    require(second.name==mission && second.section==QuestLog::Mission && second.status==QuestLog::Status::Failed &&
            second.entry.empty(),"a status before creation must create an ordered Mission");
    };
  requireJournal(script.questLog());
  return requireJournal;
  }

auto storyBeforeSave(World& world) {
  using namespace storygraph::ex;
  auto& script=world.script();
  const auto requireJournal=storyJournalBeforeSave(script);
  auto& vm=script.getVm();
  constexpr const char* itemName="ITMI_ORNAMENT_ADDON_VATRAS";
  const auto item=script.findSymbolIndex(itemName);
  auto* giver=activityNpc(world,"VLK_449_LARES");
  auto* player=world.player();
  auto* dialog=vm.find_symbol_by_name("DIA_ADDON_LARES_PATCH_INFO");
  auto* transfer=vm.find_symbol_by_name("B_GIVEINVITEMS");
  require(giver && player && dialog && transfer && item!=size_t(-1),"the transfer fixture needs Lares and the ornament dialog");
  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const bool savedEvents=events;
  const auto savedQueue=NpcDefinitionContract::queue(*giver);
  const auto savedItem=vm.global_item()->get_instance();
  StoryRuntime story(nullptr);
  StoryRuntime::Bind dialogContext(vm,giver,player);
  story.ensureLifter();
  const auto program=storygraph::liftProgram(vm,story.bytecode(),dialog->index());
  require(program.error.empty(),program.error.c_str());

  storygraph::Program handIn;
  handIn.returns=true;
  auto& ret=handIn.steps.emplace_back();
  ret.kind=storygraph::Step::Return;
  ret.value=fn("B_GIVEINVITEMS",{inst("OTHER"),inst("SELF"),sym(itemName),param(0)});
  auto& interpreter=story.interpreter(vm);
  const auto give=[&](int32_t count) {
    const auto result=interpreter.run(handIn,{storygraph::Value::integer(count)});
    require(result.ok,result.error.c_str());
    return result.returned.i;
    };
  std::vector<std::string> original;
  for(bool native:{false,true}) {
    if(native && (!savedEvents || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
      continue;
    events=native;
    NpcDefinitionContract::queue(*giver).clear();
    giver->delItem(item,uint32_t(giver->itemCount(item)));
    player->delItem(item,uint32_t(player->itemCount(item)));
    vm.global_item()->set_instance(savedItem);
    size_t bytecode=0;
    vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==transfer->address(); });
    require(give(0)==1 && give(1)==0 && vm.global_item()->get_instance()==savedItem &&
            giver->itemCount(item)==0 && player->itemCount(item)==0 && giver->aiQueueFront()==nullptr,
            "zero and refused player hand-ins must leave both inventories, ITEM and the queue alone");
    const auto result=interpreter.run(program);
    require(result.ok,result.error.c_str());
    require(giver->itemCount(item)==0 && player->itemCount(item)==1 &&
            vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(player->getItem(item)->handlePtr()),
            "Lares's lifted dialog must grant the requested ornament and set ITEM, even from an empty giver");
    require(give(1)==1 && giver->itemCount(item)==1 && player->itemCount(item)==0 &&
            vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(giver->getItem(item)->handlePtr()),
            "the hand-in must remove the ornament from the player and set ITEM to Lares's copy");
    vm.observe_instructions({});
    require(bytecode==(native ? 0u : 4u),"the same lifted dialog and hand-in must use the selected transfer executor");
    auto queue=NpcDefinitionContract::queue(*giver);
    require(queue.size()==2,"each successful hero transfer must enqueue one notification on SELF");
    for(auto y:{"YPOS_ITEMTAKEN","YPOS_ITEMGIVEN"}) {
      const auto line=queue.pop();
      require(line.act==AI_PrintScreen && line.i0==-1 && line.i1==vm.find_symbol_by_name(y)->get_int() &&
              line.i2==2 && line.s1==vm.find_symbol_by_name("FONT_SCREENSMALL")->get_string(),
              "transfer notifications must keep their order, placement, duration and font");
      }
    if(native)
      require(activityQueue(*giver)==original,"the native notices must match the original localized text");
    else
      original=activityQueue(*giver);
    }
  const auto itemText=vm.find_symbol_by_name("B_GIVEINVITEMS.ITEMNAME")->get_string();
  vm.global_item()->set_instance(savedItem);
  NpcDefinitionContract::queue(*giver)=savedQueue;
  events=savedEvents;
  return [requireJournal,item,itemText](World& restored) {
    auto& script=restored.script();
    auto& vm=script.getVm();
    requireJournal(script.questLog());
    const auto* lares=activityNpc(restored,"VLK_449_LARES");
    const auto* hero=restored.player();
    require(lares && hero && lares->itemCount(item)==1 && hero->itemCount(item)==0 &&
            vm.find_symbol_by_name("B_GIVEINVITEMS.ITEMNAME")->get_string()==itemText &&
            vm.find_symbol_by_name("B_GIVEINVITEMS.AMOUNT")->get_int()==1 &&
            vm.find_symbol_by_name("B_GIVEINVITEMS.TAKER")->get_instance().get()==
              static_cast<const daedalus::INpc*>(lares->handlePtr().get()),
            "the transferred ornament and the script transfer locals must survive saving and loading");
    };
  }

void observePlayerPhases(World& world,Npc& npc,Npc& hero,bool seated) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  const auto phases=activityPhases(script,"ZS_OBSERVEPLAYER");
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto beforeTime=npc.stateTime();
  const auto beforeState=Contract::previousState(npc);
  const auto beforeRoom=npc.handle().aivar[21];
  const auto beforeDelay=npc.handle().aivar[68];
  const auto beforeSenses=npc.handle().senses;
  const auto beforeRange=npc.handle().senses_range;
  auto& vm=script.getVm();
  auto& speech=*vm.find_symbol_by_name("B_SAY.TEXT");
  require(ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),BS_SIT)==seated &&
          npc.qDistTo(hero)<1000.f*1000.f,"observation: the fixture holds the intended nearby pose");
  std::vector<std::string> original;
  for(bool native:{false,true}) {
    enabled=native;
    npc.clearAiQueue();
    npc.aiPush(AiQueue::aiStopLookAt());
    Contract::nextAiAction(npc);
    Contract::previousState(npc)=script.findFunction("ZS_SLEEP");
    npc.handle().aivar[21]=0;
    npc.handle().senses=0;
    npc.handle().senses_range=0;
    speech.set_string("");
    size_t normalCalls=0,speechCalls=0,lookCalls=0,stopCalls=0,turnCalls=0;
    const auto helperProbe=vm.add_call_observer([&](const auto& symbol) {
      normalCalls+=symbol.name()=="PERCEPTION_SET_NORMAL";
      speechCalls+=symbol.name()=="B_SAY";
      lookCalls+=symbol.name()=="B_LOOKATNPC";
      stopCalls+=symbol.name()=="B_STOPLOOKAT";
      turnCalls+=symbol.name()=="B_TURNTONPC";
      });
    script.invokeState(&npc,&hero,nullptr,uint32_t(phases.init));
    vm.remove_call_observer(helperProbe);
    auto speechQueue=Contract::queue(npc);
    require(speechQueue.size()==(seated ? 2u : 4u),"observation: waking adds one queued voice line");
    for(size_t i=0;i<(seated ? 1u : 3u);++i)
      speechQueue.pop();
    const auto line=speechQueue.pop();
    require(line.act==AI_OutputSvm && line.target==&hero && line.s0=="$YOUDISTURBEDMYSLUMBER" &&
            speech.get_string()==line.s0 && npc.handle().senses==3 && npc.handle().senses_range==2000 &&
            npc.hasPerc(PERC_ASSESSQUIETSOUND) &&
            normalCalls==size_t(!native) && speechCalls==size_t(!native) && lookCalls==size_t(!native) &&
            stopCalls==size_t(!native) && turnCalls==size_t(!native && !seated),
            "observation: native and scripted helpers produce the same senses, look and speech without native VM calls");
    npc.clearAiQueue();
    Contract::previousState(npc)=ScriptFn();
    npc.setStateTime(0);
    script.setSeed(73106);
    auto expected=Contract::random(script);
    ActivityStream stream(world,npc);
    const auto invoke=[&](size_t phase) { return script.invokeState(&npc,&hero,nullptr,uint32_t(phase)); };
    invoke(phases.init);
    const auto delay=1+int32_t(expected()%2);
    auto queue=Contract::queue(npc);
    require(queue.size()==(seated ? 1u : 3u),"observation: only a standing listener gets stand-up and turn commands");
    if(!seated) require(queue.pop().act==AI_StandUp,"observation: stand-up precedes the look");
    const auto look=queue.pop();
    require(look.act==AI_LookAtNpc && look.target==&hero,"observation: both poses look at the same guest");
    if(!seated) require(queue.pop().act==AI_TurnToNpc,"observation: standing Init turns after looking");
    require(npc.handle().aivar[68]==delay && Contract::random(script)==expected,
            "observation: Init draws one original delay");

    npc.clearAiQueue();
    const auto boundary=(delay+1)*1000;
    npc.setStateTime(boundary-1);
    require(invoke(phases.loop)==LOOP_CONTINUE && npc.isAiQueueEmpty() && npc.stateTime()==uint64_t(boundary-1) &&
            Contract::random(script)==expected,"observation: Loop leaves the timer alone until the whole second");
    npc.setStateTime(boundary);
    const auto nextDelay=1+int32_t(expected()%2);
    require(invoke(phases.loop)==LOOP_CONTINUE && npc.handle().aivar[68]==nextDelay && npc.stateTime()==0 &&
            Contract::random(script)==expected,"observation: Loop draws once and resets the timer at the boundary");
    const auto* turn=npc.aiQueueFront();
    require(seated ? turn==nullptr : turn && turn->act==AI_TurnToNpc && turn->target==&hero,
            "observation: only the standing listener repeats the turn");

    npc.clearAiQueue();
    npc.aiPush(AiQueue::aiLookAtNpc(&hero));
    Contract::nextAiAction(npc);
    invoke(phases.end);
    require(npc.aiQueueFront() && npc.aiQueueFront()->act==AI_StopLookAt && stream.records==(native ? 4u : 0u),
            "observation: End stops the existing look and all phases use the selected executor");
    Contract::nextAiAction(npc);
    std::erase_if(stream.events,[](const auto& event) {
      return !(event.starts_with("phase ") || event.starts_with("result ") ||
               event.starts_with("queue ") || event.starts_with("random "));
      });
    if(!native) original=std::move(stream.events);
    else require(original==stream.events,"observation: phase results, queue and random draws match the VM");
    }
  enabled=wasEnabled;
  npc.handle().aivar[21]=beforeRoom;
  npc.handle().aivar[68]=beforeDelay;
  npc.handle().senses=beforeSenses;
  npc.handle().senses_range=beforeRange;
  Contract::previousState(npc)=beforeState;
  npc.setStateTime(int64_t(beforeTime));
  itemUseRoutes(world,npc,hero,seated);
  std::printf("D5.2 %s observation: Init, timed Loop, End and random stream match the VM\n",seated ? "seated" : "standing");
  }

void humanReactionPhases(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  const auto damage=activityPhases(script,"ZS_REACTTODAMAGE");
  const auto weapon=activityPhases(script,"ZS_REACTTOWEAPON");
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  auto& comment=*vm.find_symbol_by_name("PLAYER_DRAWWEAPONCOMMENT");
  const auto weaponReason=vm.find_symbol_by_name("AR_REACTTOWEAPON")->get_int();
  const auto attackCrime=vm.find_symbol_by_name("CRIME_ATTACK")->get_int();
  const auto savedAivar=std::to_array(npc.handle().aivar);
  const int order=Contract::outputOrder(script);
  std::vector<std::string> original;
  auto originalRandom=Contract::random(script);
  const auto expectLine=[&](std::string_view line) {
    auto queue=Contract::queue(npc);
    while(queue.size()>0) {
      const auto action=queue.pop();
      if(action.act==AI_OutputSvm && action.target==&hero && action.s0==line) return true;
      }
    return false;
    };
  require(npc.qDistTo(hero)<1000.f*1000.f,"human reactions: the player is in warning range");
  npc.setAttitude(ATT_NEUTRAL);
  for(bool native:{false,true}) {
    enabled=native;
    npc.clearAiQueue();
    npc.setTarget(nullptr);
    auto& aivar=npc.handle().aivar;
    std::copy(savedAivar.begin(),savedAivar.end(),aivar);
    aivar[1]=0;
    aivar[47]=0;
    comment.set_int(0);
    Contract::outputOrder(script)=order;
    script.setSeed(73106);
    ActivityStream stream(world,npc);
    size_t calls=0,attacks=0,crimes=0,invocations=0;
    const auto countVm=vm.add_call_observer([&](const auto& symbol) {
      calls+=symbol.name().starts_with("ZS_REACTTODAMAGE") || symbol.name().starts_with("ZS_REACTTOWEAPON");
      attacks+=symbol.name()=="B_ATTACK";
      crimes+=symbol.name()=="B_MEMORIZEPLAYERCRIME";
      });
    const auto invoke=[&](size_t phase,int time=0) {
      ++invocations;
      npc.setStateTime(time);
      return script.invokeState(&npc,&hero,nullptr,uint32_t(phase));
      };

    invoke(damage.init);
    require(aivar[68]==0 && npc.hasPerc(PERC_ASSESSTHEFT) && expectLine("$WHATAREYOUDOING"),
            "human reactions: damage restores perceptions and addresses the attacker");
    require(invoke(damage.loop,999)==LOOP_CONTINUE && aivar[68]==0 &&
            invoke(damage.loop,10999)==LOOP_CONTINUE && aivar[68]==1 &&
            invoke(damage.loop,11000)==LOOP_END && aivar[68]==2,
            "human reactions: damage counts whole seconds and ends above ten");
    npc.clearAiQueue();
    npc.aiPush(AiQueue::aiLookAtNpc(&hero));
    Contract::nextAiAction(npc);
    invoke(damage.end);
    auto queue=Contract::queue(npc);
    require(queue.size()==2 && queue.pop().act==AI_RemoveWeapon && queue.pop().act==AI_StopLookAt,
            "human reactions: damage End removes the weapon and stops looking at the attacker");

    npc.clearAiQueue();
    hero.setToFistMode();
    invoke(weapon.init);
    require(npc.hasPerc(PERC_ASSESSFIGHTSOUND) && comment.get_int()==1 && aivar[19]==0 && aivar[68]==0 &&
            expectLine("$WEAPONDOWN"),"human reactions: weapon Init warns and enables fight sound");
    require(invoke(weapon.loop,5999)==LOOP_CONTINUE && aivar[19]==0 &&
            invoke(weapon.loop,6000)==LOOP_CONTINUE && aivar[19]==1 && expectLine("$ISAIDWEAPONDOWN") &&
            invoke(weapon.loop,10999)==LOOP_CONTINUE && attacks==0,
            "human reactions: the warning first repeats above five and escalation waits past ten");
    require(hero.closeWeapon(true) && invoke(weapon.loop,10999)==LOOP_END && expectLine("$WISEMOVE"),
            "human reactions: sheathing ends the warning with the original reply");
    npc.clearAiQueue();
    invoke(weapon.end);
    const auto* next=npc.aiQueueFront();
    require(next && next->act==AI_StartState && next->func==script.findFunction("ZS_OBSERVEPLAYER") &&
            next->i0==1 && next->target==&hero,"human reactions: weapon End queues observation of the same player");
    hero.setToFistMode();
    require(invoke(weapon.loop,11000)==LOOP_CONTINUE && attacks==size_t(!native) && npc.target()==&hero &&
            aivar[9]==weaponReason && aivar[1]==attackCrime && crimes==size_t(!native),
            "human reactions: escalation enters the shared attack policy and records the crime");
    hero.closeWeapon(true);
    vm.remove_call_observer(countVm);
    require(calls==(native ? 0u : invocations) && stream.records==(native ? invocations : 0u),
            "human reactions: the selected executor owns every phase");
    std::erase_if(stream.events,[](const auto& event) {
      return !(event.starts_with("phase ") || event.starts_with("result ") ||
               event.starts_with("queue ") || event.starts_with("random "));
      });
    if(!native) {
      original=std::move(stream.events);
      originalRandom=Contract::random(script);
      } else {
      require(original==stream.events && originalRandom==Contract::random(script),
              "human reactions: original and native queue, phase results and RNG match");
      }
    }
  enabled=wasEnabled;
  std::puts("Human reactions: six damage and weapon phases match the VM");
  }

void drawWeaponPerceptions(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto draw=script.findFunction("B_ASSESSDRAWWEAPON");
  const auto assess=script.findFunction("B_ASSESSPLAYER");
  const auto threat=script.findFunction("B_ASSESSTHREAT");
  const auto react=script.findFunction("ZS_REACTTOWEAPON");
  const auto dagger=script.findSymbolIndex("ITMW_1H_VLK_DAGGER");
  const auto drawPc=vm.find_symbol_by_name("B_ASSESSDRAWWEAPON")->address();
  const auto threatPc=vm.find_symbol_by_name("B_ASSESSTHREAT")->address();
  const auto policyPc=vm.find_symbol_by_name("C_NPCISBOTHEREDBYWEAPON")->address();
  auto& comment=*vm.find_symbol_by_name("PLAYER_DRAWWEAPONCOMMENT");
  const int savedComment=comment.get_int();
  auto& state=Contract::aiState(npc).funcIni;
  auto& previous=Contract::previousState(npc);
  const auto savedState=state;
  const auto savedPrevious=previous;
  // The ordinary player assessment skips the room interruption if the listener is already observing.
  state=script.findFunction("ZS_OBSERVEPLAYER");
  previous=ScriptFn();
  require(Contract::aiState(npc).started && npc.qDistTo(hero)<500.f*500.f && npc.canSeeNpc(hero,false),
          "draw weapon: the active observer sees the player inside warning distance");
  struct Site { PercType perc; bool melee; };
  for(const Site site : {Site{PERC_DRAWWEAPON,true},Site{PERC_ASSESSPLAYER,true},
                         Site{PERC_ASSESSTHREAT,true},Site{PERC_DRAWWEAPON,false}}) {
    if(site.melee)
      hero.setToFightMode(dagger);
    else
      hero.setToFistMode();
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      enabled=native;
      comment.set_int(1);
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(173));
      npc.setPerceptionEnable(PERC_DRAWWEAPON,draw.ptr);
      npc.setPerceptionEnable(PERC_ASSESSPLAYER,assess.ptr);
      npc.setPerceptionEnable(PERC_ASSESSTHREAT,threat.ptr);
      size_t drawBytecode=0,threatBytecode=0,policyBytecode=0,playerAssessment=0;
      vm.observe_instructions([&](const auto&) {
        drawBytecode+=vm.pc()==drawPc;
        threatBytecode+=vm.pc()==threatPc;
        policyBytecode+=vm.pc()==policyPc;
        });
      const auto count=vm.add_call_observer([&](const auto& symbol) {
        playerAssessment+=symbol.name()=="B_ASSESSPLAYER";
        });
      const bool delivered=npc.perceptionProcess(hero,nullptr,npc.qDistTo(hero),site.perc);
      vm.remove_call_observer(count);
      vm.observe_instructions({});
      require(delivered && playerAssessment==(!native && site.perc==PERC_ASSESSPLAYER ? 1u : 0u) &&
              drawBytecode==(!native && site.perc!=PERC_ASSESSTHREAT ? 1u : 0u) &&
              threatBytecode==(!native && site.perc==PERC_ASSESSTHREAT ? 1u : 0u) && policyBytecode==(native ? 0u : 1u),
              "draw weapon: draw, player and threat callbacks skip the migrated bytecode only in native mode");
      const auto* next=npc.aiQueueFront();
      require(comment.get_int()==1 && next &&
              (site.melee ? next->act==AI_StartState && next->func==react && next->target==&hero && next->i0==0 &&
                            !npc.hasPerc(PERC_DRAWWEAPON) && !npc.hasPerc(PERC_ASSESSPLAYER)
                          : next->act==AI_Wait && npc.hasPerc(PERC_DRAWWEAPON) && npc.hasPerc(PERC_ASSESSPLAYER)),
              "draw weapon: drawn steel replaces the queue and fists leave it alone");
      auto queue=activityQueue(npc);
      if(native) require(original==queue,"draw weapon: the stock and native perception queues match");
      else original=std::move(queue);
      }
    }
  hero.closeWeapon(true);
  state=savedState;
  previous=savedPrevious;
  comment.set_int(savedComment);
  enabled=wasEnabled;
  std::puts("Draw weapon: direct perception, player assessment and threat match the VM, including the fist rejection");
  }

void warnPerception(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const auto wasEnabled=enabled;
  const auto warn=script.findFunction("B_ASSESSWARN");
  const auto observe=script.findFunction("ZS_OBSERVEPLAYER");
  const auto warnPc=vm.find_symbol_by_name("B_ASSESSWARN")->address();
  auto& state=Contract::aiState(npc).funcIni;
  const auto savedState=state;
  const auto heroGuild=hero.handle().guild;
  enum class Case { Warning, Victim, Observing, Weapon, Gate, Monster };
  for(auto kind : {Case::Warning,Case::Victim,Case::Observing,Case::Weapon,Case::Gate,Case::Monster}) {
    std::vector<std::string> original;
    for(bool native : {false,true}) {
      enabled=native;
      state=kind==Case::Observing ? observe :
            kind==Case::Weapon ? script.findFunction("ZS_REACTTOWEAPON") :
            kind==Case::Gate ? script.findFunction("ZS_GUARD_PASSAGE") : script.findFunction("ZS_STAND_WP");
      hero.handle().guild=kind==Case::Monster ? int32_t(GIL_WOLF) : heroGuild;
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(713));
      npc.setPerceptionEnable(PERC_ASSESSWARN,warn.ptr);
      size_t instructions=0;
      vm.observe_instructions([&](const auto&) { instructions+=vm.pc()==warnPc; });
      const auto delivered=npc.passPerception(hero,kind==Case::Victim ? &npc : &hero,PERC_ASSESSWARN);
      vm.observe_instructions({});
      require(delivered && instructions==size_t(!native),"warn: the selected executor owns the warning callback");
      const auto* next=npc.aiQueueFront();
      require(next && (kind==Case::Warning
                       ? next->act==AI_StartState && next->func==observe && next->target==&hero &&
                         next->i0==0 && !npc.hasPerc(PERC_ASSESSWARN)
                       : next->act==AI_Wait && npc.hasPerc(PERC_ASSESSWARN)),
              "warn: only a new human warning replaces the queue and starts observation");
      auto queue=activityQueue(npc);
      if(native) require(original==queue,"warn: stock and native queues agree");
      else original=std::move(queue);
      }
    }
  state=savedState;
  hero.handle().guild=heroGuild;
  enabled=wasEnabled;
  std::puts("Warn: source, victim and busy-listener gates match the original callback");
  }

void damagePerceptions(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto damage=script.findFunction("B_ASSESSDAMAGE");
  const auto damagePc=vm.find_symbol_by_name("B_ASSESSDAMAGE")->address();
  const auto attack=script.findFunction("ZS_ATTACK");
  const auto warn=script.findFunction("ZS_REACTTODAMAGE");
  const auto stopBurn=script.findFunction("B_STOPMAGICBURNSHORT");
  const auto reactReason=script.intSymbol("AR_REACTTODAMAGE",-1);
  const auto noneReason=script.intSymbol("AR_NONE",-1);
  const auto crimeAttack=script.intSymbol("CRIME_ATTACK",-1);
  const auto afterArena=script.intSymbol("AF_AFTER",-1);
  const auto afterArenaDamage=script.intSymbol("AF_AFTER_PLUS_DAMAGE",-1);
  auto* lobart=activityNpc(world,"BAU_950_LOBART");
  require(lobart!=nullptr,"damage perception: the fixture needs a nearby NPC attacker");
  auto& aivar=npc.handle().aivar;
  const auto savedAivar=std::to_array(aivar);
  auto& state=Contract::aiState(npc).funcIni;
  const auto savedState=state;
  const auto savedAttitude=npc.attitude();
  auto* savedTarget=npc.target();
  const auto savedLobartReason=lobart->handle().aivar[9];
  lobart->handle().aivar[9]=noneReason;
  hero.setToFistMode();

  enum class Hit { Friend, SpellEnd, HeroAttack, Bystander, AlreadyFighting };
  for(const auto hit : {Hit::Friend,Hit::SpellEnd,Hit::HeroAttack,Hit::Bystander,Hit::AlreadyFighting}) {
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      enabled=native;
      state=hit==Hit::AlreadyFighting ? attack : savedState;
      const bool warning=hit==Hit::Friend || hit==Hit::SpellEnd;
      npc.setAttitude(warning ? ATT_FRIENDLY : ATT_NEUTRAL);
      npc.setTarget(nullptr);
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(173));
      npc.setPerceptionEnable(PERC_ASSESSDAMAGE,damage.ptr);
      std::copy(savedAivar.begin(),savedAivar.end(),aivar);
      aivar[1]=0;
      aivar[15]=0;
      aivar[22]=0;
      aivar[45]=afterArena;
      aivar[61]=0;
      aivar[7]=int32_t(lobart->instanceSymbol());
      script.setSeed(73106);
      auto random=Contract::random(script);
      auto& attacker=hit==Hit::Bystander ? *lobart : hero;
      size_t bytecode=0;
      vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==damagePc; });
      const auto deliver=[&] {
        require(npc.perceptionProcess(attacker,&npc,0,PERC_ASSESSDAMAGE),"damage perception: the stock callback is delivered");
        };
      if(hit==Hit::SpellEnd)
        script.invokeState(&npc,&attacker,nullptr,stopBurn);
      else
        deliver();
      if(attacker.isPlayer())
        random();
      if(hit==Hit::AlreadyFighting) {
        require(npc.target()==nullptr && aivar[22]==int32_t(hero.instanceSymbol()),
                "damage perception: the first hit by the player remembers the source");
        deliver();
        random();
        require(npc.target()==&hero && npc.aiQueueFront() && npc.aiQueueFront()->act==AI_Wait,
                "damage perception: another hit retargets without replacing the combat queue");
        } else {
        const auto* next=npc.aiQueueFront();
        require(next && next->act==AI_StartState && next->func==(warning ? warn : attack) &&
                next->target==&attacker && next->victim==(hit==Hit::SpellEnd ? nullptr : &npc) &&
                next->i0==0 && !npc.hasPerc(PERC_ASSESSDAMAGE),
                "damage perception: warning and attack replace the queue and clear perceptions");
        if(!warning)
          require(aivar[9]==(hit==Hit::HeroAttack ? reactReason : noneReason) &&
                  aivar[1]==(hit==Hit::HeroAttack ? crimeAttack : 0),
                  "damage perception: attack reason and crime follow the source");
        }
      vm.observe_instructions({});
      const auto calls=hit==Hit::AlreadyFighting ? 2u : 1u;
      require(bytecode==(native ? 0u : calls) && Contract::random(script)==random && aivar[45]==afterArenaDamage,
              "damage perception: both dispatch routes preserve the arena flag and the Beliar weapon draw");
      auto queue=activityQueue(npc);
      if(native) require(original==queue,"damage perception: the original and native queues match");
      else original=std::move(queue);
      }
    }
  hero.closeWeapon(true);
  lobart->handle().aivar[9]=savedLobartReason;
  std::copy(savedAivar.begin(),savedAivar.end(),aivar);
  state=savedState;
  npc.setAttitude(savedAttitude);
  npc.setTarget(savedTarget);
  enabled=wasEnabled;
  std::puts("Damage perception: stock damage and spell end routes preserve warnings, attack reasons, crime and retargeting");
  specialDamagePerceptions(world,npc,hero);
  }

void humanAttackAnnouncements(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  auto& perceptions=Contract::passivePerceptions(world);
  const auto perceptionCount=perceptions.size();
  const auto priorEvents=events;
  const auto priorRandom=Contract::random(script);
  const auto order=Contract::outputOrder(script);
  const auto priorAttitude=npc.attitude();
  const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto priorSelf=vm.global_self()->get_instance();
  const auto priorOther=vm.global_other()->get_instance();
  const auto priorAivar=std::to_array(self->aivar);
  const auto priorHeroGuild=other->guild;
  const auto priorKilled=other->aivar[67];
  const auto priorVoice=self->voice;
  const auto priorArmor=hero.currentArmor() ? hero.currentArmor()->clsId() : size_t(-1);
  const auto pirateArmor=script.findSymbolIndex("ITAR_PIR_M_ADDON");
  const bool temporaryArmor=hero.itemCount(pirateArmor)==0;
  auto& gorn=*vm.find_symbol_by_name("GORN_DI");
  const auto priorGorn=gorn.get_instance();
  auto& disguiseSelf=*vm.find_symbol_by_name("C_PLAYERISFAKEPIRATE.SLF");
  auto& disguiseOther=*vm.find_symbol_by_name("C_PLAYERISFAKEPIRATE.OTH");
  auto& disguiseItem=*vm.find_symbol_by_name("C_PLAYERISFAKEPIRATE.ITM");
  const auto priorDisguiseSelf=disguiseSelf.get_instance();
  const auto priorDisguiseOther=disguiseOther.get_instance();
  const auto priorDisguiseItem=disguiseItem.get_instance();
  auto* begin=vm.find_symbol_by_name("B_SAY_ATTACKREASON");
  auto* end=vm.find_symbol_by_name("B_SAY_ATTACKEND");
  const auto piratePc=vm.find_symbol_by_name("C_PLAYERISFAKEPIRATE")->address();
  static constexpr std::array randomSymbols={"B_SAY_ATTACKREASON.RND","B_SAY_ATTACKREASON.RANDOM","B_SAY_ATTACKEND.RND",
                                             "B_SAY_ATTACKEND.RANDOM","B_SAY_ATTACKEND.RANDY"};
  std::array<int32_t,randomSymbols.size()> priorScratch{};
  for(size_t i=0;i<randomSymbols.size();++i)
    priorScratch[i]=vm.find_symbol_by_name(randomSymbols[i])->get_int();
  vm.global_self()->set_instance(self);
  vm.global_other()->set_instance(other);
  if(priorArmor!=size_t(-1)) hero.unequipItem(priorArmor);

  struct Site {
    bool ending;
    std::string_view reason;
    int32_t guild;
    std::string_view line;
    bool sound=false;
    bool party=false;
    int32_t voice=0;
    uint32_t seed=73106;
    std::array<int,randomSymbols.size()> bounds={};
    bool isGorn=false;
    bool disguise=false;
    bool byPlayer=false;
    };
  for(const Site site:{
      Site{false,"AR_THEFT",GIL_VLK,"$DIRTYTHIEF"},
      Site{false,"AR_HUMANMURDEREDHUMAN",GIL_VLK,"$YOUMURDERER",true},
      Site{false,"AR_REACTTODAMAGE",GIL_SEPERATOR_HUM,"$YOUDAREHITME"},
      Site{false,"AR_GUILDENEMY",GIL_WOLF,"$ADDON_DIEMONSTER",true,false,9,73106,{0,3}},
      Site{false,"AR_GUILDENEMY",GIL_WOLF,"",false,true,9,1,{100}},
      Site{false,"AR_GUILDENEMY",GIL_VLK,"$ADDON_DIRTYPIRATE",true,false,0,73106,{},false,true},
      Site{true,"AR_GUILDENEMY",GIL_VLK,"$KILLENEMY"},
      Site{true,"AR_GUILDENEMY",GIL_WOLF,"",false,true,9,1,{0,0,100}},
      Site{true,"AR_GUILDENEMY",GIL_DRAGON,"$ADDON_MONSTERKILLED2",false,true,9,1,{0,0,100,2}},
      Site{true,"AR_MONSTERMURDEREDHUMAN",GIL_WOLF,"$MONSTERKILLED",false,false,12,73106,{0,0,0,0,2},true},
      Site{true,"AR_MONSTERMURDEREDHUMAN",GIL_WOLF,"$GOODMONSTERKILL",false,false,12,73106,{},true,false,true},
      Site{true,"AR_GUARDSTOPSFIGHT",GIL_DJG,""}}) {
    other->guild=site.guild;
    other->aivar[67]=int32_t(site.byPlayer);
    self->aivar[7]=int32_t(hero.instanceSymbol());
    self->aivar[9]=script.intSymbol(site.reason,-1);
    self->aivar[15]=int32_t(site.party);
    self->aivar[70]=0;
    self->aivar[73]=0;
    self->voice=site.voice;
    npc.setAttitude(ATT_NEUTRAL);
    gorn.set_instance(site.isGorn ? self : nullptr);
    if(site.disguise) {
      if(temporaryArmor) hero.addItem(pirateArmor,1);
      hero.useItem(pirateArmor,Item::NSLOT,true);
      require(hero.currentArmor() && hero.currentArmor()->clsId()==pirateArmor,"attack announcements: pirate armor is equipped");
      }
    auto* callback=site.ending ? end : begin;
    for(bool native:{false,true}) {
      events=native;
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(173));
      Contract::outputOrder(script)=order;
      script.setSeed(site.seed);
      auto expectedRandom=Contract::random(script);
      disguiseSelf.set_instance(nullptr);
      disguiseOther.set_instance(nullptr);
      disguiseItem.set_instance(nullptr);
      for(const auto symbol:randomSymbols) vm.find_symbol_by_name(symbol)->set_int(-1);
      size_t body=0,pirate=0;
      vm.observe_instructions([&](const auto&) {
        body+=vm.pc()==callback->address();
        pirate+=vm.pc()==piratePc;
        });
      vm.unsafe_call_bl(callback);
      vm.observe_instructions({});
      require(body==size_t(!native) && pirate==size_t(!native && site.disguise),
              "attack announcements: the chosen path has the original or native body");
      for(size_t i=0;i<randomSymbols.size();++i) {
        const int bound=site.bounds[i];
        const int roll=bound==0 ? -1 : int(expectedRandom()%uint32_t(bound));
        require(vm.find_symbol_by_name(randomSymbols[i])->get_int()==roll,"attack announcements: exact saved random draw");
        }
      require(Contract::random(script)==expectedRandom,"attack announcements: RNG state is identical");
      auto queue=Contract::queue(npc);
      const auto& overlay=Contract::overlayQueue(npc);
      const bool said=!site.line.empty();
      require(queue.size()==1u+size_t(said && site.ending) && queue.pop().act==AI_Wait &&
              overlay.size()==size_t(said && !site.ending) && Contract::outputOrder(script)==order+int(said),
              "attack announcements: command order and output queue are preserved");
      const auto* speech=site.ending ? queue.front() : overlay.front();
      if(said)
        require(speech && speech->act==(site.ending ? AI_OutputSvm : AI_OutputSvmOverlay) &&
                speech->target==&hero && speech->s0==site.line && speech->i0==order,
                "attack announcements: exact original line and interlocutor");
      require(perceptions.size()==perceptionCount+size_t(site.sound),"attack announcements: exact fight-sound count");
      if(site.sound) {
        const auto& sound=perceptions.back();
        require(sound.what==PERC_ASSESSFIGHTSOUND && sound.self==&npc && sound.other==&hero && sound.victim==&npc,
                "attack announcements: a witness hears the original combat pair");
        perceptions.pop_back();
        }
      if(site.disguise)
        require(disguiseSelf.get_instance()==self && disguiseOther.get_instance()==other &&
                disguiseItem.get_instance()==std::static_pointer_cast<daedalus::IItem>(hero.currentArmor()->handlePtr()),
                "attack announcements: pirate disguise preserves the original formals and armor");
      }
    if(site.disguise) {
      hero.unequipItem(pirateArmor);
      if(temporaryArmor) hero.delItem(pirateArmor,1);
      }
    }
  if(priorArmor!=size_t(-1)) hero.useItem(priorArmor,Item::NSLOT,true);
  npc.clearAiQueue();
  npc.setAttitude(priorAttitude);
  std::copy(priorAivar.begin(),priorAivar.end(),self->aivar);
  self->voice=priorVoice;
  other->guild=priorHeroGuild;
  other->aivar[67]=priorKilled;
  vm.global_self()->set_instance(priorSelf);
  vm.global_other()->set_instance(priorOther);
  gorn.set_instance(priorGorn);
  disguiseSelf.set_instance(priorDisguiseSelf);
  disguiseOther.set_instance(priorDisguiseOther);
  disguiseItem.set_instance(priorDisguiseItem);
  for(size_t i=0;i<randomSymbols.size();++i)
    vm.find_symbol_by_name(randomSymbols[i])->set_int(priorScratch[i]);
  Contract::random(script)=priorRandom;
  Contract::outputOrder(script)=order;
  events=priorEvents;
  std::puts("Attack announcements: actual speech queues, fight sounds and saved draws match the VM");
  }

void humanAttackPhases(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  const auto phases=activityPhases(script,"ZS_ATTACK");
  const auto fleeCheck=vm.find_symbol_by_name("C_WANTTOFLEE")->address();
  const auto forcedRansack=vm.find_symbol_by_name("C_NPCRANSACKSALWAYS")->address();
  const auto ransack=vm.find_symbol_by_name("C_WANTTORANSACK")->address();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  auto& aivar=npc.handle().aivar;
  const auto savedAivar=std::to_array(aivar);
  auto& state=Contract::aiState(npc).funcIni;
  const auto savedState=state;
  const auto savedTime=npc.stateTime();
  const auto savedAttitude=npc.tempAttitude();
  auto* savedTarget=npc.target();
  auto* savedOther=npc.stateOther();
  auto& hp=npc.handle().attribute[NpcAttribute::HITPOINTS];
  const auto savedHp=hp;
  const auto maxHp=npc.handle().attribute[NpcAttribute::HITPOINTS_MAX];
  const auto savedInvincible=hero.handle().aivar[4];
  const auto outputOrder=Contract::outputOrder(script);
  const auto heal=script.findFunction("ZS_HEALSELF");
  const auto arenaRunning=script.intSymbol("AF_RUNNING",-1);
  const auto arenaAfter=script.intSymbol("AF_AFTER",-1);
  require(!hero.isDown() && maxHp>2 && npc.qDistTo(hero)<500.f*500.f,"human attack: a standing fight with a live target");
  {
    const auto permanent=npc.attitude();
    const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
    const auto* setAttitude=script.findSymbol("B_SETATTITUDE");
    auto* argument=script.findSymbol("B_SETATTITUDE.ATT");
    auto* actor=script.findSymbol("B_SETATTITUDE.SLF");
    const auto oldArgument=argument->get_int();
    const auto oldActor=actor->get_instance();
    for(const bool native:{false,true}) {
      enabled=native;
      npc.setAttitude(ATT_NEUTRAL);
      npc.setTempAttitude(ATT_FRIENDLY);
      unsigned bytecode=0;
      vm.observe_instructions([&](const auto&) { ++bytecode; });
      vm.push_instance(self);
      vm.push_int(ATT_HOSTILE);
      vm.unsafe_call_bl(setAttitude);
      vm.observe_instructions({});
      require((bytecode==0)==native && npc.attitude()==ATT_HOSTILE && npc.tempAttitude()==ATT_HOSTILE &&
              argument->get_int()==ATT_HOSTILE && actor->get_instance()==self,
              "combat attitude: preserve both attitudes and the saved helper arguments");
      }
    argument->set_int(oldArgument);
    actor->set_instance(oldActor);
    npc.setAttitude(permanent);
    npc.setTempAttitude(savedAttitude);
    enabled=wasEnabled;
    }
  hero.handle().aivar[4]=0;
  state=ScriptFn(phases.init);
  std::vector<std::string> original;
  auto originalRandom=Contract::random(script);
  for(bool native:{false,true}) {
    enabled=native;
    std::copy(savedAivar.begin(),savedAivar.end(),aivar);
    hp=savedHp;
    npc.setTempAttitude(savedAttitude);
    npc.setTarget(&hero);
    npc.clearAiQueue();
    npc.setStateTime(0);
    aivar[9]=script.intSymbol("AR_NONE",-1);
    aivar[27]=10;
    aivar[59]=0;
    aivar[63]=1000;
    Contract::outputOrder(script)=outputOrder;
    script.setSeed(73106);
    ActivityStream stream(world,npc);
    const std::array equipmentRules={
      vm.find_symbol_by_name("B_CREATEAMMO")->address(),
      vm.find_symbol_by_name("B_SELECTWEAPON")->address(),
      vm.find_symbol_by_name("B_SELECTSPELL")->address(),
      vm.find_symbol_by_name("C_NPCHASATTACKREASONTOKILL")->address(),
      };
    size_t bytecode=0,outcomeChecks=0,equipmentChecks=0;
    vm.observe_instructions([&](const auto&) {
      outcomeChecks+=vm.pc()==fleeCheck || vm.pc()==forcedRansack || vm.pc()==ransack;
      equipmentChecks+=std::ranges::find(equipmentRules,vm.pc())!=equipmentRules.end();
      });
    const auto count=vm.add_call_observer([&](const auto& symbol) {
      bytecode+=symbol.name()=="ZS_ATTACK" || symbol.name()=="ZS_ATTACK_LOOP" || symbol.name()=="ZS_ATTACK_END";
      });
    script.invokeState(&npc,&hero,nullptr,ScriptFn(phases.init));
    require(npc.hasPerc(PERC_ASSESSSURPRISE) && aivar[7]==int32_t(hero.instanceSymbol()) &&
            aivar[8]==0 && aivar[19]==0 && aivar[68]==0,
            "human attack: Init prepares the target, perceptions and pursuit");

    npc.clearAiQueue();
    aivar[23]=1;
    require(script.invokeState(&npc,&hero,nullptr,ScriptFn(phases.loop))==LOOP_CONTINUE && aivar[23]==0 &&
            npc.aiQueueFront() && npc.aiQueueFront()->act==AI_Wait && npc.aiQueueFront()->i0==800,
            "human attack: an ordinary Loop waits before attacking when requested");
    auto queue=Contract::queue(npc);
    bool attacks=false;
    while(queue.size()>0)
      attacks |= queue.pop().act==AI_Attack;
    require(attacks,"human attack: the live target receives the ordinary attack action");

    aivar[63]=0;
    require(script.invokeState(&npc,&hero,nullptr,ScriptFn(phases.loop))==LOOP_END && aivar[8]==1 &&
            Contract::queue(npc).size()==1 && npc.aiQueueFront()->act==AI_StandUp,
            "human attack: outside combat range the pursuit ends and the attack queue is cleared");
    npc.clearAiQueue();
    hp=maxHp/2-1;
    aivar[45]=arenaRunning;
    script.invokeState(&npc,&npc,nullptr,ScriptFn(phases.end));
    vm.remove_call_observer(count);
    vm.observe_instructions({});
    queue=Contract::queue(npc);
    require(aivar[45]==arenaAfter && npc.stateOther()==&hero && queue.size()==2 &&
            queue.pop().act==AI_RemoveWeapon,"human attack: End restores the last target, closes the arena fight and sheathes");
    const auto next=queue.pop();
    require(next.act==AI_StartState && next.func==heal && next.target==&hero && next.i0==0 &&
            bytecode==(native ? 0u : 4u) && stream.records==(native ? 4u : 0u),
            "human attack: End queues healing and all four calls use the selected executor");
    require(outcomeChecks==(native ? 0u : 3u),"human attack: Init and End use the selected combat outcome rules");
    require(native ? equipmentChecks==0 : equipmentChecks>=2,
            "human attack: ammo, weapon and intent rules use the selected executor in ordinary combat");
    std::erase_if(stream.events,[](const auto& event) {
      return !(event.starts_with("phase ") || event.starts_with("result ") ||
               event.starts_with("queue ") || event.starts_with("random "));
      });
    if(!native) {
      original=std::move(stream.events);
      originalRandom=Contract::random(script);
      } else {
      require(original==stream.events && originalRandom==Contract::random(script),
              "human attack: phase results, action queue and RNG match the original");
      }
    }
  enabled=wasEnabled;
  state=savedState;
  std::copy(savedAivar.begin(),savedAivar.end(),aivar);
  hero.handle().aivar[4]=savedInvincible;
  hp=savedHp;
  npc.setStateTime(int64_t(savedTime));
  npc.setTarget(savedTarget);
  npc.setOther(savedOther);
  npc.setTempAttitude(savedAttitude);
  std::puts("Human attack: Init, ordinary Loop, distance abort and End match the VM");
  humanAttackAnnouncements(world,npc,hero);
  }

void humanAttackOutcomeHelpers(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto savedOther=vm.global_other()->get_instance();
  const auto savedGuild=self->guild;
  const auto savedType=self->type;
  const auto symbol=[&](std::string_view name) -> daedalus::DaedalusSymbol& { return *vm.find_symbol_by_name(name); };
  const auto& flee=symbol("C_WANTTOFLEE");
  const auto& always=symbol("C_NPCRANSACKSALWAYS");
  const auto& ransack=symbol("C_WANTTORANSACK");
  const auto& finish=symbol("B_FINISHINGMOVE");
  auto& garwig=symbol("GARWIG");
  auto& rod=symbol("ROD");
  auto& raven=symbol("RAVEN");
  const auto savedGarwig=garwig.get_instance(), savedRod=rod.get_instance(), savedRaven=raven.get_instance();
  const auto hammer=script.findSymbolIndex("HOLY_HAMMER_MIS");
  const auto rodSword=script.findSymbolIndex("ITMW_2H_ROD");
  const auto sword=script.findSymbolIndex("ITMW_1H_MISC_SWORD");
  const auto dagger=script.findSymbolIndex("ITMW_1H_VLK_DAGGER");
  const auto originalSwords=npc.itemCount(sword);
  const auto predicate=[&](const auto& fn) {
    vm.push_instance(self);
    vm.unsafe_call_bl(&fn);
    return vm.pop_int();
    };
  const auto finishTarget=[&] {
    npc.clearAiQueue();
    vm.push_instance(self);
    vm.push_instance(other);
    vm.unsafe_call_bl(&finish);
    const auto* action=npc.aiQueueFront();
    require(action && action->act==AI_FinishingMove && action->target==&hero && Contract::queue(npc).size()==1 &&
            symbol("B_FINISHINGMOVE.SLF").get_instance()==self && symbol("B_FINISHINGMOVE.OTH").get_instance()==other,
            "human attack outcome: finishing binds both actors and queues the same target");
    };
  hero.addItem(hammer,1);
  hero.addItem(rodSword,1);
  vm.global_other()->set_instance(other);
  for(bool native:{false,true}) {
    enabled=native;
    size_t entries=0;
    vm.observe_instructions([&](const auto&) {
      entries+=vm.pc()==flee.address() || vm.pc()==always.address() || vm.pc()==ransack.address() || vm.pc()==finish.address();
      });
    vm.push_instance(self);
    vm.push_instance(other);
    vm.unsafe_call_bl(&flee);
    require(vm.pop_int()==0 && symbol("C_WANTTOFLEE.SLF").get_instance()==self && symbol("C_WANTTOFLEE.OTH").get_instance()==other,
            "human attack outcome: stock fleeing returns FALSE and saves both parameters");
    garwig.set_instance(other);
    rod.set_instance(other);
    require(predicate(always)==0,"human attack outcome: an unrelated actor cannot reclaim the quest items");
    garwig.set_instance(self);
    require(predicate(always)==1,"human attack outcome: the monastery guard takes back the hammer");
    garwig.set_instance(other);
    rod.set_instance(self);
    require(predicate(always)==1 && symbol("C_NPCRANSACKSALWAYS.SLF").get_instance()==self,
            "human attack outcome: Rod reclaims his sword and the check saves its actor");
    self->type=NpcType::G2_AMBIENT;
    self->guild=GIL_BAU;
    raven.set_instance(other);
    require(predicate(ransack)==1 && symbol("C_WANTTORANSACK.SLF").get_instance()==self,
            "human attack outcome: an ordinary guild permits searching the opponent");
    self->guild=GIL_PAL;
    require(predicate(ransack)==0,"human attack outcome: paladins are excluded");
    self->guild=GIL_BAU;
    self->type=NpcType::G2_FRIEND;
    require(predicate(ransack)==0,"human attack outcome: a friend does not search the hero");
    self->type=NpcType::G2_AMBIENT;
    raven.set_instance(self);
    require(predicate(ransack)==0,"human attack outcome: Raven does not search the opponent");
    self->type=savedType;
    self->guild=savedGuild;
    require(npc.closeWeapon(true),"human attack outcome: the fixture starts unarmed");
    finishTarget();
    require(npc.weaponState()==WeaponState::W1H && npc.currentMeleeWeapon() && npc.currentMeleeWeapon()->clsId()==sword &&
            npc.itemCount(sword)==std::max(originalSwords,size_t(1)),
            "human attack outcome: an unarmed finisher adds the stock sword exactly once");
    npc.setToFightMode(dagger);
    finishTarget();
    require(npc.currentMeleeWeapon() && npc.currentMeleeWeapon()->clsId()==dagger,
            "human attack outcome: an armed finisher leaves the melee weapon in place");
    require(npc.closeWeapon(true),"human attack outcome: the fixture can sheathe after finishing");
    if(originalSwords==0)
      npc.delItem(sword,1);
    vm.observe_instructions({});
    require(entries==(native ? 0u : 10u),"human attack outcome: BL takes the selected executor for all four helpers");
    }
  garwig.set_instance(savedGarwig);
  rod.set_instance(savedRod);
  raven.set_instance(savedRaven);
  vm.global_other()->set_instance(savedOther);
  hero.delItem(hammer,1);
  hero.delItem(rodSword,1);
  enabled=wasEnabled;
  std::puts("Human attack outcomes: fleeing parameters, quest items, plunder and finishing match the VM");
  }

void humanGuardCalls(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  auto& perceptions=Contract::passivePerceptions(world);
  const auto priorEvents=events;
  const auto priorOrder=Contract::outputOrder(script);
  const auto priorCount=perceptions.size();
  const auto self=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto other=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto priorSelf=vm.global_self()->get_instance(), priorOther=vm.global_other()->get_instance();
  const auto savedGuild=self->guild, savedOtherGuild=other->guild;
  const auto savedInvincible=other->aivar[4];
  const auto priorAivar=std::to_array(self->aivar);
  auto& wantedSlot=*vm.find_symbol_by_name("C_WANTTOCALLGUARDS.SLF");
  auto& toughSlot=*vm.find_symbol_by_name("C_NPCISTOUGHGUY.SLF");
  const auto priorWanted=wantedSlot.get_instance(), priorTough=toughSlot.get_instance();
  const auto* wanted=vm.find_symbol_by_name("C_WANTTOCALLGUARDS");
  const auto* call=vm.find_symbol_by_name("B_CALLGUARDS");
  const auto loop=script.findFunction("ZS_ATTACK_LOOP");
  const auto checkEmission=[&](std::string_view line,size_t voices,size_t sounds) {
    auto overlay=Contract::overlayQueue(npc);
    require(overlay.size()==voices && perceptions.size()==priorCount+sounds &&
            Contract::outputOrder(script)==priorOrder+int(voices),
            "guard call: overlay order and fight-sound count match the original");
    for(size_t i=0;i<voices;++i) {
      const auto action=overlay.pop();
      require(action.act==AI_OutputSvmOverlay && action.target==&hero && action.s0==line && action.i0==priorOrder+int(i),
              "guard call: the original alarm or request is addressed to the opponent");
      }
    for(size_t i=priorCount;i<perceptions.size();++i) {
      const auto& sound=perceptions[i];
      require(sound.what==PERC_ASSESSFIGHTSOUND && sound.self==&npc && sound.other==&hero && sound.victim==&npc,
              "guard call: the passive fight sound preserves the sender, aggressor and victim");
      }
    perceptions.resize(priorCount);
    };
  const auto prepare=[&] {
    npc.clearAiQueue();
    Contract::outputOrder(script)=priorOrder;
    };
  vm.global_self()->set_instance(self);
  vm.global_other()->set_instance(other);
  other->aivar[4]=0;
  require(!hero.isDown() && hero.playAnimByName("S_RUNL",BS_RUN,true) &&
          ActivityDefinitions::bodyStateContains(int32_t(hero.bodyState()),BS_RUN),
          "guard call: the target must be running to retain the attack state clock");
  struct Site { std::string_view reason, line; uint32_t guild; int32_t party, tough; bool checkedTough, sound; };
  for(bool native:{false,true}) {
    events=native;
    self->guild=GIL_BDT;
    self->aivar[15]=0;
    other->guild=GIL_PAL;
    vm.push_instance(other);
    vm.unsafe_call_bl(wanted);
    require(vm.pop_int()==1 && wantedSlot.get_instance()==other && vm.global_self()->get_instance()==self,
            "guard call: the predicate uses the argument guild and the global caller's party flag");
    self->aivar[15]=1;
    vm.push_instance(other);
    vm.unsafe_call_bl(wanted);
    require(vm.pop_int()==0 && wantedSlot.get_instance()==other,
            "guard call: an eligible argument cannot bypass the caller's party flag");
    other->guild=savedOtherGuild;
    for(const Site site:{
        Site{"AR_GUARDCALLEDTOROOM","$ALARM",GIL_VLK,0,0,false,false},
        Site{"AR_HUMANMURDEREDHUMAN","$ALARM",GIL_MIL,0,0,false,true},
        Site{"AR_GUILDENEMY","$GUARDS",GIL_BAU,0,0,true,true},
        Site{"AR_REACTTOWEAPON","",GIL_BAU,0,1,true,false},
        Site{"AR_GUARDSTOPSINTRUDER","$ALARM",GIL_BAU,0,1,false,true},
        Site{"AR_USEMOB","$GUARDS",GIL_VLK,0,0,true,true},
        Site{"AR_CLEARROOM","$ALARM",GIL_SLD,0,0,false,true},
        Site{"AR_THEFT","",GIL_VLK,1,0,false,false},
        Site{"AR_THEFT","",GIL_BDT,0,0,false,false}}) {
      self->guild=site.guild;
      self->aivar[9]=script.intSymbol(site.reason,-1);
      self->aivar[15]=site.party;
      self->aivar[57]=site.tough;
      toughSlot.set_instance(nullptr);
      prepare();
      size_t callEntries=0,wantedEntries=0;
      vm.observe_instructions([&](const auto&) {
        callEntries+=vm.pc()==call->address();
        wantedEntries+=vm.pc()==wanted->address();
        });
      vm.unsafe_call_bl(call);
      vm.observe_instructions({});
      checkEmission(site.line,size_t(!site.line.empty()),size_t(site.sound));
      require(callEntries==size_t(!native) && wantedEntries==size_t(!native) && wantedSlot.get_instance()==self &&
              toughSlot.get_instance()==(site.checkedTough ? self : nullptr),
              "guard call: BL selects the right body and writes the saved parameters only in the original branches");
      }
    self->guild=GIL_BAU;
    self->aivar[9]=script.intSymbol("AR_THEFT",-1);
    self->aivar[15]=0;
    self->aivar[57]=0;
    self->aivar[8]=0;
    self->aivar[19]=0;
    self->aivar[24]=std::numeric_limits<int32_t>::max();
    self->aivar[27]=60;
    self->aivar[59]=0;
    self->aivar[63]=10000;
    npc.setTarget(&hero);
    size_t loopBytecode=0;
    vm.observe_instructions([&](const auto&) { loopBytecode+=vm.pc()==call->address(); });
    for(const auto& [time,calls,sounds]:{std::array{2999,0,0},std::array{3000,1,1},std::array{8999,1,0},std::array{9000,2,1}}) {
      prepare();
      npc.setStateTime(time);
      require(script.invokeState(&npc,&hero,nullptr,loop)==LOOP_CONTINUE && self->aivar[19]==calls,
              "guard call: the attack loop advances the two counters at three and nine whole seconds");
      checkEmission("$GUARDS",size_t(sounds),size_t(sounds));
      }
    prepare();
    self->aivar[19]=0;
    npc.setStateTime(9000);
    require(script.invokeState(&npc,&hero,nullptr,loop)==LOOP_CONTINUE && self->aivar[19]==2,
            "guard call: a first late attack tick still makes both original calls");
    checkEmission("$GUARDS",2,2);
    vm.observe_instructions({});
    require(loopBytecode==(native ? 0u : 4u),"guard call: the native attack loop bypasses the original helper body");
    }
  self->guild=savedGuild;
  other->aivar[4]=savedInvincible;
  std::copy(priorAivar.begin(),priorAivar.end(),self->aivar);
  wantedSlot.set_instance(priorWanted);
  toughSlot.set_instance(priorTough);
  vm.global_self()->set_instance(priorSelf);
  vm.global_other()->set_instance(priorOther);
  hero.stopAnim("S_RUNL");
  prepare();
  events=priorEvents;
  std::puts("Guard calls: eligibility, overlay, fight sound and attack timing match the VM");
  }

void fleeAndWatchPhases(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto flee=activityPhases(script,"ZS_FLEE");
  const auto watch=activityPhases(script,"ZS_WATCHFIGHT");
  const auto heal=script.findFunction("ZS_HEALSELF");
  const auto attack=script.findFunction("ZS_ATTACK");
  const auto unconscious=script.findFunction("ZS_UNCONSCIOUS");
  auto* lobart=activityNpc(world,"BAU_950_LOBART");
  require(lobart && !hero.isDown() && npc.qDistTo(hero)<300.f*300.f,"flee and watch: a live opponent and a nearby spectator");
  auto& aivar=npc.handle().aivar;
  const auto savedAivar=std::to_array(aivar);
  const auto previousTime=npc.stateTime();
  auto* savedTarget=npc.target();
  const auto lobartPos=lobart->position();
  auto& lobartState=Contract::aiState(*lobart).funcIni;
  const auto savedLobartState=lobartState;
  const auto order=Contract::outputOrder(script);
  constexpr auto overlay="HUMANS_FLEE.MDS";
  lobart->setPosition(npc.position()+Tempest::Vec3(450,0,0));
  lobart->updateTransform();
  std::vector<std::string> original;
  for(bool native:{false,true}) {
    enabled=native;
    Contract::outputOrder(script)=order;
    std::copy(savedAivar.begin(),savedAivar.end(),aivar);
    npc.setTarget(&hero);
    npc.clearAiQueue();
    npc.setStateTime(0);
    ActivityStream stream(world,npc);
    size_t vmCalls=0,validateCalls=0,invocations=0;
    const auto count=vm.add_call_observer([&](const auto& sym) {
      vmCalls+=sym.name()=="ZS_FLEE" || sym.name()=="ZS_FLEE_LOOP" || sym.name()=="ZS_FLEE_END" ||
               sym.name().starts_with("ZS_WATCHFIGHT");
      validateCalls+=sym.name()=="B_VALIDATEOTHER";
      });
    const auto invoke=[&](size_t phase,Npc* other,Npc* victim=nullptr) {
      ++invocations;
      return script.invokeState(&npc,other,victim,ScriptFn(phase));
      };
    const auto overlayPc=vm.find_symbol_by_name("B_SAY_OVERLAY")->address();
    size_t overlayInstructions=0;
    vm.observe_instructions([&](const auto&) { overlayInstructions+=vm.pc()==overlayPc; });
    invoke(flee.init,&hero);
    vm.observe_instructions({});
    require(overlayInstructions==(native ? 0u : 1u),"flee: only the original phase executes the overlay speech body");
    require(npc.hasPerc(PERC_ASSESSMAGIC) && npc.hasOverlay(overlay) && aivar[65]==0 && npc.target()==&hero,
            "flee: fresh Init applies the overlay and keeps the active target");
    const auto* warning=Contract::overlayQueue(npc).front();
    require(npc.aiQueueFront() && npc.aiQueueFront()->act==AI_RemoveWeapon && warning &&
            warning->act==AI_OutputSvmOverlay && warning->s0=="$RUNAWAY" && warning->target==&hero,
            "flee: fresh Init queues the warning alongside sheathing the weapon");
    npc.clearAiQueue();
    require(invoke(flee.loop,&hero)==LOOP_CONTINUE && npc.aiQueueFront() && npc.aiQueueFront()->act==AI_Flee,
            "flee: a nearby standing target keeps the old flee action");
    npc.setTarget(nullptr);
    require(invoke(flee.loop,&hero)==LOOP_END && npc.isAiQueueEmpty(),"flee: losing the target clears the queue and ends");
    invoke(flee.end,&hero);
    auto queue=Contract::queue(npc);
    require(!npc.hasOverlay(overlay) && queue.size()==2 && queue.pop().act==AI_StandUp,
            "flee: End drops the overlay before standing and healing");
    const auto treatment=queue.pop();
    require(treatment.act==AI_StartState && treatment.func==heal && treatment.i0==1,
            "flee: post-flight healing uses the original transition mode");
    npc.clearAiQueue();
    aivar[7]=int32_t(hero.instanceSymbol());
    invoke(flee.init,nullptr);
    require(aivar[65]==1 && npc.target()==&hero && npc.hasOverlay(overlay) &&
            npc.aiQueueFront() && npc.aiQueueFront()->act==AI_RemoveWeapon && Contract::overlayQueue(npc).size()==0,
            "flee: re-entry with a saved target restores the chase context and skips the warning");
    invoke(flee.end,&hero);
    npc.clearAiQueue();

    lobartState=attack;
    invoke(watch.init,lobart,&hero);
    require(npc.hasPerc(PERC_ASSESSDEFEAT) && npc.hasPerc(PERC_ASSESSOTHERSDAMAGE) && aivar[19]==1 && aivar[68]==0,
            "watch fight: Init listens for defeat and blows and sets the position flag");
    require(invoke(watch.loop,lobart,&hero)==LOOP_CONTINUE,"watch fight: a close fight remains under observation");
    queue=Contract::queue(npc);
    require(queue.size()==2 && queue.front()->act==AI_TurnToNpc && queue.pop().target==lobart && queue.pop().act==AI_Dodge,
            "watch fight: at close range the old turn goes to the farther combatant before dodging");
    lobartState=unconscious;
    npc.setStateTime(1000);
    require(invoke(watch.loop,lobart,&hero)==LOOP_CONTINUE && aivar[19]==0 &&
            invoke(watch.loop,lobart,&hero)==LOOP_END && npc.isAiQueueEmpty(),
            "watch fight: a fallen opponent gives the spectator exactly one more Loop");
    invoke(watch.end,lobart,&hero);
    vm.remove_call_observer(count);
    require(vmCalls==(native ? 0u : invocations) && validateCalls==(native ? 0u : 2u) &&
            stream.records==(native ? invocations : 0u) && npc.isAiQueueEmpty(),
            "flee and watch: every phase uses the selected executor and watch End is empty");
    std::erase_if(stream.events,[](const auto& event) {
      return !(event.starts_with("phase ") || event.starts_with("result ") || event.starts_with("queue "));
      });
    if(native) require(original==stream.events,"flee and watch: both phase results and action queues match the VM");
    else original=std::move(stream.events);
    }
  enabled=wasEnabled;
  std::copy(savedAivar.begin(),savedAivar.end(),aivar);
  npc.setStateTime(int64_t(previousTime));
  npc.setTarget(savedTarget);
  lobartState=savedLobartState;
  lobart->setPosition(lobartPos);
  lobart->updateTransform();
  std::puts("Flee and watch: target restoration, overlays, dodge and grace match the VM");
  }

void humanPlunderAndArmorPhases(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto comments=script.findFunction("ZS_COMMENTFAKEGUILD");
  const auto meatState=script.findFunction("ZS_GETMEAT");
  const auto ransack=activityPhases(script,"ZS_RANSACKBODY");
  const auto meat=script.findSymbolIndex("ITFOMUTTONRAW");
  const auto gold=script.findSymbolIndex("ITMI_GOLD");
  const auto hammer=script.findSymbolIndex("HOLY_HAMMER_MIS");
  const auto goldBefore=hero.itemCount(gold);
  const auto meatBefore=hero.itemCount(meat);
  const auto npcGold=npc.itemCount(gold);
  const auto npcMeat=npc.itemCount(meat);
  const auto npcHammer=npc.itemCount(hammer);
  auto& goldScratch=*vm.find_symbol_by_name("ZS_RANSACKBODY_END.X");
  auto& meatScratch=*vm.find_symbol_by_name("ZS_GETMEAT.X");
  auto& armorScratch=*vm.find_symbol_by_name("ZS_COMMENTFAKEGUILD.ITM");
  auto& garwig=*vm.find_symbol_by_name("GARWIG");
  const auto savedGold=goldScratch.get_int();
  const auto savedMeat=meatScratch.get_int();
  const auto savedArmor=armorScratch.get_instance();
  const auto savedGarwig=garwig.get_instance();
  const auto savedItem=vm.global_item()->get_instance();
  auto* lobart=activityNpc(world,"BAU_950_LOBART");
  require(lobart && lobart->currentArmor(),"armor comment: the fixture needs Lobart's equipped armor");
  const auto order=Contract::outputOrder(script);
  const auto said=[&](std::string_view line,const Npc& to) {
    auto queue=Contract::queue(npc);
    while(queue.size()>0) {
      const auto act=queue.pop();
      if(act.act==AI_OutputSvm && act.s0==line && act.target==&to)
        return true;
      }
    return false;
    };
  std::vector<std::string> original;
  for(bool native:{false,true}) {
    enabled=native;
    Contract::outputOrder(script)=order;
    npc.clearAiQueue();
    lobart->clearAiQueue();
    ActivityStream stream(world,npc);
    size_t vmCalls=0;
    const auto observer=vm.add_call_observer([&](const auto& sym) {
      vmCalls+=sym.name()=="ZS_COMMENTFAKEGUILD" || sym.name()=="ZS_GETMEAT" || sym.name().starts_with("ZS_RANSACKBODY");
      });
    script.invokeState(&npc,lobart,nullptr,comments);
    const auto equippedArmor=std::static_pointer_cast<daedalus::IItem>(lobart->currentArmor()->handlePtr());
    require(said("$ADDON_WRONGARMOR",*lobart) && armorScratch.get_instance()==equippedArmor,
            "armor comment: the original wrong-armor line refers to the equipped item");
    for(const auto& action:activityQueue(*lobart)) stream.events.push_back("queue other "+action);
    npc.clearAiQueue();
    lobart->clearAiQueue();

    hero.addItem(meat,3);
    script.invokeState(&npc,&hero,nullptr,meatState);
    require(meatScratch.get_int()==int32_t(meatBefore+3) && hero.itemCount(meat)==0 &&
            npc.itemCount(meat)==npcMeat+meatBefore+3,"get meat: all raw meat moves to the collector");
    npc.delItem(meat,uint32_t(meatBefore+3));
    if(meatBefore>0) hero.addItem(meat,meatBefore);
    npc.clearAiQueue();

    hero.addItem(gold,7);
    script.invokeState(&npc,&hero,nullptr,ScriptFn(ransack.init));
    require(script.invokeState(&npc,&hero,nullptr,ScriptFn(ransack.loop))==LOOP_END,
            "ransack: after approach the original Loop exits");
    hero.addItem(hammer,1);
    const auto extraHammer=hero.itemCount(hammer);
    // Bind the monastery guard role to the nearby fixture NPC for the quest-item path.
    garwig.set_instance(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()));
    script.invokeState(&npc,&hero,nullptr,ScriptFn(ransack.end));
    garwig.set_instance(savedGarwig);
    require(goldScratch.get_int()==int32_t(goldBefore+7) && hero.itemCount(gold)==0 &&
            npc.itemCount(gold)==npcGold+goldBefore+7 && npc.itemCount(hammer)==npcHammer+1 &&
            hero.itemCount(hammer)==extraHammer-1 && said("$ITOOKYOURGOLD",hero),
            "ransack: the named guard takes the quest item and all gold");
    npc.delItem(hammer,1);
    script.invokeState(&npc,&hero,nullptr,ScriptFn(ransack.end));
    require(goldScratch.get_int()==int32_t(goldBefore+7) && npc.itemCount(gold)==npcGold+goldBefore+7 &&
            said("$SHITNOGOLD",hero),"ransack: a second search finds no gold and retains the recorded amount");
    npc.delItem(gold,uint32_t(goldBefore+7));
    if(goldBefore>0) hero.addItem(gold,goldBefore);
    vm.remove_call_observer(observer);
    require(vmCalls==(native ? 0u : 6u) && stream.records==(native ? 6u : 0u),
            "armor and plunder: all requested phases use the chosen executor");
    std::erase_if(stream.events,[](const auto& event) {
      return !(event.starts_with("phase ") || event.starts_with("result ") || event.starts_with("queue "));
      });
    if(native) require(original==stream.events,"armor and plunder: phase results and queued actions match the VM");
    else original=std::move(stream.events);
    }
  enabled=wasEnabled;
  goldScratch.set_int(savedGold);
  meatScratch.set_int(savedMeat);
  armorScratch.set_instance(savedArmor);
  vm.global_item()->set_instance(savedItem);
  std::puts("Armor and plunder: comments, meat, quest item and gold match the VM");
  }

void controlSpellPhases(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto tentacle=activityPhases(script,"ZS_GREENTENTACLE");
  const auto zap=activityPhases(script,"ZS_SHORTZAPPED");
  const auto warn=script.findFunction("ZS_REACTTODAMAGE");
  auto& pose=*vm.find_symbol_by_name("ZS_GREENTENTACLE.RANDY");
  const auto savedPose=pose.get_int();
  const auto savedGuild=npc.handle().guild;
  const auto savedAttitude=npc.attitude();
  const auto savedTempAttitude=npc.tempAttitude();
  const auto savedTime=npc.stateTime();
  auto& lastHitSpell=Contract::lastHitSpell(npc);
  const auto savedLastHitSpell=lastHitSpell;
  const auto tentacleSpell=script.intSymbol("SPL_GREENTENTACLE",-1);
  const auto otherSpell=script.intSymbol("SPL_FIREBOLT",-1);
  require(tentacleSpell>=0 && otherSpell>=0 && tentacleSpell!=otherSpell,"green tentacle: the fixture needs two distinct stock spell IDs");
  auto& aivar=npc.handle().aivar;
  const auto savedAivar=std::to_array(aivar);
  const auto wolfGuild=script.intSymbol("GIL_WOLF",-1);
  std::vector<std::string> original;
  auto originalRandom=Contract::random(script);
  npc.setAttitude(ATT_FRIENDLY);
  const auto expectWarning=[&](bool resume,const Npc* victim) {
    auto queue=Contract::queue(npc);
    require(queue.size()==(resume ? 2u : 1u),"spell control: the stop callback keeps the original continuation count");
    const auto reaction=queue.pop();
    require(reaction.act==AI_StartState && reaction.func==warn && reaction.target==&hero && reaction.victim==victim &&
            reaction.i0==0,"spell control: a human stop calls damage assessment with the live attacker and victim");
    if(resume) require(queue.pop().act==AI_ContinueRoutine,"spell control: lightning queues routine continuation last");
    };
  for(bool native:{false,true}) {
    enabled=native;
    npc.handle().guild=savedGuild;
    npc.setTempAttitude(savedTempAttitude);
    std::copy(savedAivar.begin(),savedAivar.end(),aivar);
    npc.clearAiQueue();
    npc.setStateTime(0);
    pose.set_int(2);
    script.setSeed(73106);
    ActivityStream stream(world,npc);
    size_t phases=0,stopCalls=0,restartCalls=0,damageCalls=0,invocations=0;
    const auto observer=vm.add_call_observer([&](const auto& sym) {
      phases+=sym.name().starts_with("ZS_GREENTENTACLE") || sym.name().starts_with("ZS_SHORTZAPPED");
      stopCalls+=sym.name()=="B_STOPGREENTENTACLE" || sym.name()=="B_STOPSHORTZAPPED";
      restartCalls+=sym.name()=="B_RESTARTGREENTENTACLE";
      damageCalls+=sym.name()=="B_ASSESSDAMAGE";
      });
    const auto invoke=[&](size_t phase,int time=0) {
      ++invocations;
      npc.setStateTime(time);
      return script.invokeState(&npc,&hero,nullptr,ScriptFn(phase));
      };
    invoke(tentacle.init);
    auto queue=Contract::queue(npc);
    require(npc.hasPerc(PERC_ASSESSMAGIC) && npc.hasPerc(PERC_ASSESSDAMAGE) &&
            queue.size()==2 && queue.pop().act==AI_StandUp,"green tentacle: Init stands up and activates both perceptions");
    const auto humanPose=queue.pop();
    require(humanPose.act==AI_PlayAnimBs && humanPose.s0=="T_STAND_2_GREENTENTACLEC_VICTIM" &&
            humanPose.i0==BS_UNCONSCIOUS && pose.get_int()==2,"green tentacle: Init keeps the spell's selected pose");
    const auto pending=activityQueue(npc);
    for(const auto& [spell,expected]:{std::pair{otherSpell,17000u},std::pair{tentacleSpell,0u}}) {
      npc.setStateTime(17000);
      lastHitSpell=spell;
      require(npc.perceptionProcess(hero,&npc,0,PERC_ASSESSMAGIC),"green tentacle: the active state receives subsequent magic");
      require(npc.stateTime()==expected && activityQueue(npc)==pending,
              "green tentacle: only a matching last hit restarts the timer and the queue is preserved");
      }
    lastHitSpell=savedLastHitSpell;
    npc.clearAiQueue();
    require(invoke(tentacle.loop,20999)==LOOP_CONTINUE && npc.isAiQueueEmpty() &&
            invoke(tentacle.loop,21000)==LOOP_END,"green tentacle: the timeout compares whole seconds");
    expectWarning(false,nullptr);
    invoke(tentacle.end);
    npc.clearAiQueue();

    invoke(zap.init);
    queue=Contract::queue(npc);
    const auto stand=npc.hasStateFlag(BS_FLAG_INTERRUPTABLE) ? AI_StandUpQuick : AI_StandUp;
    require(npc.hasPerc(PERC_ASSESSSTOPMAGIC) && queue.size()==2 && queue.pop().act==stand &&
            queue.pop().s0=="T_STAND_2_LIGHTNING_VICTIM","short zap: Init registers cancellation and queues the original animation");
    npc.clearAiQueue();
    require(invoke(zap.loop,2999)==LOOP_CONTINUE && npc.isAiQueueEmpty() && invoke(zap.loop,3000)==LOOP_CONTINUE,
            "short zap: stop is queued above two whole seconds while the Loop result stays zero");
    expectWarning(true,nullptr);
    invoke(zap.end);
    npc.clearAiQueue();

    invoke(zap.init);
    require(npc.perceptionProcess(hero,&npc,0,PERC_ASSESSSTOPMAGIC),"short zap: the stock stop-magic perception is delivered");
    expectWarning(true,&npc);
    npc.clearAiQueue();

    npc.handle().guild=uint32_t(wolfGuild);
    invoke(tentacle.init);
    queue=Contract::queue(npc);
    require(queue.size()==2 && queue.pop().act==AI_StandUp && queue.pop().s0=="T_STAND_2_FREEZE_VICTIM",
            "green tentacle: a nonhuman guild uses the freeze animation");
    require(invoke(tentacle.loop,21000)==LOOP_END && npc.tempAttitude()==ATT_HOSTILE && npc.hasPerc(PERC_ASSESSMAGIC) &&
            Contract::queue(npc).size()==1 && npc.aiQueueFront()->act==AI_StandUp,
            "green tentacle: a nonhuman stop restores magic perception and sets hostility");
    vm.remove_call_observer(observer);
    require(phases==(native ? 0u : invocations) && stopCalls==(native ? 0u : 4u) && damageCalls==(native ? 0u : 3u) &&
            restartCalls==(native ? 0u : 2u) && stream.records==(native ? invocations+3 : 0u),
            "spell control: all phases and the routed reactions use the selected executor");
    std::erase_if(stream.events,[](const auto& event) {
      return !(event.starts_with("phase ") || event.starts_with("result ") ||
               event.starts_with("queue ") || event.starts_with("random "));
      });
    if(!native) {
      original=std::move(stream.events);
      originalRandom=Contract::random(script);
      } else {
      require(original==stream.events && originalRandom==Contract::random(script),
              "spell control: phase results, queue and RNG match the original");
      }
    }
  enabled=wasEnabled;
  pose.set_int(savedPose);
  npc.handle().guild=savedGuild;
  npc.setAttitude(savedAttitude);
  npc.setTempAttitude(savedTempAttitude);
  npc.setStateTime(int64_t(savedTime));
  std::copy(savedAivar.begin(),savedAivar.end(),aivar);
  std::puts("Spell control: green tentacle and short zap match the VM including timed and perceived stops, and selective restart");
  }

void enemyPerceptions(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto enemy=script.findFunction("B_ASSESSENEMY");
  const auto enemyPc=vm.find_symbol_by_name("B_ASSESSENEMY")->address();
  const auto player=script.findFunction("B_ASSESSPLAYER");
  const auto attack=script.findFunction("ZS_ATTACK");
  const auto observe=script.findFunction("ZS_OBSERVEPLAYER");
  const auto playerReason=script.intSymbol("AR_REACTTOWEAPON",-1);
  const auto guildReason=script.intSymbol("AR_GUILDENEMY",-1);
  const auto crime=script.intSymbol("CRIME_ATTACK",-1);
  auto& state=Contract::aiState(npc).funcIni;
  auto& aivar=npc.handle().aivar;
  const auto savedState=state;
  const auto savedAivar=std::to_array(aivar);
  const auto savedTime=npc.stateTime();
  const auto savedAttitude=npc.attitude();
  auto* savedTarget=npc.target();
  const auto savedNpcGuild=npc.handle().guild;
  const auto savedHeroGuild=hero.handle().guild;
  const auto savedInvincible=hero.handle().aivar[4];
  require(Contract::aiState(npc).started && !hero.isDown() && npc.qDistTo(hero)<500.f*500.f &&
          script.guildAttitude(npc,hero)!=ATT_HOSTILE,"enemy perception: a live player is nearby without an old guild feud");
  hero.handle().aivar[4]=0;
  npc.setAttitude(ATT_HOSTILE);

  enum class Site { BeforeDelay, AfterDelay, ObservingPlayer, TalkBl, OpposedGuild, EnemyOverride };
  for(auto site:{Site::BeforeDelay,Site::AfterDelay,Site::ObservingPlayer,Site::TalkBl,Site::OpposedGuild,Site::EnemyOverride}) {
    const bool guildFeud=site==Site::OpposedGuild;
    const bool playerAssessment=site==Site::ObservingPlayer;
    const bool watching=playerAssessment || site==Site::TalkBl;
    const bool hostile=site!=Site::BeforeDelay && site!=Site::EnemyOverride;
    npc.handle().guild=guildFeud ? uint32_t(GIL_MIL) : savedNpcGuild;
    hero.handle().guild=guildFeud ? uint32_t(GIL_BDT) : savedHeroGuild;
    if(guildFeud) require(script.guildAttitude(npc,hero)==ATT_HOSTILE,"enemy perception: the stock guilds are opposed");
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      enabled=native;
      state=watching ? observe : savedState;
      std::copy(savedAivar.begin(),savedAivar.end(),aivar);
      aivar[1]=0;
      aivar[15]=0;
      aivar[47]=playerReason;
      aivar[61]=site==Site::EnemyOverride ? 1 : 0;
      npc.setTarget(nullptr);
      npc.setStateTime(site==Site::BeforeDelay ? 2999 : watching || guildFeud ? 0 : 3000);
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(173));
      npc.setPerceptionEnable(PERC_ASSESSENEMY,enemy.ptr);
      npc.setPerceptionEnable(PERC_ASSESSPLAYER,player.ptr);
      npc.setPerceptionEnable(PERC_ASSESSTALK,script.findFunction("B_ASSESSTALK").ptr);
      size_t bytecode=0,playerCalls=0;
      vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==enemyPc; });
      const auto observer=vm.add_call_observer([&](const auto& sym) { playerCalls+=sym.name()=="B_ASSESSPLAYER"; });
      const auto perc=playerAssessment ? PERC_ASSESSPLAYER : site==Site::TalkBl ? PERC_ASSESSTALK : PERC_ASSESSENEMY;
      const bool delivered=npc.perceptionProcess(hero,nullptr,npc.qDistTo(hero),perc);
      vm.remove_call_observer(observer);
      vm.observe_instructions({});
      require(delivered && bytecode==(native ? 0u : 1u) && playerCalls==(!native && playerAssessment ? 1u : 0u),
              "enemy perception: direct, player and talk callbacks skip enemy bytecode in native mode");
      const auto* next=npc.aiQueueFront();
      if(hostile) {
        require(next && next->act==AI_StartState && next->func==attack && next->target==&hero && next->i0==0 &&
                npc.target()==&hero && aivar[9]==(guildFeud ? guildReason : playerReason) &&
                aivar[1]==(guildFeud ? 0 : crime) && !npc.hasPerc(PERC_ASSESSENEMY),
                "enemy perception: target, transition, cause and crime follow the original hostility policy");
        } else {
        require(next && next->act==AI_Wait && npc.target()==nullptr && npc.hasPerc(PERC_ASSESSENEMY),
                "enemy perception: delay and enemy override preserve the queue and perception");
        }
      auto queue=activityQueue(npc);
      if(native) require(original==queue,"enemy perception: the original and native action queues match");
      else original=std::move(queue);
      }
    }
  enabled=wasEnabled;
  state=savedState;
  std::copy(savedAivar.begin(),savedAivar.end(),aivar);
  npc.setStateTime(int64_t(savedTime));
  npc.setAttitude(savedAttitude);
  npc.setTarget(savedTarget);
  npc.handle().guild=savedNpcGuild;
  hero.handle().guild=savedHeroGuild;
  hero.handle().aivar[4]=savedInvincible;
  std::puts("Enemy perception: direct, player and talk assessment match the VM for delay, observation, guild and override");
  }

void propertyCrimePerceptions(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto theft=script.findFunction("B_ASSESSTHEFT");
  const auto useMob=script.findFunction("B_ASSESSUSEMOB");
  const auto theftPc=vm.find_symbol_by_name("B_ASSESSTHEFT")->address();
  const auto useMobPc=vm.find_symbol_by_name("B_ASSESSUSEMOB")->address();
  const auto mobPossessionPc=vm.find_symbol_by_name("C_ISUSEDMOBMYPOSSESSION")->address();
  const auto itemPossessionPc=vm.find_symbol_by_name("C_ISTAKENITEMMYPOSSESSION")->address();
  const auto thiefPolicyPc=vm.find_symbol_by_name("C_WANTTOATTACKTHIEF")->address();
  const auto attack=script.findFunction("ZS_ATTACK");
  const auto guard=script.findFunction("ZS_GUARD_PASSAGE");
  const auto hammer=script.findSymbolIndex("HOLY_HAMMER_MIS");
  const auto theftReason=script.intSymbol("AR_THEFT",-1);
  const auto useReason=script.intSymbol("AR_USEMOB",-1);
  const auto theftCrime=script.intSymbol("CRIME_THEFT",-1);
  auto& hammerTaken=*vm.find_symbol_by_name("HAMMER_TAKEN");
  auto& possessionItem=*vm.find_symbol_by_name("C_ISTAKENITEMMYPOSSESSION.ITM");
  auto& possessionRoom=*vm.find_symbol_by_name("C_ISTAKENITEMMYPOSSESSION.PORTALGUILD");
  auto& mobScratch=*vm.find_symbol_by_name("B_ASSESSUSEMOB.DETMOB");
  auto& aivar=npc.handle().aivar;
  auto& state=Contract::aiState(npc).funcIni;
  const auto savedState=state;
  const auto savedAivar=std::to_array(aivar);
  auto* savedTarget=npc.target();
  auto* savedMob=hero.detectedMob();
  const auto savedItem=vm.global_item()->get_instance();
  const auto savedPossessionItem=possessionItem.get_instance();
  const auto savedPossessionRoom=possessionRoom.get_int();
  const int savedHammerTaken=hammerTaken.get_int();
  const std::string savedMobScratch(mobScratch.get_string());
  const auto savedHeroGuild=hero.handle().guild;
  auto* item=hero.addItem(hammer,1);
  require(item && !hero.interactive() && npc.canSeeNpc(hero,false) && npc.portalName()==hero.portalName() &&
          world.guildOfRoom(hero.portalName())==int32_t(GIL_PUBLIC),
          "property crime: the fixture has a real item and a visible player in a public room");
  const auto savedOwner=item->handle().owner;
  const auto savedOwnerGuild=item->handle().owner_guild;
  const auto savedItemFlags=item->handle().flags;
  const auto dropped=uint32_t(script.intSymbol("ITEM_DROPPED",0));
  require(dropped!=0 && script.guildAttitude(npc,npc)==ATT_FRIENDLY,
          "property crime: dropped items and the witness's friendly guild are available");
  Interactive* chest=nullptr;
  Interactive* furniture=nullptr;
  for(uint32_t id=0; auto* mob=world.mobsiById(id); ++id) {
    const auto scheme=mob->schemeName();
    if(scheme=="CHESTSMALL" || scheme=="CHESTBIG") chest=mob;
    else if(!scheme.empty()) furniture=mob;
    if(chest && furniture) break;
    }
  require(chest && furniture,"property crime: the fixture needs a real chest and a separate interactive");

  enum class Site { Take, GuardTake, Unowned, Dropped, RoomOwned, GuildOwned, UseChest, UseFurniture };
  for(const auto site:{Site::Take,Site::GuardTake,Site::Unowned,Site::Dropped,Site::RoomOwned,Site::GuildOwned,
                       Site::UseChest,Site::UseFurniture}) {
    const bool usingMob=site==Site::UseChest || site==Site::UseFurniture;
    const bool owned=site==Site::Take || site==Site::GuardTake;
    const bool attacks=site==Site::Take || site==Site::UseChest || site==Site::RoomOwned || site==Site::GuildOwned;
    const bool remembers=attacks || site==Site::GuardTake;
    const bool personallyChecked=owned || site==Site::Unowned;
    const int32_t roomGuild=site==Site::RoomOwned ? int32_t(npc.handle().guild)
                           : site==Site::GuildOwned ? int32_t(GIL_NONE) : int32_t(GIL_PUBLIC);
    world.assignRoomToGuild("FARM01",roomGuild);
    require(world.guildOfRoom(hero.portalName())==roomGuild,"property crime: both trials use the intended occupied room");
    const auto perc=usingMob ? PERC_ASSESSUSEMOB : PERC_ASSESSTHEFT;
    const auto callback=usingMob ? useMob : theft;
    const auto pc=usingMob ? useMobPc : theftPc;
    auto* mob=site==Site::UseChest ? chest : furniture;
    hero.handle().guild=personallyChecked ? GIL_NONE : GIL_BDT;
    require((script.guildAttitude(npc,hero)==ATT_FRIENDLY)==personallyChecked,
            "property crime: friendly guilds check ownership before the general property rule");
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      enabled=native;
      state=site==Site::GuardTake ? guard : savedState;
      std::copy(savedAivar.begin(),savedAivar.end(),aivar);
      aivar[0]=0;
      aivar[1]=0;
      aivar[55]=0;
      npc.clearAiQueue();
      npc.setTarget(nullptr);
      npc.aiPush(AiQueue::aiWait(173));
      npc.setPerceptionEnable(perc,callback.ptr);
      hammerTaken.set_int(0);
      mobScratch.set_string("");
      possessionRoom.set_int(-1);
      item->handle().owner=owned ? int32_t(npc.instanceSymbol()) : -1;
      item->handle().owner_guild=site==Site::GuildOwned ? int32_t(npc.handle().guild) : int32_t(GIL_NONE);
      item->handle().flags=daedalus::ItemFlag((uint32_t(savedItemFlags)&~dropped) | (site==Site::Dropped || site==Site::Take ? dropped : 0u));
      script.setInstanceItem(hero,hammer);
      hero.setDetectedMob(usingMob ? mob : nullptr);
      size_t bytecode=0,possessionCode=0,policyCode=0;
      vm.observe_instructions([&](const auto&) {
        bytecode+=vm.pc()==pc;
        possessionCode+=vm.pc()==(usingMob ? mobPossessionPc : itemPossessionPc);
        policyCode+=vm.pc()==thiefPolicyPc;
        });
      const bool delivered=npc.perceptionProcess(hero,nullptr,npc.qDistTo(hero),perc);
      vm.observe_instructions({});
      const auto* next=npc.aiQueueFront();
      require(delivered && bytecode==size_t(!native) && possessionCode==size_t(!native && site!=Site::Unowned) &&
              policyCode==size_t(!native && remembers) && aivar[1]==(remembers ? theftCrime : 0),
              "property crime: each route uses the selected executor and records the original crime");
      if(attacks) {
        require(next && next->act==AI_StartState && next->func==attack && next->target==&hero &&
                next->i0==0 && npc.target()==&hero && aivar[9]==(usingMob ? useReason : theftReason) &&
                !npc.hasPerc(perc),"property crime: attack preserves the target, cause and cleared perceptions");
        } else {
        require(next && next->act==AI_Wait && npc.target()==nullptr && npc.hasPerc(perc),
                "property crime: a guard report, unrelated item and non-chest preserve the queue");
        }
      require(hammerTaken.get_int()==int32_t(owned) && (!usingMob || mobScratch.get_string()==mob->schemeName()) &&
              (!usingMob && site!=Site::Unowned ? possessionRoom.get_int()==roomGuild : possessionRoom.get_int()==-1),
              "property crime: the ownership quest flag and detected scheme match the source");
      auto queue=activityQueue(npc);
      if(native) require(original==queue,"property crime: action queues match the VM");
      else original=std::move(queue);
      }
    }
  world.assignRoomToGuild("FARM01",int32_t(GIL_PUBLIC));
  enabled=wasEnabled;
  state=savedState;
  std::copy(savedAivar.begin(),savedAivar.end(),aivar);
  npc.setTarget(savedTarget);
  hero.setDetectedMob(savedMob);
  hero.handle().guild=savedHeroGuild;
  item->handle().owner=savedOwner;
  item->handle().owner_guild=savedOwnerGuild;
  item->handle().flags=savedItemFlags;
  hero.delItem(hammer,1);
  vm.global_item()->set_instance(savedItem);
  possessionItem.set_instance(savedPossessionItem);
  possessionRoom.set_int(savedPossessionRoom);
  hammerTaken.set_int(savedHammerTaken);
  mobScratch.set_string(savedMobScratch);
  std::puts("Property crime: theft, guard report and chest use match the VM");
  }

void moveMobAndSurprisePerceptions(World& world,Npc& npc,Npc& hero) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=NpcDefinitionContract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto moveMob=script.findFunction("B_MOVEMOB");
  const auto surprise=script.findFunction("B_ASSESSSURPRISE");
  auto& scratch=*vm.find_symbol_by_name("B_MOVEMOB.DOOR");
  const std::string savedScratch(scratch.get_string());
  const auto savedPos=npc.position();
  auto* savedMob=npc.detectedMob();
  auto* savedTarget=npc.target();
  auto& reason=npc.handle().aivar[9];
  const auto savedReason=reason;
  Interactive* door=nullptr;
  for(uint32_t id=0; auto* candidate=world.mobsiById(id); ++id) {
    if(candidate->schemeName()!="DOOR" || candidate->stateId()>0 || candidate->stateCount()<1)
      continue;
    npc.setPosition(candidate->displayPosition());
    if(world.availableMob(npc,"DOOR")==candidate) {
      door=candidate;
      break;
      }
    }
  require(door!=nullptr,"move mob: a closed door can be found from the NPC position");
  const int savedDoorState=door->stateId();
  for(const int state : {savedDoorState,1}) {
    door->setMobState("DOOR",state);
    require(door->stateId()==state,"move mob: door state was applied to the world");
    for(auto* detectedMob : {static_cast<Interactive*>(nullptr),door}) {
      const bool openDoor=detectedMob!=nullptr && state<=0;
      std::vector<std::string> original;
      for(bool native : {false,true}) {
        enabled=native;
        npc.setDetectedMob(detectedMob);
        npc.clearAiQueue();
        npc.aiPush(AiQueue::aiWait(173));
        size_t instructions=0;
        vm.observe_instructions([&](const auto&) { ++instructions; });
        script.invokeState(&npc,&hero,nullptr,moveMob);
        vm.observe_instructions({});
        const auto* next=npc.aiQueueFront();
        require((instructions==0)==native && scratch.get_string()==(detectedMob ? "DOOR" : "") &&
                next && next->act==(openDoor ? AI_UseMob : AI_Wait),
                "move mob: a closed door replaces the queue and each route records the same scheme");
        auto queue=activityQueue(npc);
        require(queue.size()==(detectedMob==nullptr || openDoor ? 2u : 1u),
                "move mob: an open door retains the queue and no door appends continuation");
        if(native) require(original==queue,"move mob: both routes preserve action order and arguments");
        else original=std::move(queue);
        }
      }
    }
  for(bool native : {false,true}) {
    enabled=native;
    reason=-1;
    npc.setTarget(nullptr);
    size_t instructions=0;
    vm.observe_instructions([&](const auto&) { ++instructions; });
    script.invokeState(&npc,&hero,nullptr,surprise);
    vm.observe_instructions({});
    require((instructions==0)==native && npc.target()==&hero && reason==script.intSymbol("AR_GUILDENEMY",-1),
            "surprise: both routes keep the new target and its guild enemy reason");
    }
  door->setMobState("DOOR",savedDoorState);
  npc.setPosition(savedPos);
  npc.setDetectedMob(savedMob);
  npc.setTarget(savedTarget);
  npc.clearAiQueue();
  reason=savedReason;
  scratch.set_string(savedScratch);
  enabled=wasEnabled;
  std::puts("Human perceptions: door use and combat surprise agree with the VM");
  }

void fightWatchPerceptions(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto watch=script.findFunction("ZS_WATCHFIGHT");
  const auto defeatPc=vm.find_symbol_by_name("B_ASSESSDEFEAT")->address();
  const auto cheerPc=vm.find_symbol_by_name("B_CHEERFIGHT")->address();
  const auto crime=script.intSymbol("CRIME_ATTACK",-1);
  auto& variation=*vm.find_symbol_by_name("B_CHEERFIGHT.ZUFALL");
  auto* lobart=activityNpc(world,"BAU_950_LOBART");
  require(lobart && script.guildAttitude(npc,*lobart)==ATT_FRIENDLY,
          "fight witnesses: the spectator and victim belong to allied guilds");
  const auto originalPosition=lobart->position();
  lobart->setPosition(npc.position()+Tempest::Vec3(450,0,0));
  lobart->updateTransform();
  auto& aivar=npc.handle().aivar;
  const auto savedAivar=std::to_array(aivar);
  const auto savedTime=npc.stateTime();
  const auto savedAttitude=npc.attitude();
  const auto savedVariation=variation.get_int();
  const auto savedReason=lobart->handle().aivar[9];
  const auto outputOrder=Contract::outputOrder(script);
  const auto originalRandom=Contract::random(script);
  lobart->handle().aivar[9]=script.intSymbol("AR_REACTTODAMAGE",-1);

  enum class Site { Complaint, Praise, BeforeCheer, QuietWatcher, Concern, Cheer };
  for(const auto site:{Site::Complaint,Site::Praise,Site::BeforeCheer,Site::QuietWatcher,Site::Concern,Site::Cheer}) {
    const bool defeat=site==Site::Complaint || site==Site::Praise;
    const bool reacts=site==Site::Concern || site==Site::Cheer;
    const bool likesHero=site==Site::Praise || site==Site::Cheer;
    const bool tough=site!=Site::Complaint && site!=Site::QuietWatcher;
    const int time=site==Site::BeforeCheer ? 2999 : 3000;
    const auto pc=defeat ? defeatPc : cheerPc;
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      enabled=native;
      Contract::outputOrder(script)=outputOrder;
      std::copy(savedAivar.begin(),savedAivar.end(),aivar);
      aivar[1]=0;
      aivar[57]=tough ? 1 : 0;
      npc.setAttitude(likesHero ? ATT_FRIENDLY : ATT_NEUTRAL);
      npc.clearAiQueue();
      script.invokeState(&npc,&hero,lobart,watch);
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(173));
      npc.setStateTime(time);
      script.setSeed(73106);
      auto random=Contract::random(script);
      const auto expectedRoll=reacts ? int32_t(random()%3) : -1;
      variation.set_int(-1);
      size_t bytecode=0;
      vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==pc; });
      const bool delivered=npc.perceptionProcess(hero,lobart,npc.qDistTo(*lobart),
                                                   defeat ? PERC_ASSESSDEFEAT : PERC_ASSESSOTHERSDAMAGE);
      vm.observe_instructions({});
      require(delivered && bytecode==(native ? 0u : 1u) && variation.get_int()==expectedRoll &&
              Contract::random(script)==random,"fight witnesses: exact callback dispatch and random draw");
      auto queue=Contract::queue(npc);
      if(defeat) {
        require(queue.size()==1 && aivar[1]==(likesHero ? 0 : crime),
                "fight witnesses: defeat replaces the old queue and records the appropriate crime");
        const auto speech=queue.pop();
        require(speech.act==AI_OutputSvm && speech.target==&hero && speech.s0==(likesHero ? "$GOODVICTORY" : "$OHMYGODHESDOWN"),
                "fight witnesses: defeat preserves the original praise or complaint");
        } else {
        require(npc.stateTime()==(site==Site::BeforeCheer ? 2999u : 0u) && aivar[1]==0 &&
                queue.size()==(reacts ? 3u : 1u) && queue.pop().act==AI_Wait,
                "fight witnesses: the timer resets after the threshold and the original AI wait remains");
        if(reacts) {
          const auto speech=queue.pop();
          const auto animation=queue.pop();
          const auto expectedLine=std::string(likesHero ? "$CHEERFRIEND0" : "$OOH0")+std::to_string(expectedRoll+1);
          require(speech.act==AI_OutputSvmOverlay && speech.target==&npc && speech.s0==expectedLine &&
                  animation.act==AI_PlayAnim && animation.s0==(likesHero ? "T_WATCHFIGHT_YEAH" : "T_WATCHFIGHT_OHNO"),
                  "fight witnesses: the same roll chooses the response and retains the reaction animation");
          }
        }
      auto actions=activityQueue(npc);
      if(native) require(original==actions,"fight witnesses: action queues match the VM");
      else original=std::move(actions);
      }
    }
  enabled=wasEnabled;
  std::copy(savedAivar.begin(),savedAivar.end(),aivar);
  npc.setStateTime(int64_t(savedTime));
  npc.setAttitude(savedAttitude);
  variation.set_int(savedVariation);
  lobart->handle().aivar[9]=savedReason;
  lobart->setPosition(originalPosition);
  lobart->updateTransform();
  Contract::random(script)=originalRandom;
  std::puts("Fight witnesses: defeat and cheering match the VM");
  }

#include "npc_murder_world_fixture.h"

void humanPerceptionPresets(World& world,Npc& npc) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto savedPerceptions=std::to_array(Contract::perceptions(npc));
  const auto savedTime=Contract::perceptionTime(npc);
  const auto savedSenses=npc.handle().senses;
  const auto savedRange=npc.handle().senses_range;
  auto& vmSelf=*vm.global_self();
  const auto savedSelf=vmSelf.get_instance();
  vmSelf.set_instance(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()));
  for(const auto* name : {"PERCEPTION_SET_NORMAL","PERCEPTION_SET_MINIMAL"}) {
    const auto* fn=script.findSymbol(name);
    std::array<ScriptFn,PERC_Count> original;
    uint64_t originalTime=0;
    for(bool native : {false,true}) {
      enabled=native;
      npc.handle().senses=0;
      npc.handle().senses_range=0;
      npc.setPerceptionTime(777);
      for(uint8_t slot=1; slot<PERC_Count; ++slot)
        npc.setPerceptionDisable(PercType(slot));
      size_t instructions=0;
      vm.observe_instructions([&](const auto&) { ++instructions; });
      vm.unsafe_call_bl(fn);
      vm.observe_instructions({});
      require((instructions==0)==native && npc.handle().senses==int32_t(SensesBit::SENSE_SEE | SensesBit::SENSE_HEAR) &&
              npc.handle().senses_range==script.intSymbol("PERC_DIST_ACTIVE_MAX",0),
              "human presets: both executors set the original senses and only legacy reads bytecode");
      std::array<ScriptFn,PERC_Count> actual;
      for(uint8_t slot=0; slot<PERC_Count; ++slot)
        actual[slot]=Contract::perception(npc,PercType(slot));
      if(native) require(actual==original && Contract::perceptionTime(npc)==originalTime,
                         "human presets: all callback symbols and perception timing match the VM");
      else {
        original=actual;
        originalTime=Contract::perceptionTime(npc);
        }
      }
    }
  vmSelf.set_instance(savedSelf);
  std::copy(savedPerceptions.begin(),savedPerceptions.end(),Contract::perceptions(npc));
  npc.setPerceptionTime(savedTime);
  npc.handle().senses=savedSenses;
  npc.handle().senses_range=savedRange;
  enabled=wasEnabled;
  }

void quietSoundRooms(Gothic& gothic) {
  using Contract=NpcDefinitionContract;
  ActivityEventsWorld test(gothic);
  auto& world=test.world();
  auto& script=world.script();
  auto* npc=activityNpc(world,"BAU_951_HILDA");
  const auto* post=world.findWayPoint("NW_FARM1_INHOUSE_02");
  const auto* otherRoom=world.findWayPoint("NW_FARM1_INHOUSE_01");
  const auto* beyond=world.findWayPoint("NW_FARM1_ENTRANCE_01");
  require(npc && post && otherRoom && beyond,"quiet sound: Lobart's house needs two rooms and an exit fixture");
  // Movement caches the ground ray's portal. The BSP room alone also contains untagged furniture surfaces.
  constexpr std::string_view portal="S:FARM01_NW_CITY_BODENDIELEN_01";
  const auto at=post->groundPos;
  std::optional<Tempest::Vec3> inside;
  for(const auto offset : {Tempest::Vec3(-180,0,0),Tempest::Vec3(180,0,0),Tempest::Vec3(0,0,180),Tempest::Vec3(0,0,-180),
                           Tempest::Vec3(-180,0,180),Tempest::Vec3(-180,0,-180),Tempest::Vec3(180,0,180),Tempest::Vec3(180,0,-180)}) {
    const auto floor=world.physic()->landRay(at+offset+Tempest::Vec3(0,40,0),120);
    const Tempest::Vec3 eyes(0,160,0);
    if(floor.sector!=nullptr && std::string_view(floor.sector)==portal &&
       !world.physic()->ray(at+eyes,floor.v+eyes).hasCol) {
      inside=floor.v;
      break;
      }
    }
  require(inside.has_value(),"quiet sound: the guest needs a clear floor position on the listener's portal");
  auto& hero=*world.player();
  auto& events=Contract::activityEventsOn(script);
  const bool enabled=events;
  auto& vm=script.getVm();
  const auto observe=script.findFunction("ZS_OBSERVEPLAYER");
  const auto clear=script.findFunction("ZS_CLEARROOM");
  const auto roomPc=vm.find_symbol_by_name("B_ASSESSENTERROOM")->address();
  const auto portalCollision=script.findFunction("B_ASSESSPORTALCOLLISION");
  const auto portalPc=vm.find_symbol_by_index(portalCollision.ptr)->address();
  auto& leftRoomComment=*vm.find_symbol_by_name("PLAYER_LEFTROOMCOMMENT");
  const int32_t savedComment=leftRoomComment.get_int();
  const auto setPrivateRoom=[&](bool privateRoom) {
    const auto guild=privateRoom ? int32_t(npc->handle().guild) : int32_t(GIL_PUBLIC);
    world.assignRoomToGuild("FARM01",guild);
    require(world.guildOfRoom(npc->portalName())==guild &&
            (!privateRoom || (guild>int32_t(GIL_NONE) && guild!=int32_t(GIL_PUBLIC) && world.guildOfRoom(hero.portalName())==guild)),
            "enter room: the fixture changes the guild of the occupied portal");
    };
  EventActor activity;
  activity.npc=npc;
  activity.state="ZS_STAND_WP";
  activity.phases=activityPhases(script,activity.state.c_str());
  activity.pointName=post->name;
  const auto stand=ScriptFn(activity.phases.init);
  npc->clearRoutine();
  npc->addRoutine(gtime(0,0),gtime(23,59),uint32_t(activity.phases.init),activity.pointName);
  const auto prepare=[&](bool sameRoom) {
    test.only({});
    test.stop(*npc);
    npc->clearGoTo();
    test.moveHero(sameRoom ? *inside : otherRoom->groundPos,0);
    test.start(activity,post->groundPos,npc,false);
    npc->setAttitude(ATT_NEUTRAL);
    Contract::tickRoutine(*npc);
    for(size_t i=0;i<PERC_Count;++i)
      if(i!=PERC_ASSESSQUIETSOUND) npc->setPerceptionDisable(PercType(i));
    test.only({npc,&hero});
    try {
      test.stepUntil(120,[&] {
        return world.guildOfRoom(npc->portalName())==int32_t(GIL_PUBLIC) &&
               ((npc->portalName()==hero.portalName())==sameRoom) && npc->isAiQueueEmpty();
        },"quiet sound: the listener and source settle in the requested rooms");
      }
    catch(const std::runtime_error& error) {
      const auto where=[&](Npc& actor) {
        const auto p=actor.position();
        return std::string("portal='")+std::string(actor.portalName())+"' room='"+std::string(world.roomAt(p))+"' guild="+
               std::to_string(world.guildOfRoom(actor.portalName()))+" body="+std::to_string(actor.bodyStateMasked())+
               " xyz="+std::to_string(p.x)+","+std::to_string(p.y)+","+std::to_string(p.z);
        };
      const auto* action=npc->aiQueueFront();
      throw std::runtime_error(std::string(error.what())+" at "+(sameRoom ? "the interior offset" : otherRoom->name)+": listener "+where(*npc)+" state="+
                               std::string(script.symbolName(Contract::aiState(*npc).funcIni.ptr))+" queue="+
                               (action ? std::string(AiQueue::actionName(action->act)) : "empty")+"; hero "+where(hero)+
                               " distance="+std::to_string(std::sqrt(npc->qDistTo(hero))));
      }
    npc->setDirection(hero.position()-npc->position());
    npc->updateTransform();
    if(sameRoom) test.step();
    npc->setStateTime(0);
    require(npc->isInState(stand) && npc->hasPerc(PERC_ASSESSQUIETSOUND) && npc->qDistTo(hero)<1000.f*1000.f,
            "quiet sound: the standing listener is close enough to check the room");
    if(sameRoom) require(npc->canSeeNpc(hero,false),"quiet sound: the listener sees the guest in his portal");
    };
  struct Site { bool sameRoom; bool privateRoom; bool friendly; int height; const char* label; };
  for(const Site site : {Site{true,false,false,0,"same room"},Site{false,false,false,0,"separate room"},
                         Site{true,false,false,251,"indoor height"},Site{true,true,false,0,"private room"},
                         Site{true,true,true,0,"friendly private room"}}) {
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      prepare(site.sameRoom);
      if(site.privateRoom) {
        setPrivateRoom(true);
        leftRoomComment.set_int(1);
        }
      if(site.friendly) {
        npc->setAttitude(ATT_FRIENDLY);
        npc->aiPush(AiQueue::aiLookAtNpc(&hero));
        Contract::nextAiAction(*npc);
        require(npc->canSeeSource(),"quiet sound: the friendly listener has a visible source");
        npc->aiPush(AiQueue::aiWait(173));
        }
      if(site.height!=0) {
        auto position=hero.position();
        position.y=std::nextafter(npc->position().y+float(site.height),std::numeric_limits<float>::infinity());
        test.moveHero(position,hero.rotation());
        require(int32_t(std::abs(hero.position().y-npc->position().y))==site.height &&
                world.guildOfRoom(hero.portalName())==int32_t(GIL_PUBLIC),
                "quiet sound: the elevated source retains its indoors portal for the height gate");
        }
      if(!site.sameRoom) {
        // A hostile listener with no line of sight reaches the quiet handler's own transition.
        npc->setAttitude(ATT_HOSTILE);
        npc->aiPush(AiQueue::aiStopLookAt());
        Contract::nextAiAction(*npc);
        require(!npc->canSeeSource(),"quiet sound: the outside source is unseen");
        npc->aiPush(AiQueue::aiWait(173));
        }
      auto& fakeBanditActor=*vm.find_symbol_by_name("C_PLAYERISFAKEBANDIT.SLF");
      fakeBanditActor.set_instance(nullptr);
      auto& checkedGuild=*vm.find_symbol_by_name("C_NPCISBOTHEREDBYPLAYERROOMGUILD.PORTALGUILD");
      checkedGuild.set_int(-123);
      size_t quietCalls=0,roomCalls=0,fakeBanditCalls=0,clearCalls=0;
      const auto observer=vm.add_call_observer([&](const auto& symbol) {
        quietCalls+=symbol.name()=="B_ASSESSQUIETSOUND";
        roomCalls+=symbol.name()=="B_ASSESSENTERROOM";
        fakeBanditCalls+=symbol.name()=="C_PLAYERISFAKEBANDIT";
        clearCalls+=symbol.name()=="B_CLEARPERCEPTIONS";
        });
      events=native;
      const bool heard=npc->perceptionProcess(hero,&hero,npc->qDistTo(hero),PERC_ASSESSQUIETSOUND);
      events=enabled;
      vm.remove_call_observer(observer);
      require(heard && quietCalls==(native ? 0u : 1u) && roomCalls==(!native && site.height==0 ? 1u : 0u),
              "quiet sound: ordinary perception preserves the indoor height gate in both executors");
      if(site.height==0 && !site.friendly) {
        const auto* queued=npc->aiQueueFront();
        require(queued && queued->act==AI_StartState && queued->func==(site.privateRoom ? clear : observe) &&
                queued->target==&hero && queued->i0==1 && !npc->hasPerc(PERC_ASSESSQUIETSOUND) &&
                clearCalls==size_t(!native) && fakeBanditCalls==size_t(!native && !site.sameRoom),
                "quiet sound: the room or unseen source queues its transition and clears perceptions");
        if(site.privateRoom)
          require(checkedGuild.get_int()==int32_t(npc->handle().guild),
                  "enter room: both guild checks remember the private room's owner");
        if(!site.sameRoom)
          require(fakeBanditActor.get_instance()==std::static_pointer_cast<daedalus::INpc>(npc->handlePtr()),
                  "quiet sound: the original fake-bandit helper and the native check store the same listener");
        }
      if(site.friendly) {
        const auto* queued=npc->aiQueueFront();
        require(queued && queued->act==AI_Wait && npc->hasPerc(PERC_ASSESSQUIETSOUND) && clearCalls==0,
                "quiet sound: the friendly listener keeps the queue and perception");
        }
      if(site.height!=0) require(npc->isAiQueueEmpty(),"quiet sound: the other floor leaves the original activity in place");
      const auto queue=activityQueue(*npc);
      if(native) require(queue==original,"quiet sound: the original and native handlers leave the same room reaction queue");
      else original=queue;
      if(site.privateRoom) {
        require(leftRoomComment.get_int()==0,"enter room: the private-room comment resets before rejecting a friendly listener");
        leftRoomComment.set_int(savedComment);
        setPrivateRoom(false);
        }
      }
    std::printf("%s %s: ordinary quiet perception preserves the stock queue (%zu actions)\n",
                site.privateRoom ? "D5.3" : "D5.1",site.label,original.size());
    }

  prepare(true);
  observePlayerPhases(world,*npc,hero,false);
  humanPerceptionPresets(world,*npc);
  drawWeaponPerceptions(world,*npc,hero);
  warnPerception(world,*npc,hero);
  companionPlayerPerceptions(world,*npc,hero);
  fightSoundPerceptions(world,*npc,hero);
  damagePerceptions(world,*npc,hero);
  npcPetzCrimeParity(world,*npc,hero,events);
  humanAttackPhases(world,*npc,hero);
  fleeAndWatchPhases(world,*npc,hero);
  humanPlunderAndArmorPhases(world,*npc,hero);
  controlSpellPhases(world,*npc,hero);
  enemyPerceptions(world,*npc,hero);
  playerAssessmentRoutes(world,*npc,hero);
  propertyCrimePerceptions(world,*npc,hero);
  moveMobAndSurprisePerceptions(world,*npc,hero);
  fightWatchPerceptions(world,*npc,hero);
  murderPerceptions(world,*npc,hero);
  for(bool privateRoom:{false,true}) {
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      prepare(true);
      if(privateRoom) setPrivateRoom(true);
      leftRoomComment.set_int(1);
      npc->setPerceptionEnable(PERC_ASSESSENTERROOM,portalCollision.ptr);
      npc->aiPush(AiQueue::aiWait(173));
      size_t portalBytecode=0,roomBytecode=0;
      vm.observe_instructions([&](const auto&) {
        portalBytecode+=vm.pc()==portalPc;
        roomBytecode+=vm.pc()==roomPc;
        });
      events=native;
      const bool delivered=npc->perceptionProcess(hero,&hero,npc->qDistTo(hero),PERC_ASSESSENTERROOM);
      events=enabled;
      vm.observe_instructions({});
      require(delivered && portalBytecode==(native ? 0u : 1u) && roomBytecode==portalBytecode,
              "enter room: the callback and the entering reaction execute bytecode only on the legacy path");
      const auto* queued=npc->aiQueueFront();
      require(queued && queued->act==AI_StartState && queued->func==(privateRoom ? clear : observe) &&
              queued->target==&hero && queued->i0==1 &&
              !npc->hasPerc(PERC_ASSESSENTERROOM) && !npc->hasPerc(PERC_ASSESSQUIETSOUND),
              "enter room: the room reaction replaces the old queue and clears perceptions");
      require(leftRoomComment.get_int()==(privateRoom ? 0 : 1),
              "enter room: only a private room resets the comment");
      const auto queue=activityQueue(*npc);
      if(native) require(queue==original,"enter room: original and native perceptions produce the same queue");
      else original=queue;
      if(!privateRoom) {
        Contract::nextAiAction(*npc);
        Contract::tickRoutine(*npc);
        require(npc->isInState(observe) && Contract::aiState(*npc).started && npc->hasPerc(PERC_ASSESSTHEFT),
                "observation: the room reaction starts and restores perceptions");
        }
      if(privateRoom) setPrivateRoom(false);
      leftRoomComment.set_int(savedComment);
      }
    std::printf("D5.3 %s entry: perception, action queue and room comment match the VM\n",privateRoom ? "private" : "public");
    }

  std::vector<std::string> originalDeparture;
  for(bool native:{false,true}) {
    prepare(true);
    const auto heroGuild=hero.handle().guild;
    const auto seenLeft=npc->handle().aivar[21];
    const auto leaveLanding=otherRoom->groundPos;
    test.only({&hero});
    test.moveHero(leaveLanding,hero.rotation());
    test.stepUntil(120,[&] { return hero.portalName()!=portal && world.guildOfRoom(hero.portalName())>=0; },
                   "portal departure: the hero finds the other room's tagged floor");
    const std::string formerPortal(hero.portalName());
    const auto formerGuild=world.guildOfRoom(formerPortal);
    const auto separator=formerPortal.find('_');
    require(formerPortal.starts_with("S:") && separator!=std::string::npos && separator>2,
            "portal departure: the fixture needs a named former room");
    const std::string_view formerRoom(formerPortal.data()+2,separator-2);
    world.assignRoomToGuild(formerRoom,int32_t(npc->handle().guild));
    test.moveHero(*inside,hero.rotation());
    test.stepUntil(120,[&] { return hero.portalName()==portal && hero.formerPortalName()==formerPortal; },
                   "portal departure: crossing back preserves the actual former room");
    test.only({npc,&hero});
    npc->setDirection(hero.position()-npc->position());
    npc->updateTransform();
    // The entrance policy considers personal attitude; the departure policy uses guild attitude.
    npc->setAttitude(ATT_FRIENDLY);
    hero.handle().guild=GIL_BDT;
    npc->handle().aivar[21]=0;
    npc->setPerceptionEnable(PERC_ASSESSENTERROOM,portalCollision.ptr);
    size_t portalBytecode=0;
    vm.observe_instructions([&](const auto&) { portalBytecode+=vm.pc()==portalPc; });
    events=native;
    const bool delivered=npc->perceptionProcess(hero,&hero,npc->qDistTo(hero),PERC_ASSESSENTERROOM);
    events=enabled;
    vm.observe_instructions({});
    const auto* queued=npc->aiQueueFront();
    require(delivered && portalBytecode==(native ? 0u : 1u) && npc->handle().aivar[21]==1 &&
            queued && queued->act==AI_StartState && queued->func==observe && queued->target==&hero && queued->i0==0 &&
            !npc->hasPerc(PERC_ASSESSENTERROOM),
            "portal departure: a witness of the hero leaving its guild's room records it and observes");
    if(native) require(activityQueue(*npc)==originalDeparture,"portal departure: original and native commands match");
    else originalDeparture=activityQueue(*npc);
    world.assignRoomToGuild(formerRoom,formerGuild);
    hero.handle().guild=heroGuild;
    npc->handle().aivar[21]=seenLeft;
    }
  std::puts("D5.5 former-room witness and observation queue match the VM");

  const auto roomPhases=activityPhases(script,"ZS_CLEARROOM");
  const auto roomExit=script.findFunction("B_EXITIFROOMLEFT");
  const auto roomTalk=script.findFunction("B_CLEARROOMTALK");
  const auto roomTalkPc=vm.find_symbol_by_index(roomTalk.ptr)->address();
  const auto attack=script.findFunction("ZS_ATTACK");
  const auto attackReason=vm.find_symbol_by_name("AR_CLEARROOM")->get_int();
  const auto attackPc=vm.find_symbol_by_name("B_ATTACK")->address();
  const auto roomCode=std::array{vm.find_symbol_by_index(roomPhases.init)->address(),
                                 vm.find_symbol_by_index(roomPhases.loop)->address(),
                                 vm.find_symbol_by_index(roomPhases.end)->address(),
                                 vm.find_symbol_by_index(roomExit.ptr)->address()};
  std::vector<uint8_t> roomSave;
  std::vector<std::string> roomPending;
  const auto saysTo=[](const Npc& speaker,const Npc& target,std::string_view line) {
    auto queue=Contract::queue(speaker);
    while(queue.size()!=0) {
      const auto action=queue.pop();
      if(action.act==AI_OutputSvm && action.target==&target && action.s0==line) return true;
      }
    return false;
    };
  const auto depart=otherRoom->groundPos;
  for(bool hostile:{false,true}) {
    std::vector<std::string> originalInit,originalAttack;
    for(bool native:{false,true}) {
      prepare(true);
      const auto guild=npc->handle().guild;
      const auto aivar=std::to_array(npc->handle().aivar);
      if(hostile) npc->handle().guild=GIL_MIL;
      setPrivateRoom(true);
      events=native;
      npc->setPerceptionEnable(PERC_ASSESSENTERROOM,portalCollision.ptr);
      require(npc->perceptionProcess(hero,&hero,npc->qDistTo(hero),PERC_ASSESSENTERROOM) &&
              npc->aiQueueFront() && npc->aiQueueFront()->func==clear,
              "clear room: the stock portal event starts the private-room reaction");
      std::array<size_t,4> vmEntries{};
      size_t roomTalkBytecode=0, attackBytecode=0;
      vm.observe_instructions([&](const auto&) {
        for(size_t i=0;i<roomCode.size();++i)
          vmEntries[i]+=vm.pc()==roomCode[i];
        roomTalkBytecode+=vm.pc()==roomTalkPc;
        attackBytecode+=vm.pc()==attackPc;
        });
      Contract::nextAiAction(*npc);
      Contract::tickRoutine(*npc);
      require(npc->isInState(clear) && Contract::aiState(*npc).started && npc->stateOther()==&hero &&
              npc->handle().aivar[19]==1 && npc->hasPerc(PERC_ASSESSENTERROOM) &&
              npc->hasPerc(PERC_MOVEMOB) && npc->hasPerc(PERC_ASSESSTALK),
              "clear room: Init keeps the guest and installs the stock perceptions");
      if(native) require(activityQueue(*npc)==originalInit,"clear room: the initial commands match the VM");
      else originalInit=activityQueue(*npc);

      npc->clearAiQueue();
      require(npc->perceptionProcess(hero,&hero,npc->qDistTo(hero),PERC_ASSESSTALK) &&
              roomTalkBytecode==(native ? 0u : 1u) &&
              saysTo(*npc,hero,hostile ? "$GETOUTOFHERE" : "$WHYAREYOUINHERE"),
              "clear room: addressing the owner repeats the appropriate warning through the selected callback");
      if(!hostile) {
        const auto day=int(world.time().day());
        const auto crime=vm.find_symbol_by_name("CRIME_ATTACK")->get_int();
        const auto level=vm.call_function<int32_t>("B_GETCURRENTABSOLUTIONLEVEL",std::static_pointer_cast<daedalus::INpc>(npc->handlePtr()));
        npc->handle().aivar[1]=crime;
        npc->handle().aivar[46]=level;
        for(const auto age : {2,3}) {
          npc->handle().aivar[2]=day-age;
          npc->clearAiQueue();
          require(npc->perceptionProcess(hero,&hero,npc->qDistTo(hero),PERC_ASSESSTALK) &&
                  saysTo(*npc,hero,age==2 ? "$GETOUTOFHERE" : "$WHYAREYOUINHERE"),
                  "clear room: an attack is remembered for exactly two whole days");
          }
        npc->handle().aivar[1]=aivar[1];
        npc->handle().aivar[2]=aivar[2];
        npc->handle().aivar[46]=aivar[46];
        }
      const auto loop=[&] { return script.invokeState(npc,&hero,&hero,ScriptFn(roomPhases.loop)); };
      npc->clearAiQueue();
      npc->setStateTime(2600);
      require(loop()==LOOP_CONTINUE && npc->handle().aivar[19]==0 && npc->stateTime()==0 &&
              saysTo(*npc,hero,hostile ? "$GETOUTOFHERE" : "$WHYAREYOUINHERE"),
              "clear room: the first Loop warns and resets the timer once");
      if(hostile) {
        npc->clearAiQueue();
        npc->setStateTime(5999);
        require(loop()==LOOP_CONTINUE && npc->isAiQueueEmpty() && npc->stateTime()==5999,
                "clear room: the fifth whole second still allows the guest to leave");
        npc->setStateTime(6000);
        require(loop()==LOOP_END && npc->handle().aivar[9]==attackReason && npc->aiQueueFront() &&
                npc->aiQueueFront()->act==AI_StartState && npc->aiQueueFront()->func==attack &&
                npc->aiQueueFront()->target==&hero,"clear room: the sixth second queues the original attack reason");
        require(attackBytecode==(native ? 0u : 1u),"clear room: escalation uses the selected attack executor");
        if(native) require(activityQueue(*npc)==originalAttack,"clear room: the escalation queue matches the VM");
        else originalAttack=activityQueue(*npc);
        }
      else {
        npc->setStateTime(1250);
        if(native) {
          roomPending=activityQueue(*npc);
          Tempest::MemWriter writer(roomSave);
          Serialize out(writer);
          Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
          std::memset(preview.data(),0,4);
          gothic.gameSession()->save(out,"guest warned in private room",preview);
          }
        test.only({&hero});
        test.moveHero(depart,hero.rotation());
        test.stepUntil(120,[&] { return world.guildOfRoom(hero.portalName())!=int32_t(npc->handle().guild); },
                       "clear room: the guest moves from the private room to another portal");
        test.only({npc,&hero});
        require(npc->perceptionProcess(hero,&hero,npc->qDistTo(hero),PERC_ASSESSENTERROOM) &&
                saysTo(*npc,hero,"$WISEMOVE") && !saysTo(*npc,hero,"$WHYAREYOUINHERE"),
                "clear room: the registered departure callback replaces the warning with the exit line");
        auto queue=Contract::queue(*npc);
        require(queue.size()==3 && queue.pop().act==AI_StandUp && queue.pop().act==AI_OutputSvm &&
                queue.pop().act==AI_ContinueRoutine,"clear room: the departure queue resumes the routine after the exit line");
        }
      npc->clearAiQueue();
      npc->aiPush(AiQueue::aiLookAtNpc(&hero));
      Contract::nextAiAction(*npc);
      npc->clearState(false);
      require(npc->aiQueueFront() && npc->aiQueueFront()->act==AI_StopLookAt,
              "clear room: End stops the current look");
      vm.observe_instructions({});
      const auto expected=hostile ? std::array<size_t,4>{1,3,1,3} : std::array<size_t,4>{1,1,1,2};
      require(vmEntries==(native ? std::array<size_t,4>{} : expected),
              "clear room: Init, Loop, End and the exit callback use the selected executor");
      npc->handle().guild=guild;
      std::ranges::copy(aivar,std::begin(npc->handle().aivar));
      npc->setTarget(nullptr);
      setPrivateRoom(false);
      events=enabled;
      }
    std::printf("D5.4 %s clear-room warning and response match the VM\n",hostile ? "hostile" : "neutral");
    }

  prepare(true);
  PhysicsNoise impact;
  impact.culprit=&hero;
  impact.position=hero.position();
  size_t quietCalls=0;
  const auto observer=vm.add_call_observer([&](const auto& symbol) { quietCalls+=symbol.name()=="B_ASSESSQUIETSOUND"; });
  require(npc->hearPhysicsNoise(impact) && npc->isInState(observe) && Contract::aiState(*npc).started &&
          npc->hasPerc(PERC_ASSESSTHEFT) && quietCalls==0,
          "quiet sound: native impact starts observation immediately and restores perceptions");
  vm.remove_call_observer(observer);
  npc->setStateTime(750);
  const auto pending=activityQueue(*npc);
  const auto delay=npc->handle().aivar[68];
  const auto exit=beyond->groundPos;
  std::vector<uint8_t> save;
  {
    Tempest::MemWriter writer(save);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"quiet sound during observation",preview);
  }
  gothic.clearGame().reset();
  {
    Tempest::MemReader reader(save);
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
  }
  ActivityEventsWorld loaded(gothic);
  auto& restored=loaded.world();
  auto* listener=activityNpc(restored,"BAU_951_HILDA");
  auto& guest=*restored.player();
  require(listener && listener->isInState(observe) && Contract::aiState(*listener).started &&
          listener->stateOther()==&guest && listener->stateTime()==750 && listener->handle().aivar[68]==delay &&
          activityQueue(*listener)==pending,"observation: ordinary load restores the target, elapsed time, draw and queue");
  loaded.only({listener,&guest});
  {
    ActivityStream stream(restored,*listener);
    auto& restoredVm=restored.script().getVm();
    size_t observationVmCalls=0;
    const auto countVm=restoredVm.add_call_observer([&](const auto& symbol) {
      observationVmCalls+=symbol.name().starts_with("ZS_OBSERVEPLAYER");
      });
    loaded.moveHero(exit,guest.rotation());
    loaded.stepUntil(240,[&] { return listener->isInState(stand) && Contract::aiState(*listener).started; },
                     "observation: the loaded listener returns to its original routine when the source leaves");
    restoredVm.remove_call_observer(countVm);
    require(observationVmCalls==0 && activityCount(stream.events,"phase ZS_OBSERVEPLAYER")==0 &&
            activityCount(stream.events,"phase ZS_OBSERVEPLAYER_LOOP")>0 &&
            activityCount(stream.events,"phase ZS_OBSERVEPLAYER_END")==1,
            "observation: the loaded episode continues through Loop and End without repeating Init");
  }
  std::puts("D5.2 physical sound: ordinary load resumed observation and returned to the standing routine");
  loaded.moveHero(*inside,guest.rotation());
  loaded.only({&guest});
  require(listener->playAnimByName("T_STAND_2_SIT",BS_SIT,true)!=nullptr,
          "observation: the seated witness can enter the stock sitting pose");
  loaded.stepUntil(120,[&] { return listener->animProgress("S_SIT")>=0; },"observation: the seated witness settles");
  observePlayerPhases(restored,*listener,guest,true);
  humanReactionPhases(restored,*listener,guest);
  humanAttackOutcomeHelpers(restored,*listener,guest);
  humanGuardCalls(restored,*listener,guest);

  gothic.clearGame().reset();
  {
    Tempest::MemReader reader(roomSave);
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
  }
  ActivityEventsWorld roomLoaded(gothic);
  auto& roomWorld=roomLoaded.world();
  auto* owner=activityNpc(roomWorld,"BAU_951_HILDA");
  auto& intruder=*roomWorld.player();
  require(owner && owner->isInState(clear) && Contract::aiState(*owner).started && owner->stateOther()==&intruder &&
          owner->stateTime()==1250 && owner->handle().aivar[19]==0 && owner->hasPerc(PERC_ASSESSENTERROOM) &&
          roomWorld.guildOfRoom(intruder.portalName())==int32_t(owner->handle().guild) && activityQueue(*owner)==roomPending,
          "clear room: ordinary load restores the private room, guest, timer and pending warning");
  roomLoaded.only({owner,&intruder});
  auto& roomVm=roomWorld.script().getVm();
  size_t roomVmCalls=0,initCalls=0,endCalls=0;
  {
    ActivityStream stream(roomWorld,*owner);
    const auto count=roomVm.add_call_observer([&](const auto& symbol) {
      roomVmCalls+=symbol.name().starts_with("ZS_CLEARROOM");
      });
    roomLoaded.moveHero(depart,intruder.rotation());
    roomLoaded.stepUntil(360,[&] { return owner->isInState(stand) && Contract::aiState(*owner).started; },
                         "clear room: the loaded listener returns to the standing routine after the guest leaves");
    roomVm.remove_call_observer(count);
    initCalls=activityCount(stream.events,"phase ZS_CLEARROOM");
    endCalls=activityCount(stream.events,"phase ZS_CLEARROOM_END");
  }
  require(roomVmCalls==0 && initCalls==0 && endCalls==1,
          "clear room: the loaded reaction ends through the engine without repeating Init");
  std::puts("D5.4 ordinary load restored the warning and finished the reaction after departure");
  }
