// Phase 0 microbenchmark for the GothicAI Modkit Luau runtime (reports/engine/2026-11-28-modkit-architecture/README.md).
// Measures:
//   1. Per-mod lua_State memory (lua_Alloc live/peak bytes and process RSS delta) + hard OOM isolation.
//   2. C++ <-> Luau boundary call cost (simple query, structured trade.quote, transactional TxPlan).
//   3. lua_Callbacks::interrupt overhead at different check strides + infinite-loop cutoff accuracy.
//   4. Runtime-only hot reload latency (1 mod and 10 mods of ~500 lines Luau).
//   5. Realistic Gothic II swordDamage + meleeMultiplier calculation in the Luau interpreter.

#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double elapsedNs(Clock::time_point a, Clock::time_point b) {
  return double(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

double elapsedUs(Clock::time_point a, Clock::time_point b) {
  return elapsedNs(a, b) / 1000.0;
}

double elapsedMs(Clock::time_point a, Clock::time_point b) {
  return elapsedNs(a, b) / 1e6;
}

size_t currentRssBytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
    return size_t(info.resident_size);
  return 0;
#elif defined(__linux__)
  FILE* f = std::fopen("/proc/self/statm", "r");
  if(!f)
    return 0;
  unsigned long sizePages = 0;
  unsigned long residentPages = 0;
  const int matched = std::fscanf(f, "%lu %lu", &sizePages, &residentPages);
  std::fclose(f);
  if(matched != 2)
    return 0;
  const long pageSize = sysconf(_SC_PAGESIZE);
  return pageSize > 0 ? size_t(residentPages) * size_t(pageSize) : 0;
#else
  return 0;
#endif
}

struct AllocTracker {
  size_t limitBytes = 16u * 1024u * 1024u;
  size_t currentBytes = 0;
  size_t peakBytes = 0;
  size_t oomRejects = 0;
};

void* trackedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  auto* tracker = static_cast<AllocTracker*>(ud);
  if(nsize == 0) {
    if(ptr != nullptr) {
      tracker->currentBytes -= osize;
      std::free(ptr);
    }
    return nullptr;
  }
  const size_t prev = ptr != nullptr ? osize : 0;
  if(nsize > prev) {
    const size_t grow = nsize - prev;
    if(grow > tracker->limitBytes || tracker->currentBytes + grow > tracker->limitBytes) {
      tracker->oomRejects++;
      return nullptr;
    }
  }
  void* res = std::realloc(ptr, nsize);
  if(res == nullptr)
    return nullptr;
  tracker->currentBytes = tracker->currentBytes - prev + nsize;
  if(tracker->currentBytes > tracker->peakBytes)
    tracker->peakBytes = tracker->currentBytes;
  return res;
}

struct InterruptGuard {
  uint32_t stride = 64;
  uint32_t counter = 0;
  bool active = false;
  bool triggered = false;
  Clock::time_point deadline{};
};

void interruptHandler(lua_State* L, int gc) {
  if(gc >= 0)
    return;
  auto* guard = static_cast<InterruptGuard*>(lua_getthreaddata(L));
  if(guard == nullptr || !guard->active)
    return;
  if(++guard->counter < guard->stride)
    return;
  guard->counter = 0;
  if(Clock::now() >= guard->deadline) {
    guard->triggered = true;
    luaL_error(L, "mod CPU budget exceeded");
  }
}

void watchdogAbortHandler(lua_State* L, int gc) {
  if(gc >= 0)
    return;
  auto* guard = static_cast<InterruptGuard*>(lua_getthreaddata(L));
  if(guard != nullptr)
    guard->triggered = true;
  luaL_error(L, "mod CPU budget exceeded (watchdog)");
}

// Single shared watchdog thread: cb.interrupt stays nullptr on the hot path (LUAU_UNLIKELY branch not taken).
// Only when a call outlives its deadline does the watchdog flip cb.interrupt to watchdogAbortHandler.
class CpuWatchdog {
  public:
    CpuWatchdog() : worker(&CpuWatchdog::run, this) {}
    ~CpuWatchdog() {
      {
        std::lock_guard<std::mutex> lock(mu);
        stop = true;
      }
      cv.notify_one();
      worker.join();
    }

    void arm(lua_State* L, Clock::time_point dl) {
      std::lock_guard<std::mutex> lock(mu);
      lua_callbacks(L)->interrupt = nullptr;
      target = L;
      deadline = dl;
      armed = true;
      cv.notify_one();
    }

    void disarm(lua_State* L) {
      std::lock_guard<std::mutex> lock(mu);
      armed = false;
      target = nullptr;
      lua_callbacks(L)->interrupt = nullptr;
    }

  private:
    void run() {
      std::unique_lock<std::mutex> lock(mu);
      while(!stop) {
        if(!armed) {
          cv.wait(lock, [&] { return stop || armed; });
          continue;
        }
        const auto dl = deadline;
        const bool wokenEarly = cv.wait_until(lock, dl, [&] { return stop || !armed || deadline != dl; });
        if(stop || !armed || wokenEarly)
          continue;
        if(target != nullptr && Clock::now() >= deadline) {
          lua_callbacks(target)->interrupt = watchdogAbortHandler;
          armed = false;
        }
      }
    }

    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    bool armed = false;
    lua_State* target = nullptr;
    Clock::time_point deadline{};
    std::thread worker;
};

