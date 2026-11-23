#pragma once

#include "gamescript.h"

#include "storygraph/bytecode.h"
#include "storygraph/exec.h"

#include <unordered_set>

class GameStoryExternals final : public storygraph::Externals {
  public:
    GameStoryExternals(GameScript& script, bool effects);

    storygraph::Value call(const daedalus::DaedalusSymbol& fn, const std::vector<storygraph::Value>& args) override;

  private:
    GameScript&               script;
    storygraph::VmExternals   fallback;
    bool                      effects;
  };

class GameScript::StockPrograms final {
  public:
    explicit StockPrograms(GameScript& script);

    bool run(const daedalus::DaedalusSymbol& fn, int32_t& result, bool* failed);

  private:
    struct Entry {
      storygraph::Program program;
      std::set<std::string> calls;
      };

    const Entry& entry(uint32_t id);
    bool isSupported(const daedalus::DaedalusSymbol& fn, std::unordered_set<uint32_t>& checked);

    daedalus::DaedalusVm&                 vm;
    storygraph::Bytecode                  code;
    GameStoryExternals                    engine;
    storygraph::Interpreter               interpreter;
    std::unordered_map<uint32_t,Entry>    programs;
  };
