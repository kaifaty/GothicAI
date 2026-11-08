#include "gamescript.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <limits>

#include "utils/string_frm.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "gothic.h"

using namespace Tempest;

void GameScript::npc_settofightmode(std::shared_ptr<daedalus::INpc> npcRef, int weaponSymbol) {
  if(npcRef!=nullptr && weaponSymbol>=0)
    findNpc(npcRef.get())->setToFightMode(size_t(weaponSymbol));
  }

void GameScript::npc_settofistmode(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setToFistMode();
  }

bool GameScript::npc_isinstate(std::shared_ptr<daedalus::INpc> npcRef, int stateFn) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    return npc->isInState(uint32_t(stateFn));
  return false;
  }

bool GameScript::npc_isinroutine(std::shared_ptr<daedalus::INpc> npcRef, int stateFn) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    return npc->isInRoutine(uint32_t(stateFn));
  return false;
  }

bool GameScript::npc_wasinstate(std::shared_ptr<daedalus::INpc> npcRef, int stateFn) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    return npc->wasInState(uint32_t(stateFn));
  return false;
  }

int GameScript::npc_getdisttowp(std::shared_ptr<daedalus::INpc> npcRef, std::string_view wpname) {
  auto  npc = findNpc(npcRef);
  //NOTE: in CoM, some way-point and free-points share same name - need to be precise
  auto* wp  = world().findWayPoint(wpname);

  if(npc!=nullptr && wp!=nullptr){
    float ret = std::sqrt(npc->qDistTo(wp));
    if(ret<float(std::numeric_limits<int32_t>::max()))
      return int32_t(ret); else
      return std::numeric_limits<int32_t>::max();
    } else {
    return std::numeric_limits<int32_t>::max();
    }
  }

void GameScript::npc_exchangeroutine(std::shared_ptr<daedalus::INpc> npcRef, std::string_view rname) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr) {
    auto& v = npc->handle();
    string_frm name("Rtn_",rname,'_',v.id);

    auto* sym = vm.find_symbol_by_name(name);
    size_t d = sym != nullptr ? sym->index() : 0;
    if(d>0)
      npc->excRoutine(d);
    }
  }

bool GameScript::npc_isdead(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc==nullptr || isDead(*npc);
  }

bool GameScript::npc_knowsinfo(std::shared_ptr<daedalus::INpc> npcRef, int infoinstance) {
  auto npc = findNpc(npcRef);
  if(!npc)
    return false;

  daedalus::INpc& vnpc = npc->handle();
  return doesNpcKnowInfo(vnpc, uint32_t(infoinstance));
  }

void GameScript::npc_settalentskill(std::shared_ptr<daedalus::INpc> npcRef, int t, int lvl) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setTalentSkill(Talent(t),lvl);
  }

int GameScript::npc_gettalentskill(std::shared_ptr<daedalus::INpc> npcRef, int skillId) {
  auto npc = findNpc(npcRef);
  return npc==nullptr ? 0 : npc->talentSkill(Talent(skillId));
  }

void GameScript::npc_settalentvalue(std::shared_ptr<daedalus::INpc> npcRef, int t, int lvl) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setTalentValue(Talent(t),lvl);
  }

int GameScript::npc_gettalentvalue(std::shared_ptr<daedalus::INpc> npcRef, int skillId) {
  auto npc = findNpc(npcRef);
  return npc==nullptr ? 0 : npc->talentValue(Talent(skillId));
  }

void GameScript::npc_setrefusetalk(std::shared_ptr<daedalus::INpc> npcRef, int timeSec) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setRefuseTalk(uint64_t(std::max(timeSec*1000,0)));
  }

bool GameScript::npc_refusetalk(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc && npc->isRefuseTalk();
  }

int GameScript::npc_hasitems(std::shared_ptr<daedalus::INpc> npcRef, int itemId) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr ? int(npc->itemCount(uint32_t(itemId))) : 0;
  }

bool GameScript::npc_hasspell(std::shared_ptr<daedalus::INpc> npcRef, int splId) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->inventory().hasSpell(splId);
  }

int GameScript::npc_getinvitem(std::shared_ptr<daedalus::INpc> npcRef, int itemId) {
  auto npc = findNpc(npcRef);
  auto itm = npc==nullptr ? nullptr : npc->getItem(uint32_t(itemId));
  storeItem(itm);
  if(itm!=nullptr) {
    return int(itm->handle().symbol_index());
    }
  return -1;
  }

