#pragma once

#include "daedalus/vm.h"
#include "definitionjson.h"

#include <filesystem>

class GameplayDefinitions final {
  public:
    static constexpr std::string_view stockProgram = "a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c";
    static constexpr std::string_view format = "gothic-gameplay-definitions";
    static constexpr int version = 1;
    static constexpr std::string_view focusNames[] = {"FOCUS_NORMAL","FOCUS_MELEE","FOCUS_RANGED","FOCUS_MAGIC"};

    static std::filesystem::path section(const std::filesystem::path& base) {
      return base/"definitions"/stockProgram/"gameplay.json";
      }

    static std::shared_ptr<const GameplayDefinitions> load(const std::filesystem::path& base);

    template<class T>
    std::shared_ptr<T> init(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol* symbol,
                            std::shared_ptr<T> instance = std::make_shared<T>()) const {
      vm.allocate_instance(instance,symbol);
      restore(vm,*symbol,*instance);
      return instance;
      }

    static const daedalus::DaedalusSymbol& instanceClass(daedalus::DaedalusVm& vm,
                                                         const daedalus::DaedalusSymbol& symbol) {
      const auto* cls = vm.find_symbol_by_index(symbol.parent());
      while(cls!=nullptr && cls->type()!=daedalus::DaedalusDataType::CLASS)
        cls = vm.find_symbol_by_index(cls->parent());
      DefinitionJson::require(cls!=nullptr,"gameplay definitions: missing class for "+symbol.name());
      return *cls;
      }

  private:
    explicit GameplayDefinitions(rapidjson::Document rows) : rows(std::move(rows)) {}
    void restore(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& symbol,
                 daedalus::DaedalusInstance& instance) const;

    rapidjson::Document rows;
  };
