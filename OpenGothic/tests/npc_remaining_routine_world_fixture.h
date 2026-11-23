// World witnesses for the remaining stock NPC routines.

std::vector<RoutineWitness> remainingRoutineRun(Gothic& gothic,const char* worldName,
                                                std::span<const RoutineWitnessSite> sites,bool events) {
  using Contract=NpcDefinitionContract;
  gothic.setGame(std::make_unique<GameSession>(worldName));
  std::vector<RoutineWitness> result(sites.size());
  std::vector<uint8_t> save;
  std::optional<std::array<MonsterRoutineSave,2>> monster;
  {
    ActivityEventsWorld test(gothic);
    auto& world=test.world();
    auto& script=world.script();
    auto& vm=script.getVm();
    test.only({});
    NpcWorldFixture::prepareRoutineWitnessWorld(world,events);
    std::map<std::string,size_t> vmCalls;
    const auto observer=vm.add_call_observer([&](const auto& function) { ++vmCalls[function.name()]; });
    for(size_t i=0;i<sites.size();++i) {
      const auto& site=sites[i];
      const auto* point=world.findWayPoint(site.point);
      require(point!=nullptr,(std::string(site.state)+": fixture waypoint is missing").c_str());
      auto* actor=activityNpc(world,site.actor);
      if(actor==nullptr)
        actor=world.addNpc(script.findSymbolIndex(site.actor),point->position());
      require(actor!=nullptr && !actor->isDead(),(std::string(site.state)+": fixture actor is missing or dead").c_str());
      require(activityNpc(world,site.actor)==actor,(std::string(site.state)+": fixture actor must be in the live world").c_str());
      const auto state=script.findSymbolIndex(site.state);
      require(state!=0,(std::string(site.state)+": routine script symbol is missing").c_str());
      test.stop(*actor);
      test.moveHero(point->position()+Tempest::Vec3(650,0,0),0);
      script.setSeed(73106);
      actor->setPosition(point->position());
      require(actor->startState(ScriptFn(state),site.point),(std::string(site.state)+": cannot start the fixture state").c_str());
      const auto vmBefore=vmCalls[site.state];
      const auto eventsBefore=test.stats(site.state).wakes[0];
      Contract::tickRoutine(*actor);
      auto& witness=result[i];
      witness.entry=activityQueue(*actor);
      witness.stage=actor->handle().aivar[19];
      require(actor->isInState(ScriptFn(state)) && Contract::aiState(*actor).started,
              (std::string(site.state)+": Init must run through the NPC lifecycle").c_str());
      require(vmCalls[site.state]-vmBefore==(events ? 0u : 1u) &&
              test.stats(site.state).wakes[0]-eventsBefore==(events ? 1u : 0u),
              (std::string(site.state)+": Init did not run through the expected executor").c_str());
    }
    vm.remove_call_observer(observer);
    if(events && std::string_view(worldName)=="NEWWORLD.ZEN")
      monster=monsterRoutinesWorld(gothic);
    Tempest::MemWriter writer(save);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"human routine entries",preview);
    for(size_t i=0;i<sites.size();++i) {
      const auto* actor=activityNpc(world,sites[i].actor);
      require(actor!=nullptr,(std::string(sites[i].state)+": fixture actor must survive until the save").c_str());
      result[i].position=actor->position();
      result[i].pose=actor->bodyStateMasked();
    }
  }
  gothic.clearGame().reset();
  {
    Tempest::MemReader reader(save);
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
    auto& world=*gothic.world();
    auto& script=world.script();
    auto& vm=script.getVm();
    std::map<std::string,size_t> vmCalls;
    const auto observer=vm.add_call_observer([&](const auto& function) { ++vmCalls[function.name()]; });
    if(monster) {
      for(const auto& saved:*monster) {
        auto* actor=world.npcById(saved.npc);
        const auto state=script.findFunction(saved.state);
        const auto loop=std::string(saved.state)+"_LOOP";
        require(actor && actor->isInState(state) && Contract::aiState(*actor).started &&
                activityQueue(*actor)==saved.queue && actor->stateTime()==saved.stateTime &&
                actor->handle().aivar[19]==saved.stage && world.npcId(actor->target())==saved.target &&
                world.npcId(actor->stateOther())==saved.other,
                "ordinary load must restore the monster's state, timer, stage, target and pending commands");
        actor->clearAiQueue();
        actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
        ActivityStream stream(world,*actor);
        Contract::tickRoutine(*actor);
        require(actor->isInState(state) && stream.records==1 && vmCalls[loop]==0 &&
                activityCount(stream.events,"phase "+loop)==1,
                "the loaded monster must resume in its native loop without rerunning Init");
        std::printf("Monster state %s: ordinary load restored the pending commands and resumed native Loop\n",saved.state);
        }
      }
    for(size_t i=0;i<sites.size();++i) {
      const auto& site=sites[i];
      auto* actor=activityNpc(world,site.actor);
      auto& witness=result[i];
      const auto id=ScriptFn(script.findSymbolIndex(site.state));
      require(actor!=nullptr && actor->isInState(id) && Contract::aiState(*actor).started &&
              activityQueue(*actor)==witness.entry && actor->handle().aivar[19]==witness.stage &&
              actor->bodyStateMasked()==witness.pose && (actor->position()-witness.position).length()<1.f,
              (std::string(site.state)+": ordinary reload must restore the entry before its pending commands run").c_str());
      if(std::string_view(site.state)=="ZS_GUARD_PASSAGE") {
        const auto aligned=[](const Npc& npc) {
          const auto queue=activityQueue(npc);
          return queue.size()==1 && queue.front().starts_with("AlignToWp ");
          };
        ActivityEventsWorld test(gothic);
        const auto guard=actor->position();
        for(const auto [distance,time,expect] : {std::tuple{650.f,2999,false},std::tuple{450.f,3000,false},
                                                std::tuple{650.f,3000,true}}) {
          test.moveHero(guard+Tempest::Vec3(distance,0,0),0);
          actor->clearAiQueue();
          actor->setStateTime(time);
          Contract::aiState(*actor).loopNextTime=world.tickCount();
          Contract::tickRoutine(*actor);
          require(aligned(*actor)==expect && actor->stateTime()==(expect ? 0u : uint64_t(time)),
                  "ZS_GUARD_PASSAGE: align and reset only at three seconds with the hero outside 500 cm");
        }
        require(events ? vmCalls["ZS_GUARD_PASSAGE_LOOP"]==0 : vmCalls["ZS_GUARD_PASSAGE_LOOP"]==3,
                "ZS_GUARD_PASSAGE must use the expected Loop executor across the threshold");
      }
      if(std::string_view(site.state)=="ZS_BANDIT") {
        actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
        size_t instructions=0;
        vm.observe_instructions([&](const auto&) { ++instructions; });
        actor->clearAiQueue();
        Contract::tickRoutine(*actor);
        require(actor->handle().aivar[19]==0 && !activityQueue(*actor).empty(),
                "the loaded bandit must select its campfire or guard pose exactly once");
        witness.continuation=activityQueue(*actor);
        script.setSeed(73106);
        auto* roll=script.findSymbol("ZS_BANDIT_LOOP.RANDOM");
        require(roll!=nullptr,"the bandit fixture needs the original random slot");
        roll->set_int(-1);
        for(uint64_t ms : {5999,6000}) {
          actor->clearAiQueue();
          actor->setStateTime(ms);
          Contract::tickRoutine(*actor);
          require(actor->stateTime()==(ms==5999 ? 5999u : 0u) &&
                  (ms==5999 ? actor->isAiQueueEmpty() && roll->get_int()==-1
                           : roll->get_int()>=0 && roll->get_int()<3),
                  "the bandit first checks its next watch gesture at six whole seconds");
          const auto queue=activityQueue(*actor);
          witness.continuation.insert(witness.continuation.end(),queue.begin(),queue.end());
        }
        vm.observe_instructions({});
        witness.continuation.push_back(std::to_string(roll->get_int()));
        require(events ? instructions==0 : instructions>0,
                "the loaded bandit loops must dispatch through the chosen executor");
      }
      if(std::string_view(site.state)=="ZS_GOLEMDOWN") {
        actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
        require(actor->handle().senses==int32_t(SensesBit::SENSE_SMELL) && actor->handle().senses_range==2000,
                "load must preserve the dormant golem's senses");
        actor->clearAiQueue();
        Contract::tickRoutine(*actor);
        require(Contract::aiState(*actor).loopNextTime==world.tickCount()+1000,
                "the loaded golem must keep its one-second perception interval");
        require(actor->handle().aivar[19]==0 && actor->aiQueueFront() && actor->aiQueueFront()->s0=="T_DEAD",
                "the loaded golem must settle into its dormant pose once");
        actor->clearAiQueue();
        Contract::tickRoutine(*actor);
        require(actor->isAiQueueEmpty(),"the dormant golem must keep its pose on the next loop");

        auto* hero=world.player();
        Interactive* chest=nullptr;
        for(uint32_t index=0; auto* mob=world.mobsiById(index); ++index) {
          if(mob->tag()=="NW_GOLEMCHEST") {
            chest=mob;
            break;
          }
        }
        const auto item=script.findSymbolIndex("ITSE_GOLEMCHEST_MIS");
        require(hero && chest && item && chest->inventory().itemCount(item)>0,
                "the stock golem fixture needs the mission item inside its chest");
        actor->setExternalControl(false);
        size_t instructions=0;
        auto wakeAt=[&](float offset) {
          hero->setPosition(actor->position()+Tempest::Vec3(offset,0,0));
          vm.observe_instructions([&](const auto&) { ++instructions; });
          const auto perceived=actor->passPerception(*hero,nullptr,PERC_ASSESSPLAYER);
          vm.observe_instructions({});
          require(perceived,"the dormant golem must recover its perception callback after load");
          };
        wakeAt(700.f);
        require(actor->isAiQueueEmpty(),"the golem must wait while the item is inside the chest");
        auto& inventory=chest->inventory();
        Inventory removed;
        Inventory::transfer(removed,inventory,nullptr,item,inventory.itemCount(item),world);
        wakeAt(701.f);
        require(actor->isAiQueueEmpty(),"the golem must wait while the hero is outside 700 cm");
        wakeAt(700.f);
        witness.continuation=activityQueue(*actor);
        witness.continuation.push_back(actor->handle().name[0]);
        require(actor->aiQueueFront() && actor->aiQueueFront()->s0=="T_RISE" &&
                actor->handle().no_focus==0 && actor->handle().flags==NpcFlag::NONE &&
                actor->handle().bodystate_interruptable_override==0 && actor->handle().aivar[32]==-1 &&
                actor->handle().start_aistate==int32_t(script.findSymbolIndex(MonsterScheduler)),
                "after opening the chest, the golem must rise, attack, and resume the monster dispatcher");
        require(events ? instructions==0 : instructions>0,
                "the golem wake-up must dispatch through the chosen executor");
      }
      if(std::string_view(site.state)=="ZS_SLEEP_DEEP" ||
         std::string_view(site.state)=="ZS_PRAY_SLEEPER" ||
         std::string_view(site.state)=="ZS_PRAY_SLEEPER_FP") {
        actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
        actor->clearAiQueue();
        size_t instructions=0;
        vm.observe_instructions([&](const auto&) { ++instructions; });
        if(std::string_view(site.state)=="ZS_PRAY_SLEEPER_FP") {
          ActivityEventsWorld test(gothic);
          const auto* point=world.findPoint("FP_PRAY_MONASTERY_02",false);
          require(point!=nullptr,"the prayer fixture needs its authored monastery point");
          test.freePoint(point);
          actor->attachToPoint(point);
          actor->setPosition(point->position());
          Contract::tickRoutine(*actor);
          const auto* kneel=actor->aiQueueFront();
          require(kneel && kneel->act==AI_PlayAnimBs && kneel->s0=="T_STAND_2_PRAY" &&
                  Contract::queue(*actor).size()==1,"the sleeper prayer kneels directly on the point");
        } else {
          Contract::tickRoutine(*actor);
          if(std::string_view(site.state)=="ZS_SLEEP_DEEP") {
            const auto* use=actor->aiQueueFront();
            require(use && use->act==AI_UseMob && use->s0=="BEDHIGH" && use->i0==1,
                    "the loaded deep sleep phase resumes by using the original bed");
          } else {
            require(actor->isAiQueueEmpty(),"the sleeper prayer with no IDOL nearby leaves the queue clear");
          }
        }
        vm.observe_instructions({});
        require(events ? instructions==0 : instructions>0,
                "the loaded sleep or prayer phase must use the chosen executor");
        witness.continuation=activityQueue(*actor);
      }
      const std::string_view state=site.state;
      if(state=="ZS_MAGICFLEE" || state=="ZS_MAGICSLEEP") {
        auto* hero=world.player();
        const bool fear=state=="ZS_MAGICFLEE";
        const std::string loop=std::string(state)+"_LOOP";
        const auto* loopSymbol=script.findSymbol(loop);
        const auto* stopSymbol=script.findSymbol(fear ? "B_STOPMAGICFLEE" : "B_STOPMAGICSLEEP");
        const auto* talkSymbol=script.findSymbol("B_ASSESSMAGICSLEEPTALK");
        require(hero && loopSymbol && stopSymbol && talkSymbol && actor->handle().aivar[12]==0 &&
                actor->hasPerc(PERC_ASSESSDAMAGE),"spell load must preserve the victim's state and damage callback");
        actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
        actor->setExternalControl(false);
        size_t calls=0;
        vm.observe_instructions([&](const auto&) {
          if(vm.pc()==loopSymbol->address() || vm.pc()==stopSymbol->address() || vm.pc()==talkSymbol->address())
            ++calls;
          });
        actor->clearAiQueue();
        if(!fear) {
          require(actor->passPerception(*hero,nullptr,PERC_ASSESSTALK),
                  "the sleeper's talk perception must be available after loading");
          auto talk=Contract::queue(*actor);
          require(talk.size()>0,"magic sleep talk must queue player observation");
          while(talk.size()>1)
            talk.pop();
          const auto observe=talk.pop();
          require(observe.act==AI_StartState && observe.i0==1 &&
                  observe.func==ScriptFn(script.findSymbolIndex("ZS_OBSERVEPLAYER")),
                  "magic sleep talk interrupts the pose to observe the hero");
          witness.continuation=activityQueue(*actor);
          actor->clearAiQueue();
        }
        const auto boundary=fear ? 6000 : 31000;
        const auto loopId=ScriptFn(loopSymbol->index());
        actor->aiPush(AiQueue::aiWait(1337));
        actor->setStateTime(boundary-1);
        require(script.invokeState(actor,hero,nullptr,loopId)==LOOP_CONTINUE &&
                actor->aiQueueFront() && actor->aiQueueFront()->act==AI_Wait &&
                actor->hasPerc(PERC_ASSESSDAMAGE),"the victim must keep its queue until the next whole second");
        require(actor->passPerception(*hero,nullptr,PERC_ASSESSDAMAGE) && !actor->hasPerc(PERC_ASSESSDAMAGE),
                "damage must use the restored spell release callback");
        const auto releaseOnHit=activityQueue(*actor);
        require(releaseOnHit.size()>1 && releaseOnHit.front().starts_with("Wait "),
                "damage releases the victim after commands already in the queue");
        witness.continuation.insert(witness.continuation.end(),releaseOnHit.begin(),releaseOnHit.end());
        actor->setPerceptionEnable(PERC_ASSESSDAMAGE,stopSymbol->index());
        actor->setStateTime(boundary);
        const auto ended=script.invokeState(actor,hero,nullptr,loopId);
        require(!actor->hasPerc(PERC_ASSESSDAMAGE) && ended==(fear ? LOOP_CONTINUE : LOOP_END),
                "at expiry the spell clears its damage callback and returns the stock state result");
        auto command=Contract::queue(*actor);
        require(command.size()>0,"at expiry the victim must queue its release action");
        while(command.size()>1)
          command.pop();
        const auto exit=command.pop();
        if(fear) {
          require(exit.act==AI_StartState && exit.func==ScriptFn(script.findSymbolIndex("ZS_FLEE")) &&
                  actor->target()==hero,"fear expiry must set the hero as target and start the flee state");
        } else {
          require(exit.act==AI_PlayAnim && exit.s0=="T_VICTIM_SLE_2_STAND",
                  "sleep expiry must request the get-up animation");
        }
        const auto queue=activityQueue(*actor);
        witness.continuation.insert(witness.continuation.end(),queue.begin(),queue.end());
        vm.observe_instructions({});
        require(events ? calls==0 : calls>0,"the loaded spell must use the selected callback executor");
      }
      if(state=="ZS_MAGICFREEZE" || state=="ZS_WHIRLWIND") {
        const bool freeze=state=="ZS_MAGICFREEZE";
        auto* hero=world.player();
        const auto* loop=script.findSymbol(std::string(state)+"_LOOP");
        const auto* restart=script.findSymbol(freeze ? "B_RESTARTFREEZE" : "B_RESTARTWHIRLWIND");
        const auto* stop=script.findSymbol(freeze ? "B_STOPMAGICFREEZE" : "B_STOPWHIRLWIND");
        require(hero && loop && restart && stop &&
                Contract::perceptionCallback(*actor,PERC_ASSESSMAGIC)==ScriptFn(restart->index()),
                "ordinary load must preserve the spell's restart perception");
        actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
        actor->setExternalControl(false);
        size_t calls=0;
        vm.observe_instructions([&](const auto&) {
          if(vm.pc()==loop->address() || vm.pc()==restart->address() || vm.pc()==stop->address())
            ++calls;
          });
        actor->clearAiQueue();
        auto& lastSecond=actor->handle().aivar[freeze ? 53 : 76];
        const auto hp=[&] { return actor->attribute(ATR_HITPOINTS); };
        const auto pulse=[&](uint64_t time, int damage) {
          const auto before=hp();
          actor->setStateTime(time);
          require(script.invokeState(actor,hero,nullptr,ScriptFn(loop->index()))==LOOP_CONTINUE &&
                  lastSecond==int32_t(time/1000) && hp()==before-damage,
                  "the spell must sample one damage pulse when its whole second changes");
          };
        pulse(1000,freeze ? 2 : 0);
        pulse(1700,0);
        actor->handle().guild=GIL_FIREGOLEM;
        pulse(4000,freeze ? 4 : 0);
        actor->handle().guild=GIL_ICEGOLEM;
        pulse(5000,freeze ? 1 : 0);
        Contract::lastHitSpell(*actor,freeze ? 71 : 33);
        require(actor->passPerception(*hero,nullptr,PERC_ASSESSMAGIC) && actor->stateTime()==5000 && lastSecond==5,
                "a different spell must not restart the current timer");
        Contract::lastHitSpell(*actor,freeze ? 39 : 71);
        require(actor->passPerception(*hero,nullptr,PERC_ASSESSMAGIC) && actor->stateTime()==0 && lastSecond==5,
                "a matching spell must restart the clock and preserve the last damage sample");
        pulse(freeze ? 19999 : 10999,freeze ? 1 : 0);
        actor->aiPush(AiQueue::aiWait(1337));
        actor->setTempAttitude(ATT_FRIENDLY);
        actor->setStateTime(freeze ? 20000 : 11000);
        require(script.invokeState(actor,hero,nullptr,ScriptFn(loop->index()))==LOOP_END &&
                actor->aiQueueFront() && actor->aiQueueFront()->act==AI_StandUp && Contract::queue(*actor).size()==1 &&
                actor->tempAttitude()==ATT_HOSTILE &&
                Contract::perceptionCallback(*actor,PERC_ASSESSMAGIC)==ScriptFn(script.findSymbolIndex("B_ASSESSMAGIC")),
                "the spell must restore normal magic perception and stand up at expiry");
        vm.observe_instructions({});
        require(events ? calls==0 : calls>0,"freeze and whirlwind must use the selected callback executor");
        witness.continuation=activityQueue(*actor);
      }
      if(state=="ZS_PAL_ZOMBIE") {
        auto* hero=world.player();
        const auto* rise=script.findSymbol("B_PAL_ZOMBIE_RISE");
        require(hero && rise && actor->handle().senses==int32_t(SensesBit::SENSE_SMELL) &&
                actor->handle().senses_range==2000,"the loaded zombie must preserve its dormant senses");
        actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
        actor->setExternalControl(false);
        actor->clearAiQueue();
        Contract::tickRoutine(*actor);
        require(actor->handle().aivar[19]==0 && actor->aiQueueFront() && actor->aiQueueFront()->s0=="T_DOWN",
                "the loaded zombie must queue its dormant pose once");
        actor->clearAiQueue();
        Contract::tickRoutine(*actor);
        require(actor->isAiQueueEmpty(),"the zombie must keep the dormant pose on its next loop");
        size_t calls=0;
        vm.observe_instructions([&](const auto&) { if(vm.pc()==rise->address()) ++calls; });
        hero->setPosition(actor->position()+Tempest::Vec3(1401,0,0));
        require(actor->passPerception(*hero,nullptr,PERC_ASSESSPLAYER) && actor->isAiQueueEmpty(),
                "a dormant zombie must wait when the hero is beyond 1400 cm");
        hero->setPosition(actor->position()+Tempest::Vec3(1400,0,0));
        require(actor->passPerception(*hero,nullptr,PERC_ASSESSPLAYER),"the loaded zombie must retain its wake callback");
        vm.observe_instructions({});
        require(actor->aiQueueFront() && actor->aiQueueFront()->s0=="T_RISE" &&
                actor->handle().bodystate_interruptable_override==0 && actor->handle().aivar[32]==-1 &&
                actor->handle().start_aistate==int32_t(script.findSymbolIndex(MonsterScheduler)) &&
                (events ? calls==0 : calls==2),"the zombie must rise and queue the stock attack transition");
        witness.continuation=activityQueue(*actor);
      }
      actor->clearAiQueue();
      const std::string end=std::string(site.state)+"_END";
      const auto vmBefore=vmCalls[end];
      actor->clearState(false);
      witness.departure=activityQueue(*actor);
      require(!actor->isInState(id) && vmCalls[end]-vmBefore==(events || !vm.find_symbol_by_name(end) ? 0u : 1u),
              (std::string(site.state)+": End did not run through the expected executor").c_str());
    }
    vm.remove_call_observer(observer);
    if(monster)
      ravenStoneGuardianRoutines(world);
  }
  gothic.clearGame().reset();
  return result;
  }

