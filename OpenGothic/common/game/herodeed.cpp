#include "herodeed.h"

#include <algorithm>
#include <map>

#include "game/deedwitness.h"
#include "game/gamerules.h"
#include "game/inventory.h"
#include "physics/dynamicworld.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

namespace {

HeroDeed::Person person(Npc& npc) {
  HeroDeed::Person p;
  if(npc.isPlayer())
    p.npc = "PLAYER"; else
    p.npc = npc.world().rules().symbolName(npc.instanceSymbol());
  p.guild = int32_t(npc.guild());
  p.npctype = int32_t(npc.handlePtr()->type);
  return p;
  }

HeroDeed::Person resolveOwner(Npc& hero, std::string_view name) {
  auto& rules = hero.world().rules();
  const auto id = rules.findSymbolIndex(name);
  if(!rules.isNpcInstance(id))
    return {};
  if(auto* npc = hero.world().findNpcByInstance(id))
    return person(*npc);
  // Preserve an explicit C_NPC owner even when that NPC is not in this world.
  HeroDeed::Person p;
  p.npc = rules.symbolName(id);
  if(auto instance = rules.npcRecord(id)) {
    p.guild = instance->guild;
    p.npctype = int32_t(instance->type);
    }
  return p;
  }

HeroDeed::Person resolveRoomOwner(Npc& hero, std::string_view portal) {
  const auto colon = portal.find(':');
  if(colon==std::string_view::npos)
    return resolveOwner(hero,portal);
  // Portal materials contain two room names separated by '_'. NPC instance
  // names themselves can contain '_', so accept the longest actual C_NPC name.
  auto name = portal.substr(colon+1);
  for(size_t end=name.size(); end!=std::string_view::npos;) {
    auto p = resolveOwner(hero,name.substr(0,end));
    if(!p.npc.empty())
      return p;
    if(end==0)
      break;
    end = name.rfind('_',end-1);
    }
  return {};
  }

}

int32_t HeroDeed::constant(Npc& npc, std::string_view name, int32_t fallback) {
  return npc.world().rules().intSymbol(name,fallback);
  }

int32_t HeroDeed::aivar(Npc& npc, std::string_view name) {
  const auto index = constant(npc,name);
  if(index<0 || uint32_t(index)>=NpcRecord::aivar_count)
    return 0;
  return npc.handlePtr()->aivar[size_t(index)];
  }

std::shared_ptr<HeroDeed> HeroDeed::capture(Npc& hero, std::string_view kind, Npc* victim,
                                          const Item* item, Interactive* chest, size_t count) {
  if(!hero.isPlayer() && !hero.isServerDeed(kind,victim))
    return {};
  if(!hero.isPlayer() && kind=="attack" &&
     (!NpcDeedSimulation::admitted(hero.processPolicy()) || victim==nullptr ||
      !NpcDeedSimulation::admitted(victim->processPolicy())))
    return {};
  if(!hero.isPlayer() && kind=="take_item" && (chest!=nullptr || item!=nullptr)) {
    const auto at = chest!=nullptr ? chest->position() : item->position();
    return snapshot(hero,kind,victim,item,chest,count,&at);
    }
  return snapshot(hero,kind,victim,item,chest,count);
  }

