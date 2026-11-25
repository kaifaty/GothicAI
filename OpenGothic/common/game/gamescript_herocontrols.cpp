#include "gamescript.h"

#include "game/healing.h"
#include "gothic.h"
#include "utils/string_frm.h"
#include "world/benchhooks.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace {

struct PotionHotkey {
  std::array<const char*,3> items;
  std::array<const char*,3> amounts;
  Attribute attribute;
  Attribute maximum;
  const char* emptyMessage;
  const char* fullMessage;
  };

constexpr PotionHotkey healthPotionKey = {
  {"ITPO_HEALTH_01","ITPO_HEALTH_02","ITPO_HEALTH_03"},
  {"HP_ESSENZ","HP_EXTRAKT","HP_ELIXIER"},
  ATR_HITPOINTS, ATR_HITPOINTSMAX,
  "HERO_HEALTH_POTION_MISSING", "HERO_HEALTH_POTION_FULL"
  };
constexpr PotionHotkey manaPotionKey = {
  {"ITPO_MANA_01","ITPO_MANA_02","ITPO_MANA_03"},
  {"MANA_ESSENZ","MANA_EXTRAKT","MANA_ELIXIER"},
  ATR_MANA, ATR_MANAMAX,
  "HERO_MANA_POTION_MISSING", "HERO_MANA_POTION_FULL"
  };

int32_t subtract(int32_t left, int32_t right) {
  return std::bit_cast<int32_t>(uint32_t(left)-uint32_t(right));
  }

} // namespace

void GameScript::runStockHeroFeedback(std::string_view message, std::string_view line, bool gesture) {
  Gothic::inst().onPrint(itemRuntimeText(message));
  if(gesture || !line.empty()) {
    const auto self = std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("SELF"));
    if(gesture)
      ai_playani(self,"T_DONTKNOW");
    if(!line.empty())
      sayToNpc(self,self,line,true);
    }
  }

void GameScript::runStockHeroMissingLockpick() {
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("HERO"));
  const auto trained = npc_gettalentskill(hero,itemRuntimeInt("NPC_TALENT_PICKLOCK"));
  if(trained==itemRuntimeInt("FALSE"))
    runStockHeroFeedback("PRINT_NOPICKLOCKTALENT","$NOPICKLOCKTALENT",true);
  else
    runStockHeroFeedback("PRINT_PICKLOCKMISSING","$PICKLOCKMISSING",true);
  }

void GameScript::runStockHeroEmptyLoot() {
  const auto roll = hlp_random(100);
  setItemRuntimeInt("PLAYER_PLUNDER_IS_EMPTY.RND",roll);
  if(roll<=40)
    runStockHeroFeedback("PRINT_NOTHINGTOGET","$NOTHINGTOGET");
  else if(roll<=80)
    runStockHeroFeedback("PRINT_NOTHINGTOGET02","$NOTHINGTOGET02");
  else
    runStockHeroFeedback("PRINT_NOTHINGTOGET03","$NOTHINGTOGET03");
  }

void GameScript::runStockHeroWeaponPenalty() {
  const auto hero=std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("HERO"));
  const auto hp=hero->attribute[ATR_HITPOINTS];
  const auto next=std::max(2,std::bit_cast<int32_t>(uint32_t(hp)*4u)/5);
  const auto lost=subtract(hp,next);
  setItemRuntimeInt("B_BLITZINARSCH.CURRENTHITPOINTS",hp);
  setItemRuntimeInt("B_BLITZINARSCH.BLITZINARSCH_HITPOINTS",next);
  setItemRuntimeInt("B_BLITZINARSCH.ABZUG",lost);
  if(lost<=0)
    return;
  auto suffix=std::string(itemRuntimeText("HERO_WEAPON_DAMAGE_SUFFIX"));
  suffix+=itemRuntimeText("NAME_DAMAGE");
  setItemRuntimeText("B_BLITZINARSCH.CONCATTEXT1",suffix);
  const auto message=std::to_string(lost)+suffix;
  setItemRuntimeText("B_BLITZINARSCH.CONCATTEXT2",message);
  ai_printscreen(message,-1,itemRuntimeInt("YPOS_GOLDTAKEN"),itemRuntimeText("FONT_SCREENSMALL"),2);
  wld_playeffect("spellFX_BELIARSRAGE",hero,hero,0,0,0,itemRuntimeInt("FALSE"));
  hero->attribute[ATR_HITPOINTS]=next;
  }

