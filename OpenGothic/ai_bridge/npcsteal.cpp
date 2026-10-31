#include "npcsteal.h"

#include <algorithm>
#include <cmath>

#include "game/herodeed.h"
#include "game/inventory.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "jsonread.h"
#include "jsonwriter.h"
#include "npcchests.h"
#include "npcfind.h"

namespace {
constexpr float TakeRange = 2000;
constexpr float ChestNear = 300;
constexpr float PickupNear = 100;
constexpr float PickupHeight = 200;
std::string ownerOf(Npc& npc, const NpcSteal::Target& target) {
  const auto at = target.chest!=nullptr ? target.chest->position() : target.item->position();
  const auto d = HeroDeed::snapshot(npc,"take_item",nullptr,target.item,target.chest,target.count,&at);
  if(!d->owner.npc.empty()) return d->owner.npc;
  if(!d->chestOwner.npc.empty()) return d->chestOwner.npc;
  return d->roomOwner.npc;
  }
}

NpcSteal::Target NpcSteal::resolve(World& world, Npc& npc, const rapidjson::Value& args, std::string& error) {
  Target target;
  const auto chest = JsonRead::str(args,"chest");
  auto name = AiNpc::upper(JsonRead::str(args,"what"));
  if(name.empty()) name = AiNpc::upper(JsonRead::str(args,"item"));
  target.instance = world.script().findSymbolIndex(name);
  if(target.instance==size_t(-1)) {
    error = "unknown theft item: " + name;
    return target;
    }
  if(!chest.empty()) {
    target.chest = NpcChests::find(world,chest);
    if(target.chest==nullptr) {
      error = "no chest " + std::string(chest);
      return target;
      }
    target.item = target.chest->inventory().getItem(target.instance);
    } else {
    float nearest = TakeRange*TakeRange;
    world.detectItem(npc.position(),TakeRange,[&](Item& item) {
      const float distance = npc.qDistTo(item.position());
      if(item.clsId()==target.instance && distance<nearest) {
        target.item = &item;
        nearest = distance;
        }
      });
    }
  if(target.item==nullptr) {
    error = "theft item not found: " + name;
    return target;
    }
  target.count = std::min(target.item->count(),size_t(std::max<int64_t>(1,JsonRead::integer(args,"count",1))));
  target.mission = target.item->isMission();
  target.owner = ownerOf(npc,target);
  return target;
  }

std::string NpcSteal::preview(World& world, const rapidjson::Value& args) {
  JsonWriter j("deed_target");
  std::string error;
  auto* npc = AiNpc::findNpc(world,JsonRead::str(args,"npc"));
  Target target;
  if(npc==nullptr) error = "npc not found";
  else target = resolve(world,*npc,args,error);
  std::string ownerState;
  if(auto* owner = AiNpc::findNpc(world,target.owner))
    ownerState = AiNpc::stateName(world,owner->currentState());
  std::string item;
  if(auto* symbol = world.script().findSymbol(target.instance)) item = symbol->name();
  j.str("owner",target.owner).str("item",item).integer("count",int64_t(target.count))
   .boolean("mission",target.mission).str("owner_state",ownerState);
  if(!error.empty()) j.str("error",error);
  return j.finish();
  }

bool NpcSteal::begin(World& world, Npc& npc, const rapidjson::Value& args, Active& active, std::string& error) {
  if(!JsonRead::boolean(args,"deed",false)) {
    error = "steal requires deed:true";
    return false;
    }
  active.target = resolve(world,npc,args,error);
  if(!error.empty()) return false;
  if(active.target.mission) {
    error = "a mission item cannot be stolen";
    return false;
    }
  const auto expectedItem = AiNpc::upper(JsonRead::str(args,"expected_item"));
  if(!expectedItem.empty() && world.script().findSymbolIndex(expectedItem)!=active.target.instance) {
    error = "theft item changed since preflight";
    return false;
    }
  const auto expected = JsonRead::str(args,"expected_owner");
  if(args.HasMember("expected_owner") && expected!=active.target.owner) {
    error = "theft owner changed since preflight";
    return false;
    }
  active.expectedOwner = active.target.owner;
  active.before = npc.inventory().itemCount(active.target.instance);
  active.active = true;
  npc.stopPhysicsInvestigation(false);
  npc.stopListening();
  npc.clearAiQueue();
  npc.clearState(true);
  npc.setExternalControl(true);
  npc.setServerDeed("take_item",nullptr,active.target.count,active.expectedOwner);
  if(auto* chest = active.target.chest) {
    if(chest->needToLockpick(npc)) {
      chest->setAsCracked(true);
      active.relock = true;
      }
    npc.setWalkMode(WalkBit::WM_Walk);
    if(npc.qDistTo(chest->position())>ChestNear*ChestNear)
      if(auto* waypoint = world.findWayPoint(chest->position()))
        npc.aiPush(AiQueue::aiGoToPoint(*waypoint));
    npc.aiPush(AiQueue::aiUseMob(chest->schemeName(),1));
    } else {
    npc.aiPush(AiQueue::aiGotoItem(*active.target.item));
    npc.aiPush(AiQueue::aiTakeItem(*active.target.item));
    }
  return true;
  }