std::shared_ptr<HeroDeed> HeroDeed::snapshot(Npc& hero, std::string_view kind, Npc* victim,
                                           const Item* item, Interactive* chest, size_t count,
                                           const Tempest::Vec3* ownershipPosition) {
  static uint64_t nextId = 1;
  auto d = std::make_shared<HeroDeed>();
  d->id = nextId++;
  auto& world = hero.world();
  auto& rules = world.rules();
  d->kind = kind;
  d->culprit = person(hero);
  d->heroGuild = int32_t(hero.guild());
  d->position = hero.position();
  if(ownershipPosition==nullptr) {
    d->room = hero.portalName();
    } else {
    // A preflight happens before the actor walks there. Read the target's
    // native ground material (the same source as MoveAlgo::portalName), with
    // the BSP sector as fallback, rather than borrowing the actor's old room.
    // A target on a table, a shelf or a chest's own mesh first meets that prop, which has no sector:
    // the room is the world floor under it, as MoveAlgo keeps the last floor room under a standing npc.
    const auto from = *ownershipPosition+Tempest::Vec3(0,50,0);
    const auto hasSector = [](const DynamicWorld::RayLandResult& hit) {
      return hit.hasCol && hit.sector!=nullptr && hit.sector[0]!='\0';
      };
    auto ground = world.physic()->landRay(from,500);
    if(!hasSector(ground))
      ground = world.physic()->floorRay(from,500);
    d->room = hasSector(ground) ? std::string_view(ground.sector) : world.roomAt(*ownershipPosition);
    }
  d->roomGuild = world.guildOfRoom(d->room);
  d->roomOwner = resolveRoomOwner(hero,d->room);
  if(auto* wp = world.findWayPoint(hero.position()))
    d->waypoint = wp->name;
  const auto time = world.time();
  d->day = time.day();
  d->hour = time.hour();
  d->minute = time.minute();
  if(item!=nullptr) {
    d->item = rules.symbolName(item->clsId());
    d->count = count==0 ? item->count() : count;
    d->ownerGuild = item->handle().owner_guild;
    if(item->handle().owner>0)
      d->owner = resolveOwner(hero,rules.symbolName(size_t(item->handle().owner)));
    const int32_t dropped = constant(hero,"ITEM_DROPPED",0);
    d->itemDropped = dropped!=0 && (item->itemFlag()&dropped)!=0;
    }
  if(chest!=nullptr) {
    d->chest = chest->vobNameStr();
    d->chestOwner = resolveOwner(hero,chest->ownerName());
    d->chestGuild = constant(hero,chest->ownerGuildName());
    }
  if(victim!=nullptr) {
    d->victim = person(*victim);
    d->victimIsMonster = victim->isMonster();
    d->victimIsSheep = int32_t(victim->guild())==constant(hero,"GIL_SHEEP");
    d->victimToughGuy = aivar(*victim,"AIV_ToughGuy")!=0;
    const auto attackState = rules.findSymbolIndex("ZS_ATTACK");
    d->victimInFight = attackState!=size_t(-1) && victim->isInState(ScriptFn(attackState));
    d->victimEnemy = victim->isEnemy(hero);
    const auto reason = aivar(*victim,"AIV_ATTACKREASON");
    d->victimStartedFight = d->victimInFight && victim->target()==&hero &&
                           reason!=constant(hero,"AR_ReactToDamage") &&
                           reason!=constant(hero,"AR_ReactToWeapon") &&
                           reason!=constant(hero,"AR_Theft");
    }
  return d;
  }

std::vector<HeroDeed::InventoryItem> HeroDeed::inventory(Npc& npc) {
  std::map<std::string,size_t> counts;
  for(auto i = npc.inventory().iterator(Inventory::T_Inventory); i.isValid(); ++i)
    if(auto name = npc.world().rules().symbolName(i->clsId()); !name.empty())
      counts[std::string(name)] += i.count();
  std::vector<InventoryItem> result;
  for(auto& [item,count]:counts)
    result.push_back({item,count});
  return result;
  }

void HeroDeed::witnessed(Npc& npc) {
  if(npc.isPlayer() || npc.isDown())
    return;
  auto p = person(npc);
  if(p.npc.empty() || std::any_of(witnesses.begin(),witnesses.end(),[&](const Person& i){ return i.npc==p.npc; }))
    return;
  witnesses.push_back(std::move(p));
  }

void HeroDeed::collectWitnesses(Npc& actor, Npc* harmed) {
  const auto perception = kind=="attack" ? PERC_ASSESSOTHERSDAMAGE : PERC_ASSESSTHEFT;
  actor.world().detectNpcNear([&](Npc& npc) {
    if(npc.canWitnessDeed(actor,harmed,perception))
      witnessed(npc);
    });
  }
