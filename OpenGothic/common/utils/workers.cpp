#include "workers.h"
#include "utils/string_frm.h"

#include <Tempest/Platform>
#include <Tempest/Log>
#include <stdexcept>

namespace {
std::atomic_bool serialRequested{false};
std::atomic_bool executionStarted{false};
std::atomic_bool poolCreated{false};
}

#if defined(__WINDOWS__)
#include <windows.h>
#include <processthreadsapi.h>
#endif

#if defined(__GNUC__)
#include <pthread.h>
#endif

#if !defined(__WINDOWS__)
#include <ctime>
#endif

#if defined(_MSC_VER)
void Workers::setThreadName(const char* threadName) {
  const DWORD MS_VC_EXCEPTION = 0x406D1388;
  DWORD dwThreadID = GetCurrentThreadId();
#pragma pack(push,8)
  struct THREADNAME_INFO {
    DWORD  dwType = 0x1000; // Must be 0x1000.
    LPCSTR szName;          // Pointer to name (in user addr space).
    DWORD  dwThreadID;      // Thread ID (-1=caller thread).
    DWORD  dwFlags;         // Reserved for future use, must be zero.
    };
#pragma pack(pop)

  THREADNAME_INFO info = {};
  info.szName     = threadName;
  info.dwThreadID = dwThreadID;
  info.dwFlags    = 0;

  __try {
    RaiseException(MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info );
    }
  __except(EXCEPTION_EXECUTE_HANDLER) {
    }
  }
#elif defined(__WINDOWS__)
void Workers::setThreadName(const char* threadName) {
#if defined(__GNUC__)
  pthread_setname_np(pthread_self(), threadName);
#endif
  auto k32 = GetModuleHandleA("Kernel32");
  auto fn  = GetProcAddress(k32, "SetThreadDescription");
  if(fn==nullptr)
    return;

  // nsight does not care about pthread_setname_np
  WCHAR wname[64] = {};
  for(size_t i=0; i<63 && threadName[i]; ++i)
    wname[i] = WCHAR(threadName[i]);
  auto SetThreadDescription = reinterpret_cast<HRESULT(WINAPI*)(HANDLE,PCWSTR)>(fn);
  SetThreadDescription(GetCurrentThread(), wname);
  }
#elif defined(__GNUC__) && !defined(__clang__)
void Workers::setThreadName(const char* threadName){
  pthread_setname_np(pthread_self(), threadName);
  }
#elif defined(__OSX__)
void Workers::setThreadName(const char* threadName){
  pthread_setname_np(threadName);
  }
#else
void Workers::setThreadName(const char* threadName) { (void)threadName; }
#endif

using namespace Tempest;

const size_t Workers::taskPerThread = 128;
const size_t Workers::taskPerStep   = 16;

Workers::Workers() {
  size_t id=0;
  for(auto& i:th) {
    i = std::thread([this,id]() noexcept {
      threadFunc(id);
      });
    ++id;
    }
  poolCreated.store(true,std::memory_order_relaxed);
  }

Workers::~Workers() {
  running  = false;
  workSet  = nullptr;
  workSize = MAX_THREADS;
  execWork(minWorkSize<void,void>());
  for(auto& i:th)
    i.join();
  }

Workers &Workers::inst() {
  static Workers w;
  return w;
  }

void Workers::setSerialExecution(bool enabled) {
  if(executionStarted.load(std::memory_order_relaxed))
    throw std::logic_error("worker execution must be configured before the first dispatch");
  serialRequested.store(enabled,std::memory_order_relaxed);
  }

bool Workers::serialExecution() {
  return serialRequested.load(std::memory_order_relaxed);
  }

bool Workers::workerPoolCreated() {
  return poolCreated.load(std::memory_order_relaxed);
  }

bool Workers::useSerialExecution() {
  executionStarted.store(true,std::memory_order_relaxed);
  return serialExecution();
  }

uint8_t Workers::maxThreads() {
  int32_t th = int32_t(std::thread::hardware_concurrency());
  if(th<=0)
    th = 1;
  if(th>MAX_THREADS)
    return MAX_THREADS;
  return uint8_t(th);
  }

double Workers::threadCpuUs() {
#if defined(__WINDOWS__)
  // kernel + user time in 100 ns units; Windows counts it per scheduler tick, so short spans read coarse
  FILETIME creation = {}, exit = {}, kernel = {}, user = {};
  if(!GetThreadTimes(GetCurrentThread(),&creation,&exit,&kernel,&user))
    return 0;
  const auto ticks = [](const FILETIME& t) { return (uint64_t(t.dwHighDateTime)<<32) | uint64_t(t.dwLowDateTime); };
  return double(ticks(kernel)+ticks(user))*0.1;
#else
  timespec ts = {};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts);
  return double(ts.tv_sec)*1e6 + double(ts.tv_nsec)*1e-3;
#endif
  }

void Workers::threadFunc(size_t id) {
  {
  string_frm tname("Workers [",int(id),"]");
  setThreadName(tname.c_str());
  }

  auto complete = [this] {
    std::lock_guard<std::mutex> lck(sync);
    if(taskDone.fetch_add(1)+1==int(taskCount))
      workDone.notify_one();
    };

  while(true) {
    {
    std::unique_lock<std::mutex> lck(sync);
    workWait.wait(lck, [this]() { return workTbd>0; });
    --workTbd;
    }

    if(!running) {
      complete();
      return;
      }

    if(workSet==nullptr) {
      auto idx = progressIt.fetch_add(1);
      workFunc(workSet+idx, 1);
      } else {
      taskLoop();
      }

    complete();
    }
  }

uint32_t Workers::taskLoop() {
  uint32_t count = 0;
  while(true) {
    size_t b = size_t(progressIt.fetch_add(taskPerStep));
    size_t e = std::min(b+taskPerStep, workSize);
    if(e<=b)
      break;

    void* d = workSet + b*workEltSize;
    workFunc(d,e-b);
    count += uint32_t(e-b);
    }
  return count;
  }

void Workers::execWork(uint32_t& minElts) {
  if(workSize==0)
    return;

  if(workSet!=nullptr) {
    const auto maxTheads = maxThreads();
    taskCount = uint32_t((workSize+taskPerThread-1)/taskPerThread);
    taskCount--; // main thread also do tasks
    if(taskCount>maxTheads)
      taskCount = maxTheads;
    if(taskCount<=0)
      taskCount = 1;
    } else {
    taskCount = uint32_t(workSize);
    }

  if(running && taskCount==1) {
    workFunc(workSet, workSize);
    return;
    }

  minElts = std::max<uint32_t>(minElts, taskPerThread);

  if(workSet!=nullptr && workSize<=minElts && true) {
    workFunc(workSet, workSize);
    if(minElts > workSize*2)
      minElts = 0;
    return;
    }

  progressIt.store(0);
  taskDone.store(0);

  {
  std::unique_lock<std::mutex> lck(sync);
  workTbd = int32_t(taskCount);
  }
  workWait.notify_all();

  uint32_t cnt = 0;
  if(workSet==nullptr) {
    std::this_thread::yield();
    } else {
    cnt = taskLoop(); (void)cnt;
    }

  // Sleep until all dispatched callbacks finish instead of competing with them
  // in a yield loop. The completion predicate and notification share sync.
  std::unique_lock<std::mutex> lck(sync);
  workDone.wait(lck,[this] { return taskDone.load()==int(taskCount); });
  taskDone.store(0);
  }
