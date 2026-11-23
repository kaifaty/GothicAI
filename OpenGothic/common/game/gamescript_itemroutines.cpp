#include "gamescript.h"

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <array>

namespace {
enum class Kind { HoldItem, Brush, Eat, Drink, Joint, Spit, Wash, Hammer };

struct ItemRoutine {
  std::string_view state, point, item;
  Kind             kind;
  int32_t          enterState;
  int32_t          timerSeconds = -1;
  };

constexpr ItemRoutine itemRoutines[] = {
  {"ZS_PLAY_LUTE",      "STAND", "ITMI_LUTE",       Kind::HoldItem, 1},
  {"ZS_RAKE_FP",        "PICK",  "ITMI_RAKE",       Kind::HoldItem, 1},
  {"ZS_SMOKE_JOINT",    "STAND", "ITMI_JOINT",      Kind::Joint,    0, 5},
  {"ZS_SPIT_FIRE",      "STAND", "ITLSTORCHFIRESPIT", Kind::Spit,   0, 10},
  {"ZS_STAND_DRINKING", "STAND", "ITFO_BOOZE",      Kind::Drink,    0, 7},
  {"ZS_STAND_EATING",   "STAND", {},                Kind::Eat,      0, 5},
  {"ZS_STAND_SWEEPING", "SWEEP", "ITMI_BROOM",      Kind::HoldItem, 1},
  {"ZS_SWEEP_FP",       "SWEEP", "ITMI_BRUSH",      Kind::Brush,    1},
  {"ZS_WASH_FP",        "WASH",  {},                Kind::Wash,     0},
  {"ZS_GUARD_HAMMER",   "STAND", {},                Kind::Hammer,   0, 5},
  };

constexpr std::array<std::string_view,4> food={"ITFO_APPLE","ITFO_CHEESE","ITFO_BACON","ITFOMUTTON"};
constexpr std::array<std::string_view,4> eatingGesture={
  "T_FOOD_RANDOM_1","T_FOODHUGE_RANDOM_1","T_MEAT_RANDOM_1","T_MEAT_RANDOM_1"
  };
}

