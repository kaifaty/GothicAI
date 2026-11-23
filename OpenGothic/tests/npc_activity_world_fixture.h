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

  // Exercise an original AI_GotoFP alongside the guard's event-driven reservation.
  EventActor dementor=guard;
  dementor.state="ZS_STAND_DEMENTOR";
  dementor.phases=activityPhases(script,dementor.state.c_str());
  first.setSimTier(NpcSimTier::Full);
  second.setSimTier(NpcSimTier::Full);
  auto scriptSelect = [&](Npc& npc,EventActor& actor) {
    auto& events=Contract::activityEventsOn(script);
    const bool previous=std::exchange(events,false);
    test.start(actor,wp->position(),&npc,false);
    npc.attachToPoint(wp);
    npc.updateTransform();
    Contract::tickRoutine(npc);
    activityDrain(npc);
    const auto loops=calls.phase(npc,actor.state+"_LOOP");
    Contract::tickRoutine(npc);
    events=previous;
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

void campfireApproachWorld(Gothic& gothic,ActivityEventsWorld& test,EventActor& actor,Npc& npc,
                           const WayPoint& start,const WayPoint& row,const WayPoint& seat,const ActivityCalls& calls) {
  using ActivityEvents::Stage;
  auto& world=test.world();
  auto& hero=*world.player();
  const auto heroAt=hero.position();
  const auto heroHeading=hero.rotation();
  const auto* far=world.findWayPoint("NW_BIGFARM_SHEEP2_02");
  require(far!=nullptr && seat.useCounter()==0,"campfire route starts with an available seat and a distant waypoint");
  FpLock occupied(&seat);
  const auto away=far->position()-row.position();
  const auto awayHeading=std::atan2(away.z,away.x)*180.f/float(M_PI);
  auto moveAway=[&]{ test.moveHero(far->position(),awayHeading); };
  auto moveNear=[&]{ test.moveHero(row.position(),heroHeading); };
  auto hit=[&] {
    const auto center=npc.collosionCenter();
    const Tempest::Vec3 across(130.f,0,0);
    return world.physic()->rayNpc(center-across,center+across,&hero).npcHit==&npc;
    };
  test.only({&npc});
  test.start(actor,start.position(),&npc,false);
  npc.attachToPoint(&start);
  npc.updateTransform();
  const auto inits=calls.phase(npc,actor.state)+1;
  moveAway();

  auto previous=npc.position();
  Tempest::Vec3 step;
  test.stepUntil(450,[&] {
    step=npc.position()-previous;
    previous=npc.position();
    return npc.campfireBodySuspended() && npc.currentGoToPoint()!=nullptr && std::hypot(step.x,step.z)>0.1f;
    },"the camper takes a graph step without its body");
  const auto at=npc.position();
  const auto* edge=npc.currentGoToPoint();
  const auto heading=npc.rotation();
  const auto phase=ActivityEventsWorld::stage(npc);
  const auto pose=npc.bodyStateMasked();
  const auto travel=npc.simplifiedMoveSpeed()*33.f;
  const auto center=npc.collosionCenter();
  const auto floor=world.physic()->landRay(at+Tempest::Vec3(0,30,0),60);
  const auto heightError=std::abs(floor.v.y-at.y);
  require(npc.simTier()==NpcSimTier::Simplified && phase==Stage::Approach && npc.navigationGoal().point==&row,
          "the bodyless approach retains its authored waypoint destination");
  require(!hit() && NpcDefinitionContract::physicsPosition(npc)==at,"the absent capsule's placement follows the graph");
  require(std::hypot(step.x,step.z)<=travel+0.1f && !world.physic()->sweep(center-step,center,12.f).hasCol,
          "the bodyless walk takes one unobstructed step at graph speed");
  require(floor.hasCol && heightError<=5.f,"the bodyless walk stays on its floor");
  require(seat.useCounter()==1,"the approach leaves the occupied seat with its current owner");

  auto& camera=*gothic.camera();
  camera.reset(&npc);
  const auto target=camera.destTarget();
  camera.setPosition(target+Tempest::Vec3::normalize(camera.originLwc()-target)*2000.f);
  AnimationLod::View view;
  view.make(camera.viewProj(),camera.originLwc(),nullptr);
  require(view.visible(npc) && !view.nearCamera(npc),"the camera reveals the walker outside the near camera radius");
  world.catchUpAnimation();
  auto sameWalk=[&] {
    return !npc.campfireBodySuspended() && hit() && npc.position()==at && npc.rotation()==heading &&
           NpcDefinitionContract::physicsPosition(npc)==at && npc.currentGoToPoint()==edge &&
           npc.navigationGoal().point==&row && ActivityEventsWorld::stage(npc)==phase;
    };
  require(sameWalk() && npc.bodyStateMasked()==pose,"the first visible frame restores collision on the same edge and pose");
  moveAway();
  require(npc.campfireBodySuspended() && !hit(),"the walker can leave view again without restarting its route");
  moveNear();
  require(npc.simTier()==NpcSimTier::Full && sameWalk(),"player approach restores the walker on its current edge");
  test.step();
  const auto resumed=npc.position()-at;
  require(std::hypot(resumed.x,resumed.z)<=travel+0.1f && std::abs(resumed.y)<=travel+5.f,
          "the first native step keeps the walker on the supported route");

  test.stepUntil(900,[&]{ return ActivityEventsWorld::stage(npc)==Stage::Wait && npc.isAiQueueEmpty(); },
                 "the returned body reaches the row and waits for the occupied seat");
  moveAway();
  test.stepUntil(120,[&]{ return npc.campfireBodySuspended(); },"the distant camper waits without collision");
  const auto waiting=npc.position();
  require(!hit() && npc.currentWayPoint()!=&seat && seat.useCounter()==1,"the distant waiter keeps the other owner's seat free of conflicts");
  moveNear();
  require(!npc.campfireBodySuspended() && hit() && npc.position()==waiting && ActivityEventsWorld::stage(npc)==Stage::Wait &&
          seat.useCounter()==1,"the returned waiter still waits at the same place");

  moveAway();
  test.stepUntil(120,[&]{ return npc.campfireBodySuspended(); },"the waiter dematerializes again");
  occupied=FpLock();
  test.stepUntil(900,[&]{ return npc.campfireBodySuspended() && npc.bodyStateMasked()==BS_SIT; },
                 "the freed seat wakes the camper, who walks to it and sits");
  const auto seated=npc.position();
  require(!hit() && npc.currentWayPoint()==&seat && seat.useCounter()==1,"the seated camper keeps the only lock while its body is absent");
  moveNear();
  require(hit() && !npc.campfireBodySuspended() && npc.position()==seated && npc.currentWayPoint()==&seat &&
          seat.useCounter()==1 && npc.animProgress("S_SIT")>=0 && calls.phase(npc,actor.state)==inits,
          "the same route reaches the original sitting pose with no new Init or double claim");
  test.only({});
  test.moveHero(heroAt,heroHeading);
  std::printf("E3.2 campfire: approach, camera reveal, occupied wait and seat takeover passed (step %.2f cm, support error %.2f cm)\n",
              double(std::hypot(step.x,step.z)),double(heightError));
  }

struct CampfireSave {
  struct Actor {
    uint32_t          instance;
    Tempest::Vec3     position;
    float             heading;
    BodyState         pose;
    };
  std::vector<uint8_t>  bytes;
  std::array<Actor,3>   actors={}; // walker, waiter, seat owner
  std::string           edge;
  };

bool hitsCamper(World& world,const Npc& npc) {
  const auto center=npc.collosionCenter();
  const Tempest::Vec3 across(130.f,0,0);
  return world.physic()->rayNpc(center-across,center+across,world.player()).npcHit==&npc;
  }

CampfireSave campfireGroupWorld(Gothic& gothic,ActivityEventsWorld& test,EventActor& actor,Npc& sitter,
                               const WayPoint& start,const WayPoint& row,const WayPoint& seat) {
  using ActivityEvents::Stage;
  auto& world=test.world();
  const auto* far=world.findWayPoint("NW_BIGFARM_SHEEP2_02");
  auto* waiter=activityNpc(world,"BAU_943_BAUER");
  auto* walker=activityNpc(world,"BAU_938_BAUER");
  require(far && waiter && walker && sitter.currentWayPoint()==&seat && seat.useCounter()==1,
          "the campfire group has its actors and a seat owner");
  const auto heroAt=world.player()->position();
  const auto heroHeading=world.player()->rotation();
  const auto away=far->position()-row.position();
  const auto awayHeading=std::atan2(away.z,away.x)*180.f/float(M_PI);
  test.only({walker,waiter,&sitter});
  test.start(actor,row.position(),waiter,false);
  waiter->attachToPoint(&row);
  waiter->updateTransform();
  test.moveHero(far->position(),awayHeading);
  test.stepUntil(300,[&] {
    return sitter.campfireBodySuspended() && waiter->campfireBodySuspended() && ActivityEventsWorld::stage(*waiter)==Stage::Wait;
    },"the distant seat owner and waiter both suspend their bodies");
  test.start(actor,start.position(),walker,false);
  walker->attachToPoint(&start);
  walker->updateTransform();
  auto before=walker->position();
  test.stepUntil(450,[&] {
    const auto change=walker->position()-before;
    before=walker->position();
    return walker->campfireBodySuspended() && walker->currentGoToPoint() && std::hypot(change.x,change.z)>0.1f;
    },"the third camper joins the group while walking without collision");
  require(walker->navigationGoal().point==&row && seat.useCounter()==1,
          "the walkers have the authored destination and leave the seat with its owner");

  CampfireSave save;
  const std::array<Npc*,3> group={walker,waiter,&sitter};
  for(size_t i=0;i<group.size();++i) {
    const auto& npc=*group[i];
    require(npc.campfireBodySuspended() && !hitsCamper(world,npc),"all three bodies must be absent before saving");
    save.actors[i]={npc.instanceSymbol(),npc.position(),npc.rotation(),npc.bodyStateMasked()};
    }
  save.edge=walker->currentGoToPoint()->name;
  {
    Tempest::MemWriter writer(save.bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    gothic.gameSession()->save(out,"distant campfire group",preview);
  }

  auto& camera=*gothic.camera();
  camera.reset(&sitter);
  const auto direction=Tempest::Vec3::normalize(camera.originLwc()-camera.destTarget());
  const auto center=(walker->position()+waiter->position()+sitter.position())/3.f;
  camera.setPosition(center+Tempest::Vec3(0,60,0)+direction*2500.f);
  AnimationLod::View view;
  view.make(camera.viewProj(),camera.originLwc(),nullptr);
  for(auto* npc:group)
    require(view.visible(*npc) && !view.nearCamera(*npc),"one frustum reveals the whole group from beyond the near camera radius");
  world.catchUpAnimation();
  for(size_t i=0;i<group.size();++i) {
    const auto& npc=*group[i];
    const auto& saved=save.actors[i];
    require(!npc.campfireBodySuspended() && hitsCamper(world,npc) && npc.position()==saved.position &&
            NpcDefinitionContract::physicsPosition(npc)==saved.position && npc.rotation()==saved.heading &&
            npc.bodyStateMasked()==saved.pose,"each camper has its collision, coordinates and pose in the first visible frame");
    }
  require(sitter.animProgress("S_SIT")>=0 && walker->currentGoToPoint()->name==save.edge &&
          ActivityEventsWorld::stage(*waiter)==Stage::Wait && seat.useCounter()==1,
          "one reveal preserves the walk, wait and seated owner");
  test.stop(*walker);
  test.stop(*waiter);
  test.only({});
  test.moveHero(heroAt,heroHeading);
  std::puts("E3.3 campfire: one pre-render reveal restored all three absent bodies on the same frame");
  return save;
  }

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
        const auto* far=world.findWayPoint("NW_BIGFARM_SHEEP2_02");
        check(far!=nullptr,"the distant observation point exists");
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
        check(npc.simTier()==NpcSimTier::Simplified,"the distant observation point is outside the Near rings");
        check(npc.campfireBodySuspended()==events && hit()!=events,"the distant NPC loses collision only in event mode");
        check(seated(),"the distant NPC keeps its seat, coordinates, heading and sitting pose");
        if(events) {
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
      if(events) {
        campfireApproachWorld(gothic,test,a,npc,*field,*wp,*fp,calls);
        distantCampfire=campfireGroupWorld(gothic,test,a,npc,*field,*wp,*fp);
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

void loadCampfireGroup(Gothic& gothic,const CampfireSave& save) {
  using ActivityEvents::Stage;
  require(!save.bytes.empty(),"the distant campfire group produced a complete save");
  Tempest::MemReader reader(save.bytes);
  Serialize in(reader);
  ActivityCalls calls;
  gothic.setGame(std::make_unique<GameSession>(in));
  ActivityEventsWorld test(gothic);
  auto& world=test.world();
  auto* walker=world.findNpcByInstance(save.actors[0].instance);
  auto* waiter=world.findNpcByInstance(save.actors[1].instance);
  auto* sitter=world.findNpcByInstance(save.actors[2].instance);
  const auto* seat=world.findPoint("FP_SIT_CAMPFIRE_PAUSE_02",false);
  const auto* row=world.findWayPoint("NW_FARM2_FIELD_03_B");
  const auto* edge=world.findWayPoint(save.edge);
  require(walker && waiter && sitter && seat && row && edge,"the whole-game load restores the actors, seat and waypoints");
  const std::array<Npc*,3> group={walker,waiter,sitter};
  for(size_t i=0;i<group.size();++i) {
    const auto& npc=*group[i];
    const auto& saved=save.actors[i];
    require(npc.position()==saved.position && npc.rotation()==saved.heading && npc.bodyStateMasked()==saved.pose,
            "loading the whole game restores each camper's exact position, facing and pose");
    }
  require(walker->currentGoToPoint()==edge && walker->navigationGoal().point==row,
          "the walker loads on the same edge with the same destination");
  require(waiter->currentWayPoint()!=seat && !waiter->navigationGoal().active &&
          sitter->currentWayPoint()==seat && seat->useCounter()==1 && sitter->animProgress("S_SIT")>=0,
          "the owner and the standing waiter keep their places through the load");

  test.only({walker,waiter,sitter});
  const auto heroAt=world.player()->position();
  const auto heroHeading=world.player()->rotation();
  test.moveHero(heroAt,heroHeading);
  const auto previous=walker->position();
  const auto travel=walker->simplifiedMoveSpeed()*33.f;
  test.step();
  const auto step=walker->position()-previous;
  require(walker->currentGoToPoint()==edge && std::hypot(step.x,step.z)<=travel+0.1f && std::abs(step.y)<=5.f,
          "the first tick after a load stays on the saved edge within one step");
  test.stepUntil(30,[&] {
    return walker->position()!=previous && sitter->campfireBodySuspended() && waiter->campfireBodySuspended() &&
           ActivityEventsWorld::stage(*waiter)==Stage::Wait;
    },"the walker continues and the waiter resumes waiting without collision");
  require(waiter->position()==save.actors[1].position && sitter->position()==save.actors[2].position &&
          seat->useCounter()==1 && sitter->currentWayPoint()==seat && !hitsCamper(world,*waiter) && !hitsCamper(world,*sitter),
          "waiting and sitting retain their locations and sole owner while distant after a load");

  test.moveHero(row->position(),heroHeading);
  require(hitsCamper(world,*waiter) && hitsCamper(world,*sitter) && sitter->animProgress("S_SIT")>=0 && seat->useCounter()==1,
          "approaching after the group load restores the waiting and sitting collisions without changing ownership");
  test.only({walker,waiter});
  test.stop(*sitter);
  sitter->setPosition(heroAt);
  sitter->attachToPoint(nullptr);
  test.stepUntil(900,[&] { return waiter->currentWayPoint()==seat && waiter->animProgress("S_SIT")>=0; },
                 "the loaded waiter reacts to the seat being freed and sits there");
  require(seat->useCounter()==1 && calls.phase(*walker,"ZS_SIT_CAMPFIRE")==0 && calls.phase(*waiter,"ZS_SIT_CAMPFIRE")==0 &&
          calls.phase(*sitter,"ZS_SIT_CAMPFIRE")==0,"the whole-game load and seat transfer preserve one owner and never repeat Init");
  std::printf("E3.3 campfire: whole-game load kept edge %s, first step %.2f cm, wait and seat owner; zero repeated Init\n",
              save.edge.c_str(),double(std::hypot(step.x,step.z)));
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
  // This offset from INHOUSE_02 stays in FARM01. INHOUSE_01 lies on the FARM03 portal floor.
  auto inside=post->groundPos+Tempest::Vec3(0,0,-180);
  const auto floor=world.physic()->landRay(inside+Tempest::Vec3(0,100,0),250);
  require(floor.hasCol,"quiet sound: the same-room position must have a floor");
  inside.y=floor.v.y;
  auto& hero=*world.player();
  auto& events=Contract::activityEventsOn(script);
  const bool enabled=events;
  auto& vm=script.getVm();
  const auto observe=script.findFunction("ZS_OBSERVEPLAYER");
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
    test.moveHero(sameRoom ? inside : otherRoom->groundPos,0);
    test.start(activity,post->groundPos,npc,false);
    npc->setAttitude(ATT_NEUTRAL);
    Contract::tickRoutine(*npc);
    for(size_t i=0;i<PERC_Count;++i)
      if(i!=PERC_ASSESSQUIETSOUND) npc->setPerceptionDisable(PercType(i));
    test.only({npc});
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
    npc->setStateTime(0);
    require(npc->isInState(stand) && npc->hasPerc(PERC_ASSESSQUIETSOUND) && npc->qDistTo(hero)<1000.f*1000.f,
            "quiet sound: the standing listener is close enough to check the room");
    if(sameRoom) {
      const auto room=world.roomAt(npc->position());
      require(!room.empty() && room==world.roomAt(hero.position()) && npc->canSeeNpc(hero,false),
              "quiet sound: the listener sees the hero in the same geometric room");
      }
    };
  struct Site { bool sameRoom; int height; const char* label; };
  for(const Site site : {Site{true,0,"same room"},Site{false,0,"separate room"},Site{true,251,"indoor height"}}) {
    std::vector<std::string> original;
    for(bool native:{false,true}) {
      prepare(site.sameRoom);
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
      require(heard && quietCalls==(native ? 0u : 1u) && roomCalls==(site.height==0 ? 1u : 0u),
              "quiet sound: ordinary perception preserves the indoor height gate in both executors");
      if(site.height==0) {
        const auto* queued=npc->aiQueueFront();
        require(queued && queued->act==AI_StartState && queued->func==observe && queued->target==&hero &&
                clearCalls==1 && fakeBanditCalls==(site.sameRoom ? 0u : 1u),
                "quiet sound: the public room and the unseen source reach their corresponding observation transitions");
        }
      if(site.height!=0) require(npc->isAiQueueEmpty(),"quiet sound: the other floor leaves the original activity in place");
      const auto queue=activityQueue(*npc);
      if(native) require(queue==original,"quiet sound: the original and native handlers leave the same room reaction queue");
      else original=queue;
      }
    std::printf("D5.1 %s: ordinary quiet perception preserves the stock queue (%zu actions)\n",
                site.label,original.size());
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
  test.moveHero(beyond->groundPos,hero.rotation());
  test.stepUntil(240,[&] { return npc->isInState(stand) && Contract::aiState(*npc).started; },
                 "quiet sound: the listener resumes its original routine when the source leaves");
  std::puts("D5.1 physical sound: ordinary load, immediate observation and return to the standing routine");
  }

void quietSoundAtPost(Gothic& gothic) {
  constexpr auto* state="ZS_STAND_ARMSCROSSED";
  constexpr auto* actor="VLK_412_HARAD";
  constexpr auto* point="FP_STAND_CITY_20";
  constexpr uint32_t seed=73106;
  std::string program;
  loadBaseProgram(gothic.defaultGameDatFile(),Settings::getI("GAME","language"),&program);
  if(program!="a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c") {
    std::puts("SKIP quiet sound: the fixtures are for the verified NotR program");
    return;
    }
  const auto directory=std::filesystem::temp_directory_path()/("gothic-quiet-sound-"+std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  NpcDayJournal::Options trace;
  trace.directory=directory.string();
  trace.seed=seed;
  trace.isolate=false;
  trace.traceNpcs={actor};
  const auto settled=[&](const Npc& npc, const WayPoint& post) {
    return npc.activityStage()==ActivityEvents::Stage::Hold && npc.currentWayPoint()==&post &&
           npc.isAiQueueEmpty() && npc.animProgress("S_LGUARD")>=0;
    };
  const auto run=[&](const char* leg, float height, bool native, bool checkReturn) {
    ActivityEventsWorld test(gothic);
    auto& world=test.world();
    auto& script=world.script();
    auto* npc=activityNpc(world,actor);
    const auto* post=world.findPoint(point,false);
    require(npc && post && settled(*npc,*post),"quiet sound: the listener holds the original post and pose");
    test.only({npc});
    script.setSeed(seed);
    npc->setStateTime(0);
    auto& hero=*world.player();
    auto source=npc->position()+Tempest::Vec3(0,height,0);
    source.y=std::nextafter(source.y,std::numeric_limits<float>::infinity());
    test.moveHero(source,hero.rotation());
    require(int32_t(std::abs(hero.position().y-npc->position().y))==int32_t(height),
            "quiet sound: the source has the exact integer height difference");
    require(npc->hasPerc(PERC_ASSESSQUIETSOUND),"quiet sound: the listener still has its callback");
    const auto start=npc->rotation();
    float excursion=0;
    const auto offset=[&] { return std::abs(std::remainder(npc->rotation()-start,360.f)); };
    auto& vm=script.getVm();
    const auto observers=vm.call_observer_count();
    std::string error;
    if(!NpcDayMeasurement::start(world,trace,error)) throw std::runtime_error(error);
    require(vm.call_observer_count()==observers,"quiet sound: trace-only must not attach a VM observer");
    size_t vmCalls=0,quietCalls=0,roomCalls=0;
    const auto observer=vm.add_call_observer([&](const auto& symbol) {
      ++vmCalls;
      if(symbol.name()=="B_ASSESSQUIETSOUND") ++quietCalls;
      if(symbol.name()=="B_ASSESSENTERROOM") ++roomCalls;
      });
    PhysicsNoise impact;
    impact.culprit=&hero;
    impact.position=npc->position()+Tempest::Vec3(-post->dir.z,0,post->dir.x)*300.f;
    auto& events=NpcDefinitionContract::activityEventsOn(script);
    const bool enabled=events;
    events=native;
    const bool heard=npc->hearPhysicsNoise(impact);
    events=enabled;
    require(heard,"quiet sound: the impact from a known source is heard");
    if(checkReturn) {
      const auto until=world.tickCount()+2000;
      test.stepUntil(120,[&] {
        return world.tickCount()>=until && settled(*npc,*post) && offset()<1.f && excursion>20.f;
        },"quiet sound: the listener turns and realigns on the same post",[&] {
        require(npc->currentWayPoint()==post && post->useCounter()==1,"quiet sound: the post must retain its sole owner");
        excursion=std::max(excursion,offset());
        NpcDayMeasurement::tick(world);
        });
      }
    vm.remove_call_observer(observer);
    NpcDayMeasurement::stop(world);
    require(quietCalls==(native ? 0u : 1u) && roomCalls==(height==1000.f ? 1u : 0u) &&
            (!checkReturn || vmCalls==(native ? 0u : 1u)),
            "quiet sound: the expected executor preserves the early height gate and includes the exact boundary");
    require(!std::filesystem::exists(directory/"npc-day-script.json"),"quiet sound: trace-only writes no VM summary");
    std::ifstream rows(directory/"npc-day-activity.jsonl");
    bool noiseRow=false,returnRow=false;
    for(std::string line;std::getline(rows,line);) {
      rapidjson::Document row;
      row.Parse(line.c_str());
      require(!row.HasParseError() && row.IsObject() && row.HasMember("kind"),"quiet sound: every trace row must be valid JSON");
      const std::string_view kind=row["kind"].GetString();
      if(kind=="header") continue;
      require(row["symbol"].GetString()==std::string_view(actor),"quiet sound: only the requested actor is traced");
      if(kind=="physics_noise") {
        noiseRow=row["executor"].GetString()==std::string_view(native ? "engine" : "vm");
        std::printf("D5 %s %s\n",leg,line.c_str());
        }
      if(kind=="activity_transition" && row["to"].GetString()==std::string_view("hold") &&
         row["queue_head"].GetString()==std::string_view(AiQueue::actionName(AI_AlignToFp))) {
        returnRow=row["place"].GetString()==std::string_view(point) && row["owner"].IsUint() &&
                  row["owner"].GetUint()==world.npcId(npc);
        std::printf("D5 %s %s\n",leg,line.c_str());
        }
      }
    require(noiseRow && (!checkReturn || returnRow),"quiet sound: trace identifies the executor and the return to the owned post");
    std::printf("D5 %s: %zu VM calls (%zu quiet callbacks), maximum turn %.1f degrees\n",leg,vmCalls,quietCalls,double(excursion));
    return excursion;
    };

  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  std::vector<uint8_t> save;
  {
    ActivityEventsWorld test(gothic);
    auto& world=test.world();
    auto* npc=activityNpc(world,actor);
    const auto* post=world.findPoint(point,false);
    require(npc && post,"quiet sound: the listener and his post must exist");
    test.freePoint(post);
    test.moveHero(post->position()+Tempest::Vec3(400,0,0),0);
    test.only({npc});
    world.script().setSeed(seed);
    EventActor activity;
    activity.npc=npc;
    activity.state=state;
    activity.phases=activityPhases(world.script(),state);
    activity.pointName="NW_CITY_HABOUR_POOR_AREA_PATH_20";
    test.start(activity,post->position(),npc,false);
    npc->attachToPoint(post);
    npc->setDirection(post->direction());
    npc->updateTransform();
    test.stepUntil(300,[&] { return settled(*npc,*post); },"quiet sound: Harad settles on the post");
    // Isolate the reaction from unrelated ambient perceptions throughout the comparison.
    for(size_t i=0;i<PERC_Count;++i)
      if(i!=PERC_ASSESSQUIETSOUND) npc->setPerceptionDisable(PercType(i));
    Tempest::MemWriter writer(save);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"quiet sound before impact",preview);
  }
  const auto originalTurn=run("original",1100.f,false,true);
  gothic.clearGame().reset();
  {
    Tempest::MemReader reader(save);
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
  }
  {
    ActivityEventsWorld test(gothic);
    auto* npc=activityNpc(test.world(),actor);
    const auto* post=test.world().findPoint(point,false);
    require(npc && post,"quiet sound: the ordinary load restores the listener and the post");
    test.only({npc});
    test.stepUntil(120,[&] { return settled(*npc,*post); },"quiet sound: the loaded listener restores his pose");
  }
  require(run("reloaded",1100.f,true,true)==originalTurn,"quiet sound: native replay after loading turns as far as the original");
  // The cooldown is one tick longer than the native turn. Keep the same stance for the boundary check.
  ActivityEventsWorld(gothic).step();
  run("threshold",1000.f,true,false);
  quietSoundRooms(gothic);
  gothic.clearGame().reset();
  std::filesystem::remove_all(directory);
  }

void seatedGestureAtObject(Npc& npc,uint32_t range) {
  using Contract=NpcDefinitionContract;
  auto& script=npc.world().script();
  const auto loop=Contract::aiState(npc).funcLoop;
  auto& local=*script.getVm().find_symbol_by_name(std::string(script.symbolName(loop.ptr))+".RANDOM");
  script.setSeed(73106);
  local.set_int(-1);
  auto expected=Contract::random(script);
  npc.setStateTime(5999);
  Contract::tickRoutine(npc);
  require(local.get_int()==-1 && Contract::random(script)==expected && npc.isAiQueueEmpty(),
          "seated gesture: no draw before six whole seconds");
  npc.setStateTime(6000);
  Contract::tickScheduledRoutine(npc);
  require(local.get_int()==-1 && Contract::random(script)==expected && npc.isAiQueueEmpty(),
          "seated gesture: an elapsed timer waits for the perception beat");
  constexpr std::array clips={"R_CHAIR_RANDOM_1","R_CHAIR_RANDOM_2","R_CHAIR_RANDOM_3","R_CHAIR_RANDOM_4"};
  bool full=false,silent=false;
  for(size_t i=0;i<64 && !(full && silent);++i) {
    const auto draw=uint32_t(expected()%range);
    npc.setStateTime(6000);
    Contract::tickRoutine(npc);
    require(Contract::random(script)==expected && local.get_int()==int32_t(draw) && npc.stateTime()==0,
            "seated gesture: one draw, the original saved local and a reset on every outcome");
    auto queue=Contract::queue(npc);
    for(size_t j=draw;j<clips.size();++j) {
      require(queue.size()>0,"seated gesture: every animation for the same draw must be queued");
      const auto action=queue.pop();
      require(action.act==AI_PlayAnimBs && action.s0==clips[j] && action.i0==65547,
              "seated gesture: retain the order of the cumulative animations and the sitting body state");
      }
    require(queue.size()==0,"seated gesture: the draw queues no other actions");
    full|=draw==0;
    silent|=draw>=clips.size();
    npc.clearAiQueue();
    }
  require(full && silent,"seated gesture: the fixed seed covers a full chain and a silent outcome");
  }

void workMobActivitiesWorld(ActivityEventsWorld& test,bool events) {
  using Contract=NpcDefinitionContract;
  struct Site { const char* state; const char* actor; const char* scheme; int body; int seconds; };
  constexpr std::array sites={
    Site{"ZS_SIT_THRONE","BAU_910_MARIA","THRONE",65547,0},
    Site{"ZS_SAW","BAU_964_BAUER","BAUMSAEGE",32784,15},
    Site{"ZS_REPAIR_HUT","VLK_480_FELLAN","REPAIR",32784,5},
    Site{"ZS_PRAY_INNOS","PAL_299_SERGIO","INNOS",BS_MOBINTERACT,0},
    Site{"ZS_ROAST_SCAVENGER",nullptr,"BARBQ",32784,0},
    };
  auto& world=test.world();
  auto& script=world.script();
  ActivityCalls calls;
  for(const auto& site:sites) {
    EventActor activity;
    if(site.actor) {
      auto* npc=activityNpc(world,site.actor);
      require(npc!=nullptr,"work MOB: the routine actor must exist");
      activity=test.actor(site.state,site.scheme,npc);
      } else {
      // The roast has a real object in NEWWORLD but no actor in its normal day.
      activity.npc=test.extra();
      activity.phases=activityPhases(script,site.state);
      activity.point=world.findWayPoint("NW_TROLLAREA_PORTAL_KDWWAIT_04");
      require(activity.point!=nullptr,"work MOB: the roasting waypoint must exist");
      activity.pointName=activity.point->name;
      activity.state=site.state;
      activity.scheme=site.scheme;
      }
    auto& npc=*activity.npc;
    const auto at=activity.point->position();
    test.only({&npc});
    test.moveHero(at+Tempest::Vec3(350,0,0),0);
    test.start(activity,at,&npc,false);
    Contract::tickRoutine(npc);
    npc.setPerceptionDisable(PERC_ASSESSPLAYER);
    auto* object=world.availableMob(npc,site.scheme);
    require(object!=nullptr,(activity.state+": the routine has a free object").c_str());
    test.stepUntil(750,[&] {
      return npc.interactive()==object && object->userCount()==1 && object->isStaticState() && npc.isAiQueueEmpty() &&
             ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),site.body) &&
             (!events || test.stage(npc)==ActivityEvents::Stage::Hold);
      },activity.state+" holds the real object");
    const auto loop=activity.state+"_LOOP";
    if(std::string_view(site.state)=="ZS_PRAY_INNOS") {
      // The shrine holds the non-interruptible body state, outside the Loop's timer guard.
      npc.setStateTime(6000);
      Contract::aiState(npc).loopNextTime=world.tickCount();
      Contract::tickRoutine(npc);
      require(npc.stateTime()==6000,"the shrine's actual pose keeps the original timer untouched");
      npc.clearAiQueue();
      }
    if(site.seconds>0) {
      const auto boundary=uint64_t(site.seconds+1)*1000;
      npc.setStateTime(boundary-1);
      Contract::aiState(npc).loopNextTime=world.tickCount();
      Contract::tickRoutine(npc);
      require(npc.stateTime()==boundary-1 && npc.isAiQueueEmpty(),"work MOB: the timer waits for the whole second");
      auto* repair=script.getVm().find_symbol_by_name(loop+".ZUFALL");
      script.setSeed(73106);
      auto expected=Contract::random(script);
      bool animated=false,silent=false;
      for(size_t i=0;i<(repair ? 32u : 1u);++i) {
        const int32_t roll=repair ? int32_t(expected()%100) : -1;
        npc.setStateTime(boundary);
        Contract::aiState(npc).loopNextTime=world.tickCount();
        Contract::tickRoutine(npc);
        require(npc.stateTime()==0 && Contract::random(script)==expected,"work MOB: reset and expected random stream");
        if(repair)
          require(repair->get_int()==roll,"repair: the saved roll is the draw from this perception beat");
        const auto* action=npc.aiQueueFront();
        if(roll>=0 && roll<50) {
          require(action && action->act==AI_PlayAnimBs && action->s0=="T_REPAIR_RANDOM_1" && action->i0==site.body,
                  "repair: a successful draw queues the original gesture");
          animated=true;
          } else {
          require(action==nullptr,"work MOB: a silent timer reset queues no command");
          silent=true;
          }
        npc.clearAiQueue();
        if(animated && silent)
          break;
        }
      require(silent && (!repair || animated),"work MOB: fixed draws cover the required outcomes");
      }
    const auto count=calls.phase(npc,loop);
    std::printf("Work MOB %s (events=%d): object %u, %zu Loop calls\n",site.state,int(events),object->zenId(),count);
    require((count==0)==events,"work MOB: the routine uses the requested executor");
    npc.clearState(false);
    require(object->isAttached(npc) && npc.aiQueueFront() && npc.aiQueueFront()->act==AI_UseMob &&
            npc.aiQueueFront()->i0==-1,"work MOB: queue the original leave before releasing ownership");
    test.stepUntil(650,[&] { return !object->isAttached(npc) && !test.places().reserved(npc); },activity.state+" releases its object");
    test.stop(npc);
    }
  }

void seatedActivitiesWorld(Gothic& gothic,bool events) {
  using Contract=NpcDefinitionContract;
  using ActivityEvents::Stage;
  struct Site { const char* state; const char* actor; const char* scheme; uint32_t range; };
  constexpr std::array sites={Site{"ZS_SIT_BENCH","VLK_468_CANTHAR","BENCH",8},
                              Site{"ZS_SIT_CHAIR","MIL_313_BOLTAN","CHAIR",10}};
  struct Saved { uint32_t object=0,zen=0; Tempest::Vec3 position; float heading=0; uint32_t pose=0; };
  std::array<Saved,sites.size()> seats;
  std::vector<uint8_t> save;
  const auto sitting=[](const Npc& npc,const Interactive& object) {
    return npc.interactive()==&object && object.userCount()==1 && object.isStaticState() &&
           ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),65547) && npc.isAiQueueEmpty();
    };
  const auto* output=std::getenv("OPENGOTHIC_BENCH_OUT");
  if(output && *output=='\0') output=nullptr;
  const auto directory=output ? std::filesystem::path(output)/(events ? "seated-events" : "seated-vm") :
      std::filesystem::temp_directory_path()/("gothic-seated-"+std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  {
    ActivityEventsWorld test(gothic);
    auto& world=test.world();
    auto& vm=world.script().getVm();
    NpcDayJournal::Options trace;
    trace.directory=directory.string();
    trace.seed=73106;
    trace.isolate=false;
    trace.scriptProbe=true;
    for(const auto& site:sites) trace.traceNpcs.emplace_back(site.actor);
    std::string error;
    if(!NpcDayMeasurement::start(world,trace,error)) throw std::runtime_error(error);
    std::array<size_t,sites.size()> loopCalls{};
    const auto observer=vm.add_call_observer([&](const auto& function) {
      for(size_t i=0;i<sites.size();++i)
        if(function.name()==std::string(sites[i].state)+"_LOOP") ++loopCalls[i];
      });
    ActivityEvents::reset();
    for(size_t i=0;i<sites.size();++i) {
      const auto& site=sites[i];
      auto* actor=activityNpc(world,site.actor);
      require(actor!=nullptr,"seated activity: the original routine actor must exist");
      auto activity=test.actor(site.state,site.scheme,actor);
      const auto spot=activity.point->position();
      test.only({actor});
      test.moveHero(spot+Tempest::Vec3(350,0,0),0);
      test.start(activity,spot,actor,false);
      Contract::tickRoutine(*actor);
      actor->setPerceptionDisable(PERC_ASSESSPLAYER); // keep the nearby hero out of the seating comparison
      auto* object=world.availableMob(*actor,site.scheme);
      require(object!=nullptr,"seated activity: the original routine has a free object");
      if(i==0) {
        auto& blocker=*test.extra();
        test.only({});
        test.occupy(blocker,*object,site.scheme,spot);
        require(world.availableMob(*actor,site.scheme)==nullptr,"seated activity: the contested bench is the only seat in reach");
        test.only({actor});
        test.stepUntil(180,[&] {
          return events ? test.stage(*actor)==Stage::Wait : loopCalls[i]>0;
          },"the routine waits for the occupied bench");
        require(object->userCount()==1 && object->isAttached(blocker) && actor->interactive()==nullptr,
                "seated activity: the waiter cannot take the occupied bench");
        test.leave(blocker,site.scheme,*test.awayFrom(*activity.point));
        test.stepUntil(180,[&] {
          const auto* head=actor->aiQueueFront();
          return head && head->act==AI_UseMob && head->i0==1;
          },"the freed bench wakes the waiter and queues one use");
        loadSavedNpc(*actor,savedNpc(*actor));
        require(actor->aiQueueFront() && actor->aiQueueFront()->act==AI_UseMob && !test.places().reserved(*actor),
                "seated activity: a saved use survives without a saved reservation");
        }
      test.stepUntil(600,[&]{ return sitting(*actor,*object) && (!events || test.stage(*actor)==Stage::Hold); },
                     std::string(site.state)+" holds the real object");
      seatedGestureAtObject(*actor,site.range);
      seats[i].object=world.mobsiId(object);
      seats[i].zen=object->zenId();
      std::printf("Seated %s (events=%d): object %u, %zu VM Loop calls\n",
                  site.state,int(events),object->zenId(),loopCalls[i]);
      require((loopCalls[i]==0)==events,"seated activity: the runtime selects the requested executor");
      }
    vm.remove_call_observer(observer);
    NpcDayMeasurement::stop(world);
    Tempest::MemWriter writer(save);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"bench and chair",preview);
    // The world synchronizes any deferred animation as part of saving.
    for(size_t i=0;i<sites.size();++i) {
      auto& npc=*activityNpc(world,sites[i].actor);
      require(sitting(npc,*world.mobsiById(seats[i].object)),"seated activity: both actors remain seated at save time");
      seats[i].position=npc.position();
      seats[i].heading=npc.rotation();
      seats[i].pose=npc.bodyStateMasked();
      }
  }
  gothic.clearGame().reset();
  {
    Tempest::MemReader reader(save);
    Serialize in(reader);
    ActivityCalls calls;
    gothic.setGame(std::make_unique<GameSession>(in));
    ActivityEventsWorld test(gothic);
    for(size_t i=0;i<sites.size();++i) {
      const auto& site=sites[i];
      auto* npc=activityNpc(test.world(),site.actor);
      require(npc!=nullptr,"seated activity: the whole-game load restores the actor");
      auto* object=npc->interactive();
      const auto drift=(npc->position()-seats[i].position).length();
      const auto turn=std::abs(std::remainder(npc->rotation()-seats[i].heading,360.f));
      std::printf("Seated load %s (events=%d): seat %u -> %u, owners %zu, pose %u -> %u, drift %.3f cm, turn %.3f degrees\n",
                  site.state,int(events),seats[i].zen,object ? object->zenId() : uint32_t(-1),
                  object ? size_t(object->userCount()) : 0u,seats[i].pose,uint32_t(npc->bodyStateMasked()),
                  double(drift),double(turn));
      require(object && object->zenId()==seats[i].zen && sitting(*npc,*object) &&
              drift<1.f && turn<1.f && npc->bodyStateMasked()==seats[i].pose,
              "seated activity: the whole-game load restores the same seat, position, facing and pose");
      test.only({npc});
      test.moveHero(npc->position()+Tempest::Vec3(350,0,0),0);
      npc->setPerceptionDisable(PERC_ASSESSPLAYER);
      Contract::tickRoutine(*npc);
      require(sitting(*npc,*object) && calls.phase(*npc,site.state)==0 && calls.uses(*npc)==0,
              "seated activity: loading does not repeat Init or queue another use");
      if(i==0)
        npc->clearState(false);
      else {
        Contract::aiState(*npc).eTime=test.world().time();
        Contract::tickRoutine(*npc);
        }
      require(object->isAttached(*npc) && npc->aiQueueFront() && npc->aiQueueFront()->act==AI_UseMob &&
              npc->aiQueueFront()->i0==-1,"seated activity: leaving is queued before releasing ownership");
      test.stepUntil(600,[&] { return !object->isAttached(*npc) && !test.places().reserved(*npc); },
                     std::string(site.state)+" releases its seat");
      }
    workMobActivitiesWorld(test,events);
  }
  gothic.clearGame().reset();
  if(!output) std::filesystem::remove_all(directory);
  }