// This seems specific to Gothic 1, where the inventory was grouped into categories.
// In the shared inventory, these categories are just following one after the other.
// So we should transform this to iterate over the different categories in the
// shared inventory
int GameScript::npc_getinvitembyslot(std::shared_ptr<daedalus::INpc> npcRef, int cat, int slotnr) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    storeItem(nullptr);
    return 0;
    }

  // The category flag names were global, but for the scripts, only npc_getinvitembyslot
  // ever used them, so as long as nobody implements the Gothic 1 inventory, they can
  // be limited to the comments below.
  ItmFlags f = ITM_CAT_NONE;
  switch(cat) {
    case 1: // INV_WEAPON
      f = ItmFlags(ITM_CAT_NF|ITM_CAT_FF|ITM_CAT_MUN);
      break;
    case 2: // INV_ARMOR
      f = ITM_CAT_ARMOR;
      break;
    case 3: // INV_RUNE
      f = ITM_CAT_RUNE;
      break;
    case 4: // INV_MAGIC
      f = ITM_CAT_MAGIC;
      break;
    case 5: // INV_FOOD
      f = ITM_CAT_FOOD;
      break;
    case 6: // INV_POTION
      f = ITM_CAT_POTION;
      break;
    case 7: // INV_DOC
      f = ITM_CAT_DOCS;
      break;
    case 8: // INV_MISC
      f = ItmFlags(ITM_CAT_LIGHT|ITM_CAT_NONE);
      break;
    default:
      Log::e("Unknown item category ", cat);
      storeItem(nullptr);
      return 0;
    }

  auto itm = npc==nullptr ? nullptr : npc->inventory().findByFlags(f, uint32_t(slotnr));
  // Store the found item in the global item var
  storeItem(itm);

  return itm!=nullptr ? int(itm->count()) : 0;
  }

int GameScript::npc_removeinvitem(std::shared_ptr<daedalus::INpc> npcRef, int itemId) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->delItem(uint32_t(itemId),1);
  return 0;
  }

int GameScript::npc_removeinvitems(std::shared_ptr<daedalus::INpc> npcRef, int itemId, int amount) {
  auto npc = findNpc(npcRef);

  if(npc!=nullptr && amount>0)
    npc->delItem(uint32_t(itemId),uint32_t(amount));

  return 0;
  }

int GameScript::npc_getbodystate(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);

  if(npc!=nullptr)
    return int32_t(npc->bodyState());
  return int32_t(0);
  }

std::shared_ptr<daedalus::INpc> GameScript::npc_getlookattarget(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc && npc->lookAtTarget() ? npc->lookAtTarget()->handlePtr() : nullptr;
  }

int GameScript::npc_getdisttonpc(std::shared_ptr<daedalus::INpc> aRef, std::shared_ptr<daedalus::INpc> bRef) {
  auto a = findNpc(aRef);
  auto b = findNpc(bRef);

  if(a==nullptr || b==nullptr)
    return std::numeric_limits<int32_t>::max();

  float ret = std::sqrt(a->qDistTo(*b));
  if(ret>float(std::numeric_limits<int32_t>::max()))
    return std::numeric_limits<int32_t>::max();
  return int(ret);
  }

bool GameScript::npc_hasequippedarmor(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->currentArmor()!=nullptr;
  }

void GameScript::npc_setperctime(std::shared_ptr<daedalus::INpc> npcRef, float sec) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setPerceptionTime(uint64_t(sec*1000));
  }

void GameScript::npc_percenable(std::shared_ptr<daedalus::INpc> npcRef, int pr, int fn) {
  auto npc = findNpc(npcRef);
  if(npc && fn>=0)
    npc->setPerceptionEnable(PercType(pr),size_t(fn));
  }

void GameScript::npc_percdisable(std::shared_ptr<daedalus::INpc> npcRef, int pr) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setPerceptionDisable(PercType(pr));
  }

std::string GameScript::npc_getnearestwp(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto wp  = npc ? world().findWayPoint(npc->position()) : nullptr;
  if(wp)
    return wp->name;
  return "";
  }

std::string GameScript::npc_getnextwp(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto wp  = npc ? world().findNextWayPoint(*npc) : nullptr;
  if(wp)
    return wp->name;
  return "";
  }

