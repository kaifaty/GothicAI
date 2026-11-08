#include "npc.h"

#include <algorithm>

#include "game/gamescript.h"
#include "game/herodeed.h"
#include "graphics/mesh/skeleton.h"
#include "gothic.h"
#include "testbench.h"
#include "utils/string_frm.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/world.h"

using namespace Tempest;

Item* Npc::addItem(const size_t item, size_t count) {
  return invent.addItem(item,count,owner);
  }

Item* Npc::addItem(std::unique_ptr<Item>&& i) {
  return invent.addItem(std::move(i));
  }

Item* Npc::takeItem(Item& item) {
  const bool npcTheft = !isPlayer() && isServerDeed("take_item");
  if(npcTheft && (serverDeedSimulationError()!=nullptr || item.isMission()))
    return nullptr;
  if(npcTheft) {
    const auto current = HeroDeed::capture(*this,"take_item",nullptr,&item);
    const auto& actualOwner = current->owner.npc.empty() ? current->roomOwner.npc : current->owner.npc;
    if(actualOwner!=serverDeedOwner)
      return nullptr;
    }
  if(interactive()!=nullptr)
    return nullptr;
  if(item.isTorchBurn() && (isUsingTorch() || weaponState()!=WeaponState::NoWeapon))
    return nullptr;

  auto state = bodyStateMasked();
  if(state!=BS_STAND && state!=BS_SNEAK && state!=BS_SWIM && state!=BS_DIVE) {
    return nullptr;
    }

  const auto  dpos = item.midPosition()-centerPosition();
  const auto* sq   = setAnimAngGet(Npc::Anim::ItmGet, Pose::calcAniCombVert(dpos));
  if(sq==nullptr)
    return nullptr;

  if(npcTheft && serverDeedCount>0 && serverDeedCount<item.count()) {
    auto deed = HeroDeed::capture(*this,"take_item",nullptr,&item,nullptr,serverDeedCount);
    deed->collectWitnesses(*this,nullptr);
    auto* taken = addItem(item.clsId(),serverDeedCount);
    if(taken==nullptr)
      return nullptr;
    item.setCount(item.count()-serverDeedCount);
    Gothic::inst().onHeroDeed(*deed);
    implAniWait(uint64_t(sq->totalTime()));
    return taken;
    }
  std::unique_ptr<Item> ptr = owner.takeItem(item);
  if(ptr!=nullptr && ptr->isTorchBurn()) {
   if(!toggleTorch())
     return nullptr;
    size_t torchId = owner.script().findSymbolIndex("ItLsTorch");
    if(torchId!=size_t(-1))
      return nullptr;
    ptr.reset(new Item(owner,torchId,Item::T_Inventory));
    }

  auto it = ptr.get();
  if(it==nullptr)
    return nullptr;

  auto deed = HeroDeed::capture(*this,"take_item",nullptr,it);
  if(deed!=nullptr && !isPlayer())
    deed->collectWitnesses(*this,nullptr);
  it = addItem(std::move(ptr));
  if(isPlayer() && it!=nullptr)
    owner.sendPassivePerc(*this,*this,*it,PERC_ASSESSTHEFT,std::move(deed));
  else if(it!=nullptr && deed!=nullptr)
    Gothic::inst().onHeroDeed(*deed);

  implAniWait(uint64_t(sq->totalTime()));
  return it;
  }

void Npc::onWldItemRemoved(const Item& itm) {
  if(simpleMovement.nativeItem==&itm)
    simpleMovement.nativeItem=nullptr;
  aiQueue.onWldItemRemoved(itm);
  aiQueueOverlay.onWldItemRemoved(itm);
  }