void remainingRoutineWitnesses(Gothic& gothic) {
  static constexpr RoutineWitnessSite khorinis[] = {
    {"ZS_BANDIT",              "BDT_1009_BANDIT_L",               "NW_FOREST_CAVE1_IN_04"},
    {"ZS_GOLEMDOWN",           "SHATTERED_GOLEM",                  "NW_TROLLAREA_RUINS_CAVE_09"},
    {"ZS_CIRCLE",              "KDW_1400_ADDON_SATURAS_NW",        "NW_TROLLAREA_PORTALTEMPEL_RITUAL_01"},
    {"ZS_CONCERT",             "PIR_1390_ADDON_INEXTREMO_DRPYMONTE","NW_CITY_PATH_HABOUR_03"},
    {"ZS_FLEETOWP",            "BDT_1013_BANDIT_L",                "NW_XARDAS_MONSTER_INSERT_01"},
    {"ZS_FOLLOW_PLAYER",       "BAU_931_TILL",                     "NW_BIGFARM_KITCHEN_02"},
    {"ZS_GUARD_PASSAGE",       "KDF_507_TALAMON",                  "NW_MONASTERY_CELLAR_15"},
    {"ZS_GUIDE_PLAYER",        "VLK_440_BARTOK",                    "NW_FARM1_CITYWALL_FOREST_07"},
    {"ZS_MAGICFREEZE",         "VLK_408_JORA",                      "NW_CITY_MERCHANT_PATH_38"},
    {"ZS_WHIRLWIND",           "VLK_413_BOSPER",                    "NW_CITY_MERCHANT_SHOP01_IN_02"},
    {"ZS_MAGICFLEE",           "VLK_425_REGIS",                     "NW_CITY_REGIS"},
    {"ZS_MAGICSLEEP",          "MIL_311_ANDRE",                     "NW_CITY_HABOUR_KASERN_MAIN_ENTRY"},
    {"ZS_PRACTICE_MAGIC",      "TA_TESTMODELL",                    "NW_CITY_PATH_HABOUR_03"},
    {"ZS_RUNTOWP",             "MIL_337_MIKA",                     "NW_FARM2_PATH_03"},
    {"ZS_SLEEP_DEEP",          "VLK_416_MATTEO",                   "NW_CITY_BED_MATTEO"},
    {"ZS_PRAY_SLEEPER",        "NOV_604_DYRIAN",                   "NW_MONASTERY_CHURCH_06"},
    {"ZS_PRAY_SLEEPER_FP",     "NOV_603_AGON",                      "NW_MONASTERY_CHURCH_03"},
    {"ZS_SPIT_FIRE",           "VLK_468_CANTHAR",                  "NW_CITY_PATH_HABOUR_03"},
    {"ZS_STAND_DEMENTOR",      "DMT_1202_DEMENTOR",                "NW_XARDAS_VALLEY_03"},
    {"ZS_STAND_RANGERMEETING", "MIL_350_ADDON_MARTIN",             "NW_TAVERNE_07"},
    {"ZS_WASH_FP",             "MIL_313_BOLTAN",                   "NW_CITY_PATH_HABOUR_03"},
    };
  static constexpr RoutineWitnessSite jharkendar[] = {
    {"ZS_COOK_PAN",       "PIR_1352_ADDON_ALLIGATORJACK", "AD_PIRATECAMP_AJ_03"},
    {"ZS_GHOST",          "NONE_ADDON_111_QUARHODRON",    "ADW_ANCIENTGHOST"},
    {"ZS_GHOSTWUSEL",      "NONE_ADDON_112_RHADEMES",      "ADW_ADANOSTEMPEL_RHADEMES_02"},
    {"ZS_PAL_ZOMBIE",      "MAYAZOMBIE01",                "ADW_VALLEY_PATH_072"},
    {"ZS_PICK_ORE",       "BDT_10004_ADDON_FINN",         "ADW_MINE_PICK_06"},
    {"ZS_ROAST_SCAVENGER", "PIR_1364_ADDON_PIRAT",         "ADW_PIRATECAMP_SCAVENGER"},
    };
  struct Setting {
    const char* key;
    std::optional<std::string> previous;
    explicit Setting(const char* name):key(name) { if(const auto* value=std::getenv(key)) previous=value; }
    ~Setting() { if(previous) setenv(key,previous->c_str(),1); else unsetenv(key); }
    } records("OPENGOTHIC_ACTIVITY_RECORDS"), events("OPENGOTHIC_ACTIVITY_EVENTS");
  for(const auto& [world,sites] : {std::pair{"NEWWORLD.ZEN",std::span<const RoutineWitnessSite>(khorinis)},
                                  std::pair{"ADDONWORLD.ZEN",std::span<const RoutineWitnessSite>(jharkendar)}}) {
    setenv(records.key,"0",1);
    setenv(events.key,"0",1);
    const auto original=remainingRoutineRun(gothic,world,sites,false);
    setenv(records.key,"1",1);
    setenv(events.key,"1",1);
    const auto native=remainingRoutineRun(gothic,world,sites,true);
    for(size_t i=0;i<sites.size();++i) {
      const auto& site=sites[i];
      require(native[i]==original[i],(std::string(site.state)+": queue, pose or stage differs against the original").c_str());
      std::printf("Routine witness %s in %s with %s: Init, normal load and End agree in both executors\n",
                  site.state,world,site.actor);
    }
  }
  }