void GameScript::npc_clearaiqueue(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->clearAiQueue();
  }

bool GameScript::npc_isplayer(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc && npc->isPlayer();
  }

int GameScript::npc_getstatetime(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc)
    return int32_t(npc->stateTime()/1000);
  return 0;
  }

void GameScript::npc_setstatetime(std::shared_ptr<daedalus::INpc> npcRef, int val) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setStateTime(val*1000);
  }

void GameScript::npc_changeattribute(std::shared_ptr<daedalus::INpc> npcRef, int atr, int val) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && atr>=0)
    npc->changeAttribute(Attribute(atr),val,false);
  }

bool GameScript::npc_isonfp(std::shared_ptr<daedalus::INpc> npcRef, std::string_view val) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;

  auto w = npc->currentWayPoint();
  if(w==nullptr || !MoveAlgo::isClose(*npc,*w,MAX_AI_USE_DISTANCE) || !w->checkName(val))
    return false;
  return w->isFreePoint();
  }

int GameScript::npc_getheighttonpc(std::shared_ptr<daedalus::INpc> aRef, std::shared_ptr<daedalus::INpc> bRef) {
  auto a = findNpc(aRef);
  auto b = findNpc(bRef);
  float ret = 0;
  if(a!=nullptr && b!=nullptr)
    ret = std::abs(a->position().y - b->position().y);
  return int32_t(ret);
  }

std::shared_ptr<daedalus::IItem> GameScript::npc_getequippedmeleeweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && npc->currentMeleeWeapon() != nullptr) {
    return npc->currentMeleeWeapon()->handlePtr();
    }
  return nullptr;
  }

std::shared_ptr<daedalus::IItem> GameScript::npc_getequippedrangedweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && npc->currentRangedWeapon() != nullptr) {
    return npc->currentRangedWeapon()->handlePtr();
    }
  return nullptr;
  }

std::shared_ptr<daedalus::IItem> GameScript::npc_getequippedarmor(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && npc->currentArmor()!=nullptr) {
    return npc->currentArmor()->handlePtr();
    }
  return nullptr;
  }

bool GameScript::npc_canseenpc(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef) {
  // 'see' functions are intended as ray-cast, ignoring hnpc->senses mask
  // https://discord.com/channels/989316194148433950/989333514543587339/1226664463697182760
  auto other = findNpc(otherRef);
  auto npc   = findNpc(npcRef);

  if(npc!=nullptr && other!=nullptr){
    return npc->canSeeNpc(*other,false);
    }
  return false;
  }

bool GameScript::npc_canseenpcfreelos(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef) {
  auto npc = findNpc(npcRef);
  auto oth = findNpc(otherRef);

  if(npc!=nullptr && oth!=nullptr){
    return npc->canSeeNpc(*oth,true);
    }
  return false;
  }

bool GameScript::npc_canseeitem(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itemRef) {
  auto npc = findNpc(npcRef);
  auto itm = findItem(itemRef.get());

  if(npc!=nullptr && itm!=nullptr){
    return npc->canSeeItem(*itm,false);
    }
  return false;
  }

bool GameScript::npc_hasequippedweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return (npc!=nullptr &&
     (npc->currentMeleeWeapon()!=nullptr ||
      npc->currentRangedWeapon()!=nullptr));
  }

bool GameScript::npc_hasequippedmeleeweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->currentMeleeWeapon()!=nullptr;
  }

bool GameScript::npc_hasequippedrangedweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->currentRangedWeapon()!=nullptr;
  }

int GameScript::npc_getactivespell(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return -1;

  Item* w = npc->activeWeapon();
  if(w==nullptr || !w->isSpellOrRune())
    return -1;

  makeCurrent(w);
  return w->spellId();
  }

bool GameScript::npc_getactivespellisscroll(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;

  Item* w = npc->activeWeapon();
  if(w==nullptr || !w->isSpell())
    return false;

  return true;
  }