void Npc::addItem(size_t id, Interactive &chest, size_t count) {
  auto* item = chest.inventory().getItem(id);
  if(!isPlayer() && isServerDeed("take_item") &&
     (serverDeedSimulationError()!=nullptr || (item!=nullptr && item->isMission())))
    return;
  auto deed = HeroDeed::capture(*this,"take_item",nullptr,item,&chest,
                                std::min(count,chest.inventory().itemCount(id)));
  if(deed!=nullptr && !isPlayer())
    deed->collectWitnesses(*this,nullptr);
  const auto before = invent.itemCount(id);
  Inventory::transfer(invent,chest.inventory(),nullptr,id,count,owner);
  if(deed!=nullptr && invent.itemCount(id)>before) {
    deed->count = invent.itemCount(id)-before;
    if(isPlayer())
      if(auto use = chest.heroUseDeed())
        deed->witnesses = use->witnesses;
    Gothic::inst().onHeroDeed(*deed);
    }
  }

void Npc::addItem(size_t id, Npc &from, size_t count) {
  Inventory::transfer(invent,from.invent,&from,id,count,owner);
  }

void Npc::moveItem(size_t id, Interactive &to, size_t count) {
  Inventory::transfer(to.inventory(),invent,this,id,count,owner);
  }

void Npc::sellItem(size_t id, Npc &to, size_t count) {
  if(id==owner.script().goldId()->index())
    return;
  int32_t price = invent.sellPriceOf(id);
  Inventory::transfer(to.invent,invent,this,id,count,owner);
  invent.addItem(owner.script().goldId()->index(),size_t(price)*count,owner);
  }

void Npc::buyItem(size_t id, Npc &from, size_t count) {
  if(id==owner.script().goldId()->index())
    return;

  int32_t price = from.invent.priceOf(id);
  if(price>0 && size_t(price)*count>invent.goldCount()) {
    count = invent.goldCount()/size_t(price);
    }
  if(count==0) {
    owner.script().printCannotBuyError(*this);
    return;
    }

  Inventory::transfer(invent,from.invent,nullptr,id,count,owner);
  if(price>=0)
    invent.delItem(owner.script().goldId()->index(),size_t( price)*count,*this); else
    invent.addItem(owner.script().goldId()->index(),size_t(-price)*count,owner);
  }

void Npc::dropItem(size_t id, size_t count) {
  syncDeferredAnimation();
  if(id==size_t(-1))
    return;
  size_t cnt = invent.itemCount(id);
  if(count>cnt)
    count = cnt;
  if(count<1)
    return;

  auto sk = visual.visualSkeleton();
  if(sk==nullptr)
    return;

  size_t rightHand = sk->findNode("ZS_RIGHTHAND");
  if(rightHand==size_t(-1))
    return;

  if(!setAnim(Anim::ItmDrop))
    return;

  auto mat = visual.transform();
  if(rightHand<visual.pose().boneCount())
    mat = visual.pose().bone(rightHand);

  auto it = owner.addItemDyn(id,mat,hnpc->symbol_index());
  it->setCount(count);
  invent.delItem(id,count,*this);
  }

void Npc::clearInventory() {
  invent.clear(owner.script(),*this);
  }

Item* Npc::currentArmor() {
  return invent.currentArmor();
  }

Item* Npc::currentMeleeWeapon() {
  return invent.currentMeleeWeapon();
  }

Item* Npc::currentRangedWeapon() {
  return invent.currentRangedWeapon();
  }

Item* Npc::currentShield() {
  return invent.currentShield();
  }

size_t Npc::itemCount(size_t id) const {
  return invent.itemCount(id);
  }

Item* Npc::activeWeapon() {
  return invent.activeWeapon();
  }

Item *Npc::getItem(size_t id) {
  return invent.getItem(id);
  }

void Npc::delItem(size_t item, uint32_t amount) {
  invent.delItem(item,amount,*this);
  }

void Npc::useItem(size_t item) {
  useItem(item,Item::NSLOT,false);
  }

void Npc::useItem(size_t item, uint8_t slotHint, bool force) {
  invent.use(item,*this,slotHint,force);
  }

bool Npc::hasItemUseAnimation() const {
  const auto& pose = visual.pose();
  for(size_t i=0;i<pose.logicalLayerCount();++i)
    if(pose.logicalLayer(i).state==BS_ITEMINTERACT)
      return true;
  return false;
  }

bool Npc::isUsingItemInHand() const {
  return visual.pose().isUsingItemInHand();
  }

