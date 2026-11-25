#pragma once

void murderRulePredicates(World& world, Npc& npc, Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& state=Contract::aiState(npc).funcIni;
  const auto savedState=state;
  const auto savedType=npc.handle().type;
  const auto invoke=[&](std::string_view name,int32_t expected,const std::vector<storygraph::StoryArgument>& args) {
    const auto result=script.callStoryFunction(name,&npc,&hero,args);
    require(result.ok && result.returned.type==storygraph::NativeValue::Int && result.returned.i==expected,
            "murder rules: the prepared function must return the stock predicate");
    };

  npc.handle().aivar[54]=0;
  hero.handle().aivar[9]=0;
  invoke("C_WANTTOATTACKMURDER",1,{&npc,&hero});
  require(Contract::alias(script,"C_WANTTOATTACKMURDER.SLF")==std::static_pointer_cast<NpcInstance>(npc.handlePtr()) &&
          Contract::alias(script,"C_WANTTOATTACKMURDER.OTH")==std::static_pointer_cast<NpcInstance>(hero.handlePtr()),
          "murder rules: the original witness and culprit scratch slots are written");
  hero.handle().aivar[9]=script.intConstant("AR_KILL").value();
  invoke("C_WANTTOATTACKMURDER",0,{&npc,&hero});
  hero.handle().aivar[9]=0;

  invoke("C_NPCBELONGSTOBL",0,{&npc});
  require(Contract::alias(script,"C_NPCBELONGSTOBL.SLF")==std::static_pointer_cast<NpcInstance>(npc.handlePtr()),
          "murder rules: the bandit predicate retains its actor reference");
  for(const auto kind:{NpcType::G2_BL_MAIN,NpcType::G2_BL_AMBIENT}) {
    npc.handle().type=kind;
    invoke("C_NPCBELONGSTOBL",1,{&npc});
    }
  npc.handle().type=savedType;

  invoke("C_NPCISDOWN",0,{&npc});
  require(Contract::alias(script,"C_NPCISDOWN.SLF")==std::static_pointer_cast<NpcInstance>(npc.handlePtr()),
          "murder rules: the down predicate retains its actor reference");
  for(const auto name:{"ZS_MAGICSLEEP","ZS_UNCONSCIOUS","ZS_DEAD"}) {
    state=script.findFunction(name);
    require(state.isValid(),"murder rules: the stock inactive state must exist");
    invoke("C_NPCISDOWN",1,{&npc});
    }
  state=savedState;
  invoke("C_NPCISDOWN",1,{static_cast<Npc*>(nullptr)});
  require(!Contract::alias(script,"C_NPCISDOWN.SLF"),
          "murder rules: an absent actor is down and clears the old argument");
  }

