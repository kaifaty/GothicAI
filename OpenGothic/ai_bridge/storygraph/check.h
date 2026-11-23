#pragma once

#include "effects.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace daedalus {
  class DaedalusVm;
  }

// Stage 2 of docs/story-graph.md: every lifted dialog condition against the VM running the script.
namespace storygraph {

struct Model;
struct Node;

// a VM over GOTHIC.DAT with the script classes registered; nullptr if the file can't be read
std::unique_ptr<daedalus::DaedalusVm> loadVm(const char* datPath);

// on original saves (check.cpp); with `traceNode`, both computations of that dialog are printed. With
// `effects`, what the dialogs and choices do is checked instead of when they are offered (effects.h); with
// `exec`, their programs run by the Interpreter against the VM (execcheck.h).
int checkSaves(const char* datPath, const Model& m, const std::vector<std::string>& saves, const char* traceNode,
               bool verbose, bool effects = false, bool exec = false);
// in `rounds` random states per dialog (fuzz.cpp); `only`: one dialog
int fuzzConditions(const char* datPath, const Model& m, uint32_t rounds, const char* only, bool verbose);
// what dialogs and choices do, in `rounds` random states each (fuzz.cpp)
int fuzzEffects(const char* datPath, const Model& m, uint32_t rounds, const char* only, bool verbose);
// the programs of conditions, dialogs and choices against the VM, in `rounds` random states each (fuzz.cpp)
int fuzzExec(const char* datPath, const Model& m, uint32_t rounds, const char* only, bool verbose);

// tallies of the effects check
struct EffectTotals final {
  size_t runs = 0, calls = 0, agree = 0, differ = 0, unknown = 0, vmFailed = 0, noFunction = 0;
  std::map<std::string,size_t>                     unknownWhy;
  std::vector<std::pair<std::string,std::string>>  diffs;  // node, what differs

  void add(const Node& n, const EffectCheck::Result& r);
  void merge(const EffectTotals& o) {
    runs += o.runs; calls += o.calls; agree += o.agree; differ += o.differ; unknown += o.unknown;
    vmFailed += o.vmFailed; noFunction += o.noFunction;
    for(auto& [k,v] : o.unknownWhy)
      unknownWhy[k] += v;
    diffs.insert(diffs.end(), o.diffs.begin(), o.diffs.end());
    }
  void print(const char* what, bool verbose) const;
  };

}