std::string Npc::itemUseInHandAnimation(const Item& item) const {
  const auto& flags = Gothic::physicsFlags();
  const auto main = ItmFlags(item.handle().main_flag);
  const bool deferred = ((main & ITM_CAT_POTION) && flags.meleeHealInterrupt) ||
                        ((main & ITM_CAT_FOOD) && flags.foodOverTime);
  if(!flags.meleeUseInHand || !isHuman() || !deferred)
    return {};
  const auto& scheme = item.handle().scheme_name;
  const auto weapon = weaponState();
  const char* stance = weapon==WeaponState::Fist ? "FIST" : weapon==WeaponState::W1H ? "1H" :
                       weapon==WeaponState::W2H ? "2H" : nullptr;
  const char* use = scheme=="POTIONFAST" ? "DRINK" : scheme=="FOOD" ? "EATAPPLE" :
                    scheme=="FOODHUGE" ? "EATBREAD" : scheme=="MEAT" ? "EATMEAT" : nullptr;
  if(stance==nullptr || use==nullptr)
    return {};
  string_frm clip("T_",stance,use);
  return visual.hasAnim(clip) ? std::string(clip.c_str()) : std::string();
  }

bool Npc::startItemUse(const Item& item, bool allowMissing) {
  if(hasItemUseAnimation())
    return false;
  const auto clip = itemUseInHandAnimation(item);
  if(!clip.empty()) {
    const auto bs = bodyStateMasked();
    if(interactive()!=nullptr || (bs!=BS_STAND && bs!=BS_NONE && bs!=BS_WALK && bs!=BS_RUN))
      return false;
    const auto* sq = visual.startAnimAndGet(*this,clip,0,BS_ITEMINTERACT);
    if(sq==nullptr)
      return false;
    if(!isPlayer())
      implAniWait(uint64_t(sq->totalTime()));
    if(TestBench::isActive()) {
      const auto* symbol = owner.script().findSymbol(item.clsId());
      TestBench::note(("item use "+std::string(TestBench::who(this))+": "+
                      (symbol==nullptr ? item.handle().name : symbol->name())+" in hand "+sq->name+", layer 21").c_str());
      }
    return true;
    }
  if(TestBench::isActive()) {
    const auto& flags = Gothic::physicsFlags();
    const auto& scheme = item.handle().scheme_name;
    const auto main = ItmFlags(item.handle().main_flag);
    const auto weapon = weaponState();
    std::string reason = "missing clip";
    if(scheme!="POTIONFAST" && scheme!="FOOD" && scheme!="FOODHUGE" && scheme!="MEAT")
      reason = "scheme "+scheme;
    else if(!flags.meleeUseInHand)
      reason = "melee_use_in_hand=0";
    else if(!isHuman())
      reason = "not human";
    else if(((main & ITM_CAT_POTION) && !flags.meleeHealInterrupt) ||
            ((main & ITM_CAT_FOOD) && !flags.foodOverTime))
      reason = "category effect is immediate";
    else if(weapon!=WeaponState::Fist && weapon!=WeaponState::W1H && weapon!=WeaponState::W2H)
      reason = weapon==WeaponState::Bow ? "bow" : weapon==WeaponState::CBow ? "crossbow" :
               weapon==WeaponState::Mage ? "spell" : "no melee weapon";
    const auto* symbol = owner.script().findSymbol(item.clsId());
    TestBench::note(("item use "+std::string(TestBench::who(this))+": "+
                    (symbol==nullptr ? item.handle().name : symbol->name())+" holster path: "+reason).c_str());
    }
  return setAnimItem(item.handle().scheme_name,-1,allowMissing);
  }

void Npc::beginItemUseEffect(const Item& item) {
  const auto& flags = Gothic::physicsFlags();
  const auto main = ItmFlags(item.handle().main_flag);
  const auto category = (main & ITM_CAT_POTION) ? Healing::Category::Potion :
                        (main & ITM_CAT_FOOD) ? Healing::Category::Food : Healing::Category::Other;
  const bool enabled = (category==Healing::Category::Potion && flags.meleeHealInterrupt) ||
                       (category==Healing::Category::Food && flags.foodOverTime);
  if(!enabled) {
    if(item.handle().on_state[0]!=0)
      owner.script().invokeItem(this,uint32_t(item.handle().on_state[0]));
    return;
    }
  const auto moment = Healing::effectMoment(category,flags.meleeHealInterrupt,flags.foodOverTime,
                                           hasItemUseAnimation());
  itemUse = {item.clsId(),owner.tickCount(),category==Healing::Category::Food};
  if(moment==Healing::EffectMoment::Immediate)
    finishItemUseEffect("immediate");
  }

