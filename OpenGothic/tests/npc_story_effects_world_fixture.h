// Live NPC dialogue and StoryGraph save/load cases for placed_items_contract.cpp.

#include "storyeffects.h"

void assessTalkEntry(World& world,Npc& npc,Npc& player) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& effects=Contract::activityEventsOn(script);
  const bool savedEffects=effects;
  const auto savedNpcQueue=Contract::queue(npc);
  const auto savedPlayerQueue=Contract::queue(player);
  const auto savedOutputOrder=Contract::outputOrder(script);
  const auto savedVars=std::to_array(npc.handle().aivar);
  const auto savedPerceptions=std::to_array(Contract::perceptions(npc));
  const auto savedState=Contract::aiState(npc);
  const auto savedDeadline=Contract::refusalDeadline(npc);
  const auto savedAttitude=npc.attitude();
  auto* savedOther=npc.stateOther();
  const auto savedVictim=vm.global_victim()->get_instance();
  const auto* callback=vm.find_symbol_by_name("B_ASSESSTALK");
  const auto refusal=vm.find_symbol_by_name("C_REFUSETALK")->address();
  const auto started=script.findFunction("ZS_TALK");
  const auto restore=[&] {
    Contract::queue(npc)=savedNpcQueue;
    Contract::queue(player)=savedPlayerQueue;
    Contract::outputOrder(script)=savedOutputOrder;
    std::copy(savedVars.begin(),savedVars.end(),npc.handle().aivar);
    std::copy(savedPerceptions.begin(),savedPerceptions.end(),Contract::perceptions(npc));
    Contract::aiState(npc)=savedState;
    Contract::refusalDeadline(npc)=savedDeadline;
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
        require(line && line->act==AI_OutputSvm && line->target==&player && line->s0=="$NOTNOW" &&
                line->i0==savedOutputOrder && Contract::outputOrder(script)==savedOutputOrder+1 &&
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

void nestedStockDialogue(World& world, Npc& npc, Npc& player) {
  auto& gothic=Gothic::inst();
  auto& script=world.script();
  auto& vm=script.getVm();
  auto* condition=vm.find_symbol_by_name("DIA_ADDON_CAVALORN_LETSKILLBANDITS_CONDITION");
  auto* helper=vm.find_symbol_by_name("C_BRAGOBANDITSDEAD");
  require(condition && helper,"nested dialogue needs the stock Cavalorn condition and bandit check");

  struct Restore {
    decltype(gothic.dialogScriptFn)& hook;
    decltype(gothic.dialogScriptFn) savedHook;
    std::optional<std::string> setting;
    ~Restore() {
      hook = std::move(savedHook);
      if(setting)
        setenv("GOTHIC_AI_STORYEXEC",setting->c_str(),1);
      else
        unsetenv("GOTHIC_AI_STORYEXEC");
      }
    };
  const auto* configured=std::getenv("GOTHIC_AI_STORYEXEC");
  Restore restore{gothic.dialogScriptFn,std::exchange(gothic.dialogScriptFn,{}),
                  configured ? std::optional<std::string>(configured) : std::nullopt};
  StoryRuntime::Bind context(vm,&npc,&player);
  size_t entries=0,instructions=0;
  vm.observe_instructions([&](const auto&) { ++instructions; entries+=vm.pc()==helper->address(); });

  setenv("GOTHIC_AI_STORYEXEC","0",1);
  const auto original=NpcDefinitionContract::dialogCondition(script,condition);
  require(entries==1 && instructions>0,"the compatibility switch must run the original dialogue bytecode");

  unsetenv("GOTHIC_AI_STORYEXEC");
  instructions=0;
  const auto stock=NpcDefinitionContract::dialogCondition(script,condition);
  require(stock==original && instructions==0,"the game session must resolve nested stock dialogue without a bridge");

  gothic.dialogScriptFn=[&](const auto& fn,int32_t& result) {
    if(&fn!=condition)
      return false;
    result=original+11;
    return true;
    };
  require(NpcDefinitionContract::dialogCondition(script,condition)==original+11 && instructions==0,
          "the director's replacement must take priority over the stock programme");
  vm.observe_instructions({});
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
  nestedStockDialogue(world,npc,player);
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
  talkTopicAssignments(world,npc,player);
  }

auto storyJournalBeforeSave(GameScript& script, bool nativeOnly) {
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
  stockQuestJournalEntry(script,note,nativeOnly);
  std::vector<std::string> entries={"first","second","stock journal helper"};
  if(!nativeOnly)
    entries.push_back("stock journal helper");
  const auto requireJournal=[journalStart,note,mission,entries=std::move(entries)](const QuestLog& journal) {
    require(journal.questCount()==journalStart+2,"StoryGraph journal must ignore absent and invalid effects");
    const auto& first=journal.quest(journalStart);
    const auto& second=journal.quest(journalStart+1);
    require(first.name==note && first.section==QuestLog::Note && first.status==QuestLog::Status::Success &&
            first.entry==entries,
            "StoryGraph journal must preserve note section, valid status and entry order");
    require(second.name==mission && second.section==QuestLog::Mission && second.status==QuestLog::Status::Failed &&
            second.entry.empty(),"a status before creation must create an ordered Mission");
    };
  requireJournal(script.questLog());
  return requireJournal;
  }

auto storyBeforeSave(World& world, bool nativeOnly) {
  using namespace storygraph::ex;
  auto& script=world.script();
  const auto requireJournal=storyJournalBeforeSave(script,nativeOnly);
  auto& vm=script.getVm();
  constexpr const char* itemName="ITMI_ORNAMENT_ADDON_VATRAS";
  const auto item=script.findSymbolIndex(itemName);
  auto* giver=activityNpc(world,"VLK_449_LARES");
  auto* player=world.player();
  auto* dialog=vm.find_symbol_by_name("DIA_ADDON_LARES_PATCH_INFO");
  auto* transfer=vm.find_symbol_by_name("B_GIVEINVITEMS");
  require(giver && player && dialog && transfer && item!=size_t(-1),"the transfer fixture needs Lares and the ornament dialog");

  static constexpr std::string_view directorName = "DIR_RUNTIME_SAVE_CONTRACT";
  static constexpr int32_t directorValue = -41;
  std::string directorError;
  auto* director = script.ownSymbols().addInt(vm,directorName,directorError);
  require(director!=nullptr,directorError.c_str());
  const auto directorId = director->index();
  StoryEffect setDirector;
  setDirector.kind = "set";
  setDirector.var = "dir_runtime_save_contract";
  setDirector.count = 42;
  setDirector.run(script,*giver,*player,"contract");
  require(director->get_int()==42,"a StoryGraph effect updates the legacy director symbol");
  director->set_int(directorValue);
  require(script.globalIntSymbol(setDirector.var)==directorValue,"a legacy write updates the director's owned value");

  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const bool savedEvents=events;
  if(nativeOnly)
    require(savedEvents && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off,
            "native stock contract requires activity records and events");
  const auto savedQueue=NpcDefinitionContract::queue(*giver);
  const auto savedItem=vm.global_item()->get_instance();
  StoryRuntime story(nullptr);
  StoryRuntime::Bind dialogContext(vm,giver,player);

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
  auto* amount=vm.find_symbol_by_name("B_GIVEINVITEMS.AMOUNT");
  auto* instance=vm.find_symbol_by_name("B_GIVEINVITEMS.ITEMINSTANCE");
  auto* from=vm.find_symbol_by_name("B_GIVEINVITEMS.GIVER");
  auto* to=vm.find_symbol_by_name("B_GIVEINVITEMS.TAKER");
  auto* name=vm.find_symbol_by_name("B_GIVEINVITEMS.ITEMNAME");
  auto* notice=vm.find_symbol_by_name("B_GIVEINVITEMS.CONCATTEXT");
  require(amount && instance && from && to && name && notice,"the transfer fixture needs all transfer variables");
  std::vector<std::string> original;
  std::string originalItemName,originalNotice;
  for(bool native:{false,true}) {
    if(nativeOnly && !native)
      continue;
    if(native && (!savedEvents || ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off))
      continue;
    events=native;
    NpcDefinitionContract::queue(*giver).clear();
    giver->delItem(item,uint32_t(giver->itemCount(item)));
    player->delItem(item,uint32_t(player->itemCount(item)));
    vm.global_item()->set_instance(savedItem);
    size_t bytecode=0,opcodes=0;
    bool countDialog=false;
    struct Observer {
      daedalus::DaedalusVm& vm;
      uint64_t id;
      ~Observer() { vm.remove_instruction_observer(id); }
      } observer{vm,vm.add_instruction_observer([&](const auto& machine,const auto&) {
        bytecode+=machine.pc()==transfer->address();
        opcodes+=countDialog;
        })};
    require(give(0)==1 && give(1)==0 && vm.global_item()->get_instance()==savedItem &&
            giver->itemCount(item)==0 && player->itemCount(item)==0 && giver->aiQueueFront()==nullptr,
            "zero and refused player hand-ins must leave both inventories, ITEM and the queue alone");
    player->addItem(item,1);
    require(give(1)==1 && giver->itemCount(item)==1 && player->itemCount(item)==0 &&
            vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(giver->getItem(item)->handlePtr()),
            "the hand-in must remove the ornament from the player and set ITEM to Lares's copy");

    // Leave the awarded item and the dialog's own variable writes in the saved world.
    giver->delItem(item,1);
    vm.global_item()->set_instance(savedItem);
    name->set_string("");
    notice->set_string("");
    if(native) {
      countDialog=true;
      int32_t result=0;
      bool failed=true;
      const bool handled=script.invokeStockProgram(*dialog,result,&failed);
      countDialog=false;
      require(handled && !failed && NpcDefinitionContract::hasPreparedStockProgram(script,*dialog) && opcodes==0,
              "the exact stock Lares dialog must execute from the prepared catalog without VM instructions");
      } else {
      vm.call_function<void>(dialog);
      }
    require(giver->itemCount(item)==0 && player->itemCount(item)==1 &&
            vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(player->getItem(item)->handlePtr()),
            "Lares's dialog must grant the requested ornament and set ITEM, even from an empty giver");
    require(amount->get_int()==1 && instance->get_int()==int32_t(item) &&
            from->get_instance()==std::static_pointer_cast<daedalus::INpc>(giver->handlePtr()) &&
            to->get_instance()==std::static_pointer_cast<daedalus::INpc>(player->handlePtr()) &&
            !name->get_string().empty() && !notice->get_string().empty(),
            "Lares's dialog must write the transfer arguments, item name and notification text");
    if(!native) {
      originalItemName=name->get_string();
      originalNotice=notice->get_string();
      } else if(!nativeOnly) {
      require(name->get_string()==originalItemName && notice->get_string()==originalNotice,
              "the prepared dialog must preserve the localized transfer variables");
      }
    require(bytecode==(native ? 0u : 4u),"the dialog and hand-in must use the selected transfer executor");
    auto queue=NpcDefinitionContract::queue(*giver);
    require(queue.size()==2,"each successful hero transfer must enqueue one notification on SELF");
    for(auto y:{"YPOS_ITEMGIVEN","YPOS_ITEMTAKEN"}) {
      const auto line=queue.pop();
      require(line.act==AI_PrintScreen && line.i0==-1 && line.i1==vm.find_symbol_by_name(y)->get_int() &&
              line.i2==2 && line.s1==vm.find_symbol_by_name("FONT_SCREENSMALL")->get_string(),
              "transfer notifications must keep their order, placement, duration and font");
      }
    if(!native)
      original=activityQueue(*giver);
    else if(!nativeOnly)
      require(activityQueue(*giver)==original,"the native notices must match the original localized text");
    }
  const auto itemText=name->get_string();
  vm.global_item()->set_instance(savedItem);
  NpcDefinitionContract::queue(*giver)=savedQueue;
  events=savedEvents;
  return [requireJournal,item,itemText,directorId](World& restored) {
    auto& script=restored.script();
    auto& vm=script.getVm();
    requireJournal(script.questLog());
    const auto* director = vm.find_symbol_by_name(directorName);
    require(director!=nullptr && director->index()==directorId && director->get_int()==directorValue &&
            script.globalIntSymbol(directorName)==directorValue,
            "an ordinary save restores the director variable and its original symbol ID");
    const auto* lares=activityNpc(restored,"VLK_449_LARES");
    const auto* hero=restored.player();
    const auto& variables=NpcDefinitionContract::transferVariables(script);
    require(lares && hero && lares->itemCount(item)==0 && hero->itemCount(item)==1 &&
            variables.amount==1 && variables.item==int32_t(item) && variables.itemName==itemText &&
            vm.find_symbol_by_name("B_GIVEINVITEMS.ITEMNAME")->get_string()==itemText &&
            vm.find_symbol_by_name("B_GIVEINVITEMS.AMOUNT")->get_int()==1 &&
            vm.find_symbol_by_name("B_GIVEINVITEMS.TAKER")->get_instance().get()==
              static_cast<const daedalus::INpc*>(hero->handlePtr().get()),
            "the transferred ornament and the script transfer locals must survive saving and loading");
    };
  }

auto storyAndPointRollsBeforeSave(World& world, bool nativeOnly) {
  const auto requireStory = storyBeforeSave(world,nativeOnly);
  static constexpr std::array names = {
    "ZS_STAND_GUARDING_LOOP.RANDOM", "ZS_STAND_ARMSCROSSED_LOOP.RANDOM", "ZS_STAND_WP_LOOP.RANDOM",
    "ZS_STAND_ARMSCROSSED_LOOP.EVENTRANDY", "B_EVENT_PORTAL_EARTHQUAKE.RANDY",
    };
  static constexpr std::array<int32_t,5> values = {5,2,7,1,3};
  const bool pointRollsOwned = world.script().usesVerifiedNpcProgram();
  if(pointRollsOwned)
    for(size_t i=0;i<names.size();++i)
      world.script().findSymbol(names[i])->set_int(values[i]);

  return [requireStory,pointRollsOwned](World& restored) {
    requireStory(restored);
    if(!pointRollsOwned)
      return;
    auto& script = restored.script();
    const auto& rolls = NpcDefinitionContract::pointGestureRolls(script);
    require(std::array{rolls.guard,rolls.crossedArms,rolls.waypoint,rolls.portal,rolls.earthquake}==values,
            "the ordinary save restores all five engine-owned point and earthquake rolls");
    for(size_t i=0;i<names.size();++i)
      require(script.findSymbol(names[i])->get_int()==values[i],
              "the restored point roll must be visible to the original script");
    };
  }
