#pragma once

#include "nets/smallnet.h"
#include "society/society.h"

class Serialize;

class SocietyPolicy final {
  public:
    SocietyPolicy(const std::filesystem::path& path, const Society::World& world);
    void choose(const Society::DecisionBatch& batch, std::vector<Society::Choice>& out);
    void saveState(Serialize& save) const;
    void restoreState(Serialize& save);

  private:
    static constexpr size_t Hidden = 128;
    const size_t menuSize;
    uint64_t weightsHash = 14695981039346656037ull;
    SmallNet::Linear input, memory, option;
    std::vector<std::vector<float>> hidden;
  };
