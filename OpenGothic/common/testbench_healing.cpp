#include "testbench_internal.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include "game/damagecalculator.h"
#include "game/gamescript.h"
#include "game/serialize.h"
#include "gothic.h"
#include "graphics/mesh/animation.h"
#include "graphics/mesh/hitzones.h"
#include "world/aiqueue.h"
#include "world/bullet.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

void healingObserve(Healing& state,World& world,Npc& player) {
  auto& test=state.test;
  if(test.label.empty())return;
  auto& actor=test.npc==nullptr ? player : *test.npc;
  const auto now=world.tickCount();
  if(test.npc!=nullptr && !test.queueRunAt && actor.walkMode()==WalkBit::WM_Run)
    test.queueRunAt=now;
  const bool hand=actor.isUsingItemInHand();
  test.sawHand|=hand;
  if(test.label=="wine")
    for(const auto& layer:actor.logicalBodyState().layers)
      if(layer.sequence!=nullptr && layer.sequence->name=="T_POTION_STAND_2_S0")test.nativeClip=layer.sequence->name;
  if(test.armed) {
    if(!test.handEnd)test.heldWeapon&=actor.weaponState()==test.weapon;
    if(hand) {
      if(!test.handStart)test.handStart=now;
      for(const auto& layer:actor.logicalBodyState().layers) {
        if(layer.sequence==nullptr)continue;
        const auto& seq=*layer.sequence;
        if(seq.name==test.clip) {
          test.handLayer|=seq.layer==21;
          std::vector<Animation::LogicEvent> events;
          seq.logicEvents(events);
          for(const auto& e:events) {
            test.leftInsert|=e.def==asset::MdsEventType::ITEM_INSERT && e.slot[0]=="ZS_LEFTHAND";
            if(e.def==asset::MdsEventType::ITEM_INSERT && e.slot[0]=="ZS_LEFTHAND")
              test.insertAt=layer.start+uint64_t(double(e.time)/layer.speed);
            test.sound|=e.kind==Animation::LogicEvent::Sound;
            if(e.def==asset::MdsEventType::ITEM_DESTROY)test.destroyAt=layer.start+uint64_t(double(e.time)/layer.speed);
            }
          }
        if(test.walking && seq.layer<21)
          test.walkOnly&=seq.name.find("RUNL")==std::string::npos && seq.name.find("RUNSTRAFE")==std::string::npos;
        }
      // Events are applied during the NPC tick; check complete frames inside the insert/destroy interval.
      if(test.insertAt && now>test.insertAt+34 && now<test.destroyAt) {
        const auto* item=actor.inventory().slotItem("ZS_LEFTHAND");
        const bool held=item!=nullptr && item->clsId()==test.item;
        test.leftHeld&=held;test.sawLeftItem|=held;
        }
      }
    else if(test.sawHand && !test.handEnd)test.handEnd=now;
    }
  if(test.walking && (test.baseline || hand || (test.sawHand && !test.handEnd))) {
    auto delta=actor.position()-test.previous;delta.y=0;
    test.distance+=delta.length();
    test.yawTravel+=std::abs(std::remainder(actor.rotation()-test.previousYaw,360.f));
    if(test.observedAt)test.movingMs+=now-test.observedAt;
    }
  if(test.action=="dodge")test.actionExecuted|=actor.isMeleeDodging();
  if(test.action=="attack")test.actionExecuted|=actor.isAttackAnim();
  test.previous=actor.position();test.previousYaw=actor.rotation();test.observedAt=now;
  const int hp=actor.attribute(ATR_HITPOINTS);
  if(hp>test.hp) {
    test.healed+=hp-test.hp;
    if(!test.firstHeal)test.firstHeal=world.tickCount();
    test.lastHeal=world.tickCount();
    }
  if(hp<test.hp)test.damage+=test.hp-hp;
  test.hp=hp;
  const auto count=actor.inventory().itemCount(test.item);
  if(count<test.lastItems) {
    test.lastConsume=world.tickCount();test.nextSecond=test.lastConsume;test.paused=0;++test.portions;
    }
  test.lastItems=count;
  test.sawDrink|=actor.hasState(BS_ITEMINTERACT);
  if(test.strikeDelay && actor.hasState(BS_ITEMINTERACT)) {
    test.strikeAt=now+test.strikeDelay;test.strikeDelay=0;
    say("healing native use reached; strike at "+std::to_string(test.strikeAt-test.start)+" ms after request");
    }
  if(test.lastConsume && world.tickCount()>=test.nextSecond) {
    const auto elapsed=world.tickCount()-test.lastConsume-test.paused;
    test.samples.emplace_back(elapsed,test.healed);
    say("healing sample "+test.label+": "+std::to_string(elapsed)+
      " ms after consume, hp "+std::to_string(hp)+", healed "+std::to_string(test.healed)+
      ", damage "+std::to_string(test.damage)+", items "+std::to_string(count));
    test.nextSecond+=1000;
    }
  }