NpcSteal::Progress NpcSteal::tick(World& world, Npc& npc, Active& active, std::string& detail) {
  const bool transferred = active.target.chest!=nullptr ? active.opened :
                           npc.inventory().itemCount(active.target.instance)>active.before;
  if(!transferred)
    if(const auto* reason = npc.serverDeedSimulationError()) {
      detail = reason;
      return Progress::Failed;
      }
  auto* chest = active.target.chest;
  if(chest==nullptr && npc.inventory().itemCount(active.target.instance)<=active.before) {
    // AI_GotoItem uses a ground position, while its generic arrival test uses
    // the NPC's elevated skeleton root. Admit the native pickup at arm's reach
    // rather than requiring its fallback to land within one horizontal cm.
    Item* item = nullptr;
    world.detectItem(npc.position(),TakeRange,[&](Item& candidate) {
      if(&candidate==active.target.item)
        item = &candidate;
      });
    if(item==nullptr) {
      detail = "the world theft item is gone or beyond pickup range";
      return Progress::Failed;
      }
    const auto goal = npc.navigationGoal();
    const auto delta = item->position()-npc.position();
    if(goal.active && goal.kind=="item" && delta.x*delta.x+delta.z*delta.z<=PickupNear*PickupNear &&
       std::abs(delta.y)<=PickupHeight) {
      auto current = active.target;
      current.item = item;
      if(item->isMission() || ownerOf(npc,current)!=active.expectedOwner) {
        detail = "the world theft target changed before pickup";
        return Progress::Failed;
        }
      npc.clearGoTo(); // AI_TakeItem, its animation and the actual transfer remain in the native queue.
      }
    }
  if(npc.isAiBusy()) return Progress::Running;
  if(chest!=nullptr && !active.opened) {
    if(npc.interactive()!=chest) {
      detail = "could not open the requested chest";
      return Progress::Failed;
      }
    auto* item = chest->inventory().getItem(active.target.instance);
    auto current = active.target;
    current.item = item;
    if(item==nullptr || item->isMission() || ownerOf(npc,current)!=active.expectedOwner) {
      detail = "theft target changed before transfer";
      return Progress::Failed;
      }
    npc.addItem(active.target.instance,*chest,active.target.count);
    active.opened = true;
    npc.aiPush(AiQueue::aiUseMob(chest->schemeName(),-1));
    // Once the item changes hands native self-defence may interrupt closing.
    npc.setExternalControl(false);
    return Progress::Running;
    }
  if(npc.inventory().itemCount(active.target.instance)<=active.before) {
    detail = "the stolen item did not reach its inventory";
    return Progress::Failed;
    }
  return Progress::Done;
  }

void NpcSteal::finish(Npc& npc, Active& active) {
  if(!active.active) return;
  // A disconnect/preemption before transfer must not leave a queued native
  // pickup behind after the command's deed mark has been removed.
  if(!npc.isDown() && (npc.isExternalControl() ||
                      (active.target.chest!=nullptr && npc.interactive()==active.target.chest))) {
    npc.clearAiQueue();
    npc.clearGoTo();
    }
  if(auto* chest = active.target.chest) {
    if(npc.interactive()==chest) npc.setInteraction(nullptr,true);
    if(active.relock) chest->setAsCracked(false);
    }
  npc.clearServerDeed();
  npc.setExternalControl(false);
  if(!npc.isDown() && !npc.isAttack()) npc.resumeAiRoutine();
  active.active = false;
  }
