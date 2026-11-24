#include "gamescript_inventorytransfer.h"

#include "world/objects/item.h"
#include "world/objects/npc.h"

namespace InventoryTransfer {

Result run(Npc* giver, Npc* taker, uint32_t item, int32_t amount,
           std::string_view previousItemName, const Text& text) {
  Result result;
  const bool giving = giver!=nullptr && giver->isPlayer();
  if(giving && amount>int32_t(giver->itemCount(item)))
    return result;
  result.value = 1;
  if(amount==0)
    return result;

  if(amount>0) {
    if(giver!=nullptr)
      giver->delItem(item,uint32_t(amount));
    // An NPC giver can grant the full amount even with fewer items in inventory.
    if(taker!=nullptr) {
      result.item = taker->addItem(item,size_t(amount));
      result.itemChanged = true;
      }
    }

  if(result.itemChanged)
    result.itemName = result.item!=nullptr ? result.item->handle().name : std::string{};
  else
    result.itemName = std::string(previousItemName);

  if(giving || (taker!=nullptr && taker->isPlayer())) {
    auto& notice = result.notice.emplace();
    if(item==text.gold) {
      notice.text = std::to_string(amount);
      notice.text += giving ? text.goldGiven : text.goldTaken;
      notice.y = giving ? text.goldGivenY : text.goldTakenY;
      } else {
      if(amount==1) {
        notice.text = *result.itemName;
        notice.text += giving ? text.itemGiven : text.itemTaken;
        } else {
        notice.text = std::to_string(amount);
        notice.text += giving ? text.itemsGiven : text.itemsTaken;
        notice.text += " ("+*result.itemName+")";
        }
      notice.y = giving ? text.itemGivenY : text.itemTakenY;
      }
    }
  return result;
  }

}