struct TxOp {
  enum Kind : uint8_t { Once, ConsumeItem, GiveItem, AddXp };
  Kind kind = Once;
  uint64_t actor = 0;
  int32_t itemOrAmount = 0;
  int32_t count = 0;
  std::string key;
};

struct TxPlan {
  std::vector<TxOp> ops;
};

struct HostCtx {
  int32_t actorStrength = 45;
  TxPlan plan;
  uint64_t committedOps = 0;
};

int hostGetAttr(lua_State* L) {
  auto* host = static_cast<HostCtx*>(lua_touserdata(L, lua_upvalueindex(1)));
  const int actorId = luaL_checkinteger(L, 1);
  const int attrId = luaL_checkinteger(L, 2);
  lua_pushinteger(L, host->actorStrength + (actorId & 3) + (attrId & 3));
  return 1;
}

int txOnce(lua_State* L) {
  auto* plan = static_cast<TxPlan*>(lua_touserdata(L, lua_upvalueindex(1)));
  size_t len = 0;
  const char* key = luaL_checklstring(L, 1, &len);
  plan->ops.push_back(TxOp{TxOp::Once, 0, 0, 0, std::string(key, len)});
  lua_pushboolean(L, 1);
  return 1;
}

int txConsumeItem(lua_State* L) {
  auto* plan = static_cast<TxPlan*>(lua_touserdata(L, lua_upvalueindex(1)));
  const uint64_t actor = uint64_t(luaL_checknumber(L, 1));
  const int32_t item = luaL_checkinteger(L, 2);
  const int32_t count = luaL_checkinteger(L, 3);
  plan->ops.push_back(TxOp{TxOp::ConsumeItem, actor, item, count, {}});
  return 0;
}

int txGiveItem(lua_State* L) {
  auto* plan = static_cast<TxPlan*>(lua_touserdata(L, lua_upvalueindex(1)));
  const uint64_t actor = uint64_t(luaL_checknumber(L, 1));
  const int32_t item = luaL_checkinteger(L, 2);
  const int32_t count = luaL_checkinteger(L, 3);
  plan->ops.push_back(TxOp{TxOp::GiveItem, actor, item, count, {}});
  return 0;
}

int txAddXp(lua_State* L) {
  auto* plan = static_cast<TxPlan*>(lua_touserdata(L, lua_upvalueindex(1)));
  const uint64_t actor = uint64_t(luaL_checknumber(L, 1));
  const int32_t amount = luaL_checkinteger(L, 2);
  plan->ops.push_back(TxOp{TxOp::AddXp, actor, amount, 0, {}});
  return 0;
}

struct ModVm {
  AllocTracker alloc;
  InterruptGuard guard;
  HostCtx host;
  lua_State* L = nullptr;

  explicit ModVm(size_t memLimitBytes = 16u * 1024u * 1024u) {
    alloc.limitBytes = memLimitBytes;
    L = lua_newstate(trackedAlloc, &alloc);
    if(L == nullptr) {
      std::fprintf(stderr, "fatal: lua_newstate failed\n");
      std::exit(2);
    }
    luaL_openlibs(L);
    // Strip non-deterministic / unsafe builtins (keep math, string, table, bit32, utf8, buffer, vector).
    lua_pushnil(L);
    lua_setglobal(L, "os");
    lua_pushnil(L);
    lua_setglobal(L, "debug");
    lua_pushnil(L);
    lua_setglobal(L, "collectgarbage");

    // Register minimal mock SDK table.
    lua_newtable(L);
    lua_pushlightuserdata(L, &host);
    lua_pushcclosure(L, hostGetAttr, "get_attr", 1);
    lua_setfield(L, -2, "get_attr");

    lua_newtable(L);
    lua_pushlightuserdata(L, &host.plan);
    lua_pushcclosure(L, txOnce, "once", 1);
    lua_setfield(L, -2, "once");
    lua_pushlightuserdata(L, &host.plan);
    lua_pushcclosure(L, txConsumeItem, "consume_item", 1);
    lua_setfield(L, -2, "consume_item");
    lua_pushlightuserdata(L, &host.plan);
    lua_pushcclosure(L, txGiveItem, "give_item", 1);
    lua_setfield(L, -2, "give_item");
    lua_pushlightuserdata(L, &host.plan);
    lua_pushcclosure(L, txAddXp, "add_xp", 1);
    lua_setfield(L, -2, "add_xp");
    lua_setfield(L, -2, "tx");

    lua_setglobal(L, "sdk");

    luaL_sandbox(L);
    luaL_sandboxthread(L);
    lua_setthreaddata(L, &guard);
  }

  ~ModVm() {
    if(L != nullptr)
      lua_close(L);
  }

  ModVm(const ModVm&) = delete;
  ModVm& operator=(const ModVm&) = delete;

  void loadAndRun(const std::string& source, const char* chunkName, int optLevel = 2) {
    Luau::CompileOptions opts{};
    opts.optimizationLevel = optLevel;
    opts.debugLevel = 1;
    const std::string bytecode = Luau::compile(source, opts);
    if(luau_load(L, chunkName, bytecode.data(), bytecode.size(), 0) != 0) {
      std::fprintf(stderr, "load error: %s\n", lua_tostring(L, -1));
      std::exit(2);
    }
    if(lua_pcall(L, 0, 0, 0) != LUA_OK) {
      std::fprintf(stderr, "exec error: %s\n", lua_tostring(L, -1));
      std::exit(2);
    }
  }
};

