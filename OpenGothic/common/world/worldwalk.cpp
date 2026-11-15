#include "worldwalk.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
uint64_t prepareEpoch = 0, syncEpoch = 0, checks = 0, mismatches = 0;
std::string firstNpc, firstStep;
bool reported = false;

void report() {
  if(reported)
    return;
  reported = true;
  std::fprintf(stderr,"[world-walk-check] checked=%llu mismatched=%llu\n",
               static_cast<unsigned long long>(checks),static_cast<unsigned long long>(mismatches));
  std::fflush(stderr);
  }
}

uint64_t WorldWalk::revision(Step step) { return step==Prepare ? prepareEpoch : syncEpoch; }
void WorldWalk::invalidate() { ++prepareEpoch; }
void WorldWalk::ragdollCreated() { ++prepareEpoch; ++syncEpoch; }

bool WorldWalk::checking() {
  static const bool enabled = [] {
    const auto* value = std::getenv("OPENGOTHIC_WORLD_WALK_CHECK");
    if(value==nullptr || value[0]!='1')
      return false;
    std::atexit(report);
    return true;
    }();
  return enabled;
  }

void WorldWalk::checked(const char* step, std::string_view npc, bool same) {
  ++checks;
  if(same)
    return;
  if(mismatches++==0) {
    firstNpc = npc;
    firstStep = step;
    std::fprintf(stderr,"[world-walk-check] first mismatch: npc=%s step=%s\n",firstNpc.c_str(),step);
    std::fflush(stderr);
    }
  }

uint64_t WorldWalk::checkedCount() { return checks; }
uint64_t WorldWalk::mismatchedCount() { return mismatches; }
std::string_view WorldWalk::firstMismatchNpc() { return firstNpc; }
std::string_view WorldWalk::firstMismatchStep() { return firstStep; }
void WorldWalk::reportCheck() { if(checking()) report(); }