void murderPerceptions(World& world, Npc& npc, Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  const auto callback=script.findFunction("B_ASSESSMURDER");
  const auto attack=script.findFunction("ZS_ATTACK");
  const auto guard=script.findFunction("ZS_GUARD_PASSAGE");
  const auto observe=script.findFunction("ZS_OBSERVEPLAYER");
  const auto humanReason=script.intConstant("AR_HUMANMURDEREDHUMAN").value();
  const auto monsterReason=script.intConstant("AR_MONSTERMURDEREDHUMAN").value();
  const auto sheepReason=script.intConstant("AR_SHEEPKILLER").value();
  const auto enemyReason=script.intConstant("AR_REACTTOWEAPON").value();
  const auto murderCrime=script.intConstant("CRIME_MURDER").value();
  const auto sheepCrime=script.intConstant("CRIME_SHEEPKILLER").value();
  const auto assaultCrime=script.intConstant("CRIME_ATTACK").value();
  auto* lobart=liveNpc(world,"BAU_950_LOBART");
  require(lobart && callback.isValid() && attack.isValid() && guard.isValid() && observe.isValid() &&
          npc.canSeeNpc(hero,false) && script.guildAttitude(npc,*lobart)==ATT_FRIENDLY,
          "murder perception: the fixture needs a visible culprit and an allied victim");

  const auto savedPosition=lobart->position();
  const auto savedLobartGuild=lobart->handle().guild;
  const auto savedLobartVars=std::to_array(lobart->handle().aivar);
  const auto savedHeroVars=std::to_array(hero.handle().aivar);
  const auto savedVars=std::to_array(npc.handle().aivar);
  auto& state=Contract::aiState(npc).funcIni;
  const auto savedState=state;
  const auto savedAttitude=npc.attitude();
  auto* savedTarget=npc.target();
  lobart->setPosition(npc.position()+Tempest::Vec3(450,0,0));
  lobart->updateTransform();
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
    lobart->handle().guild=sheep ? uint32_t(GIL_SHEEP) : monster ? uint32_t(script.intConstant("GIL_WOLF").value()) : savedLobartGuild;
    lobart->handle().aivar[71]=monster ? 1 : 0;
    hero.handle().aivar[52]=site==Site::QuestExemption ? 1 : 0;

    state=site==Site::Guard ? guard : enemy ? observe : savedState;
    auto& aivar=npc.handle().aivar;
    std::ranges::copy(savedVars,std::begin(aivar));
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

    const bool delivered=npc.perceptionProcess(killer,&victim,npc.qDistTo(victim),PERC_ASSESSMURDER);
    require(delivered && aivar[1]==expectedCrime,
            "murder perception: a routed witness records the original crime");
    const auto* next=npc.aiQueueFront();
    require(Contract::queue(npc).size()==1,"murder perception: each response leaves exactly one command");
    if(attacks) {
      require(next && next->act==AI_StartState && next->func==attack && next->target==&killer &&
              next->victim==&victim && next->i0==0 && npc.target()==&killer && aivar[9]==expectedReason,
              "murder perception: the attack preserves cause, culprit and victim");
      } else {
      require(next && next->act==AI_Wait && next->i0==173 && npc.target()==nullptr && npc.hasPerc(PERC_ASSESSMURDER),
              "murder perception: the guard report and quest exemption keep the waiting command");
      }
    }

  std::ranges::copy(savedVars,std::begin(npc.handle().aivar));
  std::ranges::copy(savedLobartVars,std::begin(lobart->handle().aivar));
  std::ranges::copy(savedHeroVars,std::begin(hero.handle().aivar));
  state=savedState;
  npc.setAttitude(savedAttitude);
  npc.setTarget(savedTarget);
  lobart->handle().guild=savedLobartGuild;
  lobart->setPosition(savedPosition);
  lobart->updateTransform();
  murderRulePredicates(world,npc,hero);
  std::puts("Native murder perception: six witness routes and the saved predicate arguments");
  }