std::string sampleModSource(int extraFunctions) {
  std::string src = R"LUA(
--!strict
state = {
  forged_count = 0,
  masterwork_unlocked = false,
  bonus_mult = 1.15,
}

function on_load(saved)
  if saved then
    state.forged_count = saved.forged_count or 0
    state.masterwork_unlocked = saved.masterwork_unlocked or false
  end
end

function on_ping(actor_id)
  return sdk.get_attr(actor_id, 1)
end

function on_trade_quote(q)
  if q.archetype ~= "ember_smithing:ember_blade" then
    return nil
  end
  local mult = (q.quality or 1.0)
  if (q.temperature or 0) > 400 then
    mult = mult * state.bonus_mult
  end
  if (q.durability or 100) < 30 then
    mult = mult * 0.7
  end
  return { price_multiplier = mult, allow = true }
end

function on_craft_completed(ev)
  state.forged_count += 1
  if sdk.tx.once("ember_smithing:first_forge") then
    sdk.tx.consume_item(ev.actor, 1001, 2)
    sdk.tx.give_item(ev.actor, 2001, 1)
    sdk.tx.add_xp(ev.actor, 50)
  end
  return state.forged_count
end

function tight_loop(n)
  local acc = 0
  for i = 1, n do
    acc += (i * 3) % 17
  end
  return acc
end

function infinite_loop()
  local x = 0
  while true do
    x += 1
  end
end

function memory_bomb()
  local t = {}
  while true do
    t[#t + 1] = string.rep("A", 1024)
  end
end

function calc_sword_damage(str, dtype, crit_roll, hitchance, is_monster, dmg_arr, prot_arr, speed, typical, blade, angle, target_idx, quality, temp)
  local crit_ok = is_monster or (hitchance > crit_roll)
  local total = 0
  local invincible = true
  for i = 1, 8 do
    if bit32.band(dtype, bit32.lshift(1, i - 1)) ~= 0 then
      local vd = str + dmg_arr[i] - prot_arr[i]
      if vd < 0 then vd = 0 end
      if not crit_ok then
        vd = math.floor((vd - 1) / 10)
        if vd < 0 then vd = 0 end
      end
      if prot_arr[i] >= 0 then
        total += vd
        invincible = false
      end
    end
  end
  local spd = speed / math.max(typical, 1.0)
  local lever = 0.5 + 0.5 * math.clamp(blade, 0.0, 1.0)
  local edge = 0.5 + 0.5 * math.sin(math.clamp(angle, 0.0, 90.0) * math.pi / 180.0)
  local mult = math.clamp(spd * lever * edge, 0.5, 1.5) * (0.6 ^ target_idx)
  local prop_mult = quality * (if temp > 400 then 1.1 else 1.0)
  if invincible then
    return 0
  end
  return math.max(5, math.floor(total * mult * prop_mult + 0.5))
end
)LUA";

  // Pad to ~500 lines for realistic hot-reload compilation measurements.
  for(int i = 0; i < extraFunctions; ++i) {
    src += "function helper_rule_" + std::to_string(i) + "(a, b, c)\n"
           "  local v = a * 1.05 + b * 0.5\n"
           "  if c > 10 then v += c * 0.25 else v -= 1.0 end\n"
           "  return math.clamp(v, 0, 1000)\n"
           "end\n";
  }
  return src;
}

double percentile(std::vector<double> values, double p) {
  if(values.empty())
    return 0.0;
  std::sort(values.begin(), values.end());
  const double idx = (p / 100.0) * double(values.size() - 1);
  const size_t lo = size_t(std::floor(idx));
  const size_t hi = std::min(lo + 1, values.size() - 1);
  const double frac = idx - double(lo);
  return values[lo] * (1.0 - frac) + values[hi] * frac;
}

struct BenchResults {
  // 1. Memory & OOM
  size_t singleVmAllocLive = 0;
  size_t singleVmAllocPeak = 0;
  size_t fiftyVmAllocAvg = 0;
  size_t fiftyVmRssDelta = 0;
  size_t hundredVmAllocAvg = 0;
  size_t hundredVmRssDelta = 0;
  bool oomCaughtCleanly = false;
  bool siblingIntactAfterOom = false;

  // 2. Boundary cost (ns/call)
  double pingNs = 0;
  double tradeQuoteNs = 0;
  double craftTxNs = 0;

  // 3. Interrupt overhead & cutoff (inline polling vs external watchdog)
  double loopBaselineMs = 0;
  double loopStride64OverheadPct = 0;
  double loopStride256OverheadPct = 0;
  double loopWatchdogOverheadPct = 0;
  double cutoff025MedianUs = 0;
  double cutoff025P95Us = 0;
  double cutoff025MaxUs = 0;
  double cutoff200MedianUs = 0;
  double cutoff200P95Us = 0;
  double cutoff200MaxUs = 0;
  double wdCutoff025MedianUs = 0;
  double wdCutoff025P95Us = 0;
  double wdCutoff025MaxUs = 0;
  double wdCutoff200MedianUs = 0;
  double wdCutoff200P95Us = 0;
  double wdCutoff200MaxUs = 0;