bool GameScript::runItemRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  using Phase=ActivityDefinitions::Phase;
  auto phase=Phase::Init;
  std::string_view name=symbol.name();
  if(name.ends_with("_LOOP")) {
    phase=Phase::Loop;
    name.remove_suffix(5);
    }
  else if(name.ends_with("_END")) {
    phase=Phase::End;
    name.remove_suffix(4);
    }
  const auto it=std::find_if(std::begin(itemRoutines),std::end(itemRoutines),[name](const auto& activity) {
    return activity.state==name;
    });
  if(it==std::end(itemRoutines))
    return false;
  const auto& a=*it;
  result=LOOP_CONTINUE;
  // The position and meal choice are already saved in the NPC record.
  auto& foodChoice=npc.handle().aivar[18];
  auto& stage=npc.handle().aivar[19];
  constexpr auto held=0, enter=1, walking=2;
  const auto itemName=[&]() -> std::string_view {
    if(a.kind!=Kind::Eat)
      return a.item;
    return foodChoice>=0 && size_t(foodChoice)<food.size() ? food[size_t(foodChoice)] : std::string_view{};
    };
  const auto roll=[&](uint32_t bound) {
    const auto value=rollRandom(bound,&npc,true);
    if(auto* local=findSymbol(symbol.name()+".RANDOM"))
      local->set_int(int32_t(value));
    return value;
    };
  const auto animate=[&](std::string_view animation) {
    npc.aiPush(AiQueue::aiPlayAnimBs(animation,BS_ITEMINTERACT));
    };
  const auto use=[&](std::string_view item, int32_t state) {
    npc.aiPush(AiQueue::aiUseItemToState(int32_t(findSymbolIndex(item)),state));
    };
  if(phase==Phase::Init) {
    const auto ensureItem=[&](std::string_view item) {
      const auto id=uint32_t(findSymbolIndex(item));
      if(npc.itemCount(id)==0)
        storeItem(npc.addItem(id,1));
      };
    NpcActivityHost host(world(),npc,[this](std::string_view perception) {
      vm.call_function<void>(vm.find_symbol_by_name(perception));
      });
    ActivityDefinitions::resetForActivity(host,"PERCEPTION_SET_NORMAL");
    host.setWalkMode(1);
    const auto& wp=npc.handle().wp;
    if(a.kind==Kind::Brush ? host.nearestWaypoint()!=wp : host.distanceToWaypoint(wp)>500)
      host.gotoWaypoint(wp);
    if(a.kind==Kind::Eat) {
      stage=enter;
      const auto choice=roll(uint32_t(food.size()));
      ensureItem(food[choice]);
      foodChoice=int32_t(choice);
      }
    else {
      if(!a.item.empty())
        ensureItem(a.item);
      if(a.kind!=Kind::Brush && a.kind!=Kind::Spit)
        stage=enter;
      }
    return true;
    }
  if(phase==Phase::End) {
    if(a.kind==Kind::Wash)
      npc.aiPush(AiQueue::aiPlayAnim("T_WASH_2_STAND"));
    else if(a.kind==Kind::Hammer)
      npc.aiPush(AiQueue::aiPlayAnim("T_HGUARD_2_STAND"));
    else {
      if(a.kind==Kind::Eat)
        animate("T_POTION_RANDOM_2");
      if(const auto item=itemName(); !item.empty())
        use(item,-1);
      }
    return true;
    }
  const bool onPoint=a.kind!=Kind::Spit && npc.isOnFreePoint(a.point);
  if(onPoint) {
    npc.aiPush(AiQueue::aiAlignToFp());
    if(stage==walking)
      stage=enter;
    }
  else if(const auto* point=world().findFreePoint(npc,a.point)) {
    npc.aiPush(AiQueue::aiGoToPoint(*point));
    if(a.kind!=Kind::Spit)
      npc.aiPush(AiQueue::aiStandup());
    npc.aiPush(AiQueue::aiAlignToFp());
    if(a.kind!=Kind::Spit)
      stage=walking;
    }
  else {
    npc.aiPush(AiQueue::aiAlignToWp());
    if(a.kind!=Kind::Spit && stage==walking)
      stage=enter;
    }

  if(a.kind!=Kind::Spit && stage==enter) {
    if(a.kind==Kind::Wash) {
      if(onPoint) {
        npc.aiPush(AiQueue::aiAlignToFp());
        npc.aiPush(AiQueue::aiStandup());
        npc.aiPush(AiQueue::aiPlayAnim("T_STAND_2_WASH"));
        stage=held;
        }
      }
    else if(a.kind==Kind::Hammer) {
      npc.aiPush(AiQueue::aiPlayAnim("T_STAND_2_HGUARD"));
      stage=held;
      }
    else if(const auto item=itemName(); !item.empty()) {
      use(item,a.enterState);
      if(a.kind==Kind::Eat && foodChoice==0)
        animate("T_FOOD_RANDOM_2");
      stage=held;
      }
    }
  if(a.kind==Kind::Hammer && NpcEngineCalls::distanceToWaypoint(world(),npc,"NW_MONASTERY_SANCTUM_01")<=500) {
    const auto hammer=int32_t(findSymbolIndex("HOLY_HAMMER_MIS"));
    if(npc.itemCount(uint32_t(hammer))==1) {
      npc.delItem(uint32_t(hammer),1);
      wld_insertitem(hammer,"FP_HAMMER");
      }
    }
  if(a.timerSeconds<0 || (a.kind!=Kind::Spit && stage!=held) || int32_t(npc.stateTime()/1000)<=a.timerSeconds)
    return true;

  switch(a.kind) {
    case Kind::Eat:
      animate(foodChoice>=0 && size_t(foodChoice)<eatingGesture.size() ? eatingGesture[size_t(foodChoice)] : eatingGesture[1]);
      break;
    case Kind::Drink: {
      const auto value=roll(10);
      if(value==0)
        animate("T_POTION_RANDOM_3");
      animate("T_POTION_RANDOM_1");
      if(value==1)
        animate("T_POTION_RANDOM_2");
      break;
      }
    case Kind::Joint:
      animate("T_JOINT_RANDOM_1");
      break;
    case Kind::Spit:
      use(a.item,5);
      use(a.item,0);
      break;
    case Kind::Hammer:
      if(roll(10)==0)
        npc.aiPush(AiQueue::aiPlayAnim("T_HGUARD_LOOKAROUND"));
      break;
    default:
      break;
    }
  npc.setStateTime(0);
  return true;
  }
