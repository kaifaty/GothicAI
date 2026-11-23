void monsterEnemyPerception(World& world, Npc& wolf, Npc& sheep, Npc& hero) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const auto previousEvents=events;
  const auto handler=script.findFunction("B_MM_ASSESSENEMY");
  const auto sleepingHandler=script.findFunction("B_MM_ASSESSQUIETSOUND_SLEEP");
  const auto handlerPc=script.findSymbol(size_t(handler.ptr))->address();
  const auto* sleepingSymbol=script.findSymbol(size_t(sleepingHandler.ptr));
  const auto sleepingPc=sleepingSymbol->address();
  const auto origin=wolf.position()+Tempest::Vec3(0,500,0);
  const std::array actors={&wolf,&sheep,&hero};
  struct ActorState {
    Tempest::Vec3 position;
    int32_t sensesRange;
    bool externalControl;
    decltype(std::to_array(wolf.handle().aivar)) vars;
    };
  std::array<ActorState,actors.size()> saved;
  for(size_t i=0;i<actors.size();++i) {
    auto& npc=*actors[i];
    auto& handle=npc.handle();
    saved[i]={npc.position(),handle.senses_range,npc.isExternalControl(),std::to_array(handle.aivar)};
    npc.setExternalControl(false);
    handle.senses_range=2000;
    for(auto slot:{4,15,61,71})
      handle.aivar[slot]=0;
    handle.aivar[26]=1;
    }
  struct Scene { Npc& listener; Npc& source; int distance; const char* next; bool sleeping=false; };
  const std::array scenes={Scene{wolf,sheep,250,nullptr},Scene{wolf,hero,250,"ZS_MM_ATTACK"},
                           Scene{wolf,hero,250,"ZS_MM_THREATENENEMY"},Scene{wolf,sheep,250,"ZS_MM_HUNT"},
                           Scene{sheep,wolf,250,"ZS_MM_FLEE"},Scene{sheep,wolf,400,nullptr},
                           Scene{wolf,hero,250,"ZS_MM_ATTACK",true},Scene{wolf,sheep,250,nullptr,true}};
  for(size_t i=0;i<scenes.size();++i) {
    const auto& scene=scenes[i];
    auto& listener=scene.listener;
    auto& source=scene.source;
    listener.setPosition(origin);
    listener.updateTransform();
    source.setPosition(origin+Tempest::Vec3(float(scene.distance),0,0));
    source.updateTransform();
    require(listener.canSeeNpc(source,true),"monster enemy perception: the target must be visible to the listener");
    const auto sourceGuild=source.handle().guild;
    if(scene.sleeping && !scene.next)
      source.handle().guild=listener.handle().guild;
    if(scene.sleeping)
      require((script.guildAttitude(listener,source)==ATT_HOSTILE)==(scene.next!=nullptr),
              "sleeping monster perception: the fixture must have the intended guild reaction");
    const auto perception=scene.sleeping ? PERC_ASSESSQUIETSOUND : PERC_ASSESSENEMY;
    const auto callback=scene.sleeping ? sleepingHandler : handler;
    const auto entryPc=scene.sleeping ? sleepingPc : handlerPc;
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      events=native;
      source.handle().aivar[71]=i==0 ? 1 : 0;
      listener.handle().aivar[26]=i==1 || scene.sleeping ? 0 : 1;
      listener.setTarget(nullptr);
      listener.clearAiQueue();
      listener.aiPush(AiQueue::aiWait(173));
      listener.setPerceptionEnable(perception,callback.ptr);
      size_t handlerEntries=0,enemyEntries=0;
      vm.observe_instructions([&](const auto&) { handlerEntries+=vm.pc()==entryPc; enemyEntries+=vm.pc()==handlerPc; });
      const bool delivered=listener.perceptionProcess(source,nullptr,listener.qDistTo(source),perception);
      vm.observe_instructions({});
      require(delivered && handlerEntries==(native ? 0u : 1u),"monster enemy perception: the world uses the selected handler");
      if(scene.sleeping)
        require(enemyEntries==(!native && scene.next ? 1u : 0u),"sleeping monster perception: only a hostile source reaches the enemy handler");
      const auto* front=listener.aiQueueFront();
      if(scene.next) {
        require(front && front->act==AI_StartState && front->func==script.findFunction(scene.next) && front->target==&source &&
                !listener.hasPerc(perception),"monster enemy perception: the reaction replaces the queue and clears perceptions");
        require(listener.target()==(i==2 ? nullptr : &source),"monster enemy perception: threatening leaves the current target unchanged");
        }
      else
        require(front && front->act==AI_Wait && listener.hasPerc(perception),"monster enemy perception: rejecting the source leaves the queue and perceptions intact");
      const auto queue=activityQueue(listener);
      if(native)
        require(queue==original,"monster enemy perception: original and native leave the same queue");
      else
        original=queue;
      }
    source.handle().aivar[71]=0;
    source.handle().guild=sourceGuild;
    }
  // Force the original wrapper so the VM's actual nested BL dispatch is exercised too.
  require(script.guildAttitude(wolf,hero)==ATT_HOSTILE,"monster enemy perception: the sleeping wolf must treat the hero as hostile");
  const auto previousSelf=vm.global_self()->get_instance(),previousOther=vm.global_other()->get_instance();
  vm.global_self()->set_instance(std::static_pointer_cast<daedalus::INpc>(wolf.handlePtr()));
  vm.global_other()->set_instance(std::static_pointer_cast<daedalus::INpc>(hero.handlePtr()));
  for(bool native:{false,true}) {
    events=native;
    hero.handle().aivar[71]=1;
    size_t sleepingEntries=0,enemyEntries=0;
    vm.observe_instructions([&](const auto&) {
      sleepingEntries+=vm.pc()==sleepingPc;
      enemyEntries+=vm.pc()==handlerPc;
      });
    vm.call_function<void>(sleepingSymbol);
    vm.observe_instructions({});
    require(sleepingEntries==1 && enemyEntries==(native ? 0u : 1u),
            "monster enemy perception: the sleeping callback must use the same handler through a nested VM call");
    }
  vm.global_self()->set_instance(previousSelf);
  vm.global_other()->set_instance(previousOther);
  events=previousEvents;
  for(size_t i=0;i<actors.size();++i) {
    auto& npc=*actors[i];
    npc.setPosition(saved[i].position);
    npc.updateTransform();
    npc.handle().senses_range=saved[i].sensesRange;
    npc.setExternalControl(saved[i].externalControl);
    std::copy(saved[i].vars.begin(),saved[i].vars.end(),npc.handle().aivar);
    if(!npc.isPlayer()) {
      npc.clearAiQueue();
      npc.setTarget(nullptr);
      }
    }
  std::puts("Monster enemy perception: original and native reject, attack, threaten, hunt and flee identically");
  }