  // 4. Hot reload (ms)
  double reload1ModMedianMs = 0;
  double reload10ModsMedianMs = 0;
  double reload10ModsP95Ms = 0;

  // 5. Damage calc in interpreter (ns/hit)
  double damageCalcNs = 0;
  int64_t damageChecksum = 0;
};

BenchResults runBenchmarks() {
  BenchResults r{};
  const std::string baseMod = sampleModSource(0);
  const std::string full500LineMod = sampleModSource(85); // ~510 lines

  // --- 1. Memory per isolated lua_State + OOM isolation ---
  {
    ModVm vm;
    vm.loadAndRun(baseMod, "@mod_base.luau");
    lua_gc(vm.L, LUA_GCCOLLECT, 0);
    r.singleVmAllocLive = vm.alloc.currentBytes;
    r.singleVmAllocPeak = vm.alloc.peakBytes;
  }

  {
    // Warm allocator pages slightly before RSS delta measurement.
    const size_t rssBefore50 = currentRssBytes();
    std::vector<std::unique_ptr<ModVm>> vms;
    vms.reserve(100);
    size_t sum50 = 0;
    for(int i = 0; i < 50; ++i) {
      auto vm = std::make_unique<ModVm>();
      vm->loadAndRun(baseMod, "@mod_50.luau");
      lua_gc(vm->L, LUA_GCCOLLECT, 0);
      sum50 += vm->alloc.currentBytes;
      vms.push_back(std::move(vm));
    }
    const size_t rssAfter50 = currentRssBytes();
    r.fiftyVmAllocAvg = sum50 / 50;
    r.fiftyVmRssDelta = rssAfter50 > rssBefore50 ? (rssAfter50 - rssBefore50) : 0;

    size_t sum100 = sum50;
    for(int i = 50; i < 100; ++i) {
      auto vm = std::make_unique<ModVm>();
      vm->loadAndRun(baseMod, "@mod_100.luau");
      lua_gc(vm->L, LUA_GCCOLLECT, 0);
      sum100 += vm->alloc.currentBytes;
      vms.push_back(std::move(vm));
    }
    const size_t rssAfter100 = currentRssBytes();
    r.hundredVmAllocAvg = sum100 / 100;
    r.hundredVmRssDelta = rssAfter100 > rssBefore50 ? (rssAfter100 - rssBefore50) : 0;
  }

  // Hard OOM isolation test
  {
    ModVm healthy(16u * 1024u * 1024u);
    ModVm bomb(512u * 1024u);
    healthy.loadAndRun(baseMod, "@healthy.luau");
    bomb.loadAndRun(baseMod, "@bomb.luau");

    lua_getglobal(bomb.L, "memory_bomb");
    const int status = lua_pcall(bomb.L, 0, 0, 0);
    r.oomCaughtCleanly = (status == LUA_ERRMEM && bomb.alloc.oomRejects > 0 && bomb.alloc.currentBytes <= bomb.alloc.limitBytes);
    lua_settop(bomb.L, 0);

    lua_getglobal(healthy.L, "on_ping");
    lua_pushinteger(healthy.L, 7);
    const int hStatus = lua_pcall(healthy.L, 1, 1, 0);
    r.siblingIntactAfterOom = (hStatus == LUA_OK && lua_tointeger(healthy.L, -1) == healthy.host.actorStrength + (7 & 3) + 1);
    lua_settop(healthy.L, 0);
  }

  // --- 2. C++ <-> Luau boundary call cost ---
  {
    ModVm vm;
    vm.loadAndRun(baseMod, "@boundary.luau");

    // 2a. Simple query (ping -> host_get_attr)
    lua_getglobal(vm.L, "on_ping");
    const int fnRef = lua_ref(vm.L, -1);
    lua_pop(vm.L, 1);

    constexpr int PingIters = 200000;
    // Warmup
    for(int i = 0; i < 5000; ++i) {
      lua_getref(vm.L, fnRef);
      lua_pushinteger(vm.L, i);
      lua_call(vm.L, 1, 1);
      lua_pop(vm.L, 1);
    }
    int64_t sink = 0;
    const auto t0 = Clock::now();
    for(int i = 0; i < PingIters; ++i) {
      lua_getref(vm.L, fnRef);
      lua_pushinteger(vm.L, i);
      lua_call(vm.L, 1, 1);
      sink += lua_tointeger(vm.L, -1);
      lua_pop(vm.L, 1);
    }
    const auto t1 = Clock::now();
    r.pingNs = elapsedNs(t0, t1) / double(PingIters);

    // 2b. Structured trade.quote
    lua_getglobal(vm.L, "on_trade_quote");
    const int tradeRef = lua_ref(vm.L, -1);
    lua_pop(vm.L, 1);

    constexpr int TradeIters = 100000;
    for(int i = 0; i < 2000; ++i) {
      lua_getref(vm.L, tradeRef);
      lua_createtable(vm.L, 0, 4);
      lua_pushstring(vm.L, "ember_smithing:ember_blade");
      lua_setfield(vm.L, -2, "archetype");
      lua_pushnumber(vm.L, 1.2);
      lua_setfield(vm.L, -2, "quality");
      lua_pushnumber(vm.L, 85.0);
      lua_setfield(vm.L, -2, "durability");
      lua_pushnumber(vm.L, 750.0);
      lua_setfield(vm.L, -2, "temperature");
      lua_call(vm.L, 1, 1);
      lua_pop(vm.L, 1);
    }
    double multSink = 0.0;
    const auto t2 = Clock::now();
    for(int i = 0; i < TradeIters; ++i) {
      lua_getref(vm.L, tradeRef);
      lua_createtable(vm.L, 0, 4);
      lua_pushlstring(vm.L, "ember_smithing:ember_blade", 26);
      lua_setfield(vm.L, -2, "archetype");
      lua_pushnumber(vm.L, 1.15);
      lua_setfield(vm.L, -2, "quality");
      lua_pushnumber(vm.L, 80.0);
      lua_setfield(vm.L, -2, "durability");
      lua_pushnumber(vm.L, 700.0);
      lua_setfield(vm.L, -2, "temperature");
      lua_call(vm.L, 1, 1);
      if(lua_istable(vm.L, -1)) {
        lua_getfield(vm.L, -1, "price_multiplier");
        multSink += lua_tonumber(vm.L, -1);
        lua_pop(vm.L, 1);
      }
      lua_pop(vm.L, 1);
    }
    const auto t3 = Clock::now();
    r.tradeQuoteNs = elapsedNs(t2, t3) / double(TradeIters);

    // 2c. Transactional crafting event (builds 4-op TxPlan in C++ + commits)
    lua_getglobal(vm.L, "on_craft_completed");
    const int craftRef = lua_ref(vm.L, -1);
    lua_pop(vm.L, 1);

    constexpr int CraftIters = 100000;
    vm.host.plan.ops.reserve(8);
    const auto t4 = Clock::now();
    for(int i = 0; i < CraftIters; ++i) {
      vm.host.plan.ops.clear();
      lua_getref(vm.L, craftRef);
      lua_createtable(vm.L, 0, 1);
      lua_pushnumber(vm.L, double(1000 + i));
      lua_setfield(vm.L, -2, "actor");
      lua_call(vm.L, 1, 1);
      sink += lua_tointeger(vm.L, -1);
      lua_pop(vm.L, 1);
      vm.host.committedOps += vm.host.plan.ops.size();
    }
    const auto t5 = Clock::now();
    r.craftTxNs = elapsedNs(t4, t5) / double(CraftIters);

    if(sink == 0 && multSink < 0.0)
      std::fprintf(stderr, "unreachable\n");
  }

  // --- 3. Interrupt overhead & infinite-loop cutoff accuracy ---
  {
    ModVm vm;
    vm.loadAndRun(baseMod, "@interrupt.luau");
    lua_getglobal(vm.L, "tight_loop");
    const int loopRef = lua_ref(vm.L, -1);
    lua_pop(vm.L, 1);

    CpuWatchdog watchdog;
    constexpr int LoopN = 5000000;
    auto measureLoopMs = [&](int mode, uint32_t stride) {
      // mode 0: no interrupt; mode 1: inline polling; mode 2: external watchdog (cb.interrupt == nullptr)
      std::vector<double> samples;
      samples.reserve(7);
      for(int run = 0; run < 7; ++run) {
        vm.guard.stride = stride;
        vm.guard.counter = 0;
        vm.guard.triggered = false;
        vm.guard.active = (mode == 1);
        vm.guard.deadline = Clock::now() + std::chrono::seconds(10);
        if(mode == 1)
          lua_callbacks(vm.L)->interrupt = interruptHandler;
        else if(mode == 2)
          watchdog.arm(vm.L, Clock::now() + std::chrono::seconds(10));
        else
          lua_callbacks(vm.L)->interrupt = nullptr;

        const auto a = Clock::now();
        lua_getref(vm.L, loopRef);
        lua_pushinteger(vm.L, LoopN);
        lua_call(vm.L, 1, 1);
        lua_pop(vm.L, 1);
        const auto b = Clock::now();
        if(mode == 2)
          watchdog.disarm(vm.L);
        else
          lua_callbacks(vm.L)->interrupt = nullptr;
        samples.push_back(elapsedMs(a, b));
      }
      return percentile(samples, 50.0);
    };

    // Warmup
    measureLoopMs(0, 64);
    r.loopBaselineMs = measureLoopMs(0, 64);
    const double s64 = measureLoopMs(1, 64);
    const double s256 = measureLoopMs(1, 256);
    const double wd = measureLoopMs(2, 0);
    r.loopStride64OverheadPct = (s64 / r.loopBaselineMs - 1.0) * 100.0;
    r.loopStride256OverheadPct = (s256 / r.loopBaselineMs - 1.0) * 100.0;
    r.loopWatchdogOverheadPct = (wd / r.loopBaselineMs - 1.0) * 100.0;

    // Infinite loop cutoff accuracy at 0.25 ms and 2.0 ms
    lua_getglobal(vm.L, "infinite_loop");
    const int infRef = lua_ref(vm.L, -1);
    lua_pop(vm.L, 1);

    auto measureInlineCutoffUs = [&](int budgetUs, int trials) {
      std::vector<double> samples;
      samples.reserve(size_t(trials));
      lua_callbacks(vm.L)->interrupt = interruptHandler;
      for(int i = 0; i < trials; ++i) {
        vm.guard.stride = 64;
        vm.guard.counter = 0;
        vm.guard.triggered = false;
        vm.guard.active = true;
        const auto start = Clock::now();
        vm.guard.deadline = start + std::chrono::microseconds(budgetUs);
        lua_getref(vm.L, infRef);
        const int status = lua_pcall(vm.L, 0, 0, 0);
        const auto stop = Clock::now();
        if(status != LUA_ERRRUN || !vm.guard.triggered) {
          std::fprintf(stderr, "fatal: inline interrupt failed to terminate infinite loop\n");
          std::exit(3);
        }
        lua_settop(vm.L, 0);
        samples.push_back(elapsedUs(start, stop));
      }
      lua_callbacks(vm.L)->interrupt = nullptr;
      return samples;
    };

    auto measureWatchdogCutoffUs = [&](int budgetUs, int trials) {
      std::vector<double> samples;
      samples.reserve(size_t(trials));
      for(int i = 0; i < trials; ++i) {
        vm.guard.triggered = false;
        const auto start = Clock::now();
        watchdog.arm(vm.L, start + std::chrono::microseconds(budgetUs));
        lua_getref(vm.L, infRef);
        const int status = lua_pcall(vm.L, 0, 0, 0);
        const auto stop = Clock::now();
        watchdog.disarm(vm.L);
        if(status != LUA_ERRRUN || !vm.guard.triggered) {
          std::fprintf(stderr, "fatal: watchdog interrupt failed to terminate infinite loop\n");
          std::exit(3);
        }
        lua_settop(vm.L, 0);
        samples.push_back(elapsedUs(start, stop));
      }
      return samples;
    };

    const auto c025 = measureInlineCutoffUs(250, 50);
    r.cutoff025MedianUs = percentile(c025, 50.0);
    r.cutoff025P95Us = percentile(c025, 95.0);
    r.cutoff025MaxUs = *std::max_element(c025.begin(), c025.end());

    const auto c200 = measureInlineCutoffUs(2000, 30);
    r.cutoff200MedianUs = percentile(c200, 50.0);
    r.cutoff200P95Us = percentile(c200, 95.0);
    r.cutoff200MaxUs = *std::max_element(c200.begin(), c200.end());

    const auto w025 = measureWatchdogCutoffUs(250, 50);
    r.wdCutoff025MedianUs = percentile(w025, 50.0);
    r.wdCutoff025P95Us = percentile(w025, 95.0);
    r.wdCutoff025MaxUs = *std::max_element(w025.begin(), w025.end());

    const auto w200 = measureWatchdogCutoffUs(2000, 30);
    r.wdCutoff200MedianUs = percentile(w200, 50.0);
    r.wdCutoff200P95Us = percentile(w200, 95.0);
    r.wdCutoff200MaxUs = *std::max_element(w200.begin(), w200.end());
  }

  // --- 4. Hot reload latency (1 mod and 10 mods of ~500 lines Luau) ---
  {
    auto reloadBatch = [&](int count) {
      std::vector<std::unique_ptr<ModVm>> vms;
      vms.reserve(size_t(count));
      for(int i = 0; i < count; ++i) {
        auto vm = std::make_unique<ModVm>();
        vm->loadAndRun(full500LineMod, "@reload.luau");
        vms.push_back(std::move(vm));
      }

      std::vector<double> samples;
      samples.reserve(15);
      for(int iter = 0; iter < 15; ++iter) {
        const auto start = Clock::now();
        for(int i = 0; i < count; ++i) {
          // Extract state -> destroy old lua_State -> recompile from source -> recreate -> on_load(state)
          int savedCount = 0;
          lua_getglobal(vms[size_t(i)]->L, "state");
          lua_getfield(vms[size_t(i)]->L, -1, "forged_count");
          savedCount = lua_tointeger(vms[size_t(i)]->L, -1);
          lua_pop(vms[size_t(i)]->L, 2);

          auto fresh = std::make_unique<ModVm>();
          fresh->loadAndRun(full500LineMod, "@reload.luau");
          lua_getglobal(fresh->L, "on_load");
          lua_createtable(fresh->L, 0, 2);
          lua_pushinteger(fresh->L, savedCount + 1);
          lua_setfield(fresh->L, -2, "forged_count");
          lua_pushboolean(fresh->L, 1);
          lua_setfield(fresh->L, -2, "masterwork_unlocked");
          lua_call(fresh->L, 1, 0);
          vms[size_t(i)] = std::move(fresh);
        }
        const auto stop = Clock::now();
        samples.push_back(elapsedMs(start, stop));
      }
      return samples;
    };

    const auto r1 = reloadBatch(1);
    const auto r10 = reloadBatch(10);
    r.reload1ModMedianMs = percentile(r1, 50.0);
    r.reload10ModsMedianMs = percentile(r10, 50.0);
    r.reload10ModsP95Ms = percentile(r10, 95.0);
  }

  // --- 5. Realistic Gothic II damage formula in interpreter ---
  {
    ModVm vm;
    vm.loadAndRun(baseMod, "@damage.luau");
    lua_getglobal(vm.L, "calc_sword_damage");
    const int dmgRef = lua_ref(vm.L, -1);
    lua_pop(vm.L, 1);

    // Pre-allocated read-only reusable damage/protection arrays (matches SDK userdata/table pattern).
    lua_createtable(vm.L, 8, 0);
    for(int i = 1; i <= 8; ++i) {
      lua_pushinteger(vm.L, i == 2 ? 45 : (i == 4 ? 10 : 0));
      lua_rawseti(vm.L, -2, i);
    }
    const int dmgArrRef = lua_ref(vm.L, -1);
    lua_pop(vm.L, 1);

    lua_createtable(vm.L, 8, 0);
    for(int i = 1; i <= 8; ++i) {
      lua_pushinteger(vm.L, i == 2 ? 20 : 15);
      lua_rawseti(vm.L, -2, i);
    }
    const int protArrRef = lua_ref(vm.L, -1);
    lua_pop(vm.L, 1);

    constexpr int DmgIters = 100000;
    int64_t sum = 0;
    for(int i = 0; i < 2000; ++i) {
      lua_getref(vm.L, dmgRef);
      lua_pushinteger(vm.L, 50);
      lua_pushinteger(vm.L, (1 << 1) | (1 << 3));
      lua_pushinteger(vm.L, i % 100);
      lua_pushinteger(vm.L, 60);
      lua_pushboolean(vm.L, 0);
      lua_getref(vm.L, dmgArrRef);
      lua_getref(vm.L, protArrRef);
      lua_pushnumber(vm.L, 650.0);
      lua_pushnumber(vm.L, 550.0);
      lua_pushnumber(vm.L, 0.8);
      lua_pushnumber(vm.L, 75.0);
      lua_pushinteger(vm.L, 0);
      lua_pushnumber(vm.L, 1.15);
      lua_pushnumber(vm.L, 800.0);
      lua_call(vm.L, 14, 1);
      lua_pop(vm.L, 1);
    }

    const auto a = Clock::now();
    for(int i = 0; i < DmgIters; ++i) {
      lua_getref(vm.L, dmgRef);
      lua_pushinteger(vm.L, 50);
      lua_pushinteger(vm.L, (1 << 1) | (1 << 3));
      lua_pushinteger(vm.L, i % 100);
      lua_pushinteger(vm.L, 60);
      lua_pushboolean(vm.L, 0);
      lua_getref(vm.L, dmgArrRef);
      lua_getref(vm.L, protArrRef);
      lua_pushnumber(vm.L, 650.0);
      lua_pushnumber(vm.L, 550.0);
      lua_pushnumber(vm.L, 0.8);
      lua_pushnumber(vm.L, 75.0);
      lua_pushinteger(vm.L, 0);
      lua_pushnumber(vm.L, 1.15);
      lua_pushnumber(vm.L, 800.0);
      lua_call(vm.L, 14, 1);
      sum += lua_tointeger(vm.L, -1);
      lua_pop(vm.L, 1);
    }
    const auto b = Clock::now();
    r.damageCalcNs = elapsedNs(a, b) / double(DmgIters);
    r.damageChecksum = sum;
  }

  return r;
}

} // namespace

