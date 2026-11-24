#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class Item;
class Npc;

namespace InventoryTransfer {

struct Notice {
  std::string text;
  int32_t y = 0;
  };

struct Result {
  int32_t value = 0;
  bool itemChanged = false;
  Item* item = nullptr;
  std::optional<std::string> itemName;
  std::optional<Notice> notice;
  };

struct Text {
  uint32_t gold = 0;
  std::string_view goldGiven;
  std::string_view goldTaken;
  std::string_view itemGiven;
  std::string_view itemTaken;
  std::string_view itemsGiven;
  std::string_view itemsTaken;
  int32_t goldGivenY = 0;
  int32_t goldTakenY = 0;
  int32_t itemGivenY = 0;
  int32_t itemTakenY = 0;
  };

Result run(Npc* giver, Npc* taker, uint32_t item, int32_t amount,
           std::string_view previousItemName, const Text& text);

}
