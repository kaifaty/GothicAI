// The loops after the animation pass (common/world/animationtail.h) against the full loops they replace
// (WorldObjects::updateAnimation before the plan): the same steps do work, in the same order, when a step's
// script adds, removes or changes items, also the current one. No engine or game data: the items are stand-ins
// whose steps record what they do; a step without work records nothing, as a settled NPC's step changes nothing.
#include "world/animationtail.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using AnimationTail::Contacts;
using AnimationTail::Logic;
using AnimationTail::Shadow;

struct Item final {
  int  id = 0;
  bool shadowWork = false, logicWork = false, window = false;
  bool passSkipped = false; // frozen world or OPENGOTHIC_ANIM_SKIP_FAR: the shadow loop passes it over
  bool planLies = false;    // the pass leaves out its logical-body step although it has work (a wrong plan)
  std::function<void()> script;        // what the window's damage script does in the shadow loop
  std::function<void()> contactScript; // and in the contacts loop
  int  reads = 0;               // how often a loop looked at this item
  };

using Items = std::vector<std::unique_ptr<Item>>;
using Log   = std::vector<std::string>;

void require(bool ok, const std::string& what) {
  if(!ok)
    throw std::runtime_error(what);
  }

// The steps the way the real ones behave: a window runs its script and may remove anything, this item too.
void shadowStep(Item& item, Log& log) {
  ++item.reads;
  if(item.window) {
    log.push_back("shadow "+std::to_string(item.id));
    auto script = std::move(item.script);
    item.script = {};
    if(script)
      script(); // may destroy item
    return;
    }
  if(item.shadowWork)
    log.push_back("shadow "+std::to_string(item.id));
  }

void contactStep(Item& item, Log& log) {
  ++item.reads;
  if(!item.window)
    return;
  log.push_back("contacts "+std::to_string(item.id));
  auto script = std::move(item.contactScript);
  item.contactScript = {};
  if(script)
    script(); // may destroy item
  }

void logicStep(Item& item, Log& log) {
  ++item.reads;
  if(item.logicWork)
    log.push_back("logic "+std::to_string(item.id));
  }

size_t indexOf(const Items& items, const Item* item) {
  for(size_t i=0; i<items.size(); ++i)
    if(items[i].get()==item)
      return i;
  return size_t(-1);
  }

// The loops as WorldObjects::updateAnimation ran them before the plan.
void fullLoops(Items& items, Log& log) {
  for(size_t i=0; i<items.size();) {
    auto* item = items[i].get();
    if(item->passSkipped) {
      ++i;
      continue;
      }
    shadowStep(*item,log);
    if(i<items.size() && items[i].get()==item)
      ++i;
    else if(const auto at=indexOf(items,item); at!=size_t(-1))
      i = at+1;
    }
  for(size_t i=0; i<items.size();) {
    auto* item = items[i].get();
    contactStep(*item,log);
    if(i<items.size() && items[i].get()==item)
      ++i;
    else if(const auto at=indexOf(items,item); at!=size_t(-1))
      i = at+1;
    }
  for(auto& item:items)
    logicStep(*item,log);
  }

std::vector<uint8_t> passPlan(const Items& items) {
  std::vector<uint8_t> plan;
  for(const auto& item:items)
    plan.push_back(uint8_t((item->window ? Contacts : 0) |
                           (!item->passSkipped && (item->shadowWork || item->window) ? Shadow : 0) |
                           (item->logicWork && !item->planLies ? Logic : 0)));
  return plan;
  }

// The loops of WorldObjects::updateAnimationTail over the stand-ins.
void plannedLoops(Items& items, Log& log, Log& checked) {
  AnimationTail::begin();
  const auto plan = passPlan(items);
  // OPENGOTHIC_ANIM_TAIL_CHECK: a skipped step runs the old way and must change nothing.
  const auto skipped = [&](const char* step) {
    return [&,step](Item& item) {
      const std::string kind(step);
      if(kind=="shadow" && item.passSkipped)
        return; // the full loop passes it over as well
      const auto id = std::to_string(item.id);
      const auto before = log.size();
      checked.push_back(kind+" "+id);
      if(kind=="logic") logicStep(item,log); else
      if(kind=="contacts") contactStep(item,log); else shadowStep(item,log);
      AnimationTail::checked(step,id,log.size()==before);
      };
    };
  AnimationTail::loop(items,plan,Shadow,[&](Item& item,bool planned) {
    if(!planned && item.passSkipped)
      return;
    const bool window = item.window;
    shadowStep(item,log);
    if(window)
      AnimationTail::touch();
    },skipped("shadow"));
  AnimationTail::loop(items,plan,Contacts,[&](Item& item,bool) {
    const bool window = item.window;
    contactStep(item,log);
    if(window)
      AnimationTail::touch();
    },skipped("contacts"));
  AnimationTail::loop(items,plan,Logic,[&](Item& item,bool) {
    logicStep(item,log);
    },skipped("logic"));
  AnimationTail::end();
  }