void sleepRoutineWorld(Gothic& gothic,bool events) {
  using Contract=NpcDefinitionContract;
  ActivityEventsWorld test(gothic);
  auto& world=test.world();
  auto* matteo=activityNpc(world,"VLK_416_MATTEO");
  require(matteo!=nullptr,"sleep routine: the authored bed user must exist");
  auto bedtime=test.actor("ZS_GOTOBED","BEDHIGH",matteo);
  const auto sleep=activityPhases(world.script(),"ZS_SLEEP");
  const auto* approach=test.awayFrom(*bedtime.point,200.f);
  test.only({matteo});
  test.moveHero(bedtime.point->position()+Tempest::Vec3(150,0,0),0);
  test.start(bedtime,bedtime.point->position(),matteo,false);
  Contract::tickRoutine(*matteo);
  auto samePoint=Contract::queue(*matteo);
  bool startsSleep=false;
  while(samePoint.size()>0) {
    const auto action=samePoint.pop();
    require(action.act!=AI_GoToPoint,"a sleeper already at the waypoint skips the approach");
    startsSleep |= action.act==AI_StartState && action.func==ScriptFn(sleep.init);
    }
  require(startsSleep,"the bedtime state queues sleep when the NPC is already at the waypoint");
  test.start(bedtime,approach->position(),matteo,false);
  const auto deadline=Contract::aiState(*matteo).eTime;
  Contract::tickRoutine(*matteo);
  matteo->setPerceptionDisable(PERC_ASSESSPLAYER);
  const auto approaches=activityQueue(*matteo);
  require(approaches.size()>=2 && approaches.back().starts_with("StartState "),
          "sleep routine: the approach queues the sleep transition last");

  auto& vm=world.script().getVm();
  size_t vmLoops=0;
  const auto observer=vm.add_call_observer([&](const auto& function) {
    vmLoops+=function.name()=="ZS_SLEEP_LOOP";
    });
  const auto settled=[&] {
    const auto* object=matteo->interactive();
    return matteo->isInState(ScriptFn(sleep.init)) && object && object->checkMobName("BEDHIGH") &&
           object->userCount()==1 && object->isStaticState() && matteo->isAiQueueEmpty() &&
           ActivityDefinitions::bodyStateContains(int32_t(matteo->bodyState()),BS_LIE);
    };
  test.stepUntil(1200,settled,"Matteo sleeps in a bed after approaching his routine waypoint");
  require(Contract::aiState(*matteo).eTime==deadline,"sleep routine: the transition preserves the row's end time");
  test.step(90);
  require(settled() && (vmLoops==0)==events,"sleep routine: a held bed remains settled with the selected executor");
  vm.remove_call_observer(observer);

  Contract::aiState(*matteo).eTime=world.time();
  Contract::aiState(*matteo).loopNextTime=world.tickCount();
  Contract::tickRoutine(*matteo);
  auto tail=Contract::queue(*matteo);
  require(tail.size()!=0,"sleep routine: waking queues a bed release");
  while(tail.size()>1)
    tail.pop();
  const auto release=tail.pop();
  require(release.act==AI_UseMob && release.s0=="BEDHIGH" && release.i0==-1,
          "sleep routine: the end requests the normal get-up animation");
  test.stepUntil(600,[&] { return matteo->interactive()==nullptr; },"Matteo leaves his bed");
  std::printf("Sleep routine (events=%d): %zu VM Loop calls; approach, handoff, held bed and release verified\n",
              int(events),vmLoops);
  }

