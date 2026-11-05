// The first measurement of the dialog layer: per dialog, what its condition and information functions
// touch in their own bytecode (see docs/story-graph.md, "Что показал замер").

#include "bytecode.h"
#include "expr.h"
#include "textcodec.h"
#include "vocab.h"

#include "daedalus/vm.h"
#include "daedalus/classes.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace storygraph;

namespace storygraph {
  int scanDialogs(daedalus::DaedalusVm& vm, const Bytecode& code, const char* tsvPath, size_t top);
  }

static bool subsetOf(const std::set<std::string>& a, const std::set<std::string>& b,
                     const std::set<std::string>& c = {}) {
  return std::all_of(a.begin(), a.end(), [&](const std::string& x){ return b.count(x)>0 || c.count(x)>0; });
  }

static std::string join(const std::set<std::string>& v) {
  std::string r;
  for(auto& x : v) {
    if(!r.empty())
      r += ',';
    r += ident(x);
    }
  return r;
  }

static double pct(size_t a, size_t b) { return b==0 ? 0.0 : 100.0*double(a)/double(b); }

static void printTop(const char* title, const std::map<std::string,size_t>& m, size_t n) {
  std::vector<std::pair<size_t,std::string>> v;
  for(auto& [k,c] : m)
    v.emplace_back(c,k);
  std::sort(v.rbegin(), v.rend());
  std::printf("\n%s (%zu distinct)\n", title, v.size());
  for(size_t i=0; i<v.size() && i<n; ++i)
    std::printf("  %6zu  %s\n", v[i].first, ident(v[i].second).c_str());
  }

