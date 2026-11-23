#pragma once

void murderPerceptions(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const bool wasEnabled=enabled;
  const auto callback=script.findFunction("B_ASSESSMURDER");
  const auto pc=vm.find_symbol_by_name("B_ASSESSMURDER")->address();
  const auto attack=script.findFunction("ZS_ATTACK");
  const auto guard=script.findFunction("ZS_GUARD_PASSAGE");
  const auto observe=script.findFunction("ZS_OBSERVEPLAYER");
  const auto humanReason=script.intSymbol("AR_HUMANMURDEREDHUMAN",-1);
  const auto monsterReason=script.intSymbol("AR_MONSTERMURDEREDHUMAN",-1);
  const auto sheepReason=script.intSymbol("AR_SHEEPKILLER",-1);
  const auto enemyReason=script.intSymbol("AR_REACTTOWEAPON",-1);
  const auto murderCrime=script.intSymbol("CRIME_MURDER",-1);
  const auto sheepCrime=script.intSymbol("CRIME_SHEEPKILLER",-1);
  const auto assaultCrime=script.intSymbol("CRIME_ATTACK",-1);
  auto* lobart=activityNpc(world,"BAU_950_LOBART");
  require(lobart && npc.canSeeNpc(hero,false) && script.guildAttitude(npc,*lobart)==ATT_FRIENDLY,
          "murder perception: the fixture has a visible player and an allied human victim");
  const auto savedPosition=lobart->position();
  lobart->setPosition(npc.position()+Tempest::Vec3(450,0,0));
  lobart->updateTransform();
  auto& state=Contract::aiState(npc).funcIni;
  auto& aivar=npc.handle().aivar;
  const auto savedState=state;
  const auto savedAivar=std::to_array(aivar);
  const auto savedAttitude=npc.attitude();
  auto* savedTarget=npc.target();
  const auto savedLobartGuild=lobart->handle().guild;
  const auto savedLobartTough=lobart->handle().aivar[57];
  const auto savedLobartParker=lobart->handle().aivar[71];
  const auto savedLobartQuest=lobart->handle().aivar[52];
  const auto savedHeroReason=hero.handle().aivar[9];
  const auto savedHeroQuest=hero.handle().aivar[52];
  hero.handle().aivar[9]=0;
  lobart->handle().aivar[57]=0;
  lobart->handle().aivar[52]=0;

  enum class Site { Human, Guard, Sheep, Monster, QuestExemption, EnemyPriority };
  for(const auto site:{Site::Human,Site::Guard,Site::Sheep,Site::Monster,Site::QuestExemption,Site::EnemyPriority}) {
    const bool sheep=site==Site::Sheep;
    const bool monster=site==Site::Monster;
    const bool enemy=site==Site::EnemyPriority;
    const bool attacks=site!=Site::Guard && site!=Site::QuestExemption;
    const bool recordsMurder=site==Site::Human || site==Site::Guard;
    const int expectedReason=enemy ? enemyReason : sheep ? sheepReason : monster ? monsterReason : humanReason;
    const int expectedCrime=enemy ? assaultCrime : sheep ? sheepCrime : recordsMurder ? murderCrime : 0;
    auto& killer=monster ? *lobart : hero;
    auto& victim=monster ? hero : *lobart;
    lobart->handle().guild=sheep ? uint32_t(GIL_SHEEP) : monster ? uint32_t(script.intSymbol("GIL_WOLF",-1)) : savedLobartGuild;
    lobart->handle().aivar[71]=monster ? 1 : 0;
    hero.handle().aivar[52]=site==Site::QuestExemption ? 1 : 0;

    std::vector<std::string> original;
    for(bool native:{false,true}) {
      enabled=native;
      state=site==Site::Guard ? guard : enemy ? observe : savedState;
      std::copy(savedAivar.begin(),savedAivar.end(),aivar);
      aivar[1]=0;
      aivar[15]=0;
      aivar[47]=enemyReason;
      aivar[54]=0;
      aivar[56]=0;
      aivar[61]=0;
      npc.setAttitude(enemy ? ATT_HOSTILE : ATT_NEUTRAL);
      npc.clearAiQueue();
      npc.setTarget(nullptr);
      npc.aiPush(AiQueue::aiWait(173));
      npc.setPerceptionEnable(PERC_ASSESSMURDER,callback.ptr);
      size_t bytecode=0;
      vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==pc; });
      const bool delivered=npc.perceptionProcess(killer,&victim,npc.qDistTo(victim),PERC_ASSESSMURDER);
      vm.observe_instructions({});
      require(delivered && bytecode==(native ? 0u : 1u) && aivar[1]==expectedCrime,
              "murder perception: a routed witness uses the selected executor and records the original crime");
      const auto* action=npc.aiQueueFront();
      if(attacks) {
        require(action && action->act==AI_StartState && action->func==attack && action->target==&killer &&
                action->victim==&victim && action->i0==0 && npc.target()==&killer && aivar[9]==expectedReason,
                "murder perception: the attack preserves cause, culprit and victim");
        } else {
        require(action && action->act==AI_Wait && npc.target()==nullptr && npc.hasPerc(PERC_ASSESSMURDER),
                "murder perception: the guard report and quest exemption keep the original queue");
        }
      auto queue=activityQueue(npc);
      if(native) require(original==queue,"murder perception: action queues match the VM");
      else original=std::move(queue);
      }
    }
  enabled=wasEnabled;
  std::copy(savedAivar.begin(),savedAivar.end(),aivar);
  state=savedState;
  npc.setAttitude(savedAttitude);
  npc.setTarget(savedTarget);
  lobart->handle().guild=savedLobartGuild;
  lobart->handle().aivar[57]=savedLobartTough;
  lobart->handle().aivar[71]=savedLobartParker;
  lobart->handle().aivar[52]=savedLobartQuest;
  hero.handle().aivar[9]=savedHeroReason;
  hero.handle().aivar[52]=savedHeroQuest;
  lobart->setPosition(savedPosition);
  lobart->updateTransform();
  std::puts("Murder perception: witness, quest exemptions and four attack reasons match the VM");
  }