void monsterSummonedDismissal(World& world, Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& events=Contract::activityEventsOn(script);
  auto& vm=script.getVm();
  const auto previousEvents=events;
  const auto summoned=script.findFunction("ZS_MM_RTN_SUMMONED");
  const auto handler=script.findFunction("B_SUMMONEDASSESSTALK");
  const auto handlerPc=script.findSymbol(size_t(handler.ptr))->address();
  std::vector<std::string> original;
  for(bool native:{false,true}) {
    events=native;
    auto* wolf=world.addNpc(script.findSymbolIndex("SUMMONED_WOLF"),hero.position()+Tempest::Vec3(100,0,0));
    require(wolf && wolf->attribute(ATR_HITPOINTS)>0 && wolf->handle().guild==GIL_SUMMONED_WOLF,
            "summoned dismissal: the original summoned wolf starts alive");
    wolf->setProcessPolicy(NpcProcessPolicy::AiNormal);
    require(wolf->startState(summoned,""),"summoned dismissal: the wolf must enter its summoned routine");
    Contract::tickRoutine(*wolf);
    require(wolf->hasPerc(PERC_ASSESSTALK),"summoned dismissal: the routine registers its actual talk callback");
    wolf->clearAiQueue();
    size_t entries=0;
    vm.observe_instructions([&](const auto&) { entries+=vm.pc()==handlerPc; });
    const bool delivered=wolf->perceptionProcess(hero,nullptr,wolf->qDistTo(hero),PERC_ASSESSTALK);
    vm.observe_instructions({});
    require(delivered && entries==(native ? 0u : 1u) && wolf->isDead() && wolf->attribute(ATR_HITPOINTS)==0,
            "summoned dismissal: talking dismisses the wolf through the original health path");
    if(native)
      require(activityQueue(*wolf)==original,"summoned dismissal: original and native leave the same queue");
    else
      original=activityQueue(*wolf);
    world.removeNpc(*wolf);
    }
  events=previousEvents;
  std::puts("Monster perception: sleeping hostility and summoned dismissal agree in both executors");
  }

