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
  ATR_HITPOINTS, ATR_HITPOINTSMAX, "$18827", "$18828"
  };
constexpr PotionHotkey manaPotionKey = {
  {"ITPO_MANA_01","ITPO_MANA_02","ITPO_MANA_03"},
  {"MANA_ESSENZ","MANA_EXTRAKT","MANA_ELIXIER"},
  ATR_MANA, ATR_MANAMAX, "$18825", "$18826"
  };

int32_t subtract(int32_t left, int32_t right) {
  return std::bit_cast<int32_t>(uint32_t(left)-uint32_t(right));
  }

// The formatting externals also update the VM's serialized temporary string slots.
void pushHeroNumber(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol* number) {
  vm.push_reference(number);
  vm.unsafe_call_external(vm.find_symbol_by_name("INTTOSTRING"));
  }

void appendHeroMessage(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol* text, daedalus::DaedalusSymbol* suffix) {
  vm.push_reference(text);
  vm.push_reference(suffix);
  vm.unsafe_call_external(vm.find_symbol_by_name("CONCATSTRINGS"));
  text->set_string(vm.pop_string());
  }

} // namespace

void GameScript::runStockHeroFeedback(std::string_view message, std::string_view line, bool gesture) {
  Gothic::inst().onPrint(findSymbol(message)->get_string());
  if(gesture || !line.empty()) {
    const auto self = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_self()->get_instance());
    if(gesture)
      ai_playani(self,"T_DONTKNOW");
    if(!line.empty())
      sayToNpc(self,self,line,true);
    }
  }

void GameScript::runStockHeroMissingLockpick() {
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  const auto trained = npc_gettalentskill(hero,findSymbol("NPC_TALENT_PICKLOCK")->get_int());
  if(trained==findSymbol("FALSE")->get_int())
    runStockHeroFeedback("PRINT_NOPICKLOCKTALENT","$NOPICKLOCKTALENT",true);
  else
    runStockHeroFeedback("PRINT_PICKLOCKMISSING","$PICKLOCKMISSING",true);
  }

void GameScript::runStockHeroEmptyLoot() {
  const auto roll = hlp_random(100);
  findSymbol("PLAYER_PLUNDER_IS_EMPTY.RND")->set_int(roll);
  if(roll<=40)
    runStockHeroFeedback("PRINT_NOTHINGTOGET","$NOTHINGTOGET");
  else if(roll<=80)
    runStockHeroFeedback("PRINT_NOTHINGTOGET02","$NOTHINGTOGET02");
  else
    runStockHeroFeedback("PRINT_NOTHINGTOGET03","$NOTHINGTOGET03");
  }

void GameScript::runStockHeroWeaponPenalty() {
  const auto hero = vm.global_hero()->get_instance();
  auto* attribute = findSymbol("C_NPC.ATTRIBUTE");
  const auto hp = vm.get_int(hero,attribute,ATR_HITPOINTS);
  const auto next = std::max(2,std::bit_cast<int32_t>(uint32_t(hp)*4u)/5);
  const auto lost = subtract(hp,next);
  findSymbol("B_BLITZINARSCH.CURRENTHITPOINTS")->set_int(hp);
  findSymbol("B_BLITZINARSCH.BLITZINARSCH_HITPOINTS")->set_int(next);
  auto* loss = findSymbol("B_BLITZINARSCH.ABZUG");
  loss->set_int(lost);
  if(lost<=0)
    return;
  vm.push_reference(findSymbol("$18925"));
  vm.push_reference(findSymbol("NAME_DAMAGE"));
  vm.unsafe_call_external(findSymbol("CONCATSTRINGS"));
  auto* suffix = findSymbol("B_BLITZINARSCH.CONCATTEXT1");
  suffix->set_string(vm.pop_string());

  pushHeroNumber(vm,loss);
  vm.push_reference(suffix);
  vm.unsafe_call_external(findSymbol("CONCATSTRINGS"));
  auto* message = findSymbol("B_BLITZINARSCH.CONCATTEXT2");
  message->set_string(vm.pop_string());
  ai_printscreen(message->get_string(),-1,findSymbol("YPOS_GOLDTAKEN")->get_int(),
                 findSymbol("FONT_SCREENSMALL")->get_string(),2);
  wld_playeffect("spellFX_BELIARSRAGE",hero,hero,0,0,0,findSymbol("FALSE")->get_int());
  // The script assigns HP directly and leaves at least two HP.
  vm.set_int(hero,attribute,ATR_HITPOINTS,next);
  }

