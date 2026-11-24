#pragma once

#include "gamescript.h"
#include "game/definitions/stockstoryprograms.h"

#include "gamescript_nativecall.h"

class GameScript::StockPrograms final {
  public:
    explicit StockPrograms(GameScript& script);

  private:
    friend class GameScript;
    friend struct NpcDefinitionContract;

    class Host;
    using Binding = std::pair<daedalus::DaedalusSymbol*,SymbolKind>;

    bool run(uint32_t id, int32_t& result, bool* failed, bool preparedOnly);
    bool runPrepared(const daedalus::DaedalusSymbol& fn, int32_t& result, bool* failed);
    const StockStoryPrograms::Catalog* preparedCatalog();
    void bindCatalog(const StockStoryPrograms::Catalog& catalog);
    void bindSymbol(daedalus::DaedalusSymbol& symbol, SymbolKind kind);
    Binding binding(std::string_view name) const;
    const StockStoryPrograms::Entry* program(uint32_t id) const;
    std::optional<storygraph::NativeFunction> function(std::string_view name) const;
    std::unique_ptr<Host> host(bool effects);

    GameScript&                          script;
    daedalus::DaedalusVm&                 vm;
    std::optional<StockStoryPrograms::Catalog> prepared;
    std::unordered_map<std::string_view,Binding> bindings;
    std::unordered_map<uint32_t,daedalus::DaedalusSymbol*> ids;
    std::unordered_map<std::string_view,storygraph::BoundNativeCall> calls;
  };

struct GameScript::BennetOreDialog final {
  uint32_t                                 infoId = 0;
  const storygraph::Program*               condition = nullptr;
  const storygraph::Program*               answer = nullptr;
  daedalus::DaedalusSymbol*                 conditionSymbol = nullptr;
  daedalus::DaedalusSymbol*                 answerSymbol = nullptr;
  };

struct GameScript::BennetOre final {
  std::array<BennetOreDialog,3>             dialogs;
  std::shared_ptr<const daedalus::IItem>    ore;
  InventoryTransfer::Text                  notices;
  std::string_view                         font;
  uint32_t                                 missionId = 0;
  uint32_t                                 teachingId = 0;
  const daedalus::DaedalusSymbol*           smithTalent = nullptr;
  const daedalus::DaedalusSymbol*           rescueMission = nullptr;
  daedalus::DaedalusSymbol*                oreAlias = nullptr;
  daedalus::DaedalusSymbol*                giverAlias = nullptr;
  daedalus::DaedalusSymbol*                takerAlias = nullptr;
  daedalus::DaedalusSymbol*                itemAlias = nullptr;
  };