void monsterDamagePerception(World& world, Npc& wolf, Npc& sheep, Npc& hero) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const auto previousEvents=events;
  const auto direct=script.findFunction("B_MM_ASSESSDAMAGE");
  const auto witnessed=script.findFunction("B_MM_ASSESSOTHERSDAMAGE");
  const auto attack=script.findFunction("ZS_MM_ATTACK");
  const auto flee=script.findFunction("ZS_MM_FLEE");
  const auto wolfVars=std::to_array(wolf.handle().aivar);
  const auto sheepVars=std::to_array(sheep.handle().aivar);
  const auto wolfExternal=wolf.isExternalControl(), sheepExternal=sheep.isExternalControl();
  const auto wolfState=NpcDefinitionContract::aiState(wolf);
  const auto place=[](Npc& actor,Tempest::Vec3 position) {
    actor.setPosition(position);
    actor.updateTransform();
    };
  const auto wolfPosition=wolf.position(), sheepPosition=sheep.position(), heroPosition=hero.position();
  const auto origin=wolfPosition+Tempest::Vec3(0,500,0);
  place(wolf,origin);
  place(sheep,origin+Tempest::Vec3(220,0,0));
  place(hero,origin+Tempest::Vec3(-220,0,0));
  require(wolf.canSeeNpc(hero,true) && wolf.canSeeNpc(sheep,true),"monster damage perception: both witnesses need an unobstructed view");
  wolf.setExternalControl(false);
  sheep.setExternalControl(false);
  std::vector<std::vector<std::string>> original;
  for(bool native:{false,true}) {
    events=native;
    std::copy(wolfVars.begin(),wolfVars.end(),wolf.handle().aivar);
    std::copy(sheepVars.begin(),sheepVars.end(),sheep.handle().aivar);
    auto& wolfAI=wolf.handle().aivar;
    wolfAI[15]=0;
    wolfAI[29]=0;
    NpcDefinitionContract::aiState(wolf)=wolfState;
    size_t index=0;
    auto perceive=[&](Npc& receiver,Npc& source,Npc* victim,PercType perception,ScriptFn handler,ScriptFn expected,Npc* target) {
      receiver.clearAiQueue();
      receiver.aiPush(AiQueue::aiWait(100));
      receiver.setPerceptionEnable(perception,handler.ptr);
      const auto handlerPc=script.findSymbol(size_t(handler.ptr))->address();
      size_t entries=0;
      vm.observe_instructions([&](const auto&) { entries+=vm.pc()==handlerPc; });
      const bool delivered=receiver.perceptionProcess(source,victim,receiver.qDistTo(source),perception);
      vm.observe_instructions({});
      require(delivered && entries==(native ? 0u : 1u),"monster damage perception: the original callback only executes in VM mode");
      const auto* action=receiver.aiQueueFront();
      require(action && receiver.target()==target,"monster damage perception: the target matches the selected response");
      if(expected.isValid())
        require(action->act==AI_StartState && action->func==expected && action->target==&source && action->victim==victim &&
                !receiver.hasPerc(perception),"monster damage perception: the transition replaces the old queue and clears perceptions");
      else
        require(action->act==AI_Wait && receiver.hasPerc(perception),"monster damage perception: an active or unassigned response preserves the queue");
      auto result=activityQueue(receiver);
      result.push_back(std::to_string(receiver.handle().aivar[22]));
      result.push_back(std::to_string(receiver.handle().aivar[29]));
      if(native)
        require(result==original[index],"monster damage perception: original and native queues and combat bookkeeping agree");
      else
        original.push_back(std::move(result));
      ++index;
      };

    wolf.setTarget(nullptr);
    sheep.setTarget(nullptr);
    perceive(wolf,hero,nullptr,PERC_ASSESSDAMAGE,direct,attack,&hero);
    require(wolfAI[29]==1,"monster damage perception: an attack sets combat priority");
    perceive(sheep,wolf,nullptr,PERC_ASSESSDAMAGE,direct,flee,&wolf);

    require(wolf.startState(attack,wolf.handle().wp),"monster damage perception: the wolf can take a hit during combat");
    NpcDefinitionContract::aiState(wolf).started=true;
    wolf.setTarget(&sheep);
    wolfAI[7]=int32_t(sheep.instanceSymbol());
    wolfAI[22]=-1;
    perceive(wolf,hero,nullptr,PERC_ASSESSDAMAGE,direct,{},&sheep);
    require(wolfAI[22]==int32_t(hero.instanceSymbol()),"monster damage perception: the first hit records the new attacker");
    perceive(wolf,hero,nullptr,PERC_ASSESSDAMAGE,direct,{},&hero);
    NpcDefinitionContract::aiState(wolf)=wolfState;

    wolfAI[15]=1;
    perceive(wolf,sheep,&hero,PERC_ASSESSOTHERSDAMAGE,witnessed,attack,&sheep);
    wolf.setTarget(nullptr);
    perceive(wolf,hero,&sheep,PERC_ASSESSMURDER,witnessed,attack,&sheep);
    wolf.setTarget(nullptr);
    perceive(wolf,sheep,nullptr,PERC_ASSESSFIGHTSOUND,witnessed,{},nullptr);
    }
  events=previousEvents;
  std::copy(wolfVars.begin(),wolfVars.end(),wolf.handle().aivar);
  std::copy(sheepVars.begin(),sheepVars.end(),sheep.handle().aivar);
  for(auto* actor:{&wolf,&sheep}) {
    actor->setTarget(nullptr);
    actor->clearAiQueue();
    }
  wolf.setExternalControl(wolfExternal);
  sheep.setExternalControl(sheepExternal);
  place(wolf,wolfPosition);
  place(sheep,sheepPosition);
  place(hero,heroPosition);
  std::puts("Monster damage perception: original and native attack, flee, retargeting and witnessed hits agree");
  }