bool GameScript::npc_isinfightmode(std::shared_ptr<daedalus::INpc> npcRef, int modeI) {
  auto npc  = findNpc(npcRef);
  auto mode = FightMode(modeI);

  if(npc==nullptr){
    return false;
    }

  auto st  = npc->weaponState();
  bool ret = false;
  if(mode==FightMode::FMODE_NONE){
    ret = (st==WeaponState::NoWeapon);
    }
  else if(mode==FightMode::FMODE_FIST){
    ret = (st==WeaponState::Fist);
    }
  else if(mode==FightMode::FMODE_MELEE){
    ret = (st==WeaponState::W1H || st==WeaponState::W2H);
    }
  else if(mode==FightMode::FMODE_FAR){
    ret = (st==WeaponState::Bow || st==WeaponState::CBow);
    }
  else if(mode==FightMode::FMODE_MAGIC){
    ret = (st==WeaponState::Mage);
    }
  return ret;
  }

void GameScript::npc_settarget(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef) {
  auto oth = findNpc(otherRef);
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setTarget(oth);
  }

/**
 * @brief WorldScript::npc_gettarget
 * Fill 'other' with the current npc target.
 * set by Npc_SetTarget () or Npc_GetNextTarget ().
 * - return: current target saved -> TRUE
 * no target saved -> FALSE
 */
bool GameScript::npc_gettarget(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto s   = vm.global_other();

  if(npc!=nullptr && npc->target()) {
    s->set_instance(npc->target()->handlePtr());
    return true;
    }

  s->set_instance(nullptr);
  return false;
  }

bool GameScript::npc_getnexttarget(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  Npc* ret = nullptr;

  if(npc!=nullptr){
    float dist = float(npc->handle().senses_range);
    dist*=dist;

    world().detectNpc(npc->position(),float(npc->handle().senses_range),[&,npc](Npc& oth){
      if(&oth!=npc && !oth.isDown() && oth.isEnemy(*npc) && npc->canSenseNpc(oth,true)!=SensesBit::SENSE_NONE){
        float qd = oth.qDistTo(*npc);
        if(qd<dist){
          dist=qd;
          ret = &oth;
          }
        }
      return false;
      });
    if(ret!=nullptr)
      npc->setTarget(ret);
    }

  auto s = vm.global_other();
  if(ret!=nullptr) {
    s->set_instance(ret->handlePtr());
    return true;
    } else {
    s->set_instance(nullptr);
    return false;
    }
  }

void GameScript::npc_sendpassiveperc(std::shared_ptr<daedalus::INpc> npcRef, int id, std::shared_ptr<daedalus::INpc> victimRef, std::shared_ptr<daedalus::INpc> otherRef) {
  auto npc    = findNpc(npcRef);
  auto other  = findNpc(otherRef);
  auto victim = findNpc(victimRef);

  if(npc && other && victim)
    world().sendPassivePerc(*npc,*other,*victim,id);
  else if(npc && other)
    world().sendPassivePerc(*npc,*other,id);
  }

void GameScript::npc_sendsingleperc(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::INpc> otherRef, int id) {
  auto other  = findNpc(otherRef);
  auto npc    = findNpc(npcRef);

  if(npc && other)
    other->perceptionProcess(*npc,nullptr,0,PercType(id));
  }

bool GameScript::npc_checkinfo(std::shared_ptr<daedalus::INpc> npcRef, int imp) {
  auto n    = findNpc(npcRef);
  auto hero = findNpc(vm.global_other());
  if(n==nullptr || hero==nullptr)
    return false;

  auto& pl  = hero->handle();
  auto& npc = n->handle();
  for(auto& info:dialogsInfo) {
    if(info->npc!=int32_t(npc.symbol_index()) || info->important!=imp)
      continue;
    bool npcKnowsInfo = doesNpcKnowInfo(pl,info->symbol_index());
    if(npcKnowsInfo && !info->permanent)
      continue;
    bool valid=false;
    if(info->condition) {
      auto* conditionSymbol = vm.find_symbol_by_index(uint32_t(info->condition));
      if (conditionSymbol != nullptr)
        valid = dialogFunction(conditionSymbol,true)!=0;
      }
    if(Gothic::inst().dialogFilterFn)
      Gothic::inst().dialogFilterFn(*info,valid);
    if(valid) {
      return true;
      }
    }
  if(Gothic::inst().dialogExtraFn) {
    std::vector<DlgChoice> extra;
    Gothic::inst().dialogExtraFn(npc,imp!=0,extra);
    return !extra.empty();
    }
  return false;
  }

int GameScript::npc_getportalguild(std::shared_ptr<daedalus::INpc> npcRef) {
  int32_t g   = GIL_NONE;
  auto    npc = findNpc(npcRef);
  if(npc!=nullptr)
    g = world().guildOfRoom(npc->portalName());
  return g;
  }