// Ten items; `setup` gives them work, windows and scripts. Builds the same world twice and runs both loops.
void compare(const char* name, const std::function<void(Items&)>& setup, bool expectStale) {
  auto build = [&] {
    Items items;
    for(int i=0; i<10; ++i) {
      items.push_back(std::make_unique<Item>());
      items.back()->id = i;
      }
    setup(items);
    return items;
    };
  Items full = build(), tail = build();
  Log fullLog, tailLog, checked;
  fullLoops(full,fullLog);
  plannedLoops(tail,tailLog,checked);
  std::string a, b;
  for(auto& s:fullLog) a += s+"; ";
  for(auto& s:tailLog) b += s+"; ";
  require(a==b,std::string(name)+": the planned loops did\n  "+b+"\nthe full loops did\n  "+a);
  require(AnimationTail::stale()==expectStale,std::string(name)+": the plan "+(expectStale ? "held" : "went stale"));
  if(AnimationTail::checking()) {
    require(!checked.empty() || expectStale,std::string(name)+": the check ran no skipped step");
    } else if(!expectStale) {
    // Nothing touched: a loop read only the items its plan names.
    const auto plan = passPlan(build());
    for(size_t i=0; i<tail.size(); ++i) {
      const int planned = int((plan[i]&Shadow)!=0)+int((plan[i]&Contacts)!=0)+int((plan[i]&Logic)!=0);
      require(tail[i]->reads==planned,std::string(name)+": item "+std::to_string(tail[i]->id)+" read "+
                                      std::to_string(tail[i]->reads)+" times, its plan names "+std::to_string(planned));
      }
    }
  std::printf("  %s: %zu steps with work%s\n",name,tailLog.size(),checked.empty() ? "" : ", skipped steps checked");
  }

Item* find(Items& items, int id) {
  for(auto& item:items)
    if(item->id==id)
      return item.get();
  return nullptr;
  }

void removeId(Items& items, int id) {
  for(size_t i=0; i<items.size(); ++i)
    if(items[i]->id==id) {
      items.erase(items.begin()+long(i));
      return;
      }
  }

int cases() {
  compare("settled world",[](Items& items) {
    items[2]->shadowWork = true;
    items[6]->logicWork = true;
    },false);
  // A frozen world or OPENGOTHIC_ANIM_SKIP_FAR: the shadow loop passes skipped NPCs over, the hero and the
  // arena actors not; contacts and logical bodies go on for all of them.
  compare("pass-skipped items",[](Items& items) {
    for(auto& item:items)
      item->passSkipped = item->id!=0 && item->id!=4;
    items[0]->shadowWork = items[4]->shadowWork = true;
    items[3]->shadowWork = true; // has work, but its shadow step is skipped
    items[3]->logicWork = items[7]->logicWork = true;
    },false);
  // A damage script in the shadow loop: removes an item before and one after the window owner, adds one, and
  // changes one the pass planned as settled for its logical body.
  compare("script in the shadow loop",[](Items& items) {
    items[1]->shadowWork = true;
    items[3]->window = true;
    items[3]->script = [&items] {
      removeId(items,1);
      removeId(items,5);
      items.push_back(std::make_unique<Item>());
      items.back()->id = 100;
      items.back()->shadowWork = items.back()->logicWork = true;
      find(items,7)->logicWork = true;
      find(items,8)->shadowWork = true;
      };
    },true);
  // The script removes the window owner itself.
  compare("script removes its owner",[](Items& items) {
    items[4]->window = true;
    items[4]->script = [&items] {
      find(items,6)->logicWork = true;
      removeId(items,4);
      };
    items[9]->logicWork = true;
    },true);
  // A damage script in the contacts loop: the same changes after the shadow loop has run in plan.
  compare("script in the contacts loop",[](Items& items) {
    items[0]->shadowWork = true;
    items[2]->window = true;
    items[2]->contactScript = [&items] {
      removeId(items,0);
      removeId(items,6);
      items.push_back(std::make_unique<Item>());
      items.back()->id = 101;
      items.back()->window = items.back()->logicWork = true;
      find(items,8)->logicWork = true;
      };
    items[9]->logicWork = true;
    },true);
  // A script changes an NPC planned as settled for its logical body, the array as it was: only the plan's going
  // stale makes the logical-body loop look at it.
  compare("shadow script changes a settled NPC",[](Items& items) {
    items[3]->window = true;
    items[3]->script = [&items] { find(items,7)->logicWork = true; };
    items[9]->logicWork = true;
    },true);
  // The same from the contacts loop; the owner is frozen, so the shadow loop does not touch the plan first.
  compare("contacts script changes a settled NPC",[](Items& items) {
    items[2]->passSkipped = true;
    items[2]->window = true;
    items[2]->contactScript = [&items] { find(items,7)->logicWork = true; };
    },true);
  // A frozen window owner: the shadow loop skips it, its contacts still run.
  compare("pass-skipped window owner",[](Items& items) {
    items[5]->passSkipped = true;
    items[5]->window = true;
    items[2]->logicWork = true;
    },true);
  return 0;
  }

