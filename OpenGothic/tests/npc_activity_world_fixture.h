// Live world fixture and point activity cases for placed_items_contract.cpp.

bool activityCandidate(Npc& npc) { return !npc.isPlayer() && npc.isHuman() && !npc.isDead(); }

void loadSavedNpc(Npc& npc,const std::vector<uint8_t>& bytes) {
  Tempest::MemReader reader(bytes);
  Serialize in(reader);
  in.setContext(&npc.world());
  npc.load(in,0,"/npc/");
  npc.postValidate();
  npc.setSimTier(NpcSimTier::Full);
  }

struct EventActor {
  Npc*            npc=nullptr;
  ActivityPhases  phases;
  const WayPoint* point=nullptr;
  std::string     pointName, state, scheme;
  gtime           rowStart;
  };

class ActivityEventsWorld final {
  public:
    explicit ActivityEventsWorld(Gothic& gothic):gothic(gothic) {}

    World& world() { return *gothic.world(); }
    MobReservations& places() { return world().mobReservations(); }
    static ActivityEvents::Stage stage(const Npc& npc) { return npc.activityStage(); }

    void advance(uint64_t dt) {
      gothic.gameSession()->tick(dt);
      gothic.gameSession()->updateAnimation(dt);
      }
    void moveHero(const Tempest::Vec3& at, float heading) {
      auto& hero=*world().player();
      hero.setPosition(at);
      hero.setDirection(heading);
      hero.updateTransform();
      auto& camera=*gothic.camera();
      camera.setViewport(640,480);
      camera.reset(&hero);
      world().updateLandscape(camera);
      world().updateNpcSimTiers();
      }
    void step(size_t ticks=1) {
      for(size_t i=0;i<ticks;++i)
        advance(33);
      }
    size_t stepUntil(size_t limit,const std::function<bool()>& done,const std::string& what,
                     const std::function<void()>& each={}) {
      for(size_t i=0;i<=limit;++i) {
        if(each) each();
        if(done()) return i;
        if(i<limit) step();
        }
      throw std::runtime_error("activity events: "+what+": not within "+std::to_string(limit)+" ticks");
      }
    // Only `actors` run their routines; the rest of the world stands still.
    void only(std::initializer_list<Npc*> actors) {
      for(uint32_t i=0;i<world().npcCount();++i) {
        auto* npc=world().npcById(i);
        npc->setExternalControl(std::find(actors.begin(),actors.end(),npc)==actors.end());
        }
      }
    void release() {
      for(uint32_t i=0;i<world().npcCount();++i)
        world().npcById(i)->setExternalControl(false);
      }
    void freePoint(const WayPoint* p) {
      for(uint32_t i=0;i<world().npcCount();++i)
        if(world().npcById(i)->currentWayPoint()==p)
          world().npcById(i)->attachToPoint(nullptr);
      }
    EventActor actor(const char* state,const char* scheme,Npc* fixed) {
      EventActor a;
      a.phases=activityPhases(world().script(),state);
      a.state=state;
      a.scheme=scheme;
      for(uint32_t i=0;i<world().npcCount() && a.point==nullptr;++i) {
        auto* npc=fixed!=nullptr ? fixed : world().npcById(i);
        if(fixed==nullptr && (!activityCandidate(*npc) || used.contains(npc))) continue;
        if((a.point=activityRow(*npc,a.phases.init,a.pointName,&a.rowStart))!=nullptr) a.npc=npc;
        }
      require(a.npc!=nullptr,(std::string("NEWWORLD must have an NPC with a ")+state+" row").c_str());
      used.insert(a.npc);
      return a;
      }
    // Another living human for an extra role: one symbol of its kind, so a load finds it again.
    Npc* extra() {
      for(uint32_t i=0;i<world().npcCount();++i) {
        auto* npc=world().npcById(i);
        if(!activityCandidate(*npc) || npc->isImmortal() || used.contains(npc)) continue;
        size_t same=0;
        for(uint32_t j=0;j<world().npcCount();++j)
          same+=world().npcById(j)->instanceSymbol()==npc->instanceSymbol() ? 1 : 0;
        if(same!=1) continue;
        used.insert(npc);
        return npc;
        }
      throw std::runtime_error("activity events: no spare NPC");
      }
    // The nearest point of the way net, by its links, at least `cm` (4 m) from `point`: a walk to it is a real walk.
    static const WayPoint* awayFrom(const WayPoint& point,float cm=400.f) {
      std::vector<const WayPoint*> queue={&point};
      std::set<const WayPoint*> seen={&point};
      for(size_t i=0;i<queue.size() && i<256;++i) {
        if(queue[i]->qDistTo(point.position())>cm*cm) return queue[i];
        for(const auto& c:queue[i]->connections())
          if(c.point!=nullptr && seen.insert(c.point).second) queue.push_back(c.point);
        }
      throw std::runtime_error("activity events: no way point "+std::to_string(int(cm))+" cm from "+point.name);
      }
    // The clock a minute into the activity's routine row.
    void rowTime(const EventActor& a) {
      gothic.gameSession()->setTime(gtime(world().time().day(),int32_t(a.rowStart.hour()),int32_t(a.rowStart.minute())+1));
      }
    // A fresh start of the activity: the clock inside its row, the body at `at`, Init on the next tick.
    void start(EventActor& a,const Tempest::Vec3& at,Npc* body=nullptr,bool clock=true) {
      auto& npc=body!=nullptr ? *body : *a.npc;
      if(clock)
        rowTime(a);
      detach(npc);
      npc.clearAiQueue();
      npc.clearState(true);
      NpcDefinitionContract::aiState(npc)={};
      npc.setPosition(at);
      npc.handle().wp=a.pointName;
      auto end=world().time();
      end.addMilis(6*60*60*1000);
      require(npc.startState(ScriptFn(a.phases.init),a.pointName,end,false),("the NPC must enter "+a.state).c_str());
      }
    // A quick detach leaves the object in its last state, which turns the next user back: reset it.
    static void detach(Npc& npc) {
      auto* mob=npc.interactive();
      npc.setInteraction(nullptr,true);
      if(mob!=nullptr && mob->userCount()==0)
        mob->resetPositionToTA(-1);
      }
    // `who` leaves its object the way a routine does, through AI_UseMob with the leaving state, and then
    // goes to `to`, out of the way of the next user, as its next routine row would take it.
    void leave(Npc& who,const std::string& scheme,const WayPoint& to) {
      who.aiPush(AiQueue::aiUseMob(scheme,-1));
      stepUntil(900,[&] { return who.interactive()==nullptr; },"another NPC leaves its "+scheme);
      who.aiPush(AiQueue::aiGoToPoint(to));
      }
    void stop(Npc& npc) {
      detach(npc);
      npc.clearAiQueue();
      npc.clearState(true);
      NpcDefinitionContract::aiState(npc)={};
      }
    // `who` uses `mob` through its own queue from `at` (external control still runs the queue) and stays.
    void occupy(Npc& who,Interactive& mob,const std::string& scheme,const Tempest::Vec3& at) {
      stop(who);
      who.setPosition(at);
      who.aiPush(AiQueue::aiUseMob(scheme,1));
      stepUntil(900,[&] { return who.interactive()==&mob && who.isAiQueueEmpty(); },"another NPC uses "+scheme);
      }
    // Init and the look right after it, without the world tick: the reservation is held and the use queued.
    void reserveNow(EventActor& a,Npc& npc) {
      NpcDefinitionContract::tickRoutine(npc);
      activityDrain(npc);
      NpcDefinitionContract::tickRoutine(npc);
      require(stage(npc)==ActivityEvents::Stage::Attach && places().reserved(npc)!=nullptr,
              (a.state+": the look after the queue must reserve the object and queue its use").c_str());
      }
    const ActivityEvents::StateStats& stats(const std::string& state) {
      static const ActivityEvents::StateStats none;
      const auto& states=ActivityEvents::stats().states;
      const auto it=states.find(state);
      return it==states.end() ? none : it->second;
      }
    // Nothing of the activity ran between two copies of its counters: no place check, no wake of any kind.
    static bool quiet(const ActivityEvents::StateStats& before,const ActivityEvents::StateStats& after) {
      return before.steps==after.steps && std::equal(std::begin(before.wakes),std::end(before.wakes),std::begin(after.wakes));
      }
    // A point of the way net, by its links from `a`'s point, 3 to 9 m from `mob` and reaching it as the nearest free
    // object of the scheme: an activity there walks to the object with its use queued. Not `skip`.
    const WayPoint* nearObject(const EventActor& a,Npc& npc,Interactive& mob,const WayPoint* skip=nullptr) {
      std::vector<const WayPoint*> queue={a.point};
      std::set<const WayPoint*> seen={a.point};
      for(size_t i=0;i<queue.size() && i<256;++i) {
        npc.setPosition(queue[i]->position());
        const float d=std::sqrt(npc.qDistTo(mob));
        if(queue[i]!=a.point && queue[i]!=skip && d>300.f && d<900.f && world().availableMob(npc,a.scheme)==&mob)
          return queue[i];
        for(const auto& c:queue[i]->connections())
          if(c.point!=nullptr && seen.insert(c.point).second) queue.push_back(c.point);
        }
      throw std::runtime_error("activity events: no way point 3-9 m from the "+a.scheme);
      }
    // The first free object of the world for `test`, with an attach point for `npc`.
    Interactive* object(const std::function<bool(Interactive&)>& test) {
      for(uint32_t i=0;auto* o=world().mobsiById(i);++i)
        if(o->isAvailable() && test(*o))
          return o;
      throw std::runtime_error("activity events: no such object");
      }
    // The wakes between two copies of the counters, for a failure message.
    static std::string woke(const ActivityEvents::StateStats& before,const ActivityEvents::StateStats& after) {
      std::string ret;
      for(size_t i=0;i<ActivityEvents::WakeCount;++i)
        if(after.wakes[i]!=before.wakes[i])
          ret+=" wake "+std::to_string(i)+" +"+std::to_string(after.wakes[i]-before.wakes[i]);
      return ret;
      }
    bool holds(const EventActor& a,const Npc& npc,const Interactive* mob=nullptr) {
      return stage(npc)==ActivityEvents::Stage::Hold && npc.interactive()!=nullptr &&
             (mob==nullptr || npc.interactive()==mob) && npc.interactive()->checkMobName(a.scheme);
      }
    // After an exit: the NPC gave its place up, nobody holds a reservation without its owner.
    void left(const EventActor& a,Npc& npc,Interactive& mob,Npc& other,uint64_t ownerless,const std::string& how) {
      places().audit(true);
      require(npc.interactive()!=&mob && places().reserved(npc)!=&mob,(a.state+" "+how+": the NPC must leave its place").c_str());
      require(places().isFree(mob,other),(a.state+" "+how+": the place must be free for another NPC").c_str());
      require(ActivityEvents::stats().ownerless==ownerless,(a.state+" "+how+": no reservation may lose its owner").c_str());
      }