bool GameScript::npc_isinplayersroom(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto pl  = world().player();

  if(npc!=nullptr && pl!=nullptr) {
    auto g1 = pl ->portalName();
    auto g2 = npc->portalName();
    if(g1==g2)
      return true;
    }
  return false;
  }

std::shared_ptr<daedalus::IItem> GameScript::npc_getreadiedweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return 0;
    }

  auto ret = npc->activeWeapon();
  if(ret!=nullptr) {
    makeCurrent(ret);
    return ret->handlePtr();
    } else {
    return nullptr;
    }
  }

bool GameScript::npc_hasreadiedweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;
  auto ws = npc->weaponState();
  return (ws==WeaponState::W1H || ws==WeaponState::W2H ||
          ws==WeaponState::Bow || ws==WeaponState::CBow);
  }

bool GameScript::npc_hasreadiedmeleeweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return false;
    }
  auto ws = npc->weaponState();
  return ws==WeaponState::W1H || ws==WeaponState::W2H;
  }

bool GameScript::npc_hasreadiedrangedweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;
  auto ws = npc->weaponState();
  return ws==WeaponState::Bow || ws==WeaponState::CBow;
  }

bool GameScript::npc_hasrangedweaponwithammo(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->inventory().hasRangedWeaponWithAmmo();
  }

int GameScript::npc_isdrawingspell(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return 0;

  auto ret = npc->activeWeapon();
  if(ret==nullptr || !ret->isSpell())
    return 0;

  makeCurrent(ret);
  return int32_t(ret->clsId());
  }

int GameScript::npc_isdrawingweapon(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return 0;

  auto ret = npc->activeWeapon();
  if(ret==nullptr || !ret->isSpell())
    return 0;

  makeCurrent(ret);
  return int32_t(ret->clsId());
  }

void GameScript::npc_perceiveall(std::shared_ptr<daedalus::INpc> npcRef) {
  (void)npcRef; // nop
  }

void GameScript::npc_stopani(std::shared_ptr<daedalus::INpc> npcRef, std::string_view name) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->stopAnim(name);
  }

int GameScript::npc_settrueguild(std::shared_ptr<daedalus::INpc> npcRef, int gil) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setTrueGuild(gil);
  return 0;
  }

int GameScript::npc_gettrueguild(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    return npc->trueGuild();
  return int32_t(GIL_NONE);
  }

void GameScript::npc_clearinventory(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->clearInventory();
  }

int GameScript::npc_getattitude(std::shared_ptr<daedalus::INpc> aRef, std::shared_ptr<daedalus::INpc> bRef) {
  auto a = findNpc(aRef);
  auto b = findNpc(bRef);

  if(a!=nullptr && b!=nullptr){
    auto att=personAttitude(*a,*b);
    return att; //TODO: temp attitudes
    }
  return ATT_NEUTRAL;
  }

int GameScript::npc_getpermattitude(std::shared_ptr<daedalus::INpc> aRef, std::shared_ptr<daedalus::INpc> bRef) {
  auto a = findNpc(aRef);
  auto b = findNpc(bRef);

  if(a!=nullptr && b!=nullptr){
    auto att=personAttitude(*a,*b);
    return att;
    }
  return ATT_NEUTRAL;
  }

void GameScript::npc_setattitude(std::shared_ptr<daedalus::INpc> npcRef, int att) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setAttitude(Attitude(att));
  }

void GameScript::npc_settempattitude(std::shared_ptr<daedalus::INpc> npcRef, int att) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setTempAttitude(Attitude(att));
  }

bool GameScript::npc_hasbodyflag(std::shared_ptr<daedalus::INpc> npcRef, int bodyflag) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;
  return npc->hasStateFlag(BodyState(bodyflag));
  }

int GameScript::npc_getlasthitspellid(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr){
    return 0;
    }
  return npc->lastHitSpellId();
  }

int GameScript::npc_getlasthitspellcat(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return SPELL_GOOD;

  const int id    = npc->lastHitSpellId();
  auto&     spell = spellDesc(id);
  return spell.spell_type;
  }

void GameScript::npc_playani(std::shared_ptr<daedalus::INpc> npcRef, std::string_view name) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->playAnimByName(name,BS_NONE);
  }

