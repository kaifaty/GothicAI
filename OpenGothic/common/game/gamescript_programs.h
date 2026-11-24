#pragma once

#include "gamescript.h"
#include "game/definitions/stockstoryprograms.h"

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

    bool run(const daedalus::DaedalusSymbol& fn, int32_t& result, bool* failed, bool preparedOnly);
    std::optional<int32_t> runPreparedInt(const storygraph::Program& program,
                                          const std::vector<storygraph::Value>& args, std::string& error);
    const storygraph::Program* findPrepared(const daedalus::DaedalusSymbol& fn, bool requireDependencies);

  private:
    friend struct NpcDefinitionContract;

    struct Entry {
      storygraph::Program program;
      std::set<std::string> calls;
      };

    bool runPrepared(const daedalus::DaedalusSymbol& fn, int32_t& result, bool* failed);
    const StockStoryPrograms::Catalog* preparedCatalog();
    const Entry& entry(uint32_t id);
    bool isSupported(const daedalus::DaedalusSymbol& fn, std::unordered_set<uint32_t>& checked, bool preparedOnly);

    GameScript&                          script;
    std::optional<StockStoryPrograms::Catalog> prepared;
    bool                                 preparedChecked = false;
    daedalus::DaedalusVm&                 vm;
    std::unique_ptr<storygraph::Bytecode>  code;
    GameStoryExternals                    engine;
    storygraph::Interpreter               interpreter;
    std::unordered_map<uint32_t,Entry>    programs;
  };