  private:
    Gothic&        gothic;
    std::set<Npc*> used;
  };

void guardActivityWorld(Gothic& gothic) {
  using ActivityEvents::Stage;
  using Contract = NpcDefinitionContract;
  ActivityEventsWorld test(gothic);
  auto& world = test.world();
  auto& script = world.script();
  Contract::activityEventsOn(script)=true;
  auto& first = *test.extra();
  auto* khaled=activityNpc(world,"SLD_823_KHALED");
  require(khaled && khaled!=&first,"Khaled guards the farm");
  auto& second = *khaled;
  ActivityCalls calls;
  EventActor guard;
  guard.state = "ZS_STAND_GUARDING";
  guard.phases = activityPhases(script,guard.state.c_str());
  guard.pointName = "NW_CITY_HABOUR_POOR_AREA_PATH_20";
  const auto* wp = world.findWayPoint(guard.pointName);
  const auto* post = world.findPoint("FP_STAND_CITY_20",false);
  require(wp && post,"the harbour row has a single STAND post");
  const auto* record = script.eventActivity(ScriptFn(guard.phases.init));
  require(record && record->point,"the guard uses the imported point record");
  const auto& a = *record->point;
  test.only({});
  auto init = [&](Npc& npc,const WayPoint& row) {
    guard.pointName=row.name;
    test.start(guard,row.position(),&npc,false);
    npc.attachToPoint(&row);
    npc.updateTransform();
    Contract::tickRoutine(npc);
    require(npc.isInState(ScriptFn(guard.phases.init)) && npc.handle().aivar[19]==a.initialStage,
            "the guard keeps its script symbol and Init stage");
    activityDrain(npc);
    };
  auto select = [&](Npc& npc) {
    init(npc,*wp);
    Contract::tickRoutine(npc);
    require(npc.aiQueueFront() && npc.aiQueueFront()->point==post && npc.handle().aivar[19]==a.walkingStage,
            "both guards select the same unlocked post");
    };
  auto entering = [&](const Npc& npc) {
    auto queue=Contract::queue(npc);
    while(queue.size()>0) {
      const auto action=queue.pop();
      if(action.act==AI_PlayAnim && action.s0==a.enterAnimation) return true;
      }
    return false;
    };
  test.freePoint(post);
  select(first);
  select(second);
  Contract::nextAiAction(first);
  second.aiPush(AiQueue::aiLookAtNpc(&first));
  Contract::nextAiAction(second);
  require(first.currentWayPoint()==post && post->useCounter()==1 && second.currentWayPoint()==wp &&
          second.aiQueueFront() && second.aiQueueFront()->act==AI_LookAtNpc,"one owner, the losing walk preserves external commands");
  Contract::nextAiAction(second);
  Contract::tickRoutine(second);
  require(second.activityStage()==Stage::Wait && second.handle().aivar[19]==a.poseStage &&
          second.lookAtTarget()==&first && entering(second),"the loser queues the recorded pose after the external look");

  auto reload = [](Npc& npc) { loadSavedNpc(npc,savedNpc(npc)); };
  first.aiPush(AiQueue::aiLookAtNpc(&second));
  const auto queue = activityQueue(first);
  reload(first);
  require(Contract::walking(first) && activityQueue(first)==queue && first.activityStage()==Stage::Off,
          "loading in flight preserves the walk and every saved command");
  test.only({&first,&second});
  test.stepUntil(900,[&]{ return first.activityStage()==Stage::Hold; },"loaded guard reaches the post");
  require(first.lookAtTarget()==&second && entering(first),"the owner queues the recorded pose after the saved look");
  test.stepUntil(900,[&]{ return first.activityStage()==Stage::Hold && first.isAiQueueEmpty() &&
                                  second.isAiQueueEmpty() && Contract::activityEvents(second).pending==0; },"loaded guard arrives");
  require(first.lookAtTarget()==&second && first.isOnFreePoint(a.point) && first.handle().aivar[19]==a.poseStage &&
          second.activityStage()==Stage::Wait && calls.phase(first,guard.state+"_LOOP")==0,
          "the saved look ran before taking the guard pose, with no Loop");

  // Hold a second reference across the in-place reload. The saved transition then releases the last one.
  FpLock last(post);
  const auto talk=script.findFunction("ZS_TALK");
  first.aiPush(AiQueue::aiStartState(talk,1,world.player(),nullptr,""));
  reload(first);
  last = FpLock();
  require(post->useCounter()==1 && !(Contract::activityEvents(second).pending & ActivityEvents::Freed),
          "the first release leaves the waiter asleep");
  Contract::nextAiAction(first);
  require(first.isInState(talk) && calls.phase(first,guard.state+"_END")==1 && first.aiQueueFront() &&
          first.aiQueueFront()->s0==a.leaveAnimation && post->useCounter()==0 &&
          (Contract::activityEvents(second).pending & ActivityEvents::Freed),
          "the saved transition releases the last guard lock and runs End before the first routine tick");
  test.stop(first);
  test.only({&second});
  test.stepUntil(900,[&]{ return second.activityStage()==Stage::Hold && second.isAiQueueEmpty(); },"waiter takes the freed post");
  require(second.currentWayPoint()==post && post->useCounter()==1,"the waiting guard owns the freed post once");

  // npc_goto clears the queue and moves the body directly, including teleports below 300 cm.
  auto relocate = [](Npc& npc,const Tempest::Vec3& pos) {
    npc.setInteraction(nullptr,true);
    npc.clearAiQueue();
    npc.setPosition(pos);
    npc.updateTransform();
    };
  init(first,*wp);
  Contract::tickRoutine(first);
  require(first.activityStage()==Stage::Wait && !Contract::activityEvents(first).waitPoints.empty(),"the second guard waits for the owner");
  relocate(first,wp->groundPos+Tempest::Vec3(240,0,0));
  require(first.currentWayPoint()==nullptr && Contract::activityEvents(first).waitPoints.empty(),"a moved waiter leaves its old row and subscription");
  test.only({&first});
  test.stepUntil(900,[&]{ return first.currentWayPoint()==wp && !Contract::activityEvents(first).waitPoints.empty(); },"the waiter returns to its row");
  relocate(second,post->groundPos+Tempest::Vec3(240,0,0));
  require(second.currentWayPoint()==nullptr && post->useCounter()==0 && (Contract::activityEvents(first).pending & ActivityEvents::Freed),
          "moving the owner past the post boundary immediately frees the post and wakes its waiter");
  test.stepUntil(900,[&]{ return first.activityStage()==Stage::Hold; },"the waiter takes the abandoned post");
  test.stop(first);
  test.only({&second});

  // Khaled starts closer to 9294 but chooses 12203, which has the same name.
  const auto* farm=world.findWayPoint("NW_BIGFARM_HOUSE_12");
  const auto* near=world.findPoint("FP_STAND_NW_BIGFARM_HOUSE_LEE_GUARDING",false);
  const auto* upper=world.findPoint("FP_STAND_NW_BIGFARM_HOUSE_UP2_01",false);
  const WayPoint* other=nullptr;
  world.forEachWayPoint([&](const WayPoint& p) { if(near && p.name==near->name && &p!=near) other=&p; });
  require(farm && near && other && upper,"the farm row has two identically named guard posts");
  if(near->qDistTo(farm->position())>other->qDistTo(farm->position())) std::swap(near,other);
  test.freePoint(near);
  test.freePoint(other);
  {
    FpLock blocked(near),upstairs(upper);
    init(second,*farm);
    Contract::tickRoutine(second);
    require(second.aiQueueFront() && second.aiQueueFront()->point==other,"Khaled chooses the free far post");
    blocked=FpLock();
    init(first,*farm);
    Contract::tickRoutine(first);
    require(first.aiQueueFront() && first.aiQueueFront()->point==near,"another guard chooses the now free near post");
    reload(second);
    reload(first);
    require(second.aiQueueFront() && second.aiQueueFront()->point==other && first.aiQueueFront() && first.aiQueueFront()->point==near &&
            near->useCounter()==0 && other->useCounter()==0,"both unexecuted walks retain their distinct targets without acquiring them");
    Contract::nextAiAction(second);
    Contract::nextAiAction(first);
    require(second.currentGoToPoint()==other && second.qDistTo(near)<second.qDistTo(other),"save early in the walk");
    reload(second);
    require(second.currentGoToPoint()==other && second.currentWayPoint()==other &&
            near->useCounter()==1 && other->useCounter()==1,"the saved walk and lock keep post 12203");
    test.stepUntil(900,[&]{ return second.activityStage()==Stage::Hold && second.isAiQueueEmpty(); },"Khaled reaches the far post");
    test.stop(first);
  }
  auto* roll=script.findSymbol("ZS_STAND_GUARDING_LOOP.RANDOM");
  require(roll!=nullptr,"the original guard Loop has a saved RANDOM symbol");
  const auto originalRoll=roll->get_int();
  roll->set_int(0);
  second.setStateTime(6000);
  reload(second);
  const auto before = Contract::random(script);
  Contract::tickRoutine(second);
  require(second.activityStage()==Stage::Hold && second.currentWayPoint()==other && other->useCounter()==1 &&
          second.isAiQueueEmpty() && second.stateTime()==6000 && Contract::random(script)==before && roll->get_int()==0,
          "loading the pose resolves the held post and preserves the due draw without replaying it");

  second.setStateTime(5999);
  Contract::tickRoutine(second);
  require(second.stateTime()==5999 && Contract::random(script)==before,"no draw before the sixth whole second");
  for(const auto seed:{1u,4u}) {
    script.setSeed(seed);
    auto expected = Contract::random(script);
    const auto draw = expected()%uint32_t(a.rollRange);
    second.setStateTime(6000);
    Contract::tickRoutine(second);
    const auto* action = second.aiQueueFront();
    require(Contract::random(script)==expected && calls.lastScriptRoll==&second && roll->get_int()==int32_t(draw) && second.stateTime()==0 &&
            (a.gestures[draw].animation.empty() ? action==nullptr :
             action && action->act==AI_PlayAnim && action->s0==a.gestures[draw].animation),
            "the due gesture consumes one draw, queues its recorded outcome and resets state time");
    }
  require(calls.phase(second,guard.state+"_LOOP")==0,"holding and gesturing never call the script Loop");
  script.setSeed(1);
  roll->set_int(0);
  second.setStateTime(6000);
  script.invokeState(&second,nullptr,nullptr,ScriptFn(guard.phases.loop));
  require(roll->get_int()==5,"the original Loop writes the same seeded draw");
  roll->set_int(originalRoll);
  test.stop(second);
  guard.pointName=wp->name;
  auto checkPolled = [&](bool deferred) {
    test.start(guard,wp->position(),&second,false);
    second.attachToPoint(wp);
    second.updateTransform();
    const auto loops = calls.phase(second,guard.state+"_LOOP");
    if(deferred) script.startupFloor().begin();
    Contract::tickRoutine(second);
    if(deferred) script.startupFloor().end();
    activityDrain(second);
    Contract::tickRoutine(second);
    require(second.activityStage()==Stage::Off && calls.phase(second,guard.state+"_LOOP")==loops+1,
            "the original guard Loop runs for a disabled, missing or deferred activity");
    test.stop(second);
    };
  Contract::activityEventsOn(script)=false;
  checkPolled(false);
  Contract::activityEventsOn(script)=true;
  ActivityDefinitionRuntime::Definitions empty;
  std::swap(Contract::activities(script),empty);
  checkPolled(false);
  std::swap(Contract::activities(script),empty);
  checkPolled(true);

  // This STAND user still runs the original AI_GotoFP with events enabled.
  EventActor dementor=guard;
  dementor.state="ZS_STAND_DEMENTOR";
  dementor.phases=activityPhases(script,dementor.state.c_str());
  first.setSimTier(NpcSimTier::Full);
  second.setSimTier(NpcSimTier::Full);
  auto scriptSelect = [&](Npc& npc,EventActor& actor) {
    test.start(actor,wp->position(),&npc,false);
    npc.attachToPoint(wp);
    npc.updateTransform();
    Contract::tickRoutine(npc);
    activityDrain(npc);
    const auto loops=calls.phase(npc,actor.state+"_LOOP");
    Contract::tickRoutine(npc);
    require(calls.phase(npc,actor.state+"_LOOP")==loops+1 && npc.aiQueueFront() && npc.aiQueueFront()->point==post &&
            npc.aiQueueFront()->s0.empty(),"the original VM Loop selected the harbour post through AI_GotoFP");
    };
  auto stopPair = [&] {
    for(Npc* npc:{&first,&second}) {
      test.stop(*npc);
      npc->attachToPoint(nullptr);
      }
    };
  for(bool guardFirst:{true,false}) {
    select(first);
    scriptSelect(second,dementor);
    const auto saved=activityQueue(second);
    Contract::nextAiAction(guardFirst ? first : second);
    Contract::nextAiAction(guardFirst ? second : first);
    const auto tail=activityQueue(second);
    require(post->useCounter()==1 && (guardFirst ? first : second).currentGoToPoint()==post &&
            (guardFirst ? second : first).currentWayPoint()==wp && std::equal(tail.begin(),tail.end(),saved.begin()+1,saved.end()),
            "a guard and a script share no lock in either order, and the script retains its own tail");
    if(!guardFirst)
      require(Contract::activityEvents(first).pending & ActivityEvents::AttachFailed,"the guard receives the rejected acquisition");
    stopPair();
    }
  for(bool events:{true,false}) {
    Contract::activityEventsOn(script)=events;
    scriptSelect(first,events ? dementor : guard);
    scriptSelect(second,dementor);
    Contract::nextAiAction(first);
    Contract::nextAiAction(second);
    require(first.currentGoToPoint()==post && second.currentGoToPoint()==post && post->useCounter()==2,
            "two original AI_GotoFPs both start, including with events disabled");
    stopPair();
    }
  // A save made in the polling mode has no provenance on any of the three commands.
  scriptSelect(first,guard);
  scriptSelect(second,guard);
  const auto legacy=activityQueue(second);
  Contract::activityEventsOn(script)=true;
  reload(first);
  reload(second);
  Contract::nextAiAction(first);
  Contract::nextAiAction(second);
  const auto retained=activityQueue(second);
  require(post->useCounter()==1 && (Contract::activityEvents(second).pending & ActivityEvents::AttachFailed) &&
          std::equal(retained.begin(),retained.end(),legacy.begin()+1,legacy.end()),"old unmarked queues guard one post and preserve the entire ambiguous tail");
  stopPair();

  const auto* away=world.findWayPoint("NW_CITY_PATH_HABOUR_03");
  const auto* beside=world.findPoint("FP_STAND_CITY_10",false);
  require(away && beside,"the external destination has a different post");
  test.freePoint(beside);
  test.only({}); // run queued walks without starting another routine tick
  select(second);
  {
    FpLock blocked(post),ordinary(beside);
    NpcEngineCalls::gotoWaypoint(world,second,beside->name);
    second.aiPush(AiQueue::aiStandup());
    second.aiPush(AiQueue::aiAlignToFp());
    second.aiPush(AiQueue::aiLookAtNpc(&first));
    reload(second); // the restored guard walk still competes, the external walk keeps its place
    Contract::nextAiAction(second);
    auto tail=Contract::queue(second);
    require(tail.size()>=4 && tail.pop().point==beside && tail.pop().act==AI_StandUp && tail.pop().act==AI_AlignToFp &&
            tail.pop().act==AI_LookAtNpc,"the restored loser removes only its own tail before the external GotoWP and identical external tail");
    Contract::nextAiAction(second);
    require(second.currentGoToPoint()==beside && beside->useCounter()==2 && post->useCounter()==1,
            "external GotoWP to a different, ordinarily occupied STAND still starts after loading");
    reload(second);
    test.stepUntil(1500,[&]{ return second.currentWayPoint()==wp && second.isAiQueueEmpty() && !Contract::walking(second); },"loaded external walk and return finish");
    Contract::tickRoutine(second);
    require(second.lookAtTarget()==&first && second.activityStage()==Stage::Wait && beside->useCounter()==1,
            "the loser returns to its own row after the external walk and the save");
    }
  test.stop(second);

  // A polling save made before or during an external walk contains no provenance or recorded return.
  first.attachToPoint(beside);
  for(bool inFlight:{false,true}) {
    Contract::activityEventsOn(script)=false;
    scriptSelect(second,guard);
    second.clearAiQueue();
    NpcEngineCalls::gotoWaypoint(world,second,beside->name);
    second.aiPush(AiQueue::aiLookAtNpc(&first));
    if(inFlight)
      Contract::nextAiAction(second);
    Contract::activityEventsOn(script)=true;
    reload(second);
    if(!inFlight)
      require(second.aiQueueFront() && second.aiQueueFront()->point==beside && second.aiQueueFront()->s0.empty() &&
              second.aiQueueFront()->i1==0 && !beside->isGuarded(),
              "the old queued GotoWP has no marker and its destination has an ordinary owner");
    test.stepUntil(1500,[&]{ return second.lookAtTarget()==&first; },"the saved look runs after the legacy walk");
    require(second.currentWayPoint()==beside && second.isOnFreePoint(a.point),
            "the saved look runs at the ordinarily occupied destination");
    Contract::tickRoutine(second);
    require(second.aiQueueFront() && second.aiQueueFront()->point==wp && beside->useCounter()==1,
            "the first place check keeps the ordinary owner's lock and returns the guard to its row");
    test.stop(second);
    }
  first.attachToPoint(nullptr);

  init(second,*wp);
  second.handle().wp=post->name;
  second.setPosition(away->position());
  second.attachToPoint(away);
  second.updateTransform();
  Contract::tickRoutine(second);
  require(second.aiQueueFront() && second.aiQueueFront()->point==post,"a row named for a free point uses the AI_GotoWP fallback");
  test.stop(second);

  init(second,*wp);
  {
    const auto mid=(second.mapHeadBone()+post->position()+Tempest::Vec3(0,10,0))*.5f;
    ProtoMesh table(PackedMesh(box({65,180,65},true),PackedMesh::PK_Visual),"CONTRACT_GUARD_OBSTRUCTION.MRM");
    auto obstacle=world.physic()->addStaticObj(table.attach[0].shape.get(),at(mid.x,mid.y,mid.z),"CONTRACT_GUARD_OBSTRUCTION.MRM");
    require(!post->isLocked() && !world.findFreePoint(second,a.point),"the obstacle hides the only free post");
    Contract::tickRoutine(second);
    const auto deadline=Contract::activityEvents(second).deadline;
    require(second.activityStage()==Stage::Wait && deadline!=gtime::endOfTime(),"the occluded post has a retry deadline");
    obstacle.setObjMatrix(at(mid.x,mid.y+1000,mid.z));
    require(world.findFreePoint(second,a.point)==post,"moving the obstacle opens the sightline");
    test.stepUntil(600,[&]{ return second.isAiQueueEmpty(); },"the fallback pose finishes");
    const auto watchdog=ActivityEvents::stats().watchdog;
    gothic.gameSession()->setTime(deadline);
    Contract::tickRoutine(second);
    require(second.aiQueueFront() && second.aiQueueFront()->point==post && ActivityEvents::stats().watchdog==watchdog,
            "the guard notices the cleared sightline without an emergency restart");
    second.aiPush(AiQueue::aiLookAtNpc(&first));
    gothic.gameSession()->setTime(Contract::activityEvents(second).deadline);
    Contract::tickActivityDeadline(second);
    require(ActivityEvents::stats().watchdog==watchdog+1 && second.aiQueueFront() && second.aiQueueFront()->act==AI_LookAtNpc,
            "a stalled guard walk counts one watchdog and drops only its own commands");
    }
  test.stop(second);

  const WayPoint vanished(wp->position(),"CONTRACT_WAYPOINT_ABSENT_AFTER_LOAD");
  second.aiPush(AiQueue::aiGoToPoint(vanished));
  second.aiPush(AiQueue::aiLookAtNpc(&first));
  reload(second);
  require(second.aiQueueFront() && second.aiQueueFront()->point==nullptr,"the missing waypoint loads as null");
  Contract::nextAiAction(second);
  require(second.aiQueueFront() && second.aiQueueFront()->act==AI_LookAtNpc,"a vanished waypoint leaves the next action intact");
  test.stop(second);
  Contract::random(script)=before;

  // Both deck patrols reach a new row within the Init approach radius while
  // their last waypoint can still differ. Observe the actual VM and event queues.
  std::string differences;
  auto deckRow = [&](const char* actor,const char* rowName,const char* previous,gtime time) {
    auto* npc=activityNpc(world,actor);
    const auto* row=world.findWayPoint(rowName);
    const auto* last=world.findWayPoint(previous);
    require(npc && row && last,"the ship patrol and its waypoints exist");
    for(bool ending:{false,true}) {
      std::string reference;
      for(bool events:{false,true}) {
        Contract::activityEventsOn(script)=events;
        test.stop(*npc);
        gothic.gameSession()->setTime(time);
        npc->setPosition(row->position());
        npc->attachToPoint(last);
        npc->updateTransform();
        Contract::tickRoutine(*npc);
        require(npc->isInState(ScriptFn(guard.phases.init)) && npc->handle().wp==rowName,"the authored guard row started");
        activityDrain(*npc);
        std::vector<FpLock> occupied;
        while(auto* p=world.findFreePoint(*npc,a.point)) occupied.emplace_back(*p);
        if(ending) gothic.gameSession()->setTime(Contract::aiState(*npc).eTime);
        const auto loops=calls.phase(*npc,guard.state+"_LOOP");
        Contract::tickRoutine(*npc);
        require(npc->isInState(ScriptFn(guard.phases.init))!=ending && calls.phase(*npc,guard.state+"_LOOP")==loops+!events,
                "only the VM polls and both modes finish the expired row");
        auto queue=Contract::queue(*npc);
        std::string actions;
        while(queue.size()>0) {
          auto command=queue.pop();
          actions+=(command.act==AI_PlayAnim ? command.s0 : AiQueue::actionName(command.act));
          actions+=" ";
          }
        if(!events) {
          reference="AlignToWp "+a.enterAnimation+" "+(ending ? a.leaveAnimation+" " : "");
          require(actions==reference,("unexpected VM deck queue: "+actions).c_str());
          } else if(actions!=reference) {
          differences+="\n"+std::string(actor)+(ending ? " row end" : " first check")+": VM ["+reference+"] events ["+actions+"]";
          }
        }
      }
    test.stop(*npc);
    };
  deckRow("PAL_227_SCHIFFSWACHE","SHIP_DECK_29","SHIP_DECK_28",gtime(8,59));
  deckRow("PAL_228_SCHIFFSWACHE","SHIP_DECK_03","SHIP_DECK_02",gtime(9,39));

  auto* rio=activityNpc(world,"KDW_1404_ADDON_RIORDIAN_NW");
  const auto* rioRow=world.findWayPoint("NW_TROLLAREA_PORTALTEMPEL_34");
  const auto* rioPost=world.findPoint("FP_STAND_RIO",false);
  require(rio && rioRow && rioPost,"Riordian guards the portal");
  test.only({rio});
  test.stop(*rio);
  test.freePoint(rioPost);
  gothic.gameSession()->setTime(gtime(7,30));
  rio->setPosition(rioRow->position());
  rio->attachToPoint(rioRow);
  rio->updateTransform();
  test.stepUntil(900,[&]{ return rio->activityStage()==Stage::Hold && rio->isAiQueueEmpty(); },"Riordian takes his night post");
  require(rio->currentWayPoint()==rioPost && rioPost->useCounter()==1,"Riordian owns the night post");
  gothic.gameSession()->setTime(Contract::aiState(*rio).eTime);
  Contract::tickRoutine(*rio);
  require(!rio->currentState().isValid() && rio->currentTaPoint()==rioRow,"the day row follows the night row at the same waypoint");
  if(rio->currentWayPoint()!=rioPost || rioPost->useCounter()!=1 || !rioPost->isGuarded())
    differences+="\nRiordian lost the guarded post between the two rows";
  const auto nightEnd=savedNpc(*rio);
  loadSavedNpc(*rio,nightEnd);
  if(!rioPost->isGuarded()) differences+="\nloading the row boundary lost the guard lock";
  rio->startState(talk,"");
  require(rioPost->useCounter()==0,"an interruption between guard rows frees the post");
  require(differences.empty(),differences.c_str());
  loadSavedNpc(*rio,nightEnd);
  test.stepUntil(900,[&]{ return rio->isInState(ScriptFn(guard.phases.init)) && rio->isStateStarted(); },"the next guard row starts",
                 [&]{ require(rio->currentWayPoint()==rioPost && rioPost->useCounter()==1 && rioPost->isGuarded(),
                              "the same post stays guarded through End and the next Init"); });
  const auto distance=NpcEngineCalls::distanceToWaypoint(*rio,rioRow);
  require(Contract::queue(*rio).hasMovement()==(distance>a.approachDistance),"the next Init uses its recorded approach distance");
  if(distance>a.approachDistance) {
    test.stepUntil(900,[&]{ return rio->currentWayPoint()!=rioPost; },"the new row's approach starts");
    require(rioPost->useCounter()==0,"the actual walk away from the post releases it");
    }
  std::printf("Guard row handover: one lock until the next Init; distance to the routine waypoint %d cm (threshold %d)\n",
              distance,a.approachDistance);
  rio->clearState(true);
  test.only({});
  relocate(*rio,post->position());
  NpcEngineCalls::gotoWaypoint(world,*rio,post->name);
  Contract::nextAiAction(*rio);
  require(rio->currentWayPoint()==post && post->useCounter()==1 && !post->isGuarded(),"the external walk takes an ordinary post");
  reload(*rio);
  require(!rio->currentState().isValid() && rio->currentWayPoint()==post && post->useCounter()==1 && !post->isGuarded(),
          "a former guard's external walk keeps its ordinary lock after loading");
  test.release();
  std::puts("Guard point: one owner, last release, loaded walk and pose, 5999/6000 ms, both draw outcomes, VM fallback");
  }