void Npc::finishItemUseEffect(const char* moment) {
  const auto use = itemUse;
  itemUse = {};
  invent.setCurrentItem(0);
  invent.clearSlot(*this,"",false);
  const auto* item = invent.getItem(use.item);
  if(item==nullptr) {
    clearSlotItem("");
    return;
    }
  const auto name = item->handle().name;
  const auto function = uint32_t(item->handle().on_state[0]);
  tickFoodPool();
  const auto hp = attribute(ATR_HITPOINTS), mana = attribute(ATR_MANA);
  foodHp = foodMana = 0;
  collectingFood = use.food && Gothic::physicsFlags().foodOverTime;
  if(function!=0)
    owner.script().invokeItem(this,function);
  collectingFood = false;
  if(use.food && !isDown()) {
    Healing::add(foodPool,foodHp,foodMana);
    if(TestBench::isActive()) {
      TestBench::note(("food pool "+std::string(TestBench::who(this))+": +"+std::to_string(foodHp)+
                      " hp +"+std::to_string(foodMana)+" mana over "+std::to_string(Healing::FoodDuration)+
                      " ms, pool "+std::to_string(foodPool.hp)+"/"+std::to_string(foodPool.mana)).c_str());
      }
    }
  if(TestBench::isActive()) {
    const auto* symbol = owner.script().findSymbol(use.item);
    TestBench::note(("item use "+std::string(TestBench::who(this))+": "+
                    (symbol==nullptr ? name : symbol->name())+" effect at "+moment+", +"+
                    std::to_string(use.food ? foodHp : attribute(ATR_HITPOINTS)-hp)+" hp +"+
                    std::to_string(use.food ? foodMana : attribute(ATR_MANA)-mana)+" mana at "+
                    std::to_string(owner.tickCount()-use.started)+" ms").c_str());
    }
  delItem(use.item,1);
  }

void Npc::interruptItemUse(std::string_view cause) {
  const bool inHand = isUsingItemInHand();
  if(itemUse.item==0 && !inHand)
    return;
  if(itemUse.item!=0 && TestBench::isActive()) {
    const auto* symbol = owner.script().findSymbol(itemUse.item);
    TestBench::note(("item use "+std::string(TestBench::who(this))+": "+
                    (symbol==nullptr ? std::to_string(itemUse.item) : symbol->name())+
                    (inHand ? " cancelled by " : " interrupted by ")+
                    std::string(cause)+" at "+std::to_string(owner.tickCount()-itemUse.started)+" ms, item kept").c_str());
    }
  itemUse = {};
  if(hasItemUseAnimation()) {
    visual.pose().interruptItemUse();
    aniWaitTime = 0;
    }
  invent.setCurrentItem(0);
  invent.clearSlot(*this,"",false);
  }

void Npc::tickFoodPool() {
  const auto now = owner.tickCount();
  const auto dt = now>=foodPoolTick ? now-foodPoolTick : 0;
  foodPoolTick = now;
  if(!Gothic::physicsFlags().foodOverTime || isDown()) {
    Healing::clear(foodPool);
    return;
    }
  const auto dose = Healing::advance(foodPool,dt,attribute(ATR_HITPOINTSMAX)-attribute(ATR_HITPOINTS),
                                               attribute(ATR_MANAMAX)-attribute(ATR_MANA));
  changeAttribute(ATR_HITPOINTS,dose.hp,false);
  changeAttribute(ATR_MANA,dose.mana,false);
  }

void Npc::setCurrentItem(size_t item) {
  invent.setCurrentItem(item);
  }

void Npc::unequipItem(size_t item) {
  invent.unequip(item,*this);
  }