void GameScript::runStockHeroEquipDenial(bool player, int32_t attribute, int32_t required) {
  findSymbol("G_CANNOTUSE.BISPLAYER")->set_int(player);
  findSymbol("G_CANNOTUSE.NATTRIBUTE")->set_int(attribute);
  findSymbol("G_CANNOTUSE.NVALUE")->set_int(required);
  auto* message = findSymbol("G_CANNOTUSE.STRMESSAGE");

  if(attribute==ATR_MANAMAX && required==666666) {
    message->set_string(findSymbol("PRINT_ADDON_BELIARSCOURSE_MISSING")->get_string());
    runStockHeroWeaponPenalty();
    findSymbol("SC_FAILEDTOEQUIPBELIARSWEAPON")->set_int(findSymbol("TRUE")->get_int());
    } else {
    static constexpr const char* text[] = {
      "PRINT_HITPOINTS_MISSING", "PRINT_HITPOINTS_MAX_MISSING", "PRINT_MANA_MISSING",
      "PRINT_MANA_MAX_MISSING", "PRINT_STRENGTH_MISSING", "PRINT_DEXTERITY_MISSING",
      };
    int32_t current = 0;
    auto* reason = findSymbol("$27337");
    if(attribute>=0 && size_t(attribute)<std::size(text)) {
      const auto self = vm.global_self()->get_instance();
      current = vm.get_int(self,findSymbol("C_NPC.ATTRIBUTE"),uint16_t(attribute));
      reason = findSymbol(text[attribute]);
      }
    auto* savedReason = findSymbol("G_CANNOTUSE.STRATTRIBUTEMISSING");
    savedReason->set_string(reason->get_string());
    findSymbol("G_CANNOTUSE.NATTRIBUTEVALUE")->set_int(current);
    auto* missing = findSymbol("G_CANNOTUSE.NDIFFERENCE");
    missing->set_int(subtract(required,current));
    pushHeroNumber(vm,missing);
    auto* number = findSymbol("G_CANNOTUSE.STRDIFFERENCE");
    number->set_string(vm.pop_string());
    message->set_string(number->get_string());
    appendHeroMessage(vm,message,findSymbol("$27338"));
    appendHeroMessage(vm,message,savedReason);
    }
  if(player)
    Gothic::inst().onPrint(message->get_string());
  }

void GameScript::runStockHeroCastDenial(bool player, int32_t requiredCircle, int32_t actualCircle) {
  findSymbol("G_CANNOTCAST.BISPLAYER")->set_int(player);
  findSymbol("G_CANNOTCAST.NCIRCLENEEDED")->set_int(requiredCircle);
  findSymbol("G_CANNOTCAST.NCIRCLEPOSSESSED")->set_int(actualCircle);
  auto* missing = findSymbol("G_CANNOTCAST.NDIFFERENCE");
  missing->set_int(subtract(requiredCircle,actualCircle));
  pushHeroNumber(vm,missing);
  auto* number = findSymbol("G_CANNOTCAST.STRDIFFERENCE");
  number->set_string(vm.pop_string());
  auto* message = findSymbol("G_CANNOTCAST.STRMESSAGE");
  vm.push_reference(number);
  vm.push_reference(findSymbol("$27336"));
  vm.unsafe_call_external(findSymbol("CONCATSTRINGS"));
  message->set_string(vm.pop_string());
  appendHeroMessage(vm,message,findSymbol("PRINT_MAGCIRCLES_MISSING"));
  if(player)
    Gothic::inst().onPrint(message->get_string());
  }

