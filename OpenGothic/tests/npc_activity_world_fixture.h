// Live world fixture and guard-post cases for placed_items_contract.cpp.

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

    void step(size_t ticks=1) {
      for(size_t i=0;i<ticks;++i) {
        gothic.gameSession()->tick(33);
        gothic.gameSession()->updateAnimation(33);
        }
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
  auto freePost = [&](const WayPoint* p) {
    for(uint32_t i=0;i<world.npcCount();++i)
      if(world.npcById(i)->currentWayPoint()==p)
        world.npcById(i)->attachToPoint(nullptr);
    };
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
  freePost(post);
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

  auto reload = [&](Npc& npc) {
    const auto bytes = savedNpc(npc);
    Tempest::MemReader reader(bytes);
    Serialize in(reader);
    in.setContext(&world);
    npc.load(in,0,"/npc/");
    npc.postValidate();
    npc.setSimTier(NpcSimTier::Full);
    };
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
  freePost(near);
  freePost(other);
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

  EventActor folded=guard;
  folded.state="ZS_STAND_ARMSCROSSED";
  folded.phases=activityPhases(script,folded.state.c_str());
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
    scriptSelect(second,folded);
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
    scriptSelect(first,events ? folded : guard);
    scriptSelect(second,folded);
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
  freePost(beside);
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