void guardCampfireChange(Gothic& gothic) {
  using Contract=NpcDefinitionContract;
  gothic.clearGame().reset();
  gothic.setGame(std::make_unique<GameSession>("OLDWORLD.ZEN"));
  ActivityEventsWorld test(gothic);
  auto& world=test.world();
  auto& script=world.script();
  Contract::activityEventsOn(script)=true;
  gothic.gameSession()->setTime(gtime(world.time().day(),8,1));
  auto* npc=activityNpc(world,"VLK_4148_GESTATH");
  const auto* row=world.findWayPoint("OW_DJG_ROCKCAMP_01");
  const auto* fire=world.findPoint("FP_CAMPFIRE_DJG_ROCKCAMP_01",false);
  require(npc && row && fire,"Gestath and his camp must be in OLDWORLD");
  EventActor guard;
  guard.state="ZS_STAND_GUARDING";
  guard.phases=activityPhases(script,guard.state.c_str());
  guard.pointName=row->name;
  test.only({npc});
  test.start(guard,fire->position(),npc,false);
  npc->attachToPoint(fire);
  npc->updateTransform();
  test.stepUntil(900,[&]{ return npc->activityStage()==ActivityEvents::Stage::Wait; },"Gestath waits at the morning row");
  require(npc->currentWayPoint()==row && fire->useCounter()==0,"the waiting guard has released the campfire");

  test.stop(*npc);
  Contract::activityEventsOn(script)=false;
  const auto* away=test.awayFrom(*row,800);
  npc->setPosition(away->position());
  npc->attachToPoint(away);
  npc->updateTransform();
  FpLock occupied(fire);
  NpcEngineCalls::gotoWaypoint(world,*npc,fire->name);
  Contract::nextAiAction(*npc);
  require(npc->currentGoToPoint()==fire && fire->useCounter()==2,"ordinary movement to an occupied campfire keeps its original approach");
  std::puts("Guard campfire: Gestath frees his old place at the change of row; ordinary GotoWP still approaches it when occupied");
  }