void propertyCrimePerceptions(World& world, Npc& npc, Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  const auto theft=script.findFunction("B_ASSESSTHEFT");
  const auto useMob=script.findFunction("B_ASSESSUSEMOB");
  const auto attack=script.findFunction("ZS_ATTACK");
  const auto guard=script.findFunction("ZS_GUARD_PASSAGE");
  const auto hammer=script.findItemInstance("HOLY_HAMMER_MIS").value();
  const auto theftReason=script.intConstant("AR_THEFT").value();
  const auto useReason=script.intConstant("AR_USEMOB").value();
  const auto theftCrime=script.intConstant("CRIME_THEFT").value();
  constexpr std::string_view hammerFlag="HAMMER_TAKEN";
  constexpr std::string_view possessionRoom="C_ISTAKENITEMMYPOSSESSION.PORTALGUILD";
  constexpr std::string_view possessionItem="C_ISTAKENITEMMYPOSSESSION.ITM";
  auto& aivar=npc.handle().aivar;
  auto& state=Contract::aiState(npc).funcIni;
  const auto savedState=state;
  const auto savedVars=std::to_array(aivar);
  const auto savedHeroGuild=hero.handle().guild;
  auto* item=hero.addItem(hammer,1);
  constexpr std::string_view farm="FARM01";
  world.assignRoomToGuild(farm,int32_t(GIL_PUBLIC));
  require(item && theft.isValid() && useMob.isValid() && guard.isValid() && attack.isValid() &&
          !hero.interactive() && npc.canSeeNpc(hero,false) && npc.portalName()==hero.portalName() &&
          world.guildOfRoom(hero.portalName())==int32_t(GIL_PUBLIC),
          "property crime: the fixture needs an actual quest item and visible guest in the farm portal");
  const auto dropped=uint32_t(script.intConstant("ITEM_DROPPED").value());
  const auto itemFlags=uint32_t(item->handle().flags);
  Interactive* chest=nullptr;
  Interactive* furniture=nullptr;
  for(uint32_t id=0; auto* mob=world.mobsiById(id); ++id) {
    const auto scheme=mob->schemeName();
    if(scheme=="CHESTSMALL" || scheme=="CHESTBIG") chest=mob;
    else if(!scheme.empty()) furniture=mob;
    if(chest && furniture) break;
    }
  require(chest && furniture,"property crime: the world needs a chest and a piece of furniture");

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
    const auto perc=usingMob ? PERC_ASSESSUSEMOB : PERC_ASSESSTHEFT;
    const auto callback=usingMob ? useMob : theft;
    auto* mob=site==Site::UseChest ? chest : furniture;
    world.assignRoomToGuild(farm,roomGuild);
    hero.handle().guild=personallyChecked ? GIL_NONE : GIL_BDT;
    require(world.guildOfRoom(hero.portalName())==roomGuild &&
            (script.guildAttitude(npc,hero)==ATT_FRIENDLY)==personallyChecked,
            "property crime: the room and attitude must route the intended ownership case");

    state=site==Site::GuardTake ? guard : savedState;
    std::ranges::copy(savedVars,std::begin(aivar));
    aivar[0]=0;
    aivar[1]=0;
    aivar[55]=0;
    npc.clearAiQueue();
    npc.setTarget(nullptr);
    npc.aiPush(AiQueue::aiWait(173));
    npc.setPerceptionEnable(perc,callback.ptr);
    require(script.setGlobalIntSymbol(hammerFlag,0) && script.setGlobalIntSymbol(possessionRoom,-1),
            "property crime: the native session needs the quest and portal fields");
    Contract::alias(script,possessionItem).reset();
    item->handle().owner=owned ? int32_t(npc.instanceSymbol()) : -1;
    item->handle().owner_guild=site==Site::GuildOwned ? int32_t(npc.handle().guild) : int32_t(GIL_NONE);
    item->handle().flags=ItemFlag((itemFlags&~dropped) | (site==Site::Dropped || site==Site::Take ? dropped : 0u));
    script.setInstanceItem(hero,hammer);
    hero.setDetectedMob(usingMob ? mob : nullptr);

    const bool delivered=npc.perceptionProcess(hero,nullptr,npc.qDistTo(hero),perc);
    require(delivered && aivar[1]==(remembers ? theftCrime : 0),
            "property crime: the callback records the stock crime only when warranted");
    const auto* next=npc.aiQueueFront();
    require(Contract::queue(npc).size()==1,"property crime: the response leaves exactly one command");
    if(attacks) {
      require(next && next->act==AI_StartState && next->func==attack && next->target==&hero && !next->victim &&
              next->i0==0 && npc.target()==&hero && aivar[9]==(usingMob ? useReason : theftReason) &&
              !npc.hasPerc(perc),"property crime: attack preserves the target, cause and cleared perceptions");
      } else {
      require(next && next->act==AI_Wait && next->i0==173 && npc.target()==nullptr && npc.hasPerc(perc),
              "property crime: a guard report, unrelated item and non-chest preserve the waiting command");
      }
    const bool checksItem=!usingMob && site!=Site::Unowned;
    require(script.globalIntSymbol(hammerFlag)==int32_t(owned) &&
            (!usingMob || script.stringSymbol("B_ASSESSUSEMOB.DETMOB")==mob->schemeName()) &&
            script.globalIntSymbol(possessionRoom)==(checksItem ? roomGuild : -1) &&
            Contract::alias(script,possessionItem)==(checksItem ? std::static_pointer_cast<ItemInstance>(item->handlePtr()) : nullptr),
            "property crime: the hammer flag, exact item alias and detected scheme or owner must survive the callback");
    }

  world.assignRoomToGuild(farm,int32_t(GIL_PUBLIC));
  state=savedState;
  std::ranges::copy(savedVars,std::begin(aivar));
  npc.setTarget(nullptr);
  hero.setDetectedMob(nullptr);
  hero.handle().guild=savedHeroGuild;
  Contract::alias(script,"ITEM").reset();
  Contract::alias(script,possessionItem).reset();
  hero.delItem(hammer,1);
  std::puts("Native property crime: eight item and furniture ownership routes");
  }

constexpr std::array crimeRegions={"OLDCAMP","CITY","MONASTERY","FARM","BL"};