// The switch and the accepted list through the loader the game calls.
void activitySwitch(GameScript& script,const std::vector<std::string>& accepted,const char* other,size_t pointPhases) {
  const auto base=baseProgramIndex().dir.parent_path();
  std::string sha;
  loadBaseProgram(Gothic::inst().defaultGameDatFile(),Settings::getI("GAME","language"),&sha);
  const auto* configured=std::getenv("OPENGOTHIC_ACTIVITY_RECORDS");
  const std::optional<std::string> previous=configured ? std::optional<std::string>(configured) : std::nullopt;
  struct Restore {
    const std::optional<std::string>& previous;
    ~Restore() { if(previous) setenv("OPENGOTHIC_ACTIVITY_RECORDS",previous->c_str(),1); else unsetenv("OPENGOTHIC_ACTIVITY_RECORDS"); }
    } restore{previous};
  // The phase lookup the game's dispatcher uses, for all three phases.
  auto runs=[&](const ActivityDefinitionRuntime::Definitions& definitions,const char* state) {
    if(state==nullptr) return false;
    const auto& st=script.aiState(script.findSymbolIndex(state));
    return ActivityDefinitionRuntime::find(definitions,uint32_t(st.funcIni))!=nullptr &&
           ActivityDefinitionRuntime::find(definitions,uint32_t(st.funcLoop))!=nullptr &&
           ActivityDefinitionRuntime::find(definitions,uint32_t(st.funcEnd))!=nullptr;
    };
  auto loaded=[&](const char* value) {
    if(value) setenv("OPENGOTHIC_ACTIVITY_RECORDS",value,1); else unsetenv("OPENGOTHIC_ACTIVITY_RECORDS");
    const auto definitions=ActivityDefinitionRuntime::load(base,script.getVm(),sha);
    size_t acceptedRuns=0;
    for(const auto& state:accepted)
      acceptedRuns+=runs(definitions,state.c_str()) ? 1 : 0;
    return std::tuple{acceptedRuns,runs(definitions,other),definitions.phases.size()};
    };
  const auto all=accepted.size(),captured=all+(other!=nullptr ? 1 : 0);
  require(loaded(nullptr)==std::tuple{all,false,3*all+pointPhases},"by default every accepted activity, and only those, runs from its record");
  require(loaded("1")==std::tuple{all,false,3*all+pointPhases},"another switch value keeps the accepted list");
  require(loaded("0")==std::tuple{size_t(0),false,size_t(0)},"OPENGOTHIC_ACTIVITY_RECORDS=0 must run every activity in the VM");
  require(loaded("all")==std::tuple{all,other!=nullptr,3*captured+pointPhases},
          "OPENGOTHIC_ACTIVITY_RECORDS=all must run every captured activity from its record");
  std::printf("Activity switch: default and 1 run the %zu accepted states only, 0 none, all every captured state\n",all);
  }

