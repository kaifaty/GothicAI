// Fixed worker pool of the nav-sim stand library: the eye and the motion share it.
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace navsim {

// Fixed workers that split [0, items) in chunks; the caller blocks until done.
class Pool final {
  public:
    Pool(int32_t threads, const int32_t* cpus) {
      for(int32_t i=0; i<threads; ++i)
        workers.emplace_back([this,cpu=cpus!=nullptr ? cpus[i] : -1] { loop(cpu); });
      }

    ~Pool() {
      {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
      }
      wake.notify_all();
      for(auto& w:workers)
        w.join();
      }

    size_t size() const { return workers.size(); }

    void run(size_t items, const std::function<void(size_t,size_t)>& work) {
      if(items==0)
        return;
      std::unique_lock<std::mutex> lock(mutex);
      job = &work;
      total = items;
      chunk = std::max<size_t>(1,items/(workers.size()*8));
      next.store(0);
      pending = workers.size();
      ++generation;
      wake.notify_all();
      finished.wait(lock,[this] { return pending==0; });
      job = nullptr;
      }

  private:
    void loop(int cpu) {
#if defined(__linux__)
      if(cpu>=0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu,&set);
        pthread_setaffinity_np(pthread_self(),sizeof(set),&set);
        }
#else
      (void)cpu;
#endif
      uint64_t seen = 0;
      for(;;) {
        const std::function<void(size_t,size_t)>* work = nullptr;
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait(lock,[&] { return stop || generation!=seen; });
          if(stop)
            return;
          seen = generation;
          work = job;
        }
        for(;;) {
          const size_t begin = next.fetch_add(chunk);
          if(begin>=total)
            break;
          (*work)(begin,std::min(total,begin+chunk));
          }
        std::lock_guard<std::mutex> lock(mutex);
        if(--pending==0)
          finished.notify_one();
        }
      }

    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable wake, finished;
    const std::function<void(size_t,size_t)>* job = nullptr;
    std::atomic<size_t> next{0};
    size_t total = 0, chunk = 1, pending = 0;
    uint64_t generation = 0;
    bool stop = false;
  };

}
