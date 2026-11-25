#pragma once

#include <memory>
#include <string_view>

class GameplayDefinitions;

class SvmDefinitions final {
  public:
    explicit SvmDefinitions(std::shared_ptr<const GameplayDefinitions> records);
    ~SvmDefinitions();

    std::string_view find(std::string_view speech, int id);

  private:
    std::shared_ptr<const GameplayDefinitions>                records;
  };
