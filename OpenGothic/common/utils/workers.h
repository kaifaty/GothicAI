#pragma once

#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <new>

class Workers final {
  public:
    Workers();
    ~Workers();

    static void setThreadName(const char* threadName);
    // Configure before the first dispatch. Serial execution keeps every task
    // on the caller and does not instantiate the worker pool.
    static void setSerialExecution(bool enabled);
    static bool serialExecution();
    static bool workerPoolCreated();

    template<class T,class F>
    static void parallelFor(T* b, T* e, const F& func) {
      if(useSerialExecution()) {
        for(auto it=b;it!=e;++it) func(*it);
        return;
        }
      inst().runParallelFor(b,std::distance(b,e),func);
      }

    template<class T,class F>
    static void parallelFor(std::vector<T>& data, const F& func) {
      if(useSerialExecution()) {
        for(auto& item:data) func(item);
        return;
        }
      inst().runParallelFor(data.data(),data.size(),func);
      }

    template<class T,class F>
    static void parallelTasks(std::vector<T>& data, const F& func) {
      parallelFor(data,func);
      }

    template<class F>
    static void parallelTasks(size_t taskCount, const F& func) {
      if(useSerialExecution()) {
        for(size_t index=0;index<taskCount;++index) func(index);
        return;
        }
      inst().runParallelTasks<F>(taskCount,func);
      }

    static uint8_t maxThreads();
    // the calling thread's CPU time, µs (POSIX CLOCK_THREAD_CPUTIME_ID, Windows GetThreadTimes)
    static double  threadCpuUs();

  private:
    enum { MAX_THREADS=16 };

    void            threadFunc(size_t id);
    void            execWork(uint32_t& minWorkSize);
    uint32_t        taskLoop();
    static Workers& inst();
    static bool     useSerialExecution();

    template<class T,class F>
    static uint32_t&  minWorkSize() {
      static uint32_t data = 0;
      return data;
      }


    template<class T,class F>
    void runParallelFor(T* data, size_t sz, const F& func) {
      workSet     = reinterpret_cast<uint8_t*>(data);
      workSize    = sz;
      workEltSize = sizeof(T);

      workFunc = [&func](void* data, size_t sz) {
        T* tdata = reinterpret_cast<T*>(data);
        for(size_t i=0;i<sz;++i)
          func(tdata[i]);
        };

      execWork(minWorkSize<T,F>());
      }

    template<class F>
    void runParallelTasks(size_t taskCount, const F& func) {
      workSet     = nullptr;
      workSize    = taskCount;
      workEltSize = 1;

      workFunc = [&func](void* data, size_t sz) {
        func(reinterpret_cast<uintptr_t>(data));
        };
      execWork(minWorkSize<void,F>());
      }

    static const size_t               taskPerThread;
    static const size_t               taskPerStep;
    bool                              running=true;

    std::thread                       th[MAX_THREADS];

    std::mutex                        sync;
    std::condition_variable           workWait;
    std::condition_variable           workDone;
    int32_t                           workTbd = 0;

    uint8_t*                          workSet=nullptr;
    size_t                            workSize=0, workEltSize=0;
    std::function<void(void*,size_t)> workFunc;

    std::atomic_int                   progressIt{0};
    uint32_t                          taskCount = 0;
    std::atomic_int                   taskDone{0};
  };