int32_t GameScript::stockHeroMap() {
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  auto* saved = findSymbol("PLAYERMAPINSTANCE");
  auto* oldInstance = findSymbol("PLAYER_HOTKEY_SCREEN_MAP.OLDINSTANCE");
  auto* newInstance = findSymbol("PLAYER_HOTKEY_SCREEN_MAP.NEWINSTANCE");
  int32_t old = saved->get_int();
  oldInstance->set_int(old);
  const auto saveMap = [&](int32_t item) {
    findSymbol("B_SETPLAYERMAP.MAPINSTANCE")->set_int(item);
    saved->set_int(item);
    };
  if(npc_hasitems(hero,old)<1 && old>0) {
    old = 0;
    oldInstance->set_int(old);
    saveMap(old);
    }
  newInstance->set_int(old);

  static constexpr const char* maps[] = {
    "ITWR_MAP_NEWWORLD","ITWR_MAP_SHRINE_MIS","ITWR_MAP_CAVES_MIS","ITWR_MAP_NEWWORLD_ORNAMENTS_ADDON",
    "ITWR_MAP_NEWWORLD_DEXTER","ITWR_SHATTEREDGOLEM_MIS","ITWR_MAP_ORCELITE_MIS","ITWR_MAP_NEWWORLD_CITY",
    "ITWR_MAP_OLDWORLD","ITWR_MAP_OLDWORLD_OREMINES_MIS","ITWR_MAP_ADDONWORLD","ITWR_ADDON_TREASUREMAP",
    };
  std::array<int32_t,std::size(maps)> ids;
  std::transform(std::begin(maps),std::end(maps),ids.begin(),[&](const char* name) { return int32_t(findSymbolIndex(name)); });
  const std::span<const int32_t> all(ids);
  const auto khorinis = all.first<8>();
  const auto valley = all.subspan<8,2>();
  const auto jharkendar = all.last<2>();

  const int32_t level = findSymbol("CURRENTLEVEL")->get_int();
  const auto filter = [&](std::span<const int32_t> group,const char* name) {
    if(level!=findSymbol(name)->get_int() && std::ranges::find(group,old)!=group.end())
      newInstance->set_int(0);
    };
  filter(khorinis,"NEWWORLD_ZEN");
  filter(valley,"OLDWORLD_ZEN");
  filter(jharkendar,"ADDONWORLD_ZEN");

  const auto firstOwned = [&](std::span<const int32_t> group) -> int32_t {
    const auto it = std::ranges::find_if(group,[&](int32_t id) { return npc_hasitems(hero,id)>=1; });
    return it==group.end() ? 0 : *it;
    };
  if(newInstance->get_int()<=0) {
    std::span<const int32_t> group;
    if(level==findSymbol("NEWWORLD_ZEN")->get_int())
      group = khorinis;
    else if(level==findSymbol("OLDWORLD_ZEN")->get_int())
      group = valley;
    else if(level==findSymbol("ADDONWORLD_ZEN")->get_int())
      group = jharkendar;
    newInstance->set_int(firstOwned(group));
    }
  if(old<=0 && newInstance->get_int()<=0)
    newInstance->set_int(firstOwned(all));
  const auto selected = newInstance->get_int();
  if(selected>0) {
    saveMap(selected);
    return selected;
    }
  return old;
  }

void GameScript::runStockLegacyPotionKey(bool mana) {
  const auto hero = std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
  if(npc_isinstate(hero,int32_t(ZS_Dead)))
    return;

  const auto& potion = mana ? manaPotionKey : healthPotionKey;
  auto* attribute = findSymbol("C_NPC.ATTRIBUTE");
  const auto missing = subtract(vm.get_int(hero,attribute,potion.maximum),vm.get_int(hero,attribute,potion.attribute));
  const auto use = [&](size_t size,bool mustFit) {
    const auto amount = findSymbol(potion.amounts[size])->get_int();
    const auto item = int32_t(findSymbolIndex(potion.items[size]));
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
  Gothic::inst().onPrint(findSymbol(missing!=0 ? potion.emptyMessage : potion.fullMessage)->get_string());
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
    auto* item = vm.find_symbol_by_name(potion.items[i]);
    auto* amount = vm.find_symbol_by_name(potion.amounts[i]);
    if(item==nullptr || amount==nullptr)
      throw std::runtime_error("Missing potion hotkey item or amount symbol");
    candidates[i] = {item->index(),amount->get_int(),pl.itemCount(item->index())>0};
    }
  const auto missing = pl.attribute(potion.maximum) - pl.attribute(potion.attribute);
  // PLAYER_HOTKEY_LAME_* uses the strongest dose that fits the missing amount,
  // otherwise the weakest available dose. Its no-item/full-gauge messages still run below.
  const auto selected = Healing::choosePotion(candidates,missing);
  if(selected==Healing::NoPotion) {
    note("none");
    return false;
    }
  note(vm.find_symbol_by_index(uint32_t(selected))->name());
  if(pl.weaponState()!=WeaponState::NoWeapon && pl.itemUseInHandAnimation(*pl.getItem(selected)).empty()) {
    if(Bench::isActive())
      Bench::note(("item use "+std::string(Bench::who(&pl))+": "+
                      vm.find_symbol_by_index(uint32_t(selected))->name()+" holster path: no compatible in-hand clip or deferred effect").c_str());
    pl.stopAnim("");
    pl.aiPush(AiQueue::aiRemoveWeapon());
    }
  pl.aiPush(AiQueue::aiUseItem(int32_t(selected)));
  return true;
  }