void pairedSmalltalkWorld(ActivityEventsWorld& test,bool events) {
  using Contract=NpcDefinitionContract;
  auto& world=test.world();
  auto& script=world.script();
  auto* first=activityNpc(world,"BAU_918_BAUER");
  auto* second=activityNpc(world,"BAU_919_BAUER");
  const auto* firstPost=world.findPoint("FP_SMALLTALK_NW_BIGFARM_KITCHEN_03",false);
  const auto* secondPost=world.findPoint("FP_SMALLTALK_NW_BIGFARM_KITCHEN_04",false);
  require(first && second && firstPost && secondPost,"smalltalk: the farm kitchen pair and posts exist");
  test.freePoint(firstPost);
  test.freePoint(secondPost);
  test.moveHero(firstPost->position()+Tempest::Vec3(300,0,0),0);
  test.only({first,second});
  EventActor talk;
  talk.phases=activityPhases(script,"ZS_SMALLTALK");
  talk.state="ZS_SMALLTALK";
  talk.pointName="NW_BIGFARM_KITCHEN_09";
  auto prepare=[&](Npc& actor,const WayPoint& post) {
    test.start(talk,post.position(),&actor,false);
    Contract::tickRoutine(actor);
    // This scene starts after the approach, on the two points from the day journal.
    actor.clearAiQueue();
    actor.attachToPoint(&post);
    require(actor.isOnFreePoint("SMALLTALK"),"smalltalk: the actor is at the authored free point");
    };
  prepare(*first,*firstPost);
  prepare(*second,*secondPost);
  require(first->qDistTo(*second)<500.f*500.f,"smalltalk: the other actor is within dialogue distance");

  {
    ActivityStream stream(world,*first);
    auto& vm=script.getVm();
    auto& roll=*vm.find_symbol_by_name("B_SAY_SMALLTALK.RANDOM");
    auto& choice=*vm.find_symbol_by_name("B_SAY_SMALLTALK.CHOICE");
    roll.set_int(-1);
    choice.set_int(-1);
    second->setStateTime(1200);
    script.setSeed(42);
    auto expected=Contract::random(script);
    first->setStateTime(7999);
    Contract::aiState(*first).loopNextTime=world.tickCount();
    Contract::tickRoutine(*first);
    require(roll.get_int()==-1 && choice.get_int()==-1 && Contract::random(script)==expected &&
            second->stateTime()==1200 && first->isAiQueueEmpty(),"smalltalk: no turn or RNG before eight seconds");

    const auto draw=expected()%120;
    expected(); // The original draws again with bound one.
    first->setStateTime(8000);
    Contract::aiState(*first).loopNextTime=world.tickCount();
    Contract::tickRoutine(*first);
    require(first->stateTime()==0 && second->stateTime()==4000,"smalltalk: reset the speaker and stagger the partner");
    require(Contract::random(script)==expected && roll.get_int()==int32_t(draw) && choice.get_int()==0,
            "smalltalk: preserve both draws and both saved locals");
    auto queue=Contract::queue(*first);
    require(queue.size()==2,"smalltalk: exactly one turn and one SVM line");
    const auto turn=queue.pop();
    const auto line=queue.pop();
    require(turn.act==AI_TurnToNpc && turn.target==second &&
            line.act==AI_OutputSvm && line.target==first && line.s0=="$SMALLTALK21",
            "smalltalk: queue the turn toward the partner before the seeded line to self");
    require(stream.records==size_t(events),"smalltalk: run the selected executor at the timed wake");
    }
  test.stop(*first);
  test.stop(*second);
  first->attachToPoint(nullptr);
  second->attachToPoint(nullptr);
  }