std::vector<std::string> crimeCounterNames() {
  std::vector<std::string> result;
  for(std::string_view region:crimeRegions)
    for(std::string_view crime:{"MURDER","THEFT","ATTACK","SHEEPKILLER"}) {
      if(region=="BL" && crime=="SHEEPKILLER") continue;
      result.push_back("PETZCOUNTER_"+std::string(region)+"_"+std::string(crime));
      }
  return result;
  }

void crimeMemoryCases(World& world,Npc& witness,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& record=witness.handle();
  const auto date=int32_t(world.time().day());
  const auto murder=script.intConstant("CRIME_MURDER").value(), theft=script.intConstant("CRIME_THEFT").value();
  const auto attack=script.intConstant("CRIME_ATTACK").value(), sheep=script.intConstant("CRIME_SHEEPKILLER").value();
  const auto none=script.intConstant("CRIME_NONE").value();
  const auto oldGuild=record.guild;
  const auto oldType=record.type;
  const auto oldVars=std::to_array(record.aivar);
  constexpr std::array regionalWitness={"C_NPCBELONGSTOOLDCAMP.SLF","C_NPCBELONGSTOCITY.SLF","C_NPCBELONGSTOMONASTERY.SLF",
                                        "C_NPCBELONGSTOFARM.SLF","C_NPCBELONGSTOBL.SLF"};
  constexpr std::string_view input="B_ADDPETZCRIME.CRIME";
  std::map<std::string,int32_t> saved;
  const auto counters=crimeCounterNames();
  const auto set=[&](std::string_view name,int32_t value) {
    if(!saved.contains(std::string(name)))
      saved.emplace(name,script.globalIntSymbol(name).value());
    require(script.setGlobalIntSymbol(name,value),"crime memory: a named session field must be writable");
    };
  for(std::string_view area:crimeRegions)
    set("ABSOLUTIONLEVEL_"+std::string(area),area=="BL" ? 7 : 3);

  struct Case {
    Guild guild;
    NpcType type;
    int32_t previous,age,level,crime;
    size_t jurisdiction;
    const char* added;
    const char* removed;
    const char* extraAdded=nullptr;
    const char* extraRemoved=nullptr;
    };
  const Case cases[] = {
    {GIL_VLK,NpcType::G2_AMBIENT,attack,2,3,theft,1,"PETZCOUNTER_CITY_THEFT","PETZCOUNTER_CITY_ATTACK"},
    {GIL_VLK,NpcType::G2_BL_AMBIENT,theft,7,3,murder,1,"PETZCOUNTER_CITY_MURDER","PETZCOUNTER_CITY_THEFT","PETZCOUNTER_BL_MURDER","PETZCOUNTER_BL_THEFT"},
    {GIL_MIL,NpcType::G2_OC_MAIN,attack,3,3,murder,0,"PETZCOUNTER_OLDCAMP_MURDER",nullptr},
    {GIL_NOV,NpcType::G2_AMBIENT,murder,0,2,theft,2,"PETZCOUNTER_MONASTERY_THEFT",nullptr},
    {GIL_BAU,NpcType::G2_AMBIENT,theft,7,3,murder,3,"PETZCOUNTER_FARM_MURDER","PETZCOUNTER_FARM_THEFT"},
    {GIL_BDT,NpcType::G2_BL_MAIN,none,0,7,sheep,4,nullptr,nullptr},
    {GIL_NONE,NpcType::G2_AMBIENT,none,0,0,murder,crimeRegions.size(),nullptr,nullptr},
    {GIL_VLK,NpcType::G2_AMBIENT,attack,2,3,attack,1,nullptr,nullptr},
    };
  for(const auto& c:cases) {
    record.guild=c.guild;
    record.type=c.type;
    record.aivar[1]=c.previous;
    record.aivar[2]=date-c.age;
    record.aivar[46]=c.level;
    set(input,-1);
    for(const auto& name:counters)
      set(name,0);
    for(const auto name:regionalWitness)
      Contract::alias(script,name)=std::static_pointer_cast<NpcInstance>(hero.handlePtr());
    Contract::alias(script,"B_ADDPETZCRIME.SLF")=std::static_pointer_cast<NpcInstance>(hero.handlePtr());
    Contract::alias(script,"B_DELETEPETZCRIME.SLF")=std::static_pointer_cast<NpcInstance>(hero.handlePtr());
    const auto result=script.callStoryFunction("B_MEMORIZEPLAYERCRIME",&witness,&hero,{&witness,&hero,c.crime});
    const bool equal=c.previous==c.crime;
    const int32_t level=c.guild==GIL_NONE ? 0 : (c.guild==GIL_BDT ? 7 : 3);
    require(result.ok && record.aivar[1]==c.crime && record.aivar[2]==date && record.aivar[46]==level &&
            script.globalIntSymbol(input)==(equal ? -1 : c.crime) &&
            Contract::alias(script,"B_MEMORIZEPLAYERCRIME.SLF")==std::static_pointer_cast<NpcInstance>(witness.handlePtr()) &&
            Contract::alias(script,"B_MEMORIZEPLAYERCRIME.TAETER")==std::static_pointer_cast<NpcInstance>(hero.handlePtr()) &&
            Contract::alias(script,"B_GETCURRENTABSOLUTIONLEVEL.SLF")==std::static_pointer_cast<NpcInstance>(witness.handlePtr()) &&
            Contract::alias(script,"B_ADDPETZCRIME.SLF")== (equal ? std::static_pointer_cast<NpcInstance>(hero.handlePtr()) : std::static_pointer_cast<NpcInstance>(witness.handlePtr())) &&
            Contract::alias(script,"B_DELETEPETZCRIME.SLF")== (equal ? std::static_pointer_cast<NpcInstance>(hero.handlePtr()) : std::static_pointer_cast<NpcInstance>(witness.handlePtr())),
            "crime memory: the type, date, absolution, culprit and same-crime refresh must match stock");
    for(size_t i=0;i<regionalWitness.size();++i)
      require(Contract::alias(script,regionalWitness[i])==(!equal || i<=c.jurisdiction ? std::static_pointer_cast<NpcInstance>(witness.handlePtr()) : std::static_pointer_cast<NpcInstance>(hero.handlePtr())),
              "crime memory: regional helpers retain exactly the witness references they visited");
    for(const auto& name:counters) {
      const auto matches=[&](const char* expected) { return expected && name==expected; };
      const int32_t expected=matches(c.added) || matches(c.extraAdded) ? 1 :
                            matches(c.removed) || matches(c.extraRemoved) ? -1 : 0;
      require(script.globalIntSymbol(name)==expected,
              ("crime memory: unexpected regional counter "+name).c_str());
      }
    }

  record.guild=GIL_VLK;
  record.type=NpcType::G2_BL_AMBIENT;
  for(const auto& name:counters)
    set(name,0);
  set("PETZCOUNTER_CITY_MURDER",-1);
  set("PETZCOUNTER_BL_MURDER",2);
  const auto query=[&](std::string_view function,int32_t expected) {
    const auto result=script.callStoryFunction(function,&witness,&hero,{&witness});
    require(result.ok && result.returned.type==storygraph::NativeValue::Int && result.returned.i==expected,
            ("crime report: unexpected result from "+std::string(function)).c_str());
    };
  query("B_GETGREATESTPETZCRIME",murder);
  query("B_GETTOTALPETZCOUNTER",-1);
  set("PETZCOUNTER_CITY_THEFT",std::numeric_limits<int32_t>::max());
  set("PETZCOUNTER_CITY_SHEEPKILLER",2);
  set("ABSOLUTIONLEVEL_CITY",13);
  query("B_GETGREATESTPETZCRIME",theft);
  query("B_GETTOTALPETZCOUNTER",std::numeric_limits<int32_t>::min());
  query("B_GETCURRENTABSOLUTIONLEVEL",13);
  require(Contract::alias(script,"B_GETGREATESTPETZCRIME.SLF")==std::static_pointer_cast<NpcInstance>(witness.handlePtr()) &&
          Contract::alias(script,"B_GETTOTALPETZCOUNTER.SLF")==std::static_pointer_cast<NpcInstance>(witness.handlePtr()),
          "crime report: both regional results bind the caller's live witness");

  record.guild=oldGuild;
  record.type=oldType;
  std::ranges::copy(oldVars,std::begin(record.aivar));
  for(const auto& [name,value]:saved)
    require(script.setGlobalIntSymbol(name,value),"crime memory: restore encounter values after the comparison");
  std::puts("Native crime memory: exact regional counters, two-day expiry, witness references and report priority");
  }