void healingCommand(Healing& state,const std::vector<std::string>& a,World& world,Npc& player,PlayerControl& ctrl,
                    const std::vector<Npc*>& meleeShadowActors) {
  auto& test=state.test;
  if((a.size()==4 || a.size()==5) && a[1]=="begin") {
    exec("disarm",world,player,ctrl);
    const auto item=world.script().findSymbolIndex(a[3]);
    if(item==size_t(-1))throw std::runtime_error("healing fixture item is absent");
    // Start every case with ample room for healing and a real strike safely above one-third HP.
    player.handle().attribute[ATR_HITPOINTSMAX]=1000;
    const int hp=a.size()==5 ? std::stoi(a[4]) : 700;
    player.handle().attribute[ATR_HITPOINTS]=hp;
    player.handle().flags=daedalus::NpcFlag(uint32_t(player.handle().flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
    const auto count=player.inventory().itemCount(item);
    if(count)player.delItem(item,uint32_t(count));
    test={};test.label=a[2];test.item=item;test.hp=hp;test.start=world.tickCount();
    Gothic::settingsSetI("GAME","usePotionKeys",1);
    say("healing begin "+test.label+": hp "+std::to_string(hp)+"/1000 at "+std::to_string(test.start)+" ms, interrupt "+
      std::to_string(Gothic::physicsFlags().meleeHealInterrupt)+", food "+std::to_string(Gothic::physicsFlags().foodOverTime));
    }
  else if(a.size()==3 && a[1]=="arm") {
    exec("melee-shadow weapon "+a[2],world,player,ctrl);
    test.armed=true;test.weapon=player.weaponState();
    const auto* item=player.getItem(test.item);
    // begin removes this fixture item, so obtain its scheme from a temporary inventory instance.
    if(item==nullptr)player.addItem(test.item,1);
    const auto& scheme=player.getItem(test.item)->handle().scheme_name;
    const char* suffix=scheme=="POTIONFAST" ? "DRINK" : scheme=="FOOD" ? "EATAPPLE" :
                       scheme=="FOODHUGE" ? "EATBREAD" : scheme=="MEAT" ? "EATMEAT" : "";
    test.clip="T_"+(a[2]=="fist" ? std::string("FIST") : a[2]=="1h" ? std::string("1H") : std::string("2H"))+suffix;
    player.setWalkMode(WalkBit::WM_Run);
    }
  else if(a.size()==3 && a[1]=="use") {
    test.start=world.tickCount();test.observedAt=test.start;test.previous=player.position();
    test.lastItems=player.itemCount(test.item);test.heldWeapon=true;
    if(test.lastItems!=1)throw std::runtime_error("healing in-hand fixture requires exactly one item");
    if(a[2]=="direct")player.useItem(test.item);
    else if(a[2]=="ai")player.aiPush(AiQueue::aiUseItem(int32_t(test.item)));
    else if(a[2]=="hotkey") {test.hotkey=true;exec("release heal",world,player,ctrl);}
    else throw std::runtime_error("healing use expects direct, ai or hotkey");
    healingObserve(state,world,player);
    }
  else if(a.size()==2 && a[1]=="exhaust") {
    if(!player.hasStamina())throw std::runtime_error("healing exhaustion requires melee stamina");
    for(unsigned i=0;i<16 && !player.isStaminaExhausted();++i) {
      player.stopAnim("");
      if(!player.doAttack(Npc::Anim::Attack,BS_HIT))throw std::runtime_error("healing could not exhaust the hero");
      }
    player.stopAnim("");player.setAnim(Npc::Anim::Idle);
    if(player.stamina()!=0.f || !player.isStaminaExhausted())throw std::runtime_error("healing hero is not exhausted");
    say("healing exhausted: stamina "+std::to_string(player.stamina()));
    }
  else if(a.size()==3 && a[1]=="toggle") {
    test.checkWalk=true;test.expectedWalk=a[2]=="walk" ? WalkBit::WM_Walk : WalkBit::WM_Run;
    if(a[2]!="walk" && a[2]!="sneak")throw std::runtime_error("healing toggle expects walk or sneak");
    exec("tap "+a[2],world,player,ctrl);
    say("healing toggle "+a[2]+": walk mode "+std::to_string(int(player.walkMode())));
    }
  else if(a.size()==3 && a[1]=="original") {
    if(a[2]=="potion")Gothic::settingsSetI("GAME","melee_heal_interrupt",0);
    else if(a[2]=="food")Gothic::settingsSetI("GAME","food_over_time",0);
    else throw std::runtime_error("healing original expects potion or food");
    }
  else if(a.size()==2 && a[1]=="npc-queue") {
    if(meleeShadowActors.size()!=3)throw std::runtime_error("healing NPC queue requires the bandit fixture");
    auto& npc=*meleeShadowActors[2];
    npc.clearAiQueue();npc.clearState(true);npc.clearGoTo();npc.stopWalking();npc.closeWeapon(true);npc.stopAnim("");
    npc.setExternalControl(true);
    const auto sword=world.script().findSymbolIndex("ITMW_1H_COMMON_01");
    if(sword==size_t(-1))throw std::runtime_error("healing NPC sword is absent");
    npc.setToFightMode(sword);npc.setWalkMode(WalkBit::WM_Walk);npc.setAnim(Npc::Anim::Idle);
    npc.handle().attribute[ATR_HITPOINTSMAX]=1000;npc.handle().attribute[ATR_HITPOINTS]=700;
    const auto count=npc.itemCount(test.item);if(count)npc.delItem(test.item,uint32_t(count));
    npc.addItem(test.item,1);
    test.npc=&npc;test.armed=true;test.weapon=npc.weaponState();test.clip="T_1HDRINK";
    test.start=world.tickCount();test.hp=700;test.lastItems=1;test.heldWeapon=true;
    npc.aiPush(AiQueue::aiUseItem(int32_t(test.item)));
    npc.aiPush(AiQueue::aiSetWalkMode(WalkBit::WM_Run));
    healingObserve(state,world,player);
    }
  else if(a.size()==3 && (a[1]=="baseline" || a[1]=="move")) {
    if(a[2]!="forward" && a[2]!="side")throw std::runtime_error("healing movement expects forward or side");
    test.direction=a[2]=="forward" ? 0u : 1u;
    test.walking=true;test.baseline=a[1]=="baseline";
    test.distance=0;test.movingMs=0;test.previous=player.position();test.observedAt=world.tickCount();
    if(test.baseline)player.setWalkMode(WalkBit::WM_Walk);
    // The in-hand case keeps run selected; the item layer must force the ordinary combat walk.
    exec("press "+std::string(test.direction==0 ? "forward" : "right"),world,player,ctrl);
    }
  else if(a.size()==3 && a[1]=="motion") {
    if(a[2]!="back" && a[2]!="turn")throw std::runtime_error("healing motion expects back or turn");
    test.motion=a[2];test.walking=true;
    test.distance=0;test.yawTravel=0;test.previous=player.position();test.previousYaw=player.rotation();
    test.observedAt=world.tickCount();
    }
  else if(a.size()==2 && a[1]=="baseline-end") {
    healingObserve(state,world,player);
    if(test.movingMs==0 || test.distance<20)throw std::runtime_error("healing walk reference did not move");
    const float speed=test.distance*1000.f/float(test.movingMs);
    state.walkSpeed[test.direction]=speed;
    say("healing walk reference "+std::to_string(test.direction)+": "+std::to_string(test.distance)+
        " cm in "+std::to_string(test.movingMs)+" ms, "+std::to_string(speed)+" cm/s");
    test.walking=false;
    exec("release all",world,player,ctrl);
    player.setAnim(Npc::Anim::Idle);
    }
  else if(a.size()==2 && a[1]=="second") {
    test.secondItem=world.script().findSymbolIndex("ITFO_APPLE");
    if(test.secondItem==size_t(-1))throw std::runtime_error("healing second item is absent");
    const auto count=player.itemCount(test.secondItem);
    if(count)player.delItem(test.secondItem,uint32_t(count));
    player.addItem(test.secondItem,1);
    player.useItem(test.secondItem);
    say("healing second item requested at "+std::to_string(world.tickCount()-test.start)+" ms");
    }
  else if(a.size()==3 && a[1]=="action") {
    if(!Gothic::physicsFlags().meleeUseInHand) {
      say("healing action "+a[2]+": OFF holster control, no input");
      return;
      }
    test.action=a[2];
    if(a[2]=="dodge")test.actionExecuted=player.meleeDodge(MeleeDodge::Direction::Back);
    else if(a[2]=="attack")test.actionExecuted=player.doAttack(Npc::Anim::Attack,BS_HIT);
    else if(a[2]=="parade")test.actionExecuted=player.blockSword();
    else if(a[2]=="charge")test.actionExecuted=player.beginMeleeCharge();
    else if(a[2]=="run") {player.setWalkMode(WalkBit::WM_Run);test.actionExecuted=!player.isUsingItemInHand();}
    else throw std::runtime_error("healing action expects dodge, attack, parade, charge or run");
    say("healing action "+a[2]+": accepted "+std::to_string(test.actionExecuted)+", anim "+player.animNames());
    healingObserve(state,world,player);
    }
  else if(a.size()==3 && a[1]=="hit") {
    test.strikeDelay=uint64_t(std::stoul(a[2]));
    healingObserve(state,world,player);
    }
  else if(a.size()==3 && a[1]=="strike") {
    test.strikeAt=world.tickCount()+uint64_t(std::stoul(a[2]));
    say("healing strike scheduled "+a[2]+" ms after use");
    }
  else if(a.size()==2 && a[1]=="busy") {
    const auto now=world.tickCount();
    const auto* animation=player.playAnimByName("T_1HATTACKL",BS_STAND,true);
    if(animation==nullptr || animation->layer!=1 || animation->canInterrupt(now,now,0))
      throw std::runtime_error("healing busy fixture requires a noninterruptible layer-one transition");
    }
  else if(a.size()==2 && a[1]=="retry") {
    exec("disarm",world,player,ctrl);
    const auto item=test.item;
    const auto count=player.inventory().itemCount(item);
    test.label+="-retry";test.start=world.tickCount();test.firstHeal=test.lastHeal=test.lastConsume=test.nextSecond=0;
    test.paused=0;test.portions=0;test.samples.clear();
    test.healed=test.damage=0;test.hp=player.attribute(ATR_HITPOINTS);test.lastItems=count;test.sawDrink=false;
    if(!count)player.addItem(item,1); // OFF may already have consumed the first potion.
    player.useItem(item);
    healingObserve(state,world,player);
    }
  else if(a.size()==2 && a[1]=="arrow") {
    auto* attacker=findNpc(world,player,"BDT_1013_BANDIT_L");
    const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
    if(!attacker || symbol==size_t(-1))throw std::runtime_error("healing arrow fixture is absent");
    const auto side=Vec3(-facing(player).z,0,facing(player).x);
    Vec3 from,to;bool found=false;
    for(float height:{100.f,120.f,80.f,140.f}) {
      from=player.position()+Vec3(0,height,0)+side*120.f;to=from-side*240.f;
      float distance=0;
      if(player.rayHitZone(from,to,distance)!=HitZones::None) {found=true;break;}
      }
    if(!found)throw std::runtime_error("healing arrow has no native bone intersection");
    Item item(world,symbol,Item::T_Inventory);
    Bullet arrow(world,item,from);arrow.setOrigin(attacker);arrow.setPrecise(1.f);
    DamageCalculator::Damage damage;damage[PROT_POINT]=20;arrow.setDamage(damage);
    constexpr uint64_t dt=100;
    const auto direction=(to-from)/float(dt)+Vec3(0,.5f*DynamicWorld::gravity*float(dt),0);
    arrow.setDirection(direction);
    DynamicWorld::BulletBody step(world.physic(),&arrow);
    step.setPosition(from);step.setDirection(direction);
    const int hp=player.attribute(ATR_HITPOINTS);
    ClothBulletBenchDriver::step(*world.physic(),step,direction,dt);
    say("healing arrow: hp "+std::to_string(hp)+" -> "+std::to_string(player.attribute(ATR_HITPOINTS))+
      ", native hit "+std::to_string(arrow.hitZone()!=HitZones::None)+" at "+std::to_string(world.tickCount()-test.start)+" ms");
    if(player.attribute(ATR_HITPOINTS)>=hp || arrow.hitZone()==HitZones::None)
      throw std::runtime_error("healing arrow did not deal native projectile damage");
    healingObserve(state,world,player);
    }
  else if(a.size()==2 && a[1]=="save") {
    test.saved.clear();test.savedHealed=test.healed;test.savedDamage=test.damage;
    test.savedAt=world.tickCount();test.savedPaused=test.paused;test.savedLastHeal=test.lastHeal;
    test.savedNextSecond=test.nextSecond;test.savedSamples=test.samples.size();
    {MemWriter writer(test.saved);Serialize out(writer);out.setContext(&world);out.setVersion(Serialize::Current);
    player.save(out,world.npcId(&player),"/npc/");}
    std::ofstream file(outDir+"/healing-npc.zip",std::ios::binary);
    file.write(reinterpret_cast<const char*>(test.saved.data()),std::streamsize(test.saved.size()));
    if(!file)throw std::runtime_error("cannot write healing NPC save");
    say("healing saved: hp "+std::to_string(player.attribute(ATR_HITPOINTS))+", bytes "+std::to_string(test.saved.size()));
    }
  else if(a.size()==2 && a[1]=="load") {
    if(test.saved.empty())throw std::runtime_error("healing load requires a saved NPC");
    {MemReader reader(test.saved);Serialize in(reader);in.setContext(&world);in.setVersion(Serialize::Current);
    player.load(in,world.npcId(&player),"/npc/");player.postValidate();}
    test.hp=player.attribute(ATR_HITPOINTS);test.healed=test.savedHealed;test.damage=test.savedDamage;
    test.lastHeal=test.savedLastHeal;test.samples.resize(test.savedSamples);
    const auto pause=world.tickCount()-test.savedAt;
    test.paused=test.savedPaused+pause;
    test.nextSecond=test.savedNextSecond ? test.savedNextSecond+pause : 0;
    say("healing loaded: hp "+std::to_string(test.hp)+", healed "+std::to_string(test.healed));
    }
  else if(a.size()>=3 && a[1]=="check") {
    healingObserve(state,world,player);
    auto& actor=test.npc==nullptr ? player : *test.npc;
    const bool potion=Gothic::physicsFlags().meleeHealInterrupt;
    const bool food=Gothic::physicsFlags().foodOverTime;
    const auto items=actor.inventory().itemCount(test.item);
    const uint64_t first=test.firstHeal ? test.firstHeal-test.start : 0;
    const uint64_t last=test.lastHeal ? test.lastHeal-test.start : 0;
    const auto foodElapsed=test.lastHeal>test.lastConsume+test.paused ? test.lastHeal-test.lastConsume-test.paused : 0;
    bool pass=false;
    if(a[2]=="potion" || a[2]=="hotkey") {
      const auto requested=a[2]=="potion" ? test.start : test.hotkeyAt;
      pass=test.healed==50 && (potion ? test.firstHeal>requested+1000 && test.sawDrink && test.lastConsume==test.firstHeal :
                                     test.firstHeal==requested);
      if(a[2]=="potion")pass&=items==0;
      if(a[2]=="hotkey") {
        pass&=items==(potion ? 1u : 0u); // The original script also consumes a potion at full HP.
        pass&=potion ? actor.weaponState()==WeaponState::NoWeapon ||
                      (Gothic::physicsFlags().meleeUseInHand && test.sawHand && actor.weaponState()==WeaponState::W1H) :
                      actor.weaponState()!=WeaponState::NoWeapon;
        }
      }
    else if(a[2]=="hand-potion" || a[2]=="hand-food" || a[2]=="hand-cancel" || a[2]=="hand-hit" || a[2]=="wine" || a[2]=="walk" || a[2]=="refused-action" || a[2]=="npc-queue") {
      const bool enabled=Gothic::physicsFlags().meleeUseInHand && a[2]!="wine";
      const bool cancelled=a[2]=="hand-hit" || (a[2]=="hand-cancel" && enabled);
      if(cancelled)pass=test.healed==0 && items==1 && (a[2]=="hand-hit" ? test.damage>0 : test.actionExecuted);
      else if(a[2]=="hand-food")pass=a.size()==4 && test.healed==std::stoi(a[3]) && items==0 && foodElapsed>=9900 && foodElapsed<=10100;
      else if(a[2]=="wine") {
        bool logged=false;
        std::ifstream log(outDir+"/bench.log");
        for(std::string line;std::getline(log,line);)
          logged|=line.find(": ITFO_WINE holster path: scheme POTION")!=std::string::npos;
        pass=items==0 && test.lastConsume>test.start+5000 && !test.nativeClip.empty() && logged;
        say("healing wine fallback: "+test.nativeClip+", scheme POTION logged "+std::to_string(logged));
        }
      else pass=test.healed==50 && items==0 && test.firstHeal==test.lastConsume;
      if(enabled && !test.motion.empty()) {
        pass&=test.walkOnly && (test.motion=="back" ? test.distance>10 : test.yawTravel>10);
        say("healing motion "+test.motion+": "+std::to_string(test.distance)+" cm, "+std::to_string(test.yawTravel)+" degrees");
        }
      if(a[2]=="refused-action" && enabled)pass&=!test.actionExecuted;
      if(a[2]=="hand-cancel" && enabled) {
        const bool empty=actor.inventory().slotItem("ZS_LEFTHAND")==nullptr;
        pass&=empty;
        if(test.action=="run")pass&=actor.walkMode()==WalkBit::WM_Run;
        say("healing cancelled hand: empty "+std::to_string(empty)+", walk mode "+std::to_string(int(actor.walkMode())));
        }
      if(test.checkWalk)pass&=actor.walkMode()==test.expectedWalk;
      if(a[2]=="npc-queue") {
        pass&=test.queueRunAt!=0 && actor.isAiQueueEmpty();
        if(enabled)pass&=test.handEnd!=0 && test.queueRunAt>=test.lastConsume && test.queueRunAt>=test.handEnd;
        say("healing NPC queue: run at "+std::to_string(test.queueRunAt-test.start)+" ms, hand end "+
            std::to_string(test.handEnd ? test.handEnd-test.start : 0)+" ms");
        }
      if(test.secondItem!=size_t(-1))pass&=actor.itemCount(test.secondItem)==1;
      if(enabled) {
        pass&=test.sawHand && test.heldWeapon && test.handLayer && test.leftInsert && test.sound && test.sawLeftItem && test.leftHeld;
        if(!cancelled)pass&=test.destroyAt!=0 && test.lastConsume>=test.destroyAt && test.lastConsume<=test.destroyAt+34;
        }
      else pass&=!test.sawHand && (test.hotkey ? actor.weaponState()==WeaponState::NoWeapon && !test.heldWeapon :
                                               actor.weaponState()==test.weapon && test.heldWeapon);
      if(a[2]=="walk") {
        const float speed=test.movingMs==0 ? 0 : test.distance*1000.f/float(test.movingMs);
        const float reference=state.walkSpeed[test.direction];
        if(enabled)pass&=test.walkOnly && test.distance>20 && reference>0 && std::abs(speed/reference-1.f)<=.15f;
        say("healing walk "+test.label+": "+std::to_string(test.distance)+" cm in "+std::to_string(test.movingMs)+
            " ms, "+std::to_string(speed)+" cm/s, combat walk "+std::to_string(reference)+
            " cm/s, ratio "+std::to_string(reference==0 ? 0 : speed/reference)+", walk only "+std::to_string(test.walkOnly));
        }
      say("healing hand "+test.label+": clip "+test.clip+", layer21 "+std::to_string(test.handLayer)+
          ", held weapon "+std::to_string(test.heldWeapon)+", hand duration "+std::to_string(test.handEnd>test.handStart ? test.handEnd-test.handStart : 0)+
          " ms, destroy "+std::to_string(test.destroyAt>test.start ? test.destroyAt-test.start : 0)+
          " ms, left insert "+std::to_string(test.leftInsert)+", sound "+std::to_string(test.sound)+
          ", left item seen "+std::to_string(test.sawLeftItem)+", left item held "+std::to_string(test.leftHeld)+
          ", action "+test.action+", executed "+std::to_string(test.actionExecuted));
      test.walking=false;
      exec("release all",world,player,ctrl);
      }
    else if(a[2]=="original-potion" || a[2]=="original-food") {
      pass=test.healed==(a[2]=="original-potion" ? 50 : 10) && test.firstHeal==test.start && items==0 &&
           !test.sawHand && test.heldWeapon && actor.weaponState()==test.weapon && !test.actionExecuted;
      Gothic::settingsSetI("GAME","melee_heal_interrupt",1);
      Gothic::settingsSetI("GAME","food_over_time",1);
      }
    else if(a[2]=="interrupted")pass=potion ? test.healed==0 && items==1 && test.damage>0 : test.healed==50 && test.firstHeal==test.start;
    else if(a[2]=="refused")pass=test.healed==0 && items==1 && !actor.hasState(BS_ITEMINTERACT);
    else if(a[2]=="food" && a.size()==4) {
      const int amount=std::stoi(a[3]);
      pass=test.healed==amount && items==0;
      if(food) {
        pass&=foodElapsed>=9900 && foodElapsed<=10100;
        for(const auto& [elapsed,healed]:test.samples) {
          if(elapsed<9900 && healed>=amount)pass=false;
          if(test.portions==1) {
            const auto low=elapsed>100 ? elapsed-100 : 0;
            const auto high=std::min(elapsed+100,uint64_t(10000));
            pass&=healed>=int(uint64_t(amount)*std::min(low,uint64_t(10000))/10000) &&
                  healed<=int(uint64_t(amount)*high/10000);
            }
          }
        }
      else pass&=test.firstHeal==test.start;
      }
    say("healing check "+test.label+": healed "+std::to_string(test.healed)+", damage "+std::to_string(test.damage)+
      ", items "+std::to_string(items)+", first heal "+std::to_string(first)+" ms, consume "+
      std::to_string(test.lastConsume?test.lastConsume-test.start:0)+" ms, last heal "+std::to_string(last)+
      " ms, food elapsed "+std::to_string(foodElapsed)+" ms, drink "+std::to_string(test.sawDrink)+
      ", weapon "+std::to_string(int(actor.weaponState()))+", pass "+std::to_string(pass));
    event("healing_check","\"case\":"+jsonStr(test.label)+",\"healed\":"+std::to_string(test.healed)+
      ",\"damage\":"+std::to_string(test.damage)+",\"items\":"+std::to_string(items)+",\"first_heal_ms\":"+
      std::to_string(first)+",\"last_heal_ms\":"+std::to_string(last)+",\"food_elapsed_ms\":"+std::to_string(foodElapsed)+
      ",\"consume_ms\":"+std::to_string(test.lastConsume?test.lastConsume-test.start:0)+
      ",\"pass\":"+(pass?"true":"false"));
    if(!pass)throw std::runtime_error("healing mechanic check failed: "+test.label);
    }
  else throw std::runtime_error("healing expects begin/arm/use/exhaust/toggle/original/npc-queue/baseline/move/motion/baseline-end/second/action/hit/strike/busy/retry/arrow/save/load/check");
  }

}