// The real game's item symbols and STAND post, used by both executors. The
// caller discards this world after the case, so no other actor runs in it.
void itemRoutinePhasesWorld(Gothic& gothic,bool events) {
  using Contract=NpcDefinitionContract;
  ActivityEventsWorld test(gothic);
  auto& world=test.world();
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& npc=*test.extra();
  const auto* post=world.findPoint("FP_STAND_CITY_20",false);
  require(post,"item routine: the harbour STAND post exists");
  npc.clearState(true);
  npc.setPosition(post->position());
  npc.attachToPoint(post);
  npc.updateTransform();
  require(npc.isOnFreePoint("STAND"),"item routine: the actor reached the post");
  constexpr std::array food={"ITFO_APPLE","ITFO_CHEESE","ITFO_BACON","ITFOMUTTON"};
  constexpr std::array foodGesture={"T_FOOD_RANDOM_1","T_FOODHUGE_RANDOM_1","T_MEAT_RANDOM_1","T_MEAT_RANDOM_1"};
  for(const auto* name:{"ZS_STAND_EATING","ZS_STAND_DRINKING"}) {
    const bool eating=std::string_view(name)=="ZS_STAND_EATING";
    const auto phases=activityPhases(script,name);
    std::string loopName=std::string(name)+"_LOOP";
    size_t loops=0;
    const auto observer=vm.add_call_observer([&](const auto& fn) { if(fn.name()==loopName) ++loops; });
    auto invoke=[&](size_t phase) { script.invokeState(&npc,nullptr,nullptr,ScriptFn(phase)); };
    script.setSeed(73106);
    auto random=Contract::random(script);
    const auto choice=eating ? size_t(random()%food.size()) : 0;
    const auto item=uint32_t(script.findSymbolIndex(eating ? food[choice] : "ITFO_BOOZE"));
    npc.delItem(item,uint32_t(npc.itemCount(item)));
    invoke(phases.init);
    require(npc.itemCount(item)==1 && npc.handle().aivar[19]==1 && (!eating || size_t(npc.handle().aivar[18])==choice) &&
            Contract::random(script)==random,"item routine: Init selects and creates the original item once");
    const auto created=npc.getItem(item);
    require(created && vm.global_item()->get_instance()==std::static_pointer_cast<daedalus::IItem>(created->handlePtr()),
            "item routine: item creation keeps the script ITEM global");
    npc.clearAiQueue();
    const auto limit=eating ? 6000u : 8000u;
    npc.setStateTime(limit-1);
    invoke(phases.loop);
    auto queue=Contract::queue(npc);
    auto take=[&](auto action) {
      require(queue.size()>0,"item routine: expected a queued command");
      auto next=queue.pop();
      require(next.act==action,"item routine: command type in sequence");
      return next;
      };
    auto use=[&](int state) {
      const auto a=take(AI_UseItemToState);
      require(uint32_t(a.i0)==item && a.i1==state,"item routine: command item and animation state");
      };
    auto gesture=[&](std::string_view clip) {
      const auto a=take(AI_PlayAnimBs);
      require(a.s0==clip && a.i0==BS_ITEMINTERACT,"item routine: gesture name and item-interaction pose");
      };
    take(AI_AlignToFp);
    use(0);
    if(eating && choice==0) gesture("T_FOOD_RANDOM_2");
    require(queue.size()==0 && npc.handle().aivar[19]==0 && npc.stateTime()==limit-1 &&
            Contract::random(script)==random,"item routine: enter the pose while the timer is still below threshold");
    npc.clearAiQueue();
    npc.setStateTime(limit);
    const auto draw=eating ? 0u : uint32_t(random()%10);
    invoke(phases.loop);
    queue=Contract::queue(npc);
    take(AI_AlignToFp);
    if(eating) gesture(foodGesture[choice]);
    else {
      if(draw==0) gesture("T_POTION_RANDOM_3");
      gesture("T_POTION_RANDOM_1");
      if(draw==1) gesture("T_POTION_RANDOM_2");
      const auto* local=vm.find_symbol_by_name(loopName+".RANDOM");
      require(local && uint32_t(local->get_int())==draw,"item routine: drinking keeps the saved RNG outcome");
      }
    require(queue.size()==0 && npc.stateTime()==0 && Contract::random(script)==random && loops==(events ? 0u : 2u),
            "item routine: original timed gesture with the requested executor");
    vm.remove_call_observer(observer);
    npc.clearAiQueue();
    invoke(phases.end);
    queue=Contract::queue(npc);
    if(eating) gesture("T_POTION_RANDOM_2");
    use(-1);
    require(queue.size()==0,"item routine: release the item after the last command");
    npc.clearAiQueue();
    }
  }

