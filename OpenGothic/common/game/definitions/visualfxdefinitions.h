#pragma once

#include <zenkit/DaedalusVm.hh>
#include <zenkit/addon/daedalus.hh>
#include "game/definitions/definitiontypes.h"

#include <unordered_map>
#include <memory>

class VisualFx;

class VisualFxDefinitions final {
  public:
    VisualFxDefinitions();
    ~VisualFxDefinitions();

    const VisualFx *get(std::string_view name);

  private:
    std::unique_ptr<zenkit::DaedalusVm>                       vm;
    std::unordered_map<std::string,std::unique_ptr<VisualFx>> vfx;

    std::shared_ptr<Def::EffectBase> implGet(std::string_view name);
  };