// A wrong plan: the pass leaves out the logical-body step of NPC 6, which has work. Only the check runs it
// (the planned loops alone would skip it); the check must report NPC 6 and the step.
void wrongPlan() {
  compare("wrong plan",[](Items& items) {
    items[6]->logicWork = true;
    items[6]->planLies = true;
    },false);
  require(AnimationTail::mismatchedCount()==1 && AnimationTail::firstMismatchNpc()=="6" &&
          AnimationTail::firstMismatchStep()=="logic",
          "the check did not report the wrong plan: mismatched="+std::to_string(AnimationTail::mismatchedCount())+
          " first="+std::string(AnimationTail::firstMismatchNpc())+" step="+std::string(AnimationTail::firstMismatchStep()));
  }

int child(bool check) {
  try {
    if(check)
      setenv("OPENGOTHIC_ANIM_TAIL_CHECK","1",1); else
      unsetenv("OPENGOTHIC_ANIM_TAIL_CHECK");
    require(AnimationTail::checking()==check,"OPENGOTHIC_ANIM_TAIL_CHECK is not read");
    cases();
    if(check) {
      require(AnimationTail::checkedCount()>0 && AnimationTail::mismatchedCount()==0,"the check counted nothing");
      wrongPlan();
      }
    // What the body runner does before its _exit; a second call and the report at exit print nothing more.
    AnimationTail::reportCheck();
    AnimationTail::reportCheck();
    std::fflush(stdout);
    return 0;
    }
  catch(const std::exception& e) {
    std::fprintf(stderr,"anim tail: %s\n",e.what());
    return 1;
    }
  }
}

int main() {
  // Each run's stderr goes through a pipe: the summary line must come exactly once with the check, never without,
  // both when the process leaves with _exit (the forked body runner) and when it returns through exit().
  struct Run final { bool check, quick; };
  const Run runs[] = {{false,true},{true,true},{true,false}};
  int failed = 0;
  for(const auto& run:runs) {
    std::printf("== %s, %s\n",run.check ? "OPENGOTHIC_ANIM_TAIL_CHECK=1" : "plan",run.quick ? "_exit" : "exit");
    std::fflush(stdout);
    int fds[2];
    if(pipe(fds)!=0)
      return 1;
    const auto pid = fork();
    if(pid==0) {
      close(fds[0]);
      dup2(fds[1],2);
      close(fds[1]);
      const int code = child(run.check);
      if(run.quick)
        _exit(code);
      std::exit(code);
      }
    close(fds[1]);
    std::string err;
    char buf[4096];
    for(ssize_t n; (n=read(fds[0],buf,sizeof(buf)))>0;)
      err.append(buf,size_t(n));
    close(fds[0]);
    std::fputs(err.c_str(),stderr);
    int status = 0;
    const bool exited = pid>0 && waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0;
    size_t lines = 0;
    for(size_t at=0; (at=err.find("[anim-tail-check] checked=",at))!=std::string::npos; ++at)
      ++lines;
    const bool summary = run.check ? lines==1 && err.find("mismatched=1 first=6 step=logic")!=std::string::npos
                                   : lines==0;
    if(!summary)
      std::fprintf(stderr,"anim tail: %zu summary lines, expected %d\n",lines,run.check ? 1 : 0);
    if(!exited || !summary)
      ++failed;
    }
  if(failed>0) {
    std::fprintf(stderr,"anim tail: %d of %zu runs failed\n",failed,sizeof(runs)/sizeof(runs[0]));
    return 1;
    }
  std::printf("anim tail: passed\n");
  return 0;
  }