void pointActivitiesInBothModes(Gothic& gothic) {
  const auto* configuredEvents=std::getenv("OPENGOTHIC_ACTIVITY_EVENTS");
  const std::optional<std::string> previousEvents=configuredEvents ? std::optional<std::string>(configuredEvents) : std::nullopt;
  for(bool events:{false,true}) {
    setenv("OPENGOTHIC_ACTIVITY_EVENTS",events ? "1" : "0",1);
    gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
    const auto save=pointActivitiesWorld(gothic,events);
    itemRoutinePhasesWorld(gothic,events);
    ActivityEventsWorld paired(gothic);
    pairedSmalltalkWorld(paired,events);
    sleepRoutineWorld(gothic,events);
    gothic.clearGame().reset();
    seatedActivitiesWorld(gothic,events);
    if(events) {
      loadCampfireGroup(gothic,save);
      gothic.clearGame().reset();
      quietSoundAtPost(gothic);
      }
    }
  if(previousEvents) setenv("OPENGOTHIC_ACTIVITY_EVENTS",previousEvents->c_str(),1); else unsetenv("OPENGOTHIC_ACTIVITY_EVENTS");
  }

// Rare rows use their authored actor and waypoint where the release worlds contain them.
// The concert and the test model's three rows name points absent from the four release waynets.
struct RoutineWitnessSite {
  const char* state;
  const char* actor;
  const char* point;
  };

