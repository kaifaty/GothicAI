#include "animationtail.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace AnimationTail {
namespace Detail {
std::atomic<bool> watching{false}, touched{false};
}

namespace {
// Main thread only, like the loops that fill them.
uint64_t    checks = 0, mismatches = 0;
std::string firstNpc, firstStep;

bool reported = false;

void report() {
  if(reported)
    return;
  reported = true;
  std::fprintf(stderr,"[anim-tail-check] checked=%llu mismatched=%llu%s%s%s%s\n",
               static_cast<unsigned long long>(checks),static_cast<unsigned long long>(mismatches),
               mismatches>0 ? " first=" : "",mismatches>0 ? firstNpc.c_str() : "",
               mismatches>0 ? " step=" : "",mismatches>0 ? firstStep.c_str() : "");
  std::fflush(stderr);
  }
}

void begin() {
  Detail::touched.store(false,std::memory_order_relaxed);
  Detail::watching.store(true,std::memory_order_relaxed);
  }

void end() {
  Detail::watching.store(false,std::memory_order_relaxed);
  }

bool checking() {
  static const bool enabled = [] {
    const char* v = std::getenv("OPENGOTHIC_ANIM_TAIL_CHECK");
    if(v==nullptr || v[0]!='1')
      return false;
    std::atexit(report);
    return true;
    }();
  return enabled;
  }

void checked(const char* step, std::string_view npc, bool same) {
  ++checks;
  if(same)
    return;
  if(mismatches++==0) {
    firstNpc  = std::string(npc);
    firstStep = step;
    // Also now: a headless runner may leave with _exit, past the report at exit.
    std::fprintf(stderr,"[anim-tail-check] first mismatch: npc=%s step=%s\n",firstNpc.c_str(),step);
    }
  }

void reportCheck() {
  if(checking())
    report();
  }

std::string_view firstMismatchNpc()  { return firstNpc; }
std::string_view firstMismatchStep() { return firstStep; }
uint64_t checkedCount()    { return checks; }
uint64_t mismatchedCount() { return mismatches; }

}