bool GameScript::npc_isdetectedmobownedbynpc(std::shared_ptr<daedalus::INpc> usrRef, std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto usr = findNpc(usrRef);

  if(npc!=nullptr && usr!=nullptr && usr->interactive()!=nullptr){
    auto* inst = vm.find_symbol_by_index(npc->instanceSymbol());
    auto  ow   = usr->interactive()->ownerName();
    return inst->name() == ow;
    }
  return false;
  }

bool GameScript::npc_isdetectedmobownedbyguild(std::shared_ptr<daedalus::INpc> npcRef, int guild) {
  static bool first=true;
  if(first){
    Log::e("not implemented call [npc_isdetectedmobownedbyguild]");
    first=false;
    }

  auto npc = findNpc(npcRef);
  (void)guild;

  if(npc!=nullptr && npc->detectedMob()!=nullptr) {
    auto  ow   = npc->detectedMob()->ownerName();
    (void)ow;
    //vm.setReturn(inst.name==ow ? 1 : 0);
    return false;
    }
  return false;
  }

std::string GameScript::npc_getdetectedmob(std::shared_ptr<daedalus::INpc> npcRef) {
  auto usr = findNpc(npcRef);
  if(usr!=nullptr && usr->detectedMob()!=nullptr){
    auto i = usr->detectedMob();
    return std::string(i->schemeName());
    }
  return "";
  }

bool GameScript::npc_ownedbynpc(std::shared_ptr<daedalus::IItem> itmRef, std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto itm = findItem(itmRef.get());
  if(itm==nullptr || npc==nullptr) {
    return false;
    }

  auto* sym = vm.find_symbol_by_index(uint32_t(itm->handle().owner));
  return sym != nullptr && npc->handlePtr()==sym->get_instance();
  }

bool GameScript::npc_canseesource(std::shared_ptr<daedalus::INpc> npcRef) {
  auto self = findNpc(npcRef);
  if(!self)
    return false;
  return self->canSeeSource();
  }

// Used (only?) in Gothic 1 in B_AssessEnemy, to prevent attacks during cutscenes.
bool GameScript::npc_isincutscene(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto w = Gothic::inst().world();
  if(w==nullptr)
    return false;

  if(npc!=nullptr && owner.isNpcInDialog(*npc))
    return true;

  return false;
  }

int GameScript::npc_getdisttoitem(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itmRef) {
  auto itm = findItem(itmRef.get());
  auto npc = findNpc(npcRef);
  if(itm==nullptr || npc==nullptr) {
    return std::numeric_limits<int32_t>::max();
    }
  auto dp = itm->position()-npc->position();
  return int32_t(dp.length());
  }

int GameScript::npc_getheighttoitem(std::shared_ptr<daedalus::INpc> npcRef, std::shared_ptr<daedalus::IItem> itmRef) {
  auto itm = findItem(itmRef.get());
  auto npc = findNpc(npcRef);
  if(itm==nullptr || npc==nullptr) {
    return std::numeric_limits<int32_t>::max();
    }
  auto dp = int32_t(itm->position().y-npc->position().y);
  return std::abs(dp);
  }

int GameScript::npc_getdisttoplayer(std::shared_ptr<daedalus::INpc> npcRef) {
  auto pl  = world().player();
  auto npc = findNpc(npcRef);
  if(pl==nullptr || npc==nullptr) {
    return std::numeric_limits<int32_t>::max();
    }
  auto dp = pl->position()-npc->position();
  auto l  = dp.length();
  if(l>float(std::numeric_limits<int32_t>::max())) {
    return std::numeric_limits<int32_t>::max();
    }
  return int32_t(l);
  }

int GameScript::npc_getactivespellcat(std::shared_ptr<daedalus::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return SPELL_GOOD;

  const Item* w = npc->activeWeapon();
  if(w==nullptr || !w->isSpellOrRune())
    return SPELL_GOOD;

  const int id    = w->spellId();
  auto&     spell = spellDesc(id);
  return spell.spell_type;
  }

int GameScript::npc_setactivespellinfo(std::shared_ptr<daedalus::INpc> npcRef, int v) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setActiveSpellInfo(v);
  return 0;
  }

int GameScript::npc_getactivespelllevel(std::shared_ptr<daedalus::INpc> npcRef) {
  int  v   = 0;
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    v = npc->activeSpellLevel();
  return v;
  }