// Alignment may be absent on a quiet event tick. Compare the animation commands themselves,
// including the distinction between AI_PlayAni and AI_PlayAniBS.
void checkPointClip(const Npc& npc,std::string_view name,BodyState body=BS_NONE) {
  auto queue=NpcDefinitionContract::queue(npc);
  size_t count=0;
  while(queue.size()>0) {
    const auto action=queue.pop();
    if(action.act!=AI_PlayAnim && action.act!=AI_PlayAnimBs) continue;
    ++count;
    require(action.s0==name && action.act==(body==BS_NONE ? AI_PlayAnim : AI_PlayAnimBs) &&
            (body==BS_NONE || action.i0==int32_t(body)),
            ("unexpected animation "+action.s0+" while expecting '"+std::string(name)+"'").c_str());
    }
  require(count==size_t(!name.empty()),("unexpected number of queued clips for '"+std::string(name)+"'").c_str());
  }

struct CampfireSave {
  std::vector<uint8_t>  bytes;
  Tempest::Vec3         position;
  float                 heading=0.f;
  };

CampfireSave pointActivitiesWorld(Gothic& gothic,bool events) {
  CampfireSave distantCampfire;
  using Contract=NpcDefinitionContract;
  ActivityEventsWorld test(gothic);
  auto& world=test.world();
  auto& script=world.script();
  auto& vm=script.getVm();
  require(Contract::activityEventsOn(script)==events,"the new session must read OPENGOTHIC_ACTIVITY_EVENTS");
  test.only({}); // Advance the world's movement and animations, choose each routine tick below.
  ActivityEvents::reset();
  ActivityCalls calls;
  auto symbol=[&](const std::string& name) -> daedalus::DaedalusSymbol& {
    auto* sym=script.findSymbol(name);
    require(sym!=nullptr,("missing script symbol "+name).c_str());
    return *sym;
    };
  const auto quakePc=symbol("B_EVENT_PORTAL_EARTHQUAKE").address();
  struct Site { const char* state; const char* actor; const char* row; const char* fp; };
  const Site sites[]={
    {"ZS_SIT_CAMPFIRE","BAU_944_EHNIM","NW_FARM2_FIELD_03_B","FP_SIT_CAMPFIRE_PAUSE_02"},
    {"ZS_STAND_ARMSCROSSED","VLK_412_HARAD","NW_CITY_HABOUR_POOR_AREA_PATH_20","FP_STAND_CITY_20"},
    {"ZS_STAND_WP","VLK_412_HARAD","NW_CITY_HABOUR_POOR_AREA_PATH_20",nullptr},
    {"ZS_PEE","VLK_458_RUPERT","NW_CITY_MERCHANT_TRADE_03","FP_PEE_CITY_02"},
    };
  std::string differences;
  for(const auto& site:sites) {
    EventActor a;
    a.state=site.state;
    a.phases=activityPhases(script,site.state);
    a.pointName=site.row;
    const bool fire=a.state=="ZS_SIT_CAMPFIRE", pee=a.state=="ZS_PEE", folded=a.state=="ZS_STAND_ARMSCROSSED";
    const auto* wp=world.findWayPoint(site.row);
    const auto* fp=site.fp ? world.findPoint(site.fp,false) : nullptr;
    auto check=[&](bool ok,const char* message) { require(ok,(a.state+": "+message).c_str()); };
    auto* actor=activityNpc(world,site.actor);
    check(actor && !actor->isDead(),"the actor is alive in NEWWORLD");
    auto& npc=*actor;
    check(wp && !wp->isFreePoint() && (!site.fp || (fp && fp->isFreePoint())),"the row and its point exist");
    if(fp) test.freePoint(fp);
    const auto loop=a.state+"_LOOP";
    const auto loopPc=symbol(loop).address();
    size_t vmLoops=0,quakes=0;
    struct Unobserve {
      daedalus::DaedalusVm& vm;
      ~Unobserve() { vm.observe_instructions({}); }
      } unobserve{vm};
    vm.observe_instructions([&](const auto&) {
      if(vm.pc()==loopPc && vm.global_self()->get_instance()==std::static_pointer_cast<daedalus::INpc>(npc.handlePtr())) ++vmLoops;
      if(vm.pc()==quakePc) ++quakes;
      });
    auto drain=[&] { test.stepUntil(900,[&]{ return npc.isAiQueueEmpty(); },a.state+" finishes its commands"); };
    auto awaitPerception=[&] {
      test.stepUntil(600,[&]{ return world.tickCount()>=Contract::aiState(npc).loopNextTime; },a.state+" reaches its next perception tick");
      };
    // Inspect the queue before implAiTick consumes an action on the same frame.
    auto tick=[&] { awaitPerception(); Contract::tickScheduledRoutine(npc); };
    auto play=[&](const char* name,BodyState body=BS_NONE) {
      checkPointClip(npc,name,body);
      test.stepUntil(600,[&]{ return npc.animProgress(name)>=0; },a.state+" plays "+name);
      drain();
      };
    auto start=[&] {
      test.start(a,wp->position(),&npc,false);
      npc.attachToPoint(wp);
      npc.updateTransform();
      tick();
      if(!site.fp) {
        auto init=Contract::queue(npc);
        while(init.size()>2) init.pop();
        check(init.size()==2 && init.front()->act==AI_GoToPoint && init.pop().point==wp && init.pop().act==AI_AlignToWp,
              "the waypoint Init ends with a walk to its routine waypoint followed by alignment");
        }
      drain();
      npc.setStateTime(0);
      };
    npc.handle().aivar[19]=73; // The campfire script never writes the TA position.
    const auto* away=ActivityEventsWorld::awayFrom(*wp,800.f);
    test.start(a,away->position(),&npc,false);
    npc.attachToPoint(away);
    npc.updateTransform();
    const auto initBeforeWalk=calls.phase(npc,a.state);
    tick();
    test.stepUntil(300,[&]{ return Contract::walking(npc); },a.state+" starts its Init approach");
    npc.clearAiQueue();
    tick();
    const auto* resumed=npc.aiQueueFront();
    check(calls.phase(npc,a.state)==initBeforeWalk+1 && npc.isInState(ScriptFn(a.phases.init)) &&
          (!resumed || resumed->act!=AI_GoToPoint || resumed->point!=wp),
          "clearing an active approach resumes the state without another Init or a new walk to its waypoint");
    start();
    const char* scheme=fire ? "CAMPFIRE" : pee ? "PEE" : "STAND";
    check(fp ? world.findFreePoint(npc,scheme)==fp : world.findFreePoint(npc,"STAND")!=nullptr,
          "the row has a visible free point (also for the WP-only control)");
    FpLock occupied(fp);
    tick();
    if(fire) {
      check(npc.aiQueueFront()==nullptr && npc.handle().aivar[19]==73,"an occupied campfire queues nothing");
      const auto waiting=savedNpc(npc);
      const auto* field=world.findWayPoint("NW_FARM2_FIELD_03");
      check(field && wp->qDistTo(field->position())>500*500,"the field is outside the Init walk radius");
      npc.setPosition(field->position());
      npc.updateTransform();
      check(world.findFreePoint(npc,scheme)==nullptr,"the displaced camper has no free seat in view");
      tick();
      check(npc.isAiQueueEmpty(),"a displaced camper waits without walking back to the routine waypoint");
      loadSavedNpc(npc,savedNpc(npc));
      tick();
      check(npc.isAiQueueEmpty(),"a loaded camper searches from the saved position");
      if(!events)
        npc.attachToPoint(nullptr); // The VM leaves the old ordinary waypoint on direct relocation.
      check(npc.currentWayPoint()==nullptr,"the displaced camper waits with no current point");
      occupied=FpLock();
      tick();
      check(npc.isAiQueueEmpty(),"freeing a seat out of reach leaves the displaced camper waiting");
      const auto* outside=world.findPoint("FP_STAND_SPIELPLATZ_05",false);
      const auto* inside=world.findPoint("FP_STAND_SPIELPLATZ_06",false);
      const auto* seat=world.findPoint("FP_CAMPFIRE_SPIELPLATZ_01",false);
      check(outside && inside && seat && outside->qDistTo(inside->position())<MAX_AI_USE_DISTANCE*MAX_AI_USE_DISTANCE,
            "a short move at the playground crosses the seat's search boundary");
      test.freePoint(seat);
      npc.setPosition(outside->position());
      npc.updateTransform();
      tick();
      check(!world.findFreePoint(npc,scheme) && npc.isAiQueueEmpty(),"the waiting NPC starts outside the seat's reach");
      npc.setPosition(inside->position());
      npc.updateTransform();
      tick();
      check(npc.aiQueueFront() && npc.aiQueueFront()->act==AI_GoToPoint && npc.aiQueueFront()->point==seat,
            "moving the same waiter a short distance with no current point finds the now visible seat");
      occupied=FpLock(fp);
      loadSavedNpc(npc,waiting);
      tick();
      } else if(pee) {
      check(Contract::queue(npc).size()==1 && npc.aiQueueFront()->act==AI_AlignToWp && npc.handle().aivar[19]==1,
            "an occupied PEE point only aligns the waiting NPC to the waypoint");
      npc.clearState(false);
      checkPointClip(npc,"T_PEE_2_STAND"); // End is unconditional, even before the first PEE pose.
      start();
      tick();
      drain();
      } else {
      auto queue=Contract::queue(npc);
      if(folded) check(queue.size()>=3 && queue.pop().act==AI_AlignToWp && queue.pop().act==AI_StandUp,
                       "waiting with arms crossed aligns and stands up before the pose");
      play("T_STAND_2_LGUARD");
      check(npc.currentWayPoint()==wp && npc.handle().aivar[19]==0,"the NPC takes the low guard pose at its waypoint");
      auto& gesture=symbol(loop+".RANDOM");
      auto& event=symbol("ZS_STAND_ARMSCROSSED_LOOP.EVENTRANDY");
      auto& sound=symbol("B_EVENT_PORTAL_EARTHQUAKE.RANDY");
      auto& level=symbol("CURRENTLEVEL");
      const int addon=symbol("ADDONWORLD_ZEN").get_int(),pirate=symbol("GIL_PIR").get_int(),guild=npc.handle().guild;
      const int originalLevel=level.get_int();
      auto& raven=symbol("RAVENISDEAD");
      const int originalRaven=raven.get_int();
      const auto early=Contract::random(script);
      gesture.set_int(-1);
      event.set_int(-1);
      awaitPerception();
      npc.setStateTime(5999);
      Contract::tickScheduledRoutine(npc);
      check(Contract::random(script)==early && npc.stateTime()==5999 && gesture.get_int()==-1 &&
            event.get_int()==-1 && quakes==0,"no gesture or story draw before the sixth whole second");
      checkPointClip(npc,"");
      drain();
      const auto due=Contract::aiState(npc).loopNextTime;
      check(due>world.tickCount()+1,"real perception interval leaves a tick before the due draw");
      script.setSeed(0); // No clip or earthquake, so the queue stays empty at the real draw tick.
      auto expected=Contract::random(script);
      const auto nativeRoll=int(expected()%(folded ? 7u : 10u));
      const auto nativeStory=folded ? int(expected()%200) : -1;
      const auto nativeLoops=vmLoops;
      const auto beforeDue=Contract::random(script);
      npc.setStateTime(5999); // Draining the VM alignment above advanced the world clock.
      test.only({&npc});
      test.advance(1);
      check(world.tickCount()<due && Contract::aiState(npc).loopNextTime==due && npc.stateTime()==6000 &&
            vmLoops==nativeLoops && gesture.get_int()==-1 && event.get_int()==-1 && Contract::random(script)==beforeDue,
            "the real NPC tick at 6000 ms precedes perception and leaves both draws pending");
      test.stepUntil(600,[&]{ return world.tickCount()>=due; },a.state+" reaches perception in the normal NPC tick");
      test.only({});
      check(Contract::aiState(npc).loopNextTime>world.tickCount() && npc.stateTime()==0 &&
            gesture.get_int()==nativeRoll && event.get_int()==nativeStory && quakes==0 &&
            Contract::random(script)==expected && vmLoops==nativeLoops+size_t(!events),
            "the first real perception tick performs one gesture decision in either mode");
      auto draw=[&](uint32_t seed,int map,int dead,int faction,bool quake,bool hasPortal=true) {
        level.set_int(map);
        raven.set_int(dead);
        npc.handle().guild=faction;
        gesture.set_int(-1);
        event.set_int(-1);
        sound.set_int(-1);
        const auto before=quakes;
        script.setSeed(seed);
        auto expected=Contract::random(script);
        const auto roll=int(expected()%(folded ? 7u : 10u));
        const int eventRoll=folded && hasPortal ? int(expected()%200) : -1;
        const int soundRoll=quake ? int(expected()%4) : -1;
        awaitPerception();
        npc.setStateTime(6000);
        Contract::tickScheduledRoutine(npc);
        check(gesture.get_int()==roll && event.get_int()==eventRoll && quakes==before+quake &&
              sound.get_int()==soundRoll && Contract::random(script)==expected && npc.stateTime()==0,
              "the due gesture, story draw and earthquake's own sound draw must follow the script order");
        const char* clips[]={"T_LGUARD_SCRATCH","T_LGUARD_STRETCH","T_LGUARD_CHANGELEG"};
        checkPointClip(npc,roll<3 ? clips[roll] : "");
        drain();
        check(npc.currentWayPoint()==wp,"gesturing at the waypoint must not claim a free point");
        };
      // Seed 940 gives 2 of 7 (1 of 10 at a WP), then 1 of 200. Change one story condition at a time.
      draw(940,addon,0,guild,folded);
      if(folded) {
        draw(940,originalLevel,0,guild,false);
        draw(940,addon,0,pirate,false);
        }
      draw(63,addon,0,guild,false); // 0 of 7, 2 of 10, misses the earthquake
      draw(1658,addon,1,guild,false); // 1 of 7, 0 of 10; Raven is dead on a winning story draw
      draw(0,addon,0,guild,false); // no gesture, still resets the timer
      if(folded && events) {
        // Classic has the same pose and gestures, but its record ends at the timer reset.
        auto& portal=Contract::activities(script).points.at(a.state).portal;
        const auto addonPortal=portal;
        portal.reset();
        draw(940,addon,0,guild,false,false);
        portal=addonPortal;
        }
      level.set_int(originalLevel);
      raven.set_int(originalRaven);
      npc.handle().guild=guild;
      }
    if(fp) {
      occupied=FpLock();
      awaitPerception();
      if(folded) npc.setStateTime(6000);
      const auto beforeWalk=Contract::random(script);
      Contract::tickScheduledRoutine(npc);
      const auto* walk=npc.aiQueueFront();
      check(walk && walk->act==AI_GoToPoint && walk->point==fp && fp->useCounter()==0,
            "freeing the point queues a walk to that point");
      checkPointClip(npc,"");
      if(folded) check(npc.handle().aivar[19]==2 && npc.stateTime()==6000 && Contract::random(script)==beforeWalk,
                       "selecting a free STAND point defers both due draws while walking");
      if(pee) {
        check(!MoveAlgo::isClose(npc,*fp,MAX_AI_USE_DISTANCE),"the PEE approach starts outside the pose radius");
        test.stepUntil(900,[&]{ return npc.isAiQueueEmpty(); },"Rupert reaches his PEE point",[&] {
          if(!npc.isOnFreePoint(scheme)) {
            checkPointClip(npc,"");
            check(npc.animProgress("T_STAND_2_PEE")<0,"the PEE clip started before arrival");
            }
          });
        } else drain();
      check(npc.currentWayPoint()==fp && npc.isOnFreePoint(scheme) && fp->useCounter()==1,"the walk arrives with one owner");
      if(fire) {
        const auto arrival=savedNpc(npc);
        const auto before=world.time();
        check(!ActivityDefinitions::bodyStateContains(npc.bodyState(),BS_SIT),"the camper has arrived before the sit animation");
        gothic.gameSession()->setTime(gtime(before.day(),14,0));
        Contract::aiState(npc).eTime=world.time();
        tick();
        check(fp->useCounter()==1 && npc.currentWayPoint()==fp,"the last Loop keeps the seat while queuing both sit and rise");
        auto handover=Contract::queue(npc);
        std::vector<std::string> clips;
        while(handover.size()>0) {
          const auto action=handover.pop();
          if(action.act==AI_PlayAnimBs) clips.push_back(action.s0);
          }
        check(clips==std::vector<std::string>{"T_STAND_2_SIT","T_SIT_2_STAND"},"the last Loop seats the camper before End raises it");
        gothic.gameSession()->setTime(before);
        loadSavedNpc(npc,arrival);
        }
      tick();
      if(pee) play("T_STAND_2_PEE");
      else if(fire) {
        play("T_STAND_2_SIT",BS_SIT);
        test.stepUntil(300,[&]{ return npc.animProgress("S_SIT")>=0 && ActivityDefinitions::bodyStateContains(npc.bodyState(),BS_SIT); },
                       "the campfire owner settles into BS_SIT");
        const auto inits=calls.phase(npc,a.state);
        loadSavedNpc(npc,savedNpc(npc));
        for(int i=0;i<2;++i) { // The load check and the routine tick after its queue drains.
          tick();
          checkPointClip(npc,"");
          drain();
          }
        check(calls.phase(npc,a.state)==inits && npc.currentWayPoint()==fp && fp->useCounter()==1 &&
              ActivityDefinitions::bodyStateContains(npc.bodyState(),BS_SIT) && npc.handle().aivar[19]==73,
              "loading the seated NPC preserves its pose and lock without restarting Init");

        auto& hero=*world.player();
        const auto originalHero=hero.position();
        const auto originalHeading=hero.rotation();
        const auto* far=world.findWayPoint("NW_CITY_MERCHANT_SHOP01_FRONT_03");
        check(far && npc.qDistTo(far->position())>25600.f*25600.f,"the far observation point is beyond the Near rings");
        const auto feet=npc.position();
        const auto heading=npc.rotation();
        const auto center=npc.collosionCenter();
        const Tempest::Vec3 across(130.f,0,0);
        auto hit=[&] { return world.physic()->rayNpc(center-across,center+across,&hero).npcHit==&npc; };
        auto seated=[&] {
          return npc.position()==feet && npc.rotation()==heading && npc.currentWayPoint()==fp && fp->useCounter()==1 &&
                 npc.bodyStateMasked()==BS_SIT && npc.animProgress("S_SIT")>=0 && calls.phase(npc,a.state)==inits;
          };
        test.moveHero(wp->position(),originalHeading);
        check(hit() && seated(),"the seated NPC starts with a queryable collision body");
        test.only({&npc});
        const auto away=far->position()-feet;
        const auto awayHeading=std::atan2(away.z,away.x)*180.f/float(M_PI);
        test.moveHero(far->position(),awayHeading);
        test.step(4);
        check(npc.simTier()==NpcSimTier::Simplified && npc.campfireBodySuspended()==events && hit()!=events && seated(),
              "the distant campfire preserves the seat and pose and suspends the body in event mode");
        if(events) {
          distantCampfire.position=npc.position();
          distantCampfire.heading=npc.rotation();
          {
            Tempest::MemWriter writer(distantCampfire.bytes);
            Serialize out(writer);
            Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
            gothic.gameSession()->save(out,"distant campfire",preview);
          }
          gothic.camera()->reset(&npc);
          world.catchUpAnimation();
          check(hit() && !npc.campfireBodySuspended() && seated(),"a camera cut restores the body before drawing");
          test.moveHero(far->position(),awayHeading);
          test.step(3);
          check(npc.campfireBodySuspended(),"the seated body can be suspended again after a camera cut");
          }
        test.moveHero(wp->position(),originalHeading);
        check(hit() && !npc.campfireBodySuspended() && seated(),"approaching restores one collision body at the original seated pose");
        test.only({});
        test.moveHero(originalHero,originalHeading);
        } else drain();
      }
    if(fire) {
      const auto seated=savedNpc(npc);
      const auto* field=world.findWayPoint("NW_FARM2_FIELD_03");
      for(bool rowEnd:{false,true}) {
        loadSavedNpc(npc,seated);
        const auto ends=calls.phase(npc,a.state+"_END");
        if(rowEnd) {
          gothic.gameSession()->setTime(gtime(world.time().day(),14,0));
          check(npc.currentTaPoint()==field && field!=wp,"the next real routine row leaves the campfire");
          Contract::aiState(npc).eTime=world.time();
          tick();
          } else {
          npc.clearState(false);
          }
        check(calls.phase(npc,a.state+"_END")==ends+1 && fp->useCounter()==1 && npc.currentWayPoint()==fp,
              "End queues the rise while one owner still holds the seat");
        checkPointClip(npc,"T_SIT_2_STAND",BS_STAND);
        if(rowEnd) {
          loadSavedNpc(npc,savedNpc(npc));
          check(fp->useCounter()==1 && npc.currentWayPoint()==fp,"loading with End queued keeps the seat occupied");
          }
        test.stepUntil(600,[&]{ return npc.animProgress("T_SIT_2_STAND")>=0; },"the camper starts rising");
        check(fp->useCounter()==1 && npc.currentWayPoint()==fp,"the rising camper still holds the seat");
        if(rowEnd)
          test.only({&npc});
        else
          npc.aiPush(AiQueue::aiGoToPoint(*field));
        test.stepUntil(900,[&]{ return npc.currentWayPoint()!=fp; },"the camper leaves its seat through the AI queue");
        test.only({});
        check(fp->useCounter()==0,"walking away releases the seat");
        drain();
        }
      } else {
      if(folded) {
        PhysicsNoise impact;
        impact.position=npc.position()+Tempest::Vec3(-fp->dir.z,0,fp->dir.x)*300.f;
        npc.setProcessPolicy(NpcProcessPolicy::AiNormal);
        npc.setExternalControl(false);
        check(npc.hearPhysicsNoise(impact),"the idle NPC hears an impact with no known culprit");
        npc.setExternalControl(true);
        npc.setDirection(impact.position-npc.position());
        test.stepUntil(120,[until=world.tickCount()+2000,&world]{ return world.tickCount()>until; },"the noise reaction ends");
        tick();
        check(npc.aiQueueFront() && npc.aiQueueFront()->act==AI_AlignToFp,"the post regains its facing after the native turn");
        drain();
        }
      npc.clearState(false);
      if(fp) check(npc.currentWayPoint()==fp && fp->useCounter()==1,"interrupting the pose leaves its occupied point held");
      checkPointClip(npc,pee ? "T_PEE_2_STAND" : "T_LGUARD_2_STAND");
      drain();
      }
    npc.attachToPoint(nullptr);
    const auto& stats=test.stats(a.state);
    auto wakes=[&](ActivityEvents::Wake why) { return stats.wakes[std::countr_zero(unsigned(why))]; };
    const auto phases=calls.phase(npc,loop);
    const auto observed=a.state+" (events="+std::to_string(events)+"): "+std::to_string(phases)+" Loop phases, "+
        std::to_string(vmLoops)+" VM entries, "+std::to_string(wakes(ActivityEvents::Freed))+" freed wakes, "+
        std::to_string(wakes(ActivityEvents::Gesture))+" gestures, "+std::to_string(quakes)+" earthquakes";
    std::puts(observed.c_str());
    const bool dispatched=events ? vmLoops==0 && wakes(ActivityEvents::Entered)>0 &&
                                   (wakes(ActivityEvents::Freed)>0)==(fp!=nullptr) &&
                                   (wakes(ActivityEvents::Gesture)>0)==(!fire && !pee) && (!fire || wakes(ActivityEvents::Loaded)>0)
                                 : vmLoops>0 && ActivityEventsWorld::quiet({},stats);
    if(phases!=vmLoops || !dispatched) differences+="\n"+observed;
    }
  require(differences.empty(),("point activity dispatch:"+differences).c_str());
  return distantCampfire;
  }