void GameScript::runStockHeroEquipDenial(bool player, int32_t attribute, int32_t required) {
  setItemRuntimeInt("G_CANNOTUSE.BISPLAYER",player);
  setItemRuntimeInt("G_CANNOTUSE.NATTRIBUTE",attribute);
  setItemRuntimeInt("G_CANNOTUSE.NVALUE",required);
  if(attribute==ATR_MANAMAX && required==666666) {
    setItemRuntimeText("G_CANNOTUSE.STRMESSAGE",itemRuntimeText("PRINT_ADDON_BELIARSCOURSE_MISSING"));
    runStockHeroWeaponPenalty();
    setItemRuntimeInt("SC_FAILEDTOEQUIPBELIARSWEAPON",itemRuntimeInt("TRUE"));
    } else {
    static constexpr std::string_view captions[] = {
      "PRINT_HITPOINTS_MISSING", "PRINT_HITPOINTS_MAX_MISSING", "PRINT_MANA_MISSING",
      "PRINT_MANA_MAX_MISSING", "PRINT_STRENGTH_MISSING", "PRINT_DEXTERITY_MISSING",
      };
    int32_t current=0;
    auto reason=itemRuntimeText("HERO_EQUIP_UNKNOWN_REASON");
    if(attribute>=0 && size_t(attribute)<std::size(captions)) {
      const auto self=std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("SELF"));
      if(self)
        current=self->attribute[attribute];
      reason=itemRuntimeText(captions[attribute]);
      }
    const auto missing=subtract(required,current);
    const auto number=std::to_string(missing);
    auto message=number;
    message+=itemRuntimeText("HERO_EQUIP_ATTRIBUTE_SUFFIX");
    message+=reason;
    setItemRuntimeText("G_CANNOTUSE.STRATTRIBUTEMISSING",reason);
    setItemRuntimeInt("G_CANNOTUSE.NATTRIBUTEVALUE",current);
    setItemRuntimeInt("G_CANNOTUSE.NDIFFERENCE",missing);
    setItemRuntimeText("G_CANNOTUSE.STRDIFFERENCE",number);
    setItemRuntimeText("G_CANNOTUSE.STRMESSAGE",message);
    }
  if(player)
    Gothic::inst().onPrint(itemRuntimeText("G_CANNOTUSE.STRMESSAGE"));
  }

void GameScript::runStockHeroCastDenial(bool player, int32_t requiredCircle, int32_t actualCircle) {
  const auto missing=subtract(requiredCircle,actualCircle);
  const auto number=std::to_string(missing);
  auto message=number;
  message+=itemRuntimeText("HERO_CAST_CIRCLE_SUFFIX");
  message+=itemRuntimeText("PRINT_MAGCIRCLES_MISSING");
  setItemRuntimeInt("G_CANNOTCAST.BISPLAYER",player);
  setItemRuntimeInt("G_CANNOTCAST.NCIRCLENEEDED",requiredCircle);
  setItemRuntimeInt("G_CANNOTCAST.NCIRCLEPOSSESSED",actualCircle);
  setItemRuntimeInt("G_CANNOTCAST.NDIFFERENCE",missing);
  setItemRuntimeText("G_CANNOTCAST.STRDIFFERENCE",number);
  setItemRuntimeText("G_CANNOTCAST.STRMESSAGE",message);
  if(player)
    Gothic::inst().onPrint(message);
  }

int32_t GameScript::stockHeroMap() {
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("HERO"));
  constexpr auto saved="PLAYERMAPINSTANCE";
  constexpr auto oldInstance="PLAYER_HOTKEY_SCREEN_MAP.OLDINSTANCE";
  constexpr auto newInstance="PLAYER_HOTKEY_SCREEN_MAP.NEWINSTANCE";
  int32_t old=itemRuntimeInt(saved);
  setItemRuntimeInt(oldInstance,old);
  const auto saveMap = [&](int32_t item) {
    setItemRuntimeInt("B_SETPLAYERMAP.MAPINSTANCE",item);
    setItemRuntimeInt(saved,item);
    };
  if(npc_hasitems(hero,old)<1 && old>0) {
    old = 0;
    setItemRuntimeInt(oldInstance,old);
    saveMap(old);
    }
  setItemRuntimeInt(newInstance,old);

  static constexpr const char* maps[] = {
    "ITWR_MAP_NEWWORLD","ITWR_MAP_SHRINE_MIS","ITWR_MAP_CAVES_MIS","ITWR_MAP_NEWWORLD_ORNAMENTS_ADDON",
    "ITWR_MAP_NEWWORLD_DEXTER","ITWR_SHATTEREDGOLEM_MIS","ITWR_MAP_ORCELITE_MIS","ITWR_MAP_NEWWORLD_CITY",
    "ITWR_MAP_OLDWORLD","ITWR_MAP_OLDWORLD_OREMINES_MIS","ITWR_MAP_ADDONWORLD","ITWR_ADDON_TREASUREMAP",
    };
  std::array<int32_t,std::size(maps)> ids;
  std::transform(std::begin(maps),std::end(maps),ids.begin(),[&](const char* name) { return int32_t(findItemInstance(name).value()); });
  const std::span<const int32_t> all(ids);
  const auto khorinis = all.first<8>();
  const auto valley = all.subspan<8,2>();
  const auto jharkendar = all.last<2>();

  const int32_t level = itemRuntimeInt("CURRENTLEVEL");
  const auto filter = [&](std::span<const int32_t> group,const char* name) {
    if(level!=itemRuntimeInt(name) && std::ranges::find(group,old)!=group.end())
      setItemRuntimeInt(newInstance,0);
    };
  filter(khorinis,"NEWWORLD_ZEN");
  filter(valley,"OLDWORLD_ZEN");
  filter(jharkendar,"ADDONWORLD_ZEN");

  const auto firstOwned = [&](std::span<const int32_t> group) -> int32_t {
    const auto it = std::ranges::find_if(group,[&](int32_t id) { return npc_hasitems(hero,id)>=1; });
    return it==group.end() ? 0 : *it;
    };
  if(itemRuntimeInt(newInstance)<=0) {
    std::span<const int32_t> group;
    if(level==itemRuntimeInt("NEWWORLD_ZEN"))
      group = khorinis;
    else if(level==itemRuntimeInt("OLDWORLD_ZEN"))
      group = valley;
    else if(level==itemRuntimeInt("ADDONWORLD_ZEN"))
      group = jharkendar;
    setItemRuntimeInt(newInstance,firstOwned(group));
    }
  if(old<=0 && itemRuntimeInt(newInstance)<=0)
    setItemRuntimeInt(newInstance,firstOwned(all));
  const auto selected = itemRuntimeInt(newInstance);
  if(selected>0) {
    saveMap(selected);
    return selected;
    }
  return old;
  }

