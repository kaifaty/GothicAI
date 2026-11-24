#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace TradeInventoryData {

struct Grant {
  uint32_t recipient = 0;
  int32_t item = 0;
  int32_t amount = 0;
  std::optional<uint32_t> itemSymbol;
  };

struct Bonus {
  enum class Condition { HeroGuild, SecretSign } condition;
  std::vector<Grant> grants, following;
  };

struct Batch {
  std::optional<uint32_t> granted;
  std::optional<int32_t> minimumChapter;
  bool requiresOldWorld = false;
  std::vector<Grant> grants;
  std::optional<Bonus> bonus;
  };

struct Stock {
  uint32_t function = 0;
  uint32_t actor = 0;
  std::vector<Batch> batches;
  };

struct Record {
  int32_t hero = 0;
  std::vector<std::pair<int32_t,int32_t>> runes, junk;
  std::vector<std::pair<int32_t,uint32_t>> traders;
  std::vector<std::pair<uint32_t,uint32_t>> restocks;
  std::vector<uint32_t> defeated;
  std::vector<Stock> stock;
  };

std::optional<Record> load(const std::filesystem::path& base);
void write(const std::filesystem::path& base, const Record& record);

}