void pointActivitiesInBothModes(Gothic& gothic) {
  const auto* configuredEvents=std::getenv("OPENGOTHIC_ACTIVITY_EVENTS");
  const std::optional<std::string> previousEvents=configuredEvents ? std::optional<std::string>(configuredEvents) : std::nullopt;
  for(bool events:{false,true}) {
    setenv("OPENGOTHIC_ACTIVITY_EVENTS",events ? "1" : "0",1);
    gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
    const auto save=pointActivitiesWorld(gothic,events);
    gothic.clearGame().reset();
    if(events) {
      require(!save.bytes.empty(),"the distant campfire produced a complete save");
      Tempest::MemReader reader(save.bytes);
      Serialize in(reader);
      gothic.setGame(std::make_unique<GameSession>(in));
      ActivityEventsWorld test(gothic);
      auto& world=test.world();
      auto* npc=activityNpc(world,"BAU_944_EHNIM");
      const auto* fp=world.findPoint("FP_SIT_CAMPFIRE_PAUSE_02",false);
      const auto* wp=world.findWayPoint("NW_FARM2_FIELD_03_B");
      require(npc && fp && wp && npc->currentWayPoint()==fp && fp->useCounter()==1 &&
              npc->position()==save.position && npc->rotation()==save.heading &&
              npc->bodyStateMasked()==BS_SIT && npc->animProgress("S_SIT")>=0,
              "loading the whole game restores the exact occupied point and settled sitting animation");
      gothic.camera()->setViewport(640,480);
      ActivityCalls calls;
      test.only({npc});
      test.stepUntil(30,[&]{ return npc->campfireBodySuspended(); },"the loaded distant sitter parks its body");
      const auto center=npc->collosionCenter();
      const Tempest::Vec3 across(130.f,0,0);
      test.moveHero(wp->position(),world.player()->rotation());
      require(!npc->campfireBodySuspended() && world.physic()->rayNpc(center-across,center+across,world.player()).npcHit==npc &&
              npc->position()==save.position && npc->rotation()==save.heading && npc->currentWayPoint()==fp &&
              fp->useCounter()==1 && calls.phase(*npc,"ZS_SIT_CAMPFIRE")==0 &&
              npc->bodyStateMasked()==BS_SIT && npc->animProgress("S_SIT")>=0,
              "approaching after the whole-game load restores collision, position, seat and sitting pose");
      std::puts("E3.1 campfire: distant collision removed, seat retained, approach and whole-game load preserved S_SIT");
      gothic.clearGame().reset();
      }
    }
  if(previousEvents) setenv("OPENGOTHIC_ACTIVITY_EVENTS",previousEvents->c_str(),1); else unsetenv("OPENGOTHIC_ACTIVITY_EVENTS");
  }
