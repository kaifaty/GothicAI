#pragma once

#include "records/definitions.h"

#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace daedalus {
class DaedalusVm;
}

class VisualFx;

class VisualFxDefinitions final {
  public:
    VisualFxDefinitions();
    ~VisualFxDefinitions();

    const VisualFx *get(std::string_view name);

  private:
    std::unique_ptr<daedalus::DaedalusVm>                    vm;
    std::unordered_map<std::string,std::unique_ptr<VisualFx>> vfx;
    std::unordered_map<std::string,Def::EffectBase>          effects;
    std::unordered_map<std::string,Def::ParticleEffectEmitKey> keys;

    std::optional<Def::EffectBase>            findEffect(std::string_view name);
    std::optional<Def::ParticleEffectEmitKey> findKey(std::string_view name);
  };