void monsterPlayerWarnAndSurprise(World& world, Npc& wolf, Npc& sheep, Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& events=Contract::activityEventsOn(script);
  const auto previousEvents=events;
  const auto attack=script.findFunction("ZS_MM_ATTACK");
  const auto threat=script.findFunction("ZS_MM_THREATENENEMY");
  const auto player=script.findFunction("B_MM_ASSESSPLAYER");
  const auto warn=script.findFunction("B_MM_ASSESSWARN");
  const auto surprise=script.findFunction("B_MM_ASSESSSURPRISE");
  const auto dragonTalkPc=script.findSymbol("B_ASSIGNDRAGONTALK")->address();
  const auto wolfState=Contract::aiState(wolf), sheepState=Contract::aiState(sheep);
  const auto wolfPosition=wolf.position(), sheepPosition=sheep.position(), heroPosition=hero.position();
  const auto sheepGuild=sheep.handle().guild;
  const auto wolfVars=std::to_array(wolf.handle().aivar);
  auto& invincible=hero.handle().aivar[4];
  const auto previousInvincibility=invincible;
  auto* previousOther=wolf.stateOther();
  const auto previousExternal=wolf.isExternalControl();
  const auto place=[](Npc& actor,Tempest::Vec3 position) {
    actor.setPosition(position);
    actor.updateTransform();
    };
  place(sheep,wolfPosition+Tempest::Vec3(-220,0,0));
  place(hero,wolfPosition+Tempest::Vec3(220,0,0));
  wolf.setExternalControl(false);

  std::vector<std::vector<std::string>> original;
  for(bool native:{false,true}) {
    events=native;
    auto& ai=wolf.handle().aivar;
    std::copy(wolfVars.begin(),wolfVars.end(),ai);
    ai[15]=0;
    Contract::aiState(wolf)=wolfState;
    Contract::aiState(sheep)=sheepState;
    sheep.handle().guild=sheepGuild;
    wolf.setTarget(nullptr);
    wolf.setOther(nullptr);
    size_t index=0;
    auto perceive=[&](Npc& source,Npc* victim,PercType perception,ScriptFn handler,size_t expectedDragonTalk=0) {
      wolf.clearAiQueue();
      wolf.aiPush(AiQueue::aiWait(100));
      wolf.setPerceptionEnable(perception,handler.ptr);
      const auto handlerPc=script.findSymbol(size_t(handler.ptr))->address();
      size_t entries=0,dragonTalkEntries=0;
      vm.observe_instructions([&](const auto&) {
        entries+=vm.pc()==handlerPc;
        dragonTalkEntries+=vm.pc()==dragonTalkPc;
        });
      const bool delivered=wolf.perceptionProcess(source,victim,wolf.qDistTo(source),perception);
      vm.observe_instructions({});
      require(delivered && entries==(native ? 0u : 1u) && dragonTalkEntries==expectedDragonTalk,
              "monster perceptions: the handler follows the executor and preserves the real dragon dialogue call");
      auto witness=activityQueue(wolf);
      witness.push_back(std::to_string(world.npcId(wolf.target())));
      witness.push_back(std::to_string(world.npcId(wolf.stateOther())));
      witness.push_back(std::to_string(ai[3]));
      if(native)
        require(witness==original[index],"monster perceptions: original and native leave the same queue and target context");
      else
        original.push_back(std::move(witness));
      ++index;
      };

    invincible=1;
    perceive(hero,nullptr,PERC_ASSESSPLAYER,player);
    invincible=0;
    perceive(hero,nullptr,PERC_ASSESSPLAYER,player,1);
    perceive(hero,nullptr,PERC_ASSESSSURPRISE,surprise);
    require(wolf.target()==&hero && wolf.aiQueueFront() && wolf.aiQueueFront()->act==AI_Wait,
            "monster surprise: the new target does not replace the queued action");

    require(wolf.startState(attack,wolf.handle().wp),"monster warning: the receiver must enter an attack");
    Contract::aiState(wolf).started=true;
    ai[7]=int32_t(hero.instanceSymbol());
    ai[29]=1;
    perceive(sheep,&hero,PERC_ASSESSWARN,warn);
    require(wolf.target()==&hero && wolf.stateOther()==&hero,"monster warning: protect an existing combat target and restore OTHER");
    ai[29]=0;
    perceive(sheep,&hero,PERC_ASSESSWARN,warn);
    require(wolf.target()==&sheep && wolf.isAiQueueEmpty(),"monster warning: an eating predator chooses the prey instead");
    Contract::aiState(wolf)=wolfState;

    sheep.handle().guild=wolf.handle().guild;
    ai[50]=1;
    require(sheep.startState(threat,sheep.handle().wp),"monster warning: the caller must enter threat");
    perceive(sheep,&hero,PERC_ASSESSWARN,warn);
    auto commands=Contract::queue(wolf);
    require(commands.size()==3 && commands.pop().act==AI_Wait && commands.pop().act==AI_SetWalkMode &&
            commands.pop().target==&hero,"monster warning: the pack runs to the warned target without breaking the existing queue");
    require(sheep.startState(attack,sheep.handle().wp),"monster warning: the caller must enter attack");
    perceive(sheep,&hero,PERC_ASSESSWARN,warn);
    require(wolf.target()==&hero && wolf.aiQueueFront() && wolf.aiQueueFront()->func==attack,
            "monster warning: the pack joins a member's attack against the victim");
    }
  events=previousEvents;
  Contract::aiState(wolf)=wolfState;
  Contract::aiState(sheep)=sheepState;
  sheep.handle().guild=sheepGuild;
  std::copy(wolfVars.begin(),wolfVars.end(),wolf.handle().aivar);
  invincible=previousInvincibility;
  wolf.setOther(previousOther);
  wolf.setTarget(nullptr);
  wolf.setExternalControl(previousExternal);
  wolf.clearAiQueue();
  sheep.clearAiQueue();
  place(sheep,sheepPosition);
  place(hero,heroPosition);
  std::puts("Monster player, warning and surprise perceptions: original and native target context and queues agree");
  }