struct RoutineWitness {
  std::vector<std::string> entry, departure;
  Tempest::Vec3            position;
  uint32_t                 pose = 0;
  int32_t                  stage = 0;
  bool                     operator==(const RoutineWitness& other) const {
    return entry==other.entry && departure==other.departure && pose==other.pose && stage==other.stage &&
           (position-other.position).length()<1.f;
    }
  };

struct MonsterRoutineSave {
  std::vector<std::string> queue;
  uint64_t                 stateTime;
  int32_t                  stage;
  };

constexpr const char* MonsterSaveActor = "BALTHASAR_SHEEP1";
constexpr const char* MonsterScheduler = "ZS_MM_ALLSCHEDULER";

MonsterRoutineSave monsterRoutinesWorld(Gothic& gothic) {
  using Contract=NpcDefinitionContract;
  ActivityEventsWorld test(gothic);
  auto& world=test.world();
  auto& script=world.script();
  auto& session=*gothic.gameSession();
  const auto originalTime=world.time();
  const auto scheduler=ScriptFn(script.findSymbolIndex(MonsterScheduler));
  const auto roam=ScriptFn(script.findSymbolIndex("ZS_MM_RTN_ROAM"));
  const auto rest=ScriptFn(script.findSymbolIndex("ZS_MM_RTN_REST"));
  const auto sleep=ScriptFn(script.findSymbolIndex("ZS_MM_RTN_SLEEP"));
  require(scheduler.isValid(),"monster routine fixture needs the original dispatcher");
  auto compare=[&](Npc& npc,const char* what,size_t expectedPhases,const auto& prepare,const auto& act) {
    std::vector<std::string> original;
    for(bool events:{false,true}) {
      Contract::activityEventsOn(script)=events;
      prepare();
      script.setSeed(73106);
      ActivityStream stream(world,npc);
      act();
      const auto phases=std::count_if(stream.events.begin(),stream.events.end(),[](const auto& event) {
        return event.starts_with("phase ");
        });
      require(size_t(phases)==expectedPhases && stream.records==(events ? expectedPhases : 0),
              (std::string(what)+": phases must use the selected executor").c_str());
      std::erase_if(stream.events,[](const auto& event) {
        return !(event.starts_with("phase ") || event.starts_with("result ") || event.starts_with("queue ") ||
                 event.starts_with("random ") || event.starts_with("point "));
        });
      auto random=Contract::random(script);
      stream.events.push_back("next random "+std::to_string(random()));
      stream.events.push_back("state time "+std::to_string(npc.stateTime()));
      stream.events.push_back("stage "+std::to_string(npc.handle().aivar[19]));
      if(!events) original=std::move(stream.events);
      else if(stream.events!=original) {
        const auto [before,after]=std::mismatch(original.begin(),original.end(),stream.events.begin(),stream.events.end());
        throw std::runtime_error(std::string(what)+": VM "+(before==original.end() ? "<none>" : *before)+
                                 ", native "+(after==stream.events.end() ? "<none>" : *after));
        }
    }
    };
  struct Site { const char* state; const char* actor; const char* point; const char* freePoint; size_t window; };
  static constexpr Site sites[]={
    {"ZS_MM_RTN_SLEEP",     "SHADOWBEAST",       "NW_FOREST_PATH_35_06",        nullptr,                              30},
    {"ZS_MM_RTN_REST",      "GRIMBALD_SNAPPER1", "NW_TROLLAREA_BRIGDE_01",      "FP_ROAM_NW_TROLLAREA_BRIGDE_01_01",   32},
    {"ZS_MM_RTN_ROAM",      MonsterSaveActor,    "NW_FARM4_BALTHASAR",         "FP_ROAM_FARM4_SHEEP_04",             34},
    {"ZS_MM_RTN_EATGROUND", "SCAVENGER",         "NW_FOREST_PATH_38_MONSTER",  "FP_ROAM_NW_FOREST_PATH_38_MONSTER_04",36},
    {"ZS_MM_RTN_WUSEL",     "BLOODFLY",          "NW_FARM1_CITYWALL_FOREST_06","FP_ROAM_FARM1_FORREST_BLOODFLY_02",  38},
    {"ZS_MM_RTN_ORCSIT",    "ORCSHAMAN_SIT",     "NW_PASS_ORKS_02",            "FP_CAMPFIRE_SHAMAN_05",              40},
    };
  for(const auto& site:sites) {
    auto* actor=activityNpc(world,site.actor);
    const auto* point=world.findWayPoint(site.point);
    const auto* fp=site.freePoint ? world.findPoint(site.freePoint,false) : point;
    const auto state=ScriptFn(script.findSymbolIndex(site.state));
    require(actor && !actor->isDead() && point && fp && state.isValid(),
            (std::string(site.state)+": stock fixture actor, locations and state must exist").c_str());
    auto& npc=*actor;
    npc.setSimTier(NpcSimTier::Full);
    npc.setProcessPolicy(NpcProcessPolicy::AiNormal);
    const auto vars=std::to_array(npc.handle().aivar);
    auto prepare=[&] {
      test.stop(npc);
      npc.clearGoTo();
      npc.setOther(nullptr);
      npc.setVictim(nullptr);
      npc.setTarget(nullptr);
      npc.setPerceptionTime(1000);
      test.freePoint(fp);
      session.setTime(gtime(0,23,59));
      std::copy(vars.begin(),vars.end(),npc.handle().aivar);
      npc.handle().aivar[site.window]=22;
      npc.handle().aivar[site.window+1]=6;
      npc.setPosition(point->position());
      npc.attachToPoint(point);
      require(npc.startState(state,point->name),"the monster must enter its fixture state");
      };
    compare(npc,site.state,5,prepare,[&] {
      Contract::tickRoutine(npc);
      npc.clearAiQueue();
      npc.setPosition(fp->position());
      npc.attachToPoint(fp);
      // The first beat establishes the pose or wander timer. The second also covers WUSEL's one-second threshold.
      npc.setStateTime(999);
      Contract::tickRoutine(npc);
      require(npc.isInState(state),"a monster activity must continue inside its overnight window");
      npc.clearAiQueue();
      npc.setStateTime(6000);
      session.setTime(gtime(1,0,0));
      Contract::tickRoutine(npc);
      require(npc.isInState(state),"a monster activity must continue across midnight");
      npc.clearAiQueue();
      session.setTime(gtime(1,6,0));
      Contract::tickRoutine(npc);
      const auto* next=npc.aiQueueFront();
      require(!npc.isInState(state) && next && next->act==AI_StartState && next->func==scheduler && next->i0==1,
              "a monster activity must end and queue its dispatcher at the end of the window");
      });
    test.stop(npc);
    std::copy(vars.begin(),vars.end(),npc.handle().aivar);
    std::printf("Monster routine %s: Init, active Loop, hour-boundary Loop and End agree in both executors\n",site.state);
    }

  auto& sheep=*activityNpc(world,MonsterSaveActor);
  const auto vars=std::to_array(sheep.handle().aivar);
  require(sheep.handle().start_aistate==int32_t(scheduler.ptr),"the fixture sheep must use the dispatcher as its start state");
  auto prepare=[&] {
    test.stop(sheep);
    std::copy(vars.begin(),vars.end(),sheep.handle().aivar);
    for(size_t i=30;i<42;++i) sheep.handle().aivar[i]=0;
    sheep.handle().aivar[30]=22;
    sheep.handle().aivar[31]=6;
    sheep.handle().aivar[34]=-1;
    };
  compare(sheep,"monster dispatcher",5,prepare,[&] {
    auto choose=[&](gtime when,ScriptFn expected) {
      session.setTime(when);
      sheep.handle().aivar[29]=0;
      Contract::tickRoutine(sheep);
      const auto* next=sheep.aiQueueFront();
      require(sheep.isInState(scheduler) && next && next->act==AI_StartState && next->func==expected && next->i0==1 &&
              next->s0.empty() && sheep.handle().aivar[29]==script.findSymbol("PRIO_EAT")->get_int(),
              "dispatcher must queue the first active state, or rest, and restore eat priority");
      Contract::nextAiAction(sheep);
      require(sheep.isInState(expected),"the NPC lifecycle must take the dispatcher command");
      };
    choose(gtime(1,5,59),sleep);
    Contract::tickRoutine(sheep);
    sheep.clearAiQueue();
    sheep.aiPush(AiQueue::aiStartState(rest,1,nullptr,nullptr,""));
    Contract::nextAiAction(sheep);
    require(sheep.isInState(rest),"a queued state change must interrupt the active monster routine");
    sheep.clearState(true);
    sheep.clearAiQueue();
    // An NPC without a human routine returns through start_aistate on its next routine tick.
    choose(gtime(1,6,0),roam);
    sheep.clearState(true);
    sheep.handle().aivar[34]=0;
    choose(gtime(1,6,0),rest);
    sheep.clearState(true);
    });
  std::copy(vars.begin(),vars.end(),sheep.handle().aivar);
  sheep.handle().aivar[34]=-1;
  session.setTime(originalTime);
  require(sheep.startState(roam,"NW_FARM4_BALTHASAR"),"the fixture sheep must enter roam before saving");
  Contract::tickRoutine(sheep);
  sheep.setStateTime(1500);
  require(!sheep.isAiQueueEmpty(),"the fixture must save a pending monster command");
  return {activityQueue(sheep),sheep.stateTime(),sheep.handle().aivar[19]};
  }