int storygraph::scanDialogs(daedalus::DaedalusVm& vm, const Bytecode& code, const char* tsvPath, size_t top) {
  const daedalus::DaedalusScript& dat = vm;
  std::vector<std::shared_ptr<daedalus::IInfo>> infos;
  vm.enumerate_instances_by_class_name("C_INFO", [&](daedalus::DaedalusSymbol& sym){
    infos.push_back(vm.init_instance<daedalus::IInfo>(&sym));
    });

  FILE* tsv = tsvPath ? std::fopen(tsvPath,"w") : nullptr;
  if(tsv)
    std::fprintf(tsv, "info\tnpc\timportant\tpermanent\ttrade\tdescription\tcond_externals\tcond_calls\t"
                      "cond_reads\tcond_members\tcond_knows\tinfo_externals\tinfo_calls\tinfo_writes\toutputs\tchoices\n");

  std::map<std::string,size_t> condExternals, condCalls, condMembers, infoExternals, infoCalls;
  std::set<int32_t>     npcs;
  std::set<std::string> condReads, infoWrites;
  std::set<uint32_t>    choiceFns;
  size_t important=0, permanent=0, trade=0, withChoices=0, outputs=0, writing=0, knowsEdges=0;
  size_t plain=0, withHelpers=0, effectsStandard=0, effectsBespoke=0, chapterChange=0;
  std::map<std::string,size_t> bespoke;

  for(auto& info : infos) {
    npcs.insert(info->npc);
    important += info->important!=0;
    permanent += info->permanent!=0;
    trade     += info->trade!=0;

    FnSummary c = code.summarize(uint32_t(info->condition));
    FnSummary e = code.summarize(uint32_t(info->information));
    // the choices an information function offers, and the choices those offer
    std::vector<uint32_t> todo = e.choices;
    std::set<uint32_t>    seen;
    while(!todo.empty()) {
      uint32_t fn = todo.back();
      todo.pop_back();
      if(!seen.insert(fn).second)
        continue;
      FnSummary ch = code.summarize(fn);
      e.outputs += ch.outputs;
      e.externals.insert(ch.externals.begin(), ch.externals.end());
      e.calls.insert(ch.calls.begin(), ch.calls.end());
      e.writes.insert(ch.writes.begin(), ch.writes.end());
      todo.insert(todo.end(), ch.choices.begin(), ch.choices.end());
      }
    choiceFns.insert(seen.begin(), seen.end());
    withChoices += !seen.empty();
    outputs     += e.outputs;
    writing     += !e.writes.empty();
    knowsEdges  += c.infos.size();

    bool condPlain = subsetOf(c.externals, vocab::PredicateExternals);
    plain       += condPlain && c.calls.empty();
    withHelpers += condPlain && subsetOf(c.calls, vocab::PredicateHelpers);
    bool std     = subsetOf(e.externals, vocab::StandardExternals, vocab::PredicateExternals) &&
                   subsetOf(e.calls, vocab::StandardHelpers, vocab::PredicateHelpers);
    effectsStandard += std;
    chapterChange   += e.calls.count("B_KAPITELWECHSEL");
    if(!std) {
      effectsBespoke++;
      for(auto& x : e.calls)
        if(!vocab::StandardHelpers.count(x) && !vocab::PredicateHelpers.count(x))
          bespoke[x]++;
      }

    for(auto& x : c.externals) condExternals[x]++;
    for(auto& x : c.calls)     condCalls[x]++;
    for(auto& x : c.members)   condMembers[x]++;
    for(auto& x : e.externals) infoExternals[x]++;
    for(auto& x : e.calls)     infoCalls[x]++;
    condReads.insert(c.reads.begin(), c.reads.end());
    infoWrites.insert(e.writes.begin(), e.writes.end());

    if(tsv) {
      auto* self = dat.find_symbol_by_index(info->symbol_index());
      auto* npc  = dat.find_symbol_by_index(uint32_t(info->npc));
      std::fprintf(tsv, "%s\t%s\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%u\t%zu\n",
                   self ? ident(self->name()).c_str() : "?", npc ? ident(npc->name()).c_str() : "?",
                   info->important, info->permanent, info->trade, TextCodec::toUtf8(info->description).c_str(),
                   join(c.externals).c_str(), join(c.calls).c_str(), join(c.reads).c_str(), join(c.members).c_str(),
                   join(c.infos).c_str(), join(e.externals).c_str(), join(e.calls).c_str(), join(e.writes).c_str(),
                   e.outputs, seen.size());
      }
    }
  if(tsv)
    std::fclose(tsv);

  std::set<std::string> storyVars;
  for(auto& v : infoWrites)
    if(condReads.count(v))
      storyVars.insert(v);
  auto mis = [](const std::set<std::string>& s){
    return size_t(std::count_if(s.begin(), s.end(), [](const std::string& x){ return x.rfind("MIS_",0)==0; }));
    };

  size_t n = infos.size();
  std::printf("dialogs (C_INFO):            %zu, npcs with dialogs %zu\n", n, npcs.size());
  std::printf("  important / permanent / trade: %zu / %zu / %zu\n", important, permanent, trade);
  std::printf("  with Info_AddChoice:       %zu (%zu choice functions)\n", withChoices, choiceFns.size());
  std::printf("  AI_Output lines:           %zu\n", outputs);
  std::printf("conditions\n");
  std::printf("  plain predicates only:     %zu (%.1f%%)\n", plain, pct(plain,n));
  std::printf("  + generic helpers:         %zu (%.1f%%)\n", withHelpers, pct(withHelpers,n));
  std::printf("  Npc_KnowsInfo edges:       %zu\n", knowsEdges);
  std::printf("information functions\n");
  std::printf("  standard effects only:     %zu (%.1f%%)\n", effectsStandard, pct(effectsStandard,n));
  std::printf("  story-specific code:       %zu dialogs, %zu distinct script functions\n", effectsBespoke, bespoke.size());
  std::printf("  change the chapter:        %zu\n", chapterChange);
  std::printf("  write globals:             %zu\n", writing);
  std::printf("story state\n");
  std::printf("  globals read by conditions:    %zu (MIS_* %zu)\n", condReads.size(), mis(condReads));
  std::printf("  globals written by dialogs:    %zu (MIS_* %zu)\n", infoWrites.size(), mis(infoWrites));
  std::printf("  both (dialog-to-dialog state): %zu (MIS_* %zu)\n", storyVars.size(), mis(storyVars));

  if(top>0) {
    printTop("condition externals",   condExternals, top);
    printTop("condition script calls", condCalls,    top);
    printTop("condition members",     condMembers,   top);
    printTop("information externals", infoExternals, top);
    printTop("information script calls", infoCalls,  top);
    printTop("story-specific script calls", bespoke, top);
    }
  return 0;
  }
