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

    bool run(uint32_t id, int32_t& result, bool* failed);
    bool runPrepared(uint32_t id, std::string_view name, int32_t& result, bool* failed);
    const StockStoryPrograms::Entry* program(uint32_t id) const;
    const StockStoryPrograms::Entry* program(std::string_view name) const;
    std::optional<storygraph::NativeFunction> function(std::string_view name) const;
    std::unique_ptr<Host> host(bool effects);

    GameScript&                          script;
    StockStoryPrograms::Catalog          prepared;
  };

struct GameScript::BennetOreDialog final {
  uint32_t                                 infoId = 0;
  uint32_t                                 conditionId = 0;
  uint32_t                                 answerId = 0;
  const storygraph::Program*               condition = nullptr;
  const storygraph::Program*               answer = nullptr;
  };

struct GameScript::BennetOre final {
  static constexpr std::string_view missionName = "MIS_BENNET_BRINGORE";
  static constexpr std::string_view teachingName = "BENNET_TEACHSMITH";

  template<class Script>
  static auto progressField(Script& script, std::string_view name) -> decltype(&script.bennetOreMission) {
    if(name==missionName)
      return &script.bennetOreMission;
    if(name==teachingName)
      return &script.bennetOreTeaching;
    return nullptr;
    }

  std::array<BennetOreDialog,3>             dialogs;
  std::shared_ptr<const ItemInstance>       ore;
  InventoryTransfer::Text                  notices;
  std::string_view                         font;
  };
