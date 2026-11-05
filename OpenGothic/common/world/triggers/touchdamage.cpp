#include "touchdamage.h"

#include "world/objects/npc.h"
#include "world/world.h"
#include "game/serialize.h"

TouchDamage::TouchDamage(Vob* parent, World &world, const asset::VTouchDamage& dmg, Flags flags)
  :AbstractTrigger(parent,world,dmg,flags) {
  barrier        = dmg.barrier;
  blunt          = dmg.blunt;
  edge           = dmg.edge;
  fire           = dmg.fire;
  fly            = dmg.fly;
  magic          = dmg.magic;
  point          = dmg.point;
  fall           = dmg.fall;
  damage         = dmg.damage;
  repeatDelaySec = dmg.repeat_delay_sec;
  }

void TouchDamage::onTrigger(const TriggerEvent&/*evt*/) {
  }

void TouchDamage::onIntersect(Npc& n) {
  AbstractTrigger::onIntersect(n);
  enableTicks();
  }

void TouchDamage::tick(uint64_t dt) {
  AbstractTrigger::tick(dt);

  if(world.tickCount()<=repeatTimeout)
    return;

  for(auto npc:intersections()) {
    bool mask[daedalus::DamageType::NUM] = {};
    mask[daedalus::DamageType::BARRIER] = barrier;
    mask[daedalus::DamageType::BLUNT]   = blunt;
    mask[daedalus::DamageType::EDGE]    = edge;
    mask[daedalus::DamageType::FIRE]    = fire;
    mask[daedalus::DamageType::FLY]     = fly;
    mask[daedalus::DamageType::MAGIC]   = magic;
    mask[daedalus::DamageType::POINT]   = point;
    mask[daedalus::DamageType::FALL]    = fall;

    auto& hnpc = npc->handle();
    for(size_t i=0; i<daedalus::DamageType::NUM; ++i) {
      if(!mask[i])
        continue;
      takeDamage(*npc,int32_t(damage),hnpc.protection[i]);
      }
    }

  repeatTimeout = world.tickCount() + uint64_t(repeatDelaySec*1000);

  if(intersections().empty())
    disableTicks();
  }

void TouchDamage::takeDamage(Npc& npc, int32_t val, int32_t prot) {
  if(prot<0) // Filter immune
    return;
  npc.changeAttribute(ATR_HITPOINTS,-std::max(val-prot,0),false);
  }
