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
        StockOracleActors context(vm,&npc,&player);
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
    ~Restore() { hook = std::move(savedHook); }
    } restore{gothic.dialogScriptFn,std::exchange(gothic.dialogScriptFn,{})};
  StockOracleActors context(vm,&npc,&player);
  size_t entries=0,instructions=0;
  vm.observe_instructions([&](const auto&) { ++instructions; entries+=vm.pc()==helper->address(); });

  const auto original=vm.call_function<int32_t>(condition);
  require(entries==1 && instructions>0,"the dialogue oracle must run the original nested bytecode");

  instructions=0;
  const auto stock=NpcDefinitionContract::dialogCondition(script,condition);
  require(stock==original && instructions==0,"the game session must resolve nested stock dialogue without a bridge");

  gothic.dialogScriptFn=[&](uint32_t fn,int32_t& result) {
    if(fn!=condition->index())
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
    const auto result=story.run(program,nullptr,nullptr);
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
  StockOracleActors dialogContext(vm,giver,player);

  storygraph::Program handIn;
  handIn.returns=true;
  auto& ret=handIn.steps.emplace_back();
  ret.kind=storygraph::Step::Return;
  ret.value=fn("B_GIVEINVITEMS",{inst("OTHER"),inst("SELF"),sym(itemName),param(0)});
  GameStoryExternals engine(script,true);
  storygraph::Interpreter interpreter(vm,engine);
  const auto give=[&](int32_t count) {
    if(nativeOnly) {
      const auto result=story.run(handIn,giver,player,false,{storygraph::NativeValue::integer(count)});
      require(result.ok,result.error.c_str());
      return result.returned.i;
      }
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
      const bool handled=script.invokeStockProgram(dialog->index(),result,&failed);
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

#if defined(GOTHIC_BENNET_LOOKUP_AUDIT)
void requireNativeQuestBase() {
  const auto& programs = baseProgramIndex().dir;
  require(std::filesystem::is_regular_file(programs/"programs.json"),"the native quest base must contain its program index");
  for(const auto& file : std::filesystem::directory_iterator(programs))
    require(file.path().filename()=="programs.json","the native quest base must contain no executable program payloads");

  for(const auto& section : std::filesystem::directory_iterator(programs.parent_path()/"definitions")) {
    if(!section.is_directory())
      continue;
    for(const auto* name : {"program-symbols.json","program-symbols.symbols","symbols.json"}) {
      const auto file = section.path()/name;
      require(!std::filesystem::exists(std::filesystem::symlink_status(file)),
              ("the native quest base must not contain " + file.string()).c_str());
      }
    }
  std::puts("Native quest base: no executable program payloads or full symbol files");
  }
#endif

void bennetOreContract(const std::filesystem::path& caseDir) {
#if !defined(GOTHIC_BENNET_LOOKUP_AUDIT)
  (void)caseDir;
  require(false,"Bennet native contract requires the full-symbol lookup guard");
#else
  using Contract = NpcDefinitionContract;
  using Counts = std::pair<size_t,size_t>;
  constexpr auto mission = "MIS_BENNET_BRINGORE";
  constexpr auto trained = "BENNET_TEACHSMITH";
  constexpr auto directorVar = "DIR_BENNET_SAVE_CONTRACT";
  constexpr auto directorNpc = "DIR_BENNET_WITNESS_CONTRACT";
  constexpr uint32_t startId = 23816, startFn = 23818, leadId = 23819, leadFn = 23821;
  constexpr uint32_t infoId = 23822, conditionFn = 23823, answerFn = 23824, ore = 6928;
  constexpr std::array starterLines = {"15_00", "06_07", "15_08", "06_09", "15_10", "06_11"};
  constexpr std::array leadLines = {"15_00", "06_01", "06_02", "06_03", "06_04"};
  constexpr std::array handInLines = {
    "DIA_Bennet_BringOre_15_00", "DIA_Bennet_BringOre_06_01",
    "DIA_Bennet_BringOre_06_02", "DIA_Bennet_BringOre_06_03"};
  constexpr std::array trainingLines = {"15_00", "06_12", "15_13", "06_14", "06_15", "06_16", "06_17"};
  require(caseDir.is_absolute(),"Bennet contract case directory must be absolute");
  const auto* game = std::getenv("GOTHIC_DIR");
  require(game && *game,"GOTHIC_DIR must point to prepared Gothic II settings");
  setenv("GOTHIC_AI","0",1);
  const char* args[] = {"gothic-placed-items-contract","-g",game};
  CommandLine commandLine(3,args);
  Resources resources;
  Gothic gothic;
  requireNativeQuestBase();
  gothic.isNpcInDialogFn = [](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  std::filesystem::create_directories(caseDir);

  {
    std::vector<uint8_t> oldBytes;
    {
      Tempest::MemWriter writer(oldBytes);
      Serialize out(writer);
      SaveGameHeader header;
      header.version=56;
      header.world="NEWWORLD.ZEN";
      out.setEntry("header");
      out.write(header);
    }
    Tempest::MemReader reader(oldBytes.data(),oldBytes.size());
    Serialize in(reader);
    bool rejected=false;
    try {
      GameSession legacy(in);
      }
    catch(const std::runtime_error& error) {
      rejected=std::string_view(error.what()).starts_with("Unsupported save version 56");
      }
    require(rejected && gothic.world()==nullptr,"the normal loader must reject v56 before creating a world");
    std::puts("Bennet save contract: v56 rejected before world creation");
  }

  const auto actors = [&] {
    auto& world=*gothic.world();
    auto* smith=activityNpc(world,"SLD_809_BENNET");
    require(world.player()!=nullptr && smith!=nullptr,"Bennet contract needs both the hero and Bennet");
    require(world.script().usesVerifiedNpcProgram() && world.script().getVm().size()==0,
            "Bennet contract requires prepared stock records and no bytecode");
    return std::pair{world.player(),smith};
    };
  const auto save = [&](const char* title, const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    {
      Tempest::MemWriter writer(bytes);
      Serialize out(writer);
      Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
      std::memset(preview.data(),0,4);
      gothic.gameSession()->save(out,title,preview);
    }
    Tempest::MemReader reader(bytes.data(),bytes.size());
    Serialize in(reader);
    SaveGameHeader header;
    require(in.setEntry("header"),"Bennet save must have the normal save header");
    in.read(header);
    require(header.version==57 && in.setEntry("game/bennet") && in.setEntry(ScriptVars::StateEntry) &&
            in.setEntry("game/director") && !in.setEntry("game/state") && !in.setEntry("game/daedalus") && !in.setEntry("game/symbols"),
            "Bennet save must use only the native v57 session layout");
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    require(bool(out),"Bennet contract could not write a new save");
    };
  const auto load = [&](const std::filesystem::path& path) {
    gothic.clearGame().reset();
    std::ifstream file(path,std::ios::binary);
    require(bool(file),"Bennet contract could not read its new save");
    const std::string bytes{std::istreambuf_iterator<char>{file},std::istreambuf_iterator<char>{}};
    Tempest::MemReader reader(bytes.data(),bytes.size());
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
    return actors();
    };
  const auto offer = [](Npc& hero, Npc& bennet, uint32_t scriptFn) -> std::optional<GameScript::DlgChoice> {
    const auto choices=hero.world().script().dialogChoices(hero.handlePtr(),bennet.handlePtr(),{},false);
    const auto found=std::find_if(choices.begin(),choices.end(),[=](const auto& c) { return c.scriptFn==scriptFn; });
    return found==choices.end() ? std::nullopt : std::optional<GameScript::DlgChoice>(*found);
    };
  const auto run = [](const GameScript::DlgChoice& choice, Npc& hero, Npc& bennet, const char* label) {
    auto& script=hero.world().script();
    auto& vm=script.getVm();
    const auto scopes=Contract::bennetLookupScopeCount();
    size_t opcodes=0;
    const auto observer=vm.add_instruction_observer([&](const auto&,const auto&) { ++opcodes; });
    script.exec(choice,hero,bennet);
    vm.remove_instruction_observer(observer);
    const auto reads=Contract::bennetLookupCount();
    require(Contract::bennetLookupScopeCount()>scopes && reads==0 && opcodes==0,
            "Bennet's normal route must run under the lookup guard without table reads or bytecode");
    std::printf("Bennet %s: full table reads=%zu, opcodes=%zu\n",label,reads,opcodes);
    };
  const auto setOre = [](Npc& npc, size_t count) {
    npc.delItem(ore,uint32_t(npc.itemCount(ore)));
    if(count!=0)
      require(npc.addItem(ore,count)!=nullptr,"Bennet contract could not prepare an ore stack");
    };
  const auto clearQueues = [](Npc& hero, Npc& bennet) {
    Contract::queue(hero).clear();
    Contract::queue(bennet).clear();
    Contract::outputOrder(hero.world().script())=0;
    };
  const auto holdings = [](Npc& hero, Npc& bennet) -> Counts {
    return {hero.itemCount(ore),bennet.itemCount(ore)};
    };
  const auto requireReplies = [](Npc& hero, Npc& bennet, std::string_view prefix, const auto& suffixes) {
    std::array<bool,std::tuple_size_v<std::decay_t<decltype(suffixes)>>> seen={};
    for(auto* speaker : {&hero,&bennet}) {
      auto queue=Contract::queue(*speaker);
      while(queue.size()>0) {
        const auto reply=queue.pop();
        require(reply.act==AI_Output && reply.i0>=0 && size_t(reply.i0)<seen.size(),
                "Bennet reply must use the stock output order");
        const size_t index=reply.i0;
        const std::string_view suffix=suffixes[index];
        require(!seen[index] && reply.s0==std::string(prefix)+std::string(suffix) &&
                (speaker==&hero)==suffix.starts_with("15_") && reply.target==(speaker==&hero ? &bennet : &hero),
                "Bennet reply must preserve its text key, speaker and listener");
        seen[index]=true;
        }
      }
    require(std::all_of(seen.begin(),seen.end(),[](bool value) { return value; }),"every Bennet reply must be queued");
    };
  const auto queueTrace = [](Npc& hero, Npc& bennet) {
    return std::pair{activityQueue(hero),activityQueue(bennet)};
    };

  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  auto [hero,bennet]=actors();
  auto& original=hero->world().script();
  Contract::checkBennetLookupProbe(original);
  Contract::watchBennetLookups(original);
  require(original.globalIntSymbol(mission)==0 && original.globalIntSymbol(trained)==0 &&
          !offer(*hero,*bennet,answerFn) && !offer(*hero,*bennet,leadFn),
          "new game must have neither accepted the quest nor learned ore smithing");
  hero->handle().guild=int32_t(GIL_SLD);
  hero->setTalentSkill(TALENT_SMITH,1);
  require(original.setGlobalIntSymbol("PLAYER_TALENT_SMITH",1),"Bennet fixture needs the normal smithing prerequisite");
  setOre(*hero,5);
  setOre(*bennet,0);

  std::string error;
  auto* ownVar=original.ownSymbols().addInt(original.getVm(),directorVar,error);
  require(ownVar!=nullptr,error.c_str());
  ownVar->set_int(37);
  const auto* ownNpc=original.ownSymbols().addNpc(original.getVm(),directorNpc,"NONE_100_XARDAS",error);
  require(ownNpc!=nullptr,error.c_str());
  const auto directorId=ownNpc->index();
  require(hero->world().addNpc(directorId,hero->position())!=nullptr,"Bennet fixture must add the director NPC to the world");

  clearQueues(*hero,*bennet);
  const auto starter=offer(*hero,*bennet,startFn);
  require(starter && starter->sort==7 && !starter->title.empty(),"fresh Bennet must offer the normal ore smithing topic");
  run(*starter,*hero,*bennet,"offer accepted");
  requireReplies(*hero,*bennet,"DIA_Bennet_WannaSmithORE_",starterLines);
  require(original.globalIntSymbol(mission)==1 && original.globalIntSymbol(trained)==0 &&
          original.doesNpcKnowInfo(hero->handle(),startId) && holdings(*hero,*bennet)==Counts{5,0},
          "Bennet's starter must set LOG_RUNNING through its normal dialogue without taking ore");

  clearQueues(*hero,*bennet);
  const auto lead=offer(*hero,*bennet,leadFn);
  require(lead && lead->sort==8,"the active quest must offer the ore location");
  run(*lead,*hero,*bennet,"ore location");
  requireReplies(*hero,*bennet,"DIA_Bennet_WhereOre_",leadLines);
  require(original.doesNpcKnowInfo(hero->handle(),leadId) && !offer(*hero,*bennet,leadFn),
          "Bennet's ore location reply must only be offered once");

  setOre(*hero,4);
  require(!offer(*hero,*bennet,answerFn),"Bennet must decline delivery with four ore");
  setOre(*hero,5);
  clearQueues(*hero,*bennet);
  const auto ready=caseDir/"bennet-native-running.sav";
  save("Bennet LOG_RUNNING",ready);

  for(const bool merge:{false,true}) {
    std::tie(hero,bennet)=load(ready);
    auto& script=hero->world().script();
    auto& vm=script.getVm();
    Contract::watchBennetLookups(script);
    require(script.globalIntSymbol(mission)==1 && holdings(*hero,*bennet)==Counts{5,0} &&
            script.doesNpcKnowInfo(hero->handle(),startId) && script.doesNpcKnowInfo(hero->handle(),leadId) &&
            !script.doesNpcKnowInfo(hero->handle(),infoId),
            "reloading the active archive must restore the started quest and its known topics");
    if(merge) {
      setOre(*hero,6);
      setOre(*bennet,1);
      hero->getItem(ore)->handle().value=217;
      bennet->getItem(ore)->handle().value=219;
      }

    gothic.dialogScriptFn=[](uint32_t fn,int32_t& result) {
      if(fn!=conditionFn)
        return false;
      result=0;
      return true;
      };
    require(!offer(*hero,*bennet,answerFn),"the director's condition override must have priority");
    gothic.dialogScriptFn={};
    const auto choice=offer(*hero,*bennet,answerFn);
    require(choice && choice->sort==9 && !choice->title.empty(),"Bennet must offer to take five ore");
    clearQueues(*hero,*bennet);
    if(!merge) {
      daedalus::IInfo wrongId;
      wrongId.npc=choice->handle->npc;
      wrongId.condition=choice->handle->condition;
      wrongId.information=choice->handle->information;
      require(Contract::isBennetOreDialog(script,*choice->handle) && !Contract::isBennetOreDialog(script,wrongId),
              "Bennet's route must require the prepared dialogue ID");
      auto& binding=Contract::bennetAnswer(script);
      const auto* answer=binding;
      auto incomplete=*answer;
      storygraph::Step missing;
      missing.kind=storygraph::Step::Call;
      missing.call=storygraph::ex::call("BENNET_MISSING_DEPENDENCY",{});
      incomplete.steps.push_back(std::move(missing));
      const auto before=queueTrace(*hero,*bennet);
      binding=&incomplete;
      run(*choice,*hero,*bennet,"failed preflight");
      binding=answer;
      require(holdings(*hero,*bennet)==Counts{5,0} && script.globalIntSymbol(mission)==1 &&
              !script.doesNpcKnowInfo(hero->handle(),infoId) && queueTrace(*hero,*bennet)==before,
              "Bennet dependency failure must preserve counts, mission, known dialogue and queued effects");
      }
    const int32_t heroXp=hero->experience();
    const auto journal=script.questLog().questCount();
    std::vector<Counts> counts;
    Contract::bennetTransferObserver(script)=[&](Npc& player,Npc& smith) { counts.push_back(holdings(player,smith)); };
    run(*choice,*hero,*bennet,merge ? "merge hand-in" : "cold hand-in");
    Contract::bennetTransferObserver(script)={};
    require(counts==std::vector<Counts>{merge ? Counts{1,6} : Counts{0,5},merge ? Counts{3,4} : Counts{2,3}},
            "Bennet must receive five ore before returning two");
    require(script.globalIntSymbol(mission)==2 && script.doesNpcKnowInfo(hero->handle(),infoId) &&
            hero->experience()==heroXp && script.questLog().questCount()==journal,
            "Bennet must finish the quest without changing experience or journal");
    if(merge)
      require(hero->getItem(ore)->handle().value==217 && bennet->getItem(ore)->handle().value==219,
              "both original ore stacks must retain their properties");

    auto heroQueue=Contract::queue(*hero);
    auto bennetQueue=Contract::queue(*bennet);
    require(heroQueue.size()==1 && bennetQueue.size()==5 && heroQueue.front()->act==AI_Output &&
            heroQueue.front()->s0==handInLines[0] && heroQueue.front()->i0==0,
            "Bennet hand-in must open with the hero's stock reply");
    for(int i=1;i<=3;++i) {
      const auto row=bennetQueue.pop();
      require(row.act==AI_Output && row.s0==handInLines[size_t(i)] && row.i0==i,"Bennet's reply order must stay stock");
      if(i==1 || i==3)
        require(bennetQueue.pop().act==AI_PrintScreen,"Bennet's two transfers must each queue a notice in order");
      }
    const auto queue=queueTrace(*hero,*bennet);

    const auto assertTransfer = [](Npc& player, Npc& smith) {
      auto& script=player.world().script();
      auto& vm=script.getVm();
      const auto& vars=Contract::transferVariables(script);
      require(vars.amount==2 && vars.item==int32_t(ore) && !vars.itemName.empty() && !vars.notice.empty() &&
              vm.find_symbol_by_name("B_GIVEINVITEMS.GIVER")->get_instance().get()==static_cast<daedalus::INpc*>(smith.handlePtr().get()) &&
              vm.find_symbol_by_name("B_GIVEINVITEMS.TAKER")->get_instance().get()==static_cast<daedalus::INpc*>(player.handlePtr().get()) &&
              vm.global_item()->get_instance().get()==static_cast<daedalus::IItem*>(player.getItem(ore)->handlePtr().get()),
              "Bennet transfer must preserve its variables, actors and the hero's exact ITEM reference");
      };
    assertTransfer(*hero,*bennet);
    // Two references to different inventory objects of the same item class.
    vm.find_symbol_by_index(ore)->set_instance(std::static_pointer_cast<daedalus::IItem>(bennet->getItem(ore)->handlePtr()));
    const auto finished=caseDir/(merge ? "bennet-native-merged.sav" : "bennet-native-finished.sav");
    save("Bennet LOG_SUCCESS",finished);
    std::tie(hero,bennet)=load(finished);
    auto& reloaded=hero->world().script();
    Contract::watchBennetLookups(reloaded);
    require(reloaded.globalIntSymbol(mission)==2 && holdings(*hero,*bennet)==counts.back() &&
            reloaded.doesNpcKnowInfo(hero->handle(),infoId) && !offer(*hero,*bennet,answerFn) &&
            !offer(*hero,*bennet,leadFn) && queueTrace(*hero,*bennet)==queue,
            "ordinary reload must restore Bennet's result, exact queue and one-shot topics");
    assertTransfer(*hero,*bennet);
    require(reloaded.getVm().find_symbol_by_index(ore)->get_instance().get()==
              static_cast<daedalus::IItem*>(bennet->getItem(ore)->handlePtr().get()),
            "the other ore reference must retain Bennet as its owner after reload");
    const auto* witness=hero->world().findNpcByInstance(directorId);
    require(reloaded.globalIntSymbol(directorVar)==37 && witness && reloaded.symbolName(directorId)==directorNpc,
            "ordinary reload must preserve director variables and the spawned NPC identity");
    std::printf("Bennet %s: %zu/%zu then %zu/%zu, LOG_SUCCESS restored in v%u\n",
                merge ? "merging" : "cold",counts[0].first,counts[0].second,counts[1].first,counts[1].second,unsigned(Serialize::Current));

    if(!merge) {
      // The permanent smithing topic teaches ore smithing after the hand-in.
      clearQueues(*hero,*bennet);
      const auto training=offer(*hero,*bennet,startFn);
      require(bool(training),"the ore smithing topic must remain available after successful delivery");
      bool overridden=false;
      gothic.dialogScriptFn=[&](uint32_t fn,int32_t&) {
        overridden=fn==startFn;
        return overridden;
        };
      reloaded.exec(*training,*hero,*bennet);
      require(overridden && reloaded.globalIntSymbol(trained)==0 &&
              Contract::queue(*hero).size()==0 && Contract::queue(*bennet).size()==0,
              "the director's starter-topic override must have priority");
      gothic.dialogScriptFn={};
      run(*training,*hero,*bennet,"ore smithing unlocked");
      requireReplies(*hero,*bennet,"DIA_Bennet_WannaSmithORE_",trainingLines);
      require(reloaded.globalIntSymbol(trained)==1 && !offer(*hero,*bennet,startFn),
              "Bennet must unlock ore smithing through the stock follow-up and then hide the topic");
      }
    }
  gothic.clearGame().reset();
#endif
  }

void nativeStoryGraphContract(const std::filesystem::path& caseDir) {
#if !defined(GOTHIC_BENNET_LOOKUP_AUDIT)
  (void)caseDir;
  require(false,"native StoryGraph contract requires the full-symbol lookup guard");
#else
  using Contract = NpcDefinitionContract;
  constexpr auto stock = "DIA_ADDON_CAVALORN_LETSKILLBANDITS";
  constexpr auto actorName = "BAU_4300_ADDON_CAVALORN";
  constexpr auto directorVar = "DIR_NATIVE_STORY_RESULT";
  constexpr auto directorNpc = "DIR_NATIVE_STORY_ACTOR";
  constexpr auto authored = "DIR_NATIVE_STORY";
  constexpr auto continuation = "DIR_NATIVE_STORY_CONTINUE";
  constexpr auto topic = "Native StoryGraph contract";
  constexpr std::array banditNames={"BDT_1013_BANDIT_L","BDT_1014_BANDIT_L","BDT_1015_BANDIT_L"};
  require(caseDir.is_absolute(),"native StoryGraph contract case directory must be absolute");
  const auto* game=std::getenv("GOTHIC_DIR");
  require(game && *game,"GOTHIC_DIR must point to prepared Gothic II settings");
  setenv("GOTHIC_AI","0",1);
  const char* args[]={"gothic-placed-items-contract","-g",game};
  CommandLine commandLine(3,args);
  Resources resources;
  Gothic gothic;
  requireNativeQuestBase();
  gothic.isNpcInDialogFn=[](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  const auto journalBefore = gothic.world()->script().questLog().questCount();
  const auto stockId = gothic.world()->script().findSymbolIndex(stock);
  const auto stateInit = gothic.world()->script().findFunction("ZS_SHORTZAPPED");
  const auto stateLoop = gothic.world()->script().findFunction("ZS_SHORTZAPPED_LOOP");
  const auto stateEnd = gothic.world()->script().findFunction("ZS_SHORTZAPPED_END");
  require(stateInit.isValid() && stateLoop.isValid() && stateEnd.isValid(),"the state fixture must have all three phases");
  std::optional<uint32_t> directorNpcId;
  {
    auto& world=*gothic.world();
    auto& script=world.script();
    auto& vm=script.getVm();
    auto* hero=world.player();
    auto* actor=activityNpc(world,actorName);
    require(hero && actor && script.usesVerifiedNpcProgram() && vm.size()==0 && stockId!=size_t(-1),
            "native StoryGraph contract needs stock Cavalorn and no bytecode");
    Contract::checkBennetLookupProbe(script);

    constexpr std::array aliases={"AMBUSHER_1013","AMBUSHER_1014","AMBUSHER_1015"};
    std::array<Npc*,banditNames.size()> bandits={};
    std::array<daedalus::DaedalusSymbol*,aliases.size()> aliasSlots={};
    for(size_t i=0;i<bandits.size();++i) {
      bandits[i]=activityNpc(world,banditNames[i]);
      if(bandits[i]==nullptr)
        bandits[i]=world.addNpc(script.findSymbolIndex(banditNames[i]),actor->position());
      aliasSlots[i]=vm.find_symbol_by_name(aliases[i]);
      require(bandits[i] && aliasSlots[i] && !bandits[i]->isDead(),"the ambush fixture needs three live actors");
      aliasSlots[i]->set_instance(nullptr);
      bandits[i]->handle().aivar[61]=1;
      }
    require(script.setGlobalIntSymbol("MIS_ADDON_CAVALORN_KILLBRAGO",1) &&
            script.setGlobalIntSymbol("MIS_ADDON_CAVALORN_LETTER2VATRAS",0) &&
            script.setGlobalIntSymbol("MIS_ADDON_NEFARIUS_BRINGMISSINGORNAMENTS",0) &&
            script.setGlobalIntSymbol("BDT_1013_AWAY",0) &&
            script.setGlobalIntSymbol("CAVALORN_ADDON_TEACHPLAYER",1),"Cavalorn contract needs the stock quest flags");
    hero->handle().aivar[86]=35; // REAL_TALENT_BOW, the two-point price tier.
    hero->handle().hitchance[TALENT_BOW]=35;
    hero->handle().lp=2;
    const auto teachFn=script.findSymbolIndex("DIA_ADDON_CAVALORN_TEACH_INFO");
    const auto bowFn=script.findSymbolIndex("DIA_ADDON_CAVALORN_TEACH_BOW_1");
    Contract::queue(*hero).clear();
    Contract::queue(*actor).clear();
    bandits.front()->clearState(true);

    std::vector<std::string> chosen;
    StoryRuntime runtime(nullptr);
    DialogGraph graph([](std::string&&) {},nullptr,runtime);
    graph.chosen=[&](Npc& speaker,const std::string& node) {
      require(&speaker==actor,"the StoryGraph event must name Cavalorn");
      chosen.push_back(node);
      };
    const auto apply=[&](const char* operation) {
      rapidjson::Document json;
      require(!json.Parse(operation).HasParseError(),"invalid StoryGraph contract patch");
      const auto error=graph.applyOp(json);
      require(error.empty(),error.c_str());
      };
    const auto offered=[&](std::string_view name) {
      const auto nodes=graph.offered(world,*actor);
      return std::ranges::find(nodes,name)!=nodes.end();
      };
    const auto choose=[&](std::string_view name) {
      std::string error;
      require(graph.choose(world,*actor,name,error),error.c_str());
      };

    const auto reads=BennetLookupAudit::lookups;
    size_t instructions=0;
    const auto observer=vm.add_instruction_observer([&](const auto&,const auto&) { ++instructions; });
    {
      struct Guard {
        Guard() { ++BennetLookupAudit::depth; }
        ~Guard() { --BennetLookupAudit::depth; }
        } guard;
      const auto previousItem=vm.global_item()->get_instance();
      const auto previousName=actor->handle().name[4];
      auto item=std::make_shared<daedalus::IItem>(); // A native record with no VM class allocation.
      item->text[5]="Story field test";
      vm.global_item()->set_instance(item);
      rapidjson::Document fields;
      fields.Parse(R"([
        {"do":"set","target":["field",["inst","ITEM"],"FLAGS"],"value":["int",-2147483648]},
        {"do":"set","target":["field",["inst","SELF"],"NAME",4],"value":["field",["inst","ITEM"],"TEXT",5]},
        {"do":"return","value":["field",["inst","SELF"],"NAME",4]}
      ])");
      const auto updated=script.runStoryProgram(storygraph::readProgram(fields,true),actor,hero,true);
      require(updated.ok && updated.returned.s==item->text[5] && actor->handle().name[4]==item->text[5] &&
              uint32_t(item->flags)==0x80000000u,"StoryGraph must read and write NPC and item fields without a VM member binding");
      fields.Parse(R"([{"do":"return","value":["field",["inst","ITEM"],"NAME",1]}])");
      const auto wrongIndex=script.runStoryProgram(storygraph::readProgram(fields,true),actor,hero,true);
      require(!wrongIndex.ok && wrongIndex.started,"the NPC name array must not extend an item's scalar name");
      actor->handle().name[4]=previousName;

      const auto runFields=[&](const std::shared_ptr<daedalus::DaedalusInstance>& record, const char* program) {
        vm.global_item()->set_instance(record);
        fields.Parse(program);
        return script.runStoryProgram(storygraph::readProgram(fields,true),actor,hero,true);
        };
      auto info=std::make_shared<daedalus::IInfo>();
      const auto infoResult=runFields(info,R"([
        {"do":"set","target":["field",["inst","ITEM"],"DESCRIPTION"],"value":["str","Story topic"]},
        {"do":"return","value":["field",["inst","ITEM"],"DESCRIPTION"]}])");
      require(infoResult.ok && infoResult.returned.s=="Story topic" && info->description=="Story topic",
              "StoryGraph must access dialog fields on the dialog record");
      auto focus=std::make_shared<daedalus::IFocus>();
      const auto focusResult=runFields(focus,R"([
        {"do":"set","target":["field",["inst","ITEM"],"NPC_RANGE1"],"value":["int",1069547520]},
        {"do":"return","value":["field",["inst","ITEM"],"NPC_RANGE1"]}])");
      require(focusResult.ok && focusResult.returned.type==storygraph::NativeValue::Float &&
              focusResult.returned.f==1.5f && focus->npc_range1==1.5f,
              "StoryGraph must preserve the float bit pattern in a focus field");
      auto menuRecord=std::make_shared<daedalus::IMenuItem>();
      const auto menuResult=runFields(menuRecord,R"([
        {"do":"set","target":["field",["inst","ITEM"],"TEXT",9],"value":["str","Story menu"]},
        {"do":"return","value":["field",["inst","ITEM"],"TEXT",9]}])");
      require(menuResult.ok && menuResult.returned.s=="Story menu" && menuRecord->text[9]=="Story menu",
              "StoryGraph must use the menu's own text extent when both object classes have TEXT");
      const auto nullResult=runFields(nullptr,R"([{"do":"return","value":["field",["inst","ITEM"],"NPC_RANGE1"]}])");
      require(nullResult.ok && nullResult.returned.type==storygraph::NativeValue::Int && nullResult.returned.i==0,
              "a missing focus instance must still read as integer zero");
      vm.global_item()->set_instance(previousItem);

      std::string symbolError;
      directorNpcId=script.addDirectorNpcSymbol(directorNpc,actorName,symbolError);
      require(directorNpcId.has_value(),symbolError.c_str());
      require(script.storyNpcId(directorNpc)==directorNpcId,"native story lookup must resolve the new director NPC id");
      require(!offered(stock),"Cavalorn must hide the topic when the ambush actors are absent");
      for(size_t i=0;i<bandits.size();++i)
        aliasSlots[i]->set_instance(std::static_pointer_cast<daedalus::INpc>(bandits[i]->handlePtr()));
      require(offered(stock),"Cavalorn must offer the stock topic after its nested ambush check");
      apply(R"({"op":"program","node":"DIA_ADDON_CAVALORN_LETSKILLBANDITS","part":"condition",
                "steps":[{"do":"return","value":["int",0]}]})");
      require(!offered(stock),"the director's condition must replace the stock condition");
      apply(R"({"op":"program","node":"DIA_ADDON_CAVALORN_LETSKILLBANDITS","part":"condition","steps":null})");
      choose(stock);
      const auto* waypoint=actor->currentTaPoint();
      require(script.doesNpcKnowInfo(hero->handle(),stockId) &&
              std::ranges::all_of(bandits,[](Npc* bandit) { return bandit->handle().aivar[61]==0; }) &&
              hero->aiQueueFront() && hero->aiQueueFront()->s0=="DIA_Addon_Cavalorn_LETSKILLBANDITS_15_00" &&
              waypoint && waypoint->name=="NW_XARDAS_BANDITS_LEFT",
              "Cavalorn must mark the topic heard, update the ambush, queue the stock line and change routine");

      apply(R"({"op":"vars","vars":{"DIR_NATIVE_STORY_RESULT":0}})");
      apply(R"({"op":"add_node","node":{"id":"DIR_NATIVE_STORY","npc":"BAU_4300_ADDON_CAVALORN",
        "description":"Discuss the ambush","condition":["!",["fn","C_BRAGOBANDITSDEAD"]],"program":[
        {"do":"set","target":["var","DIR_NATIVE_STORY_RESULT"],"value":["int",10]},
        {"do":"choice","id":"DIR_NATIVE_STORY_CONTINUE","text":"Record the ambush","steps":[
          {"do":"set","target":["var","DIR_NATIVE_STORY_RESULT"],"value":["int",42]},
          {"do":"call","call":["call","LOG_CREATETOPIC",["str","Native StoryGraph contract"],["int",0]]},
          {"do":"call","call":["call","LOG_ADDENTRY",["str","Native StoryGraph contract"],["str","Ambush recorded"]]}
        ]}]}})");
      require(offered(authored),"the director's new topic must use the same stock helper");
      choose(authored);
      const auto choices=graph.choices(world,*actor,authored);
      require(choices==std::vector<std::pair<std::string,std::string>>{{continuation,"Record the ambush"}},
              "the authored program must expose its continuation in the normal menu");
      choose(continuation);
      require(chosen==std::vector<std::string>{stock,authored,continuation},"the director must see all three choices");

      const auto menu=actor->dialogChoices(*hero,{},false);
      const auto teach=std::ranges::find(menu,teachFn,&GameScript::DlgChoice::scriptFn);
      require(teach!=menu.end(),"Cavalorn must offer the stock training dialogue");
      script.exec(*teach,*hero,*actor);
      const auto lessons=script.updateDialog(*teach,*hero,*actor);
      const auto bow=std::ranges::find(lessons,bowFn,&GameScript::DlgChoice::scriptFn);
      require(bow!=lessons.end() && bow->title.ends_with("2 LP"),
              "Cavalorn's bow lesson must display the learner's two-point price");
      script.exec(*bow,*hero,*actor);
      require(hero->handle().lp==0 && hero->handle().hitchance[TALENT_BOW]==36 && hero->handle().aivar[86]==36,
              "Cavalorn must debit two LP and update current and trained bow skill");
      const auto afterPurchase=script.callStoryFunction("B_TEACHFIGHTTALENTPERCENT",actor,hero,
                                                       {actor,hero,int32_t(TALENT_BOW),1,90});
      require(afterPurchase.ok && afterPurchase.returned.i==0 && hero->handle().lp==0 &&
              hero->handle().hitchance[TALENT_BOW]==36 && hero->handle().aivar[86]==36,
              "a repeat lesson without LP must refuse without changing the learner");
      require(script.globalIntSymbol("B_TEACHFIGHTTALENTPERCENT.KOSTEN")==2 &&
              script.globalIntSymbol("B_TEACHFIGHTTALENTPERCENT.REALHITCHANCE")==36,
              "the training result must update the exact stock named values");

      rapidjson::Document invalid;
      invalid.Parse(R"([{"do":"set","target":["var","DIR_NATIVE_STORY_RESULT"],"value":["int",99]},
                         {"do":"call","call":["fn","MISSING_STORY_CONTRACT_HELPER"]}])");
      const auto rejected=runtime.run(storygraph::readProgram(invalid,false),actor,hero);
      require(!rejected.ok && !rejected.started,"an unresolved dependency must be rejected before executing the first effect");

      require(bandits.front()->startState(stateInit,{}),"the bandit must enter a new state without a VM lookup");
      const auto& state=Contract::aiState(*bandits.front());
      require(state.funcIni==stateInit && state.funcLoop==stateLoop && state.funcEnd==stateEnd &&
              std::string_view(state.hint)=="ZS_SHORTZAPPED",
              "a first state transition must resolve the stock Init, Loop and End records");
    }
    ActivityCalls activityCalls;
    Contract::tickRoutine(*bandits.front());
    require(Contract::aiState(*bandits.front()).started && activityCalls.phase(*bandits.front(),"ZS_SHORTZAPPED")==1,
            "the game must run the state Init before its mid-state save");
    vm.remove_instruction_observer(observer);
    require(BennetLookupAudit::lookups==reads && instructions==0,"normal StoryGraph and bridge paths must avoid full table reads and bytecode");
  }

  std::filesystem::create_directories(caseDir);
  const auto savePath=caseDir/"native-storygraph.sav";
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"Native StoryGraph contract",preview);
  }
  std::ofstream file(savePath,std::ios::binary|std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  file.close();
  require(bool(file),"could not write the native StoryGraph save");
  gothic.clearGame().reset();
  std::ifstream saved(savePath,std::ios::binary);
  require(bool(saved),"could not reopen the native StoryGraph save");
  const std::string savedBytes{std::istreambuf_iterator<char>{saved},std::istreambuf_iterator<char>{}};
  {
    Tempest::MemReader reader(savedBytes.data(),savedBytes.size());
    Serialize in(reader);
    SaveGameHeader header;
    require(in.setEntry("header"),"the native StoryGraph save must have the normal save header");
    in.read(header);
    require(header.version==Serialize::Current,"the StoryGraph contract must reload a newly written current save");
  }
  Tempest::MemReader reader(savedBytes.data(),savedBytes.size());
  Serialize in(reader);
  gothic.setGame(std::make_unique<GameSession>(in));
  auto& world=*gothic.world();
  auto& script=world.script();
  const auto* cavalorn=activityNpc(world,actorName);
  require(world.player()->handle().lp==0 && world.player()->handle().hitchance[TALENT_BOW]==36 &&
          world.player()->handle().aivar[86]==36 && script.globalIntSymbol("B_TEACHFIGHTTALENTPERCENT.KOSTEN")==2 &&
          script.globalIntSymbol("B_TEACHFIGHTTALENTPERCENT.REALHITCHANCE")==36,
          "ordinary reload must restore the purchased skill, LP and saved training values");
  const auto* waypoint=cavalorn ? cavalorn->currentTaPoint() : nullptr;
  require(waypoint && waypoint->name=="NW_XARDAS_BANDITS_LEFT" &&
          std::ranges::all_of(banditNames,[&](const char* name) {
            auto* bandit=activityNpc(world,name);
            return bandit!=nullptr && bandit->handle().aivar[61]==0;
            }),"ordinary reload must restore Cavalorn's changed waypoint and the ambush actors' flags");
  const auto& journal=script.questLog();
  const auto* directorEntry=script.ownSymbols().find(directorNpc);
  require(directorEntry && directorEntry->like==actorName && script.storyNpcId(directorNpc)==directorNpcId,
          "ordinary reload must restore the director NPC definition and its id");
  require(script.globalIntSymbol(directorVar)==42 && script.doesNpcKnowInfo(gothic.world()->player()->handle(),stockId) &&
          journal.questCount()==journalBefore+1 && journal.quest(journalBefore).name==topic &&
          journal.quest(journalBefore).entry==std::vector<std::string>{"Ambush recorded"},
          "ordinary reload must restore the stock dialogue and the authored variable and journal effects");
  auto* bandit=activityNpc(world,banditNames.front());
  require(bandit && bandit->isInState(stateInit),"ordinary reload must restore the active state");
  const auto& state=Contract::aiState(*bandit);
  require(state.started && state.funcLoop==stateLoop && state.funcEnd==stateEnd &&
          std::string_view(state.hint)=="ZS_SHORTZAPPED","ordinary reload must keep the state phases and name");
  ActivityCalls activityCalls;
  script.invokeState(bandit,nullptr,nullptr,state.funcLoop);
  bandit->clearState(false);
  require(activityCalls.phase(*bandit,"ZS_SHORTZAPPED")==0 && activityCalls.phase(*bandit,"ZS_SHORTZAPPED_LOOP")==1 &&
          activityCalls.phase(*bandit,"ZS_SHORTZAPPED_END")==1,
          "a restored state must continue through Loop and End without replaying Init");

  const auto ore=script.findItemInstance("ITMI_NUGGET");
  require(ore.has_value(),"the inventory contract needs the stock ore type");
  auto* hero=world.player();
  auto* heroOre=hero->addItem(*ore,1);
  auto* npcOre=bandit->addItem(*ore,1);
  require(heroOre && npcOre && heroOre!=npcOre,"each owner must have its own stack of the same item");
  auto& vm=script.getVm();
  using NpcRef=std::shared_ptr<daedalus::INpc>;
  vm.register_external("NPC_GETINVITEM",std::function<int32_t(NpcRef,int32_t)>{[](NpcRef,int32_t) -> int32_t {
    throw std::runtime_error("The VM inventory callback must not run for a native story call");
    }});
  std::vector<const daedalus::DaedalusInstance*> observed;
  vm.observe_externals([&](const auto& symbol) {
    if(symbol.name()=="NPC_GETINVITEM") {
      require(std::get<int32_t>(vm.stack_frame(0).value)==int32_t(*ore),"the external probe must see the item type");
      observed.push_back(std::get<std::shared_ptr<daedalus::DaedalusInstance>>(vm.stack_frame(1).value).get());
      }
    });
  vm.push_int(0x12345678);
  const auto lookup=[&](Npc* owner) {
    return script.callStoryFunction("NPC_GETINVITEM",bandit,hero,{owner,int32_t(*ore)});
    };
  const auto playerLookup=lookup(hero);
  require(playerLookup.ok && playerLookup.returned.i==int32_t(*ore) &&
          vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(heroOre->handlePtr()),
          "native inventory lookup must bind the actual hero stack");
  const auto npcLookup=lookup(bandit);
  require(npcLookup.ok && npcLookup.returned.i==int32_t(*ore) &&
          vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(npcOre->handlePtr()),
          "native inventory lookup must distinguish stacks held by different actors");
  storygraph::Program wrongType;
  wrongType.returns=true;
  auto& ret=wrongType.steps.emplace_back();
  ret.kind=storygraph::Step::Return;
  ret.value=storygraph::ex::call("NPC_GETINVITEM",{storygraph::ex::inst("ITEM"),storygraph::ex::integer(int32_t(*ore))});
  const auto rejected=script.runStoryProgram(wrongType,bandit,hero,true);
  require(!rejected.ok && rejected.error.find("wrong type")!=std::string::npos &&
          vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(npcOre->handlePtr()),
          "an item in the NPC argument must fail without changing ITEM");
  const auto nullLookup=lookup(nullptr);
  require(nullLookup.ok && nullLookup.returned.i==-1 && vm.global_item()->get_instance()==nullptr,
          "native inventory lookup must keep the stock result and clear ITEM for a null NPC");
  vm.observe_externals({});
  require(observed==std::vector<const daedalus::DaedalusInstance*>{
              static_cast<daedalus::INpc*>(hero->handlePtr().get()),static_cast<daedalus::INpc*>(bandit->handlePtr().get()),
              static_cast<daedalus::IItem*>(npcOre->handlePtr().get()),nullptr} &&
          vm.pop_int()==0x12345678,
          "the external probe must see exact actors and leave the VM stack intact");

  vm.register_external("NPC_GETNEARESTWP",std::function<std::string(NpcRef)>{[](NpcRef) -> std::string {
    throw std::runtime_error("The VM waypoint callback must not run for a native story call");
    }});
  const auto expectedWaypoint=NpcEngineCalls::nearestWaypoint(world,*bandit);
  const auto nearest=script.callStoryFunction("NPC_GETNEARESTWP",bandit,hero,{bandit});
  require(!expectedWaypoint.empty() && nearest.ok && nearest.returned.type==storygraph::NativeValue::Str &&
          nearest.returned.s==expectedWaypoint,"native waypoint lookup must return the engine's owning string");
  vm.register_external("AI_WAIT",std::function<void(NpcRef,float)>{[](NpcRef,float) {
    throw std::runtime_error("The VM wait callback must not run for a native story call");
    }});
  Contract::queue(*bandit).clear();
  const auto waited=script.callStoryFunction("AI_WAIT",bandit,hero,{bandit,0.25f});
  const auto* queued=bandit->aiQueueFront();
  require(waited.ok && queued && queued->act==AI_Wait && queued->i0==250,
          "native wait must keep the original floating-point seconds and enqueue 250 ms");

  vm.register_external("INTTOFLOAT",std::function<float(int32_t)>{[](int32_t) -> float {
    throw std::runtime_error("The VM float conversion must not run for a native story call");
    }});
  vm.register_external("DOC_PRINTLINE",std::function<void(int32_t,int32_t,std::string_view)>{
    [](int32_t,int32_t,std::string_view) { throw std::runtime_error("The VM document callback must not run"); }});
  std::vector<std::string> commonCalls;
  vm.observe_externals([&](const auto& symbol) { commonCalls.push_back(symbol.name()); });
  const auto number=script.callStoryFunction("INTTOFLOAT",bandit,hero,{-31});
  require(number.ok && number.returned.type==storygraph::NativeValue::Float && number.returned.f==-31.f,
          "native numeric conversion must return a float");
  const auto text=script.callStoryFunction("CONCATSTRINGS",bandit,hero,{std::string("native "),std::string("document")});
  require(text.ok && text.returned.s=="native document","native strings must keep their original bytes");

  constexpr uint32_t randomSeed=73106,randomBound=100019;
  script.setSeed(randomSeed);
  std::mt19937 expectedRandom(randomSeed);
  const auto draw=script.callStoryFunction("HLP_RANDOM",bandit,hero,{int32_t(randomBound)});
  require(draw.ok && draw.returned.i==int32_t(expectedRandom()%randomBound),
          "the session random callback must override the shared Gothic callback");

  struct DocumentCapture {
    std::string text;
    void record(const DocumentMenu::Show& document) { text=document.pages.at(0).text; }
    } documentCapture;
  gothic.onShowDocument.bind(&documentCapture,&DocumentCapture::record);
  const auto document=script.callStoryFunction("DOC_CREATE",bandit,hero,{});
  require(document.ok && document.returned.i>=0,"native document creation must return a handle");
  const auto handle=document.returned.i;
  const auto pages=script.callStoryFunction("DOC_SETPAGES",bandit,hero,{handle,1});
  const auto line=script.callStoryFunction("DOC_PRINTLINE",bandit,hero,{handle,0,text.returned.s});
  const auto show=script.callStoryFunction("DOC_SHOW",bandit,hero,{handle});
  gothic.onShowDocument.ubind(&documentCapture,&DocumentCapture::record);
  vm.observe_externals({});
  require(pages.ok && line.ok && show.ok && documentCapture.text=="native document\n" &&
          commonCalls==std::vector<std::string>{"INTTOFLOAT","HLP_RANDOM","DOC_CREATE","DOC_SETPAGES","DOC_PRINTLINE","DOC_SHOW"},
          "native documents must preserve contents and the existing external observer order");
  gothic.clearGame().reset();
#endif
  }

bool runStoryContract(int argc, const char** argv) {
  if(argc==3 && std::string_view(argv[1])=="--bennet-ore") {
    bennetOreContract(argv[2]);
    std::puts("Bennet ore contract passed");
    return true;
    }
  if(argc==3 && std::string_view(argv[1])=="--native-storygraph") {
    nativeStoryGraphContract(argv[2]);
    std::puts("Native StoryGraph contract passed");
    return true;
    }
  require(argc==1,"usage: gothic-placed-items-contract [--bennet-ore CASE_DIR | --native-storygraph CASE_DIR]");
  return false;
  }