int main(int argc, char** argv) {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  bool asJson = false;
  for(int i = 1; i < argc; ++i) {
    if(std::string_view(argv[i]) == "--json")
      asJson = true;
  }

  const BenchResults r = runBenchmarks();
  if(!r.oomCaughtCleanly || !r.siblingIntactAfterOom) {
    std::fprintf(stderr, "FAIL: OOM isolation check failed (caught=%d sibling=%d)\n",
                 int(r.oomCaughtCleanly), int(r.siblingIntactAfterOom));
    return 1;
  }

  if(asJson) {
    std::printf(
      "{\n"
      "  \"single_vm_alloc_live_bytes\": %zu,\n"
      "  \"single_vm_alloc_peak_bytes\": %zu,\n"
      "  \"fifty_vm_alloc_avg_bytes\": %zu,\n"
      "  \"fifty_vm_rss_delta_bytes\": %zu,\n"
      "  \"hundred_vm_alloc_avg_bytes\": %zu,\n"
      "  \"hundred_vm_rss_delta_bytes\": %zu,\n"
      "  \"oom_caught_cleanly\": %s,\n"
      "  \"sibling_intact_after_oom\": %s,\n"
      "  \"boundary_ping_ns\": %.1f,\n"
      "  \"boundary_trade_quote_ns\": %.1f,\n"
      "  \"boundary_craft_tx_ns\": %.1f,\n"
      "  \"interrupt_baseline_ms\": %.3f,\n"
      "  \"interrupt_inline_stride64_overhead_pct\": %.2f,\n"
      "  \"interrupt_inline_stride256_overhead_pct\": %.2f,\n"
      "  \"interrupt_watchdog_overhead_pct\": %.2f,\n"
      "  \"inline_cutoff_250us_median_us\": %.1f,\n"
      "  \"inline_cutoff_250us_p95_us\": %.1f,\n"
      "  \"inline_cutoff_250us_max_us\": %.1f,\n"
      "  \"inline_cutoff_2000us_median_us\": %.1f,\n"
      "  \"inline_cutoff_2000us_p95_us\": %.1f,\n"
      "  \"inline_cutoff_2000us_max_us\": %.1f,\n"
      "  \"watchdog_cutoff_250us_median_us\": %.1f,\n"
      "  \"watchdog_cutoff_250us_p95_us\": %.1f,\n"
      "  \"watchdog_cutoff_250us_max_us\": %.1f,\n"
      "  \"watchdog_cutoff_2000us_median_us\": %.1f,\n"
      "  \"watchdog_cutoff_2000us_p95_us\": %.1f,\n"
      "  \"watchdog_cutoff_2000us_max_us\": %.1f,\n"
      "  \"reload_1_mod_500loc_median_ms\": %.3f,\n"
      "  \"reload_10_mods_500loc_median_ms\": %.3f,\n"
      "  \"reload_10_mods_500loc_p95_ms\": %.3f,\n"
      "  \"damage_calc_interpreter_ns\": %.1f,\n"
      "  \"damage_checksum\": %lld\n"
      "}\n",
      r.singleVmAllocLive, r.singleVmAllocPeak,
      r.fiftyVmAllocAvg, r.fiftyVmRssDelta,
      r.hundredVmAllocAvg, r.hundredVmRssDelta,
      r.oomCaughtCleanly ? "true" : "false",
      r.siblingIntactAfterOom ? "true" : "false",
      r.pingNs, r.tradeQuoteNs, r.craftTxNs,
      r.loopBaselineMs, r.loopStride64OverheadPct, r.loopStride256OverheadPct, r.loopWatchdogOverheadPct,
      r.cutoff025MedianUs, r.cutoff025P95Us, r.cutoff025MaxUs,
      r.cutoff200MedianUs, r.cutoff200P95Us, r.cutoff200MaxUs,
      r.wdCutoff025MedianUs, r.wdCutoff025P95Us, r.wdCutoff025MaxUs,
      r.wdCutoff200MedianUs, r.wdCutoff200P95Us, r.wdCutoff200MaxUs,
      r.reload1ModMedianMs, r.reload10ModsMedianMs, r.reload10ModsP95Ms,
      r.damageCalcNs, static_cast<long long>(r.damageChecksum));
    return 0;
  }

  std::printf("=== GothicAI Modkit Phase 0 Benchmark (Luau 0.748 interpreter) ===\n");
  std::printf("[1] Memory per isolated lua_State:\n");
  std::printf("    1 mod   : live = %.1f KiB, peak = %.1f KiB\n",
              double(r.singleVmAllocLive) / 1024.0, double(r.singleVmAllocPeak) / 1024.0);
  std::printf("    50 mods : avg alloc = %.1f KiB/mod, RSS delta = %.2f MiB\n",
              double(r.fiftyVmAllocAvg) / 1024.0, double(r.fiftyVmRssDelta) / (1024.0 * 1024.0));
  std::printf("    100 mods: avg alloc = %.1f KiB/mod, RSS delta = %.2f MiB\n",
              double(r.hundredVmAllocAvg) / 1024.0, double(r.hundredVmRssDelta) / (1024.0 * 1024.0));
  std::printf("    OOM trap: caught=%s, sibling_intact=%s\n",
              r.oomCaughtCleanly ? "yes" : "no", r.siblingIntactAfterOom ? "yes" : "no");

  std::printf("[2] C++ <-> Luau boundary cost:\n");
  std::printf("    simple query (ping + host_get_attr): %.1f ns/call\n", r.pingNs);
  std::printf("    structured trade.quote (table in/out): %.1f ns/call\n", r.tradeQuoteNs);
  std::printf("    crafting TxPlan (4 host ops + commit): %.1f ns/call\n", r.craftTxNs);

  std::printf("[3] lua_Callbacks::interrupt overhead & cutoff:\n");
  std::printf("    5M-iter tight loop baseline: %.3f ms\n", r.loopBaselineMs);
  std::printf("    overhead: inline(stride=64) %+.2f%%, inline(stride=256) %+.2f%%, watchdog %+.2f%%\n",
              r.loopStride64OverheadPct, r.loopStride256OverheadPct, r.loopWatchdogOverheadPct);
  std::printf("    inline  while(true) 250 us : median=%.1f us, P95=%.1f us, max=%.1f us\n",
              r.cutoff025MedianUs, r.cutoff025P95Us, r.cutoff025MaxUs);
  std::printf("    inline  while(true) 2000 us: median=%.1f us, P95=%.1f us, max=%.1f us\n",
              r.cutoff200MedianUs, r.cutoff200P95Us, r.cutoff200MaxUs);
  std::printf("    watchdog while(true) 250 us : median=%.1f us, P95=%.1f us, max=%.1f us\n",
              r.wdCutoff025MedianUs, r.wdCutoff025P95Us, r.wdCutoff025MaxUs);
  std::printf("    watchdog while(true) 2000 us: median=%.1f us, P95=%.1f us, max=%.1f us\n",
              r.wdCutoff200MedianUs, r.wdCutoff200P95Us, r.wdCutoff200MaxUs);

  std::printf("[4] Runtime-only hot reload (~500 LOC Luau, source->compile->sandbox->on_load):\n");
  std::printf("    1 mod  : median = %.3f ms\n", r.reload1ModMedianMs);
  std::printf("    10 mods: median = %.3f ms, P95 = %.3f ms\n", r.reload10ModsMedianMs, r.reload10ModsP95Ms);

  std::printf("[5] Combat swordDamage + meleeMultiplier in interpreter:\n");
  std::printf("    %.1f ns/hit (%.3f us/hit), checksum=%lld\n",
              r.damageCalcNs, r.damageCalcNs / 1000.0, static_cast<long long>(r.damageChecksum));
  return 0;
}
