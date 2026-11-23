#include "serialize.h"

#include "savegameheader.h"
#include "world/world.h"
#include "world/fplock.h"
#include "world/waypoint.h"
#include "world/records/npcrecord.h"
#include "world/records/itemrecord.h"

#include <Tempest/Log>

Serialize::Serialize(Tempest::ODevice& fout) : SaveArchive(fout) {}

Serialize::Serialize(Tempest::IDevice& fin) : SaveArchive(fin) {}

Serialize::~Serialize() noexcept(false) = default;

void Serialize::setContext(World* ctx) {
  this->ctx = ctx;
  wldName = ctx!=nullptr ? ctx->name() : "_";
  }

void Serialize::implWrite(WeaponState w) {
  implWrite(uint8_t(w));
  }

void Serialize::implRead(WeaponState &w) {
  implRead(reinterpret_cast<uint8_t&>(w));
  }

void Serialize::implWrite(const WayPoint* wptr) {
  implWrite(wptr ? wptr->name : "");
  }

void Serialize::implRead(const WayPoint*& wptr) {
  implRead(tmpStr);
  wptr = ctx==nullptr ? nullptr : ctx->findPoint(tmpStr,false);
  }

void Serialize::implWrite(const ScriptFn& fn) {
  uint32_t v = uint32_t(-1);
  if(fn.ptr<uint64_t(std::numeric_limits<uint32_t>::max()))
    v = uint32_t(fn.ptr);
  implWrite(v);
  }

void Serialize::implRead(ScriptFn& fn) {
  uint32_t v=0;
  implRead(v);
  if(v==std::numeric_limits<uint32_t>::max())
    fn.ptr = size_t(-1); else
    fn.ptr = v;
  }

void Serialize::implWrite(const Npc* npc) {
  uint32_t id = ctx==nullptr ? uint32_t(-1) : ctx->npcId(npc);
  implWrite(id);
  }

void Serialize::implRead(const Npc *&npc) {
  uint32_t id=uint32_t(-1);
  implRead(id);
  npc = ctx==nullptr ? nullptr : ctx->npcById(id);
  }

void Serialize::implWrite(Npc *npc) {
  uint32_t id = ctx==nullptr ? uint32_t(-1) : ctx->npcId(npc);
  implWrite(id);
  }

void Serialize::implRead(Npc *&npc) {
  uint32_t id=uint32_t(-1);
  read(id);
  npc = ctx==nullptr ? nullptr : ctx->npcById(id);
  }

void Serialize::implWrite(Interactive* mobsi) {
  uint32_t id = ctx==nullptr ? uint32_t(-1) : ctx->mobsiId(mobsi);
  implWrite(id);
  }

void Serialize::implRead(Interactive*& mobsi) {
  uint32_t id=uint32_t(-1);
  implRead(id);
  mobsi = ctx==nullptr ? nullptr : ctx->mobsiById(id);
  }

void Serialize::implWrite(const SaveGameHeader& p) {
  p.save(*this);
  }

void Serialize::implRead(SaveGameHeader& p) {
  p = SaveGameHeader(*this);
  }

void Serialize::implWrite(const NpcRecord& h) {
  write(h.id,h.name,h.slot,h.effect,int32_t(h.type));
  write(int32_t(h.flags));
  write(h.attribute,h.hitchance,h.protection,h.damage);
  write(h.damage_type,h.guild,h.level);
  write(h.mission);
  write(h.fight_tactic,h.weapon,h.voice,h.voice_pitch,h.body_mass);
  write(h.daily_routine,h.start_aistate);
  write(h.spawnpoint,h.spawn_delay,h.senses,h.senses_range);
  write(h.aivar);
  write(h.wp,h.exp,h.exp_next,h.lp,h.bodystate_interruptable_override,h.no_focus);
  }

void Serialize::readNpc(NpcRecord& h) {
  read(h.id,h.name,h.slot,h.effect, reinterpret_cast<int32_t&>(h.type));
  read(reinterpret_cast<int32_t&>(h.flags));
  read(h.attribute,h.hitchance,h.protection,h.damage);
  read(h.damage_type,h.guild,h.level);
  read(h.mission);
  read(h.fight_tactic,h.weapon,h.voice,h.voice_pitch,h.body_mass);
  read(h.daily_routine,h.start_aistate);
  read(h.spawnpoint,h.spawn_delay,h.senses,h.senses_range);
  read(h.aivar);
  read(h.wp,h.exp,h.exp_next,h.lp,h.bodystate_interruptable_override,h.no_focus);
  }

void Serialize::implWrite(const ItemRecord& h) {
  write(h.id,h.name,h.name_id,h.hp,h.hp_max,h.main_flag);
  write(int32_t(h.flags),h.weight,h.value,h.damage_type,h.damage_total,h.damage);
  write(h.wear,h.protection,h.nutrition,h.cond_atr,h.cond_value,h.change_atr,h.change_value,h.magic);
  write(h.on_equip,h.on_unequip,h.on_state);
  write(h.owner,h.owner_guild,h.disguise_guild,h.visual,h.visual_change);
  write(h.effect,h.visual_skin,h.scheme_name,h.material);
  write(h.munition,h.spell,h.range,h.mag_circle);
  write(h.description,h.text,h.count);
  write(h.inv_zbias,h.inv_rot_x,h.inv_rot_y,h.inv_rot_z,h.inv_animate);
  }

void Serialize::readItem(ItemRecord& h) {
  read(h.id,h.name,h.name_id,h.hp,h.hp_max,h.main_flag);
  read(reinterpret_cast<int32_t&>(h.flags),h.weight,h.value,h.damage_type,h.damage_total,h.damage);
  read(h.wear,h.protection,h.nutrition,h.cond_atr,h.cond_value,h.change_atr,h.change_value,h.magic);
  read(h.on_equip,h.on_unequip,h.on_state);
  read(h.owner,h.owner_guild,h.disguise_guild,h.visual,h.visual_change);
  read(h.effect,h.visual_skin,h.scheme_name,h.material);
  read(h.munition,h.spell,h.range,h.mag_circle);
  read(h.description,h.text,h.count);
  read(h.inv_zbias,h.inv_rot_x,h.inv_rot_y,h.inv_rot_z,h.inv_animate);
  }

void Serialize::implWrite(const FpLock &fp) {
  fp.save(*this);
  }

void Serialize::implRead(FpLock &fp) {
  fp.load(*this);
  }