void GameScript::runStockPotionHotkey(bool mana) {
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("HERO"));
  if(npc_isinstate(hero,int32_t(ZS_Dead)))
    return;

  const auto& potion = mana ? manaPotionKey : healthPotionKey;
  const auto missing=hero ? subtract(hero->attribute[potion.maximum],hero->attribute[potion.attribute]) : 0;
  const auto use = [&](size_t size,bool mustFit) {
    const auto amount = itemRuntimeInt(potion.amounts[size]);
    const auto item = int32_t(findItemInstance(potion.items[size]).value());
    if(npc_hasitems(hero,item)==0 || (mustFit && missing<amount))
      return false;
    npc_changeattribute(hero,potion.attribute,amount);
    npc_removeinvitem(hero,item);
    snd_play("DRINKBOTTLE");
    return true;
    };
  for(size_t size=potion.items.size(); size>0; --size)
    if(use(size-1,true))
      return;
  if(missing!=0) {
    for(size_t size=0; size<potion.items.size(); ++size)
      if(use(size,false))
        return;
    }
  Gothic::inst().onPrint(itemRuntimeText(missing!=0 ? potion.emptyMessage : potion.fullMessage));
  }

bool GameScript::startHotkeyPotion(Npc& pl, bool mana) {
  const char* kind = mana ? "mana" : "health";
  auto note = [kind](std::string_view item) {
    if(Bench::isActive())
      Bench::note(string_frm("heal hotkey ",kind,": ",item).c_str());
    };
  if(pl.isDown()) {
    note("none");
    return true;
    }
  if(pl.hasState(BS_ITEMINTERACT) || pl.hasQueuedItemUse())
    return true;

  const auto& potion = mana ? manaPotionKey : healthPotionKey;
  Healing::Potion candidates[3] = {};
  for(size_t i=0; i<potion.items.size(); ++i) {
    const auto id=findItemInstance(potion.items[i]);
    if(!id)
      throw std::runtime_error("missing potion hotkey item "+std::string(potion.items[i]));
    candidates[i]={*id,itemRuntimeInt(potion.amounts[i]),pl.itemCount(*id)>0};
    }
  const auto missing = pl.attribute(potion.maximum) - pl.attribute(potion.attribute);
  // PLAYER_HOTKEY_LAME_* uses the strongest dose that fits the missing amount,
  // otherwise the weakest available dose. The caller handles feedback when none can be used.
  const auto selected = Healing::choosePotion(candidates,missing);
  if(selected==Healing::NoPotion) {
    note("none");
    return false;
    }
  const auto itemName=storyItemName(uint32_t(selected));
  note(itemName);
  if(pl.weaponState()!=WeaponState::NoWeapon && pl.itemUseInHandAnimation(*pl.getItem(selected)).empty()) {
    if(Bench::isActive())
      Bench::note(("item use "+std::string(Bench::who(&pl))+": "+
                      std::string(itemName)+" holster path: no compatible in-hand clip or deferred effect").c_str());
    pl.stopAnim("");
    pl.aiPush(AiQueue::aiRemoveWeapon());
    }
  pl.aiPush(AiQueue::aiUseItem(int32_t(selected)));
  return true;
  }
