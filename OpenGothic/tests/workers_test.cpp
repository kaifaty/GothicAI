// The worker pool (common/utils/workers): the grain of OPENGOTHIC_WORKERS_GRAIN, short arrays on the caller and
// the serial mode. No engine or game data. The grain is read once at the first dispatch and the serial mode is set
// before it, so each case runs in its own process; this one forks before it touches the pool.
#include "utils/workers.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
const size_t sizes[] = {0,1,127,128,129,1053,5000};

void require(bool ok, const std::string& what) {
  if(!ok)
    throw std::runtime_error(what);
  }

// The threads one parallelFor of n elements may run on: the caller, and the pool when more than one task.
size_t threadBound(size_t n, size_t grain) {
  if(n<=128)
    return 1;
  size_t tasks = (n+grain-1)/grain-1;
  tasks = std::min<size_t>(tasks,Workers::maxThreads());
  return tasks<=1 ? 1 : tasks+1;
  }

// Every element and every task index exactly once; parallelFor stays within the threads its grain allows.
void dispatch(size_t grain, bool serial) {
  const auto caller = std::this_thread::get_id();
  for(const size_t n:sizes) {
    std::vector<uint32_t> items(n);
    std::iota(items.begin(),items.end(),0u);
    std::vector<std::atomic<uint32_t>> seen(n);
    std::vector<std::thread::id> who(n);
    Workers::parallelFor(items,[&](uint32_t& item) {
      seen[item].fetch_add(1,std::memory_order_relaxed);
      who[item] = std::this_thread::get_id();
      // Some work, so the pool's threads find elements left when they wake.
      volatile uint32_t spin = 0;
      for(uint32_t i=0; i<2000; ++i)
        spin = spin+i;
      });
    for(size_t i=0; i<n; ++i)
      require(seen[i].load()==1,"parallelFor of "+std::to_string(n)+" handled element "+std::to_string(i)+" "+
                                std::to_string(seen[i].load())+" times");
    const std::set<std::thread::id> threads(who.begin(),who.end());
    const size_t bound = serial ? 1 : threadBound(n,grain);
    require(threads.size()<=bound,"parallelFor of "+std::to_string(n)+" at grain "+std::to_string(grain)+" ran on "+
                                  std::to_string(threads.size())+" threads, more than "+std::to_string(bound));
    if(bound==1 && n>0)
      require(*threads.begin()==caller,"parallelFor of "+std::to_string(n)+" left the calling thread");

    std::vector<std::atomic<uint32_t>> tasks(n);
    Workers::parallelTasks(n,[&](size_t index) {
      tasks[index].fetch_add(1,std::memory_order_relaxed);
      });
    for(size_t i=0; i<n; ++i)
      require(tasks[i].load()==1,"parallelTasks("+std::to_string(n)+") ran index "+std::to_string(i)+" "+
                                 std::to_string(tasks[i].load())+" times");
    std::printf("  grain %zu, %zu elements: %zu threads (bound %zu)\n",grain,n,threads.size(),bound);
    }
  }

int child(const char* grain, size_t expected, bool serial) {
  try {
    if(grain!=nullptr)
      setenv("OPENGOTHIC_WORKERS_GRAIN",grain,1); else
      unsetenv("OPENGOTHIC_WORKERS_GRAIN");
    if(serial)
      Workers::setSerialExecution(true);
    dispatch(expected,serial);
    if(serial)
      require(!Workers::workerPoolCreated(),"the serial mode created the worker pool");
    else
      require(Workers::taskGrain()==expected,std::string("OPENGOTHIC_WORKERS_GRAIN=")+(grain ? grain : "(unset)")+
                                             " gives grain "+std::to_string(Workers::taskGrain())+", not "+
                                             std::to_string(expected));
    std::fflush(stdout);
    return 0;
    }
  catch(const std::exception& e) {
    std::fprintf(stderr,"workers: %s\n",e.what());
    return 1;
    }
  }
}

int main() {
  struct Case final { const char* grain; size_t expected; bool serial; };
  const Case cases[] = {
    {"128",128,false}, {"64",64,false}, {"16",16,false},
    // the default, and values outside 16..1024 fall back to it with a line in the log
    {nullptr,64,false}, {"15",64,false}, {"1025",64,false}, {"abc",64,false}, {"64x",64,false},
    {nullptr,64,true},
    };
  int failed = 0;
  for(const auto& c:cases) {
    std::printf("== %s, OPENGOTHIC_WORKERS_GRAIN=%s\n",c.serial ? "serial" : "pool",c.grain ? c.grain : "(unset)");
    std::fflush(stdout);
    const auto pid = fork();
    if(pid==0)
      _exit(child(c.grain,c.expected,c.serial));
    int status = 0;
    if(pid<0 || waitpid(pid,&status,0)!=pid || !WIFEXITED(status) || WEXITSTATUS(status)!=0) {
      std::fprintf(stderr,"workers: case failed\n");
      ++failed;
      }
    }
  if(failed>0) {
    std::fprintf(stderr,"workers: %d of %zu cases failed\n",failed,sizeof(cases)/sizeof(cases[0]));
    return 1;
    }
  std::printf("workers: %zu cases passed\n",sizeof(cases)/sizeof(cases[0]));
  return 0;
  }