void ravenStoneGuardianRoutines(World& world) {
  auto& script=world.script();
  auto& vm=script.getVm();
  auto* raven=script.findSymbol("RAVENISDEAD");
  const auto guardian=script.findSymbolIndex("STONEGUARDIAN");
  require(raven && guardian!=0,"the Raven fixture needs the original quest flag and stone guardian");
  const auto prior=raven->get_int();
  auto& events=NpcDefinitionContract::activityEventsOn(script);
  const auto enabled=events;
  const auto* point=world.findWayPoint("NW_FARM4_BALTHASAR");
  require(point!=nullptr,"the Raven fixture needs a position for the disposable guardian");
  size_t kills=0;
  const auto observer=vm.add_call_observer([&](const auto& function) {
    if(function.name()=="B_KILLNPC") ++kills;
    });
  raven->set_int(1);
  for(const auto* state:{MonsterScheduler,"ZS_MM_RTN_REST_LOOP"}) {
    const auto id=ScriptFn(script.findSymbolIndex(state));
    require(id.isValid(),"the Raven fixture needs both guardian routine phases");
    for(bool native:{false,true}) {
      events=native;
      auto* npc=world.addNpc(guardian,point->position());
      require(npc && npc->handle().guild==GIL_Stoneguardian && npc->attribute(ATR_HITPOINTS)>0,
              "the Raven fixture guardian must start alive");
      npc->handle().flags=NpcFlag::IMMORTAL;
      npc->handle().aivar[19]=1;
      npc->handle().aivar[32]=-1;
      const auto before=kills;
      script.invokeState(npc,nullptr,nullptr,id);
      require(kills==before+1 && npc->attribute(ATR_HITPOINTS)==0 && npc->handle().flags==NpcFlag::NONE,
              "Raven's death must invoke the original helper on the guardian and clear its HP and flags");
      world.removeNpc(*npc);
      }
    }
  vm.remove_call_observer(observer);
  raven->set_int(prior);
  events=enabled;
  std::puts("Monster dispatcher and REST Loop: both executors remove stone guardians after Raven's death");
  }

std::vector<RoutineWitness> remainingRoutineRun(Gothic& gothic,const char* worldName,
                                                std::span<const RoutineWitnessSite> sites,bool events) {
  using Contract=NpcDefinitionContract;
  gothic.setGame(std::make_unique<GameSession>(worldName));
  std::vector<RoutineWitness> result(sites.size());
  std::vector<uint8_t> save;
  std::optional<MonsterRoutineSave> monster;
  {
    ActivityEventsWorld test(gothic);
    auto& world=test.world();
    auto& script=world.script();
    auto& vm=script.getVm();
    test.only({});
    ActivityEvents::reset();
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
      auto* sheep=activityNpc(world,MonsterSaveActor);
      const auto roam=ScriptFn(script.findSymbolIndex("ZS_MM_RTN_ROAM"));
      require(sheep && sheep->isInState(roam) && Contract::aiState(*sheep).started &&
              activityQueue(*sheep)==monster->queue && sheep->stateTime()==monster->stateTime &&
              sheep->handle().aivar[19]==monster->stage,
              "ordinary load must restore the monster's state, timer, stage and pending commands");
      sheep->clearAiQueue();
      sheep->setProcessPolicy(NpcProcessPolicy::AiNormal);
      ActivityStream stream(world,*sheep);
      Contract::tickRoutine(*sheep);
      require(sheep->isInState(roam) && stream.records==1 && vmCalls["ZS_MM_RTN_ROAM_LOOP"]==0,
              "the loaded monster must resume in its native loop without rerunning Init");
      std::puts("Monster routine ZS_MM_RTN_ROAM: ordinary load restored the pending commands and resumed native Loop");
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
    {"ZS_CIRCLE",              "KDW_1400_ADDON_SATURAS_NW",        "NW_TROLLAREA_PORTALTEMPEL_RITUAL_01"},
    {"ZS_CONCERT",             "PIR_1390_ADDON_INEXTREMO_DRPYMONTE","NW_CITY_PATH_HABOUR_03"},
    {"ZS_FLEETOWP",            "BDT_1013_BANDIT_L",                "NW_XARDAS_MONSTER_INSERT_01"},
    {"ZS_FOLLOW_PLAYER",       "BAU_931_TILL",                     "NW_BIGFARM_KITCHEN_02"},
    {"ZS_GUARD_PASSAGE",       "KDF_507_TALAMON",                  "NW_MONASTERY_CELLAR_15"},
    {"ZS_GUIDE_PLAYER",        "VLK_440_BARTOK",                    "NW_FARM1_CITYWALL_FOREST_07"},
    {"ZS_PRACTICE_MAGIC",      "TA_TESTMODELL",                    "NW_CITY_PATH_HABOUR_03"},
    {"ZS_RUNTOWP",             "MIL_337_MIKA",                     "NW_FARM2_PATH_03"},
    {"ZS_SPIT_FIRE",           "VLK_468_CANTHAR",                  "NW_CITY_PATH_HABOUR_03"},
    {"ZS_STAND_DEMENTOR",      "DMT_1202_DEMENTOR",                "NW_XARDAS_VALLEY_03"},
    {"ZS_STAND_RANGERMEETING", "MIL_350_ADDON_MARTIN",             "NW_TAVERNE_07"},
    {"ZS_WASH_FP",             "MIL_313_BOLTAN",                   "NW_CITY_PATH_HABOUR_03"},
    };
  static constexpr RoutineWitnessSite jharkendar[] = {
    {"ZS_COOK_PAN",       "PIR_1352_ADDON_ALLIGATORJACK", "AD_PIRATECAMP_AJ_03"},
    {"ZS_GHOST",          "NONE_ADDON_111_QUARHODRON",    "ADW_ANCIENTGHOST"},
    {"ZS_GHOSTWUSEL",      "NONE_ADDON_112_RHADEMES",      "ADW_ADANOSTEMPEL_RHADEMES_02"},
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
