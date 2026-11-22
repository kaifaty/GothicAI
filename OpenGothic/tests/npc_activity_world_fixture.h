// Shared by the mob and free-point cases in placed_items_contract.cpp.

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
