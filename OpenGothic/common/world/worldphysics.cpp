#include "world.h"
#include "game/constants.h"
#include "objects/item.h"
#include "objects/npc.h"
#include "benchhooks.h"

#include <algorithm>

void World::setOriginalClothVisible(bool visible) {
  if(view()!=nullptr)
    view()->setOriginalClothVisible(visible);
  }

PhysicsHost::PhysicalSpell World::physicalSpell(int32_t spellId) const {
  switch(rules().physicalSpell(spellId)) {
    case ::PhysicalSpell::Fireball: return PhysicsHost::PhysicalSpell::Fireball;
    case ::PhysicalSpell::WindFist: return PhysicsHost::PhysicalSpell::WindFist;
    default: return PhysicsHost::PhysicalSpell::None;
    }
  }

std::weak_ptr<void> World::npcHandle(const Npc& npc) const { return npc.handlePtr(); }

Npc* World::resolveNpc(const std::shared_ptr<void>& handle) const {
  return rules().findNpc(static_cast<NpcRecord*>(handle.get()));
  }

std::string World::npcTraceName(const Npc& npc) const {
  if(npc.isPlayer())
    return "PLAYER";
  return std::string(npc.world().rules().symbolName(size_t(npc.instanceSymbol())));
  }

std::string_view World::npcSymbolName(const Npc* npc) const { return Bench::who(npc); }

Tempest::Vec3 World::npcPosition(const Npc& npc) const { return npc.position(); }
uint64_t World::npcTickCount(const Npc& npc) const { return npc.world().tickCount(); }
bool World::npcUsesSimplifiedMovement(const Npc& npc) const { return npc.usesSimplifiedMovement(); }
uint32_t World::npcCreationSeq(const Npc& npc) const { return npc.creationSeq(); }
void World::finishNpcSupport(Npc& npc, uint64_t dt) { npc.finishCharacterVirtualSupport(dt); }

HitZones::Zone World::npcRayHitZone(const Npc& npc, const Tempest::Vec3& from,
                                  const Tempest::Vec3& to, float& distance) const {
  return npc.rayHitZone(from,to,distance);
  }

bool World::npcBodySinks(const Npc& npc) const {
  const auto guild=npc.trueGuild();
  return guild==GIL_GOBBO_SKELETON || guild==GIL_SUMMONED_GOBBO_SKELETON ||
         guild==GIL_SKELETON || guild==GIL_SUMMONED_SKELETON || guild==GIL_SKELETON_MAGE ||
         guild==GIL_SHADOWBEAST_SKELETON || guild==GIL_STONEGOLEM || guild==GIL_SUMMONED_GOLEM ||
         guild==GIL_Stoneguardian || guild==GIL_SummonedGuardian;
  }

ItemMaterial World::itemMaterial(const Item& item) const { return ItemMaterial(item.handle().material); }
std::string_view World::itemVisual(const Item& item) const { return item.handle().visual; }

bool World::isPlacedItem(const Item& item) const { return item.isPlacedItem(); }
uint32_t World::placedItemZenId(const Item& item) const { return item.placedItemZenId(); }
void World::setPhysicsMatrix(Item& item, const Tempest::Matrix4x4& matrix) { item.setPhysicsMatrix(matrix); }
void World::setPhysicsDisable(Item& item) { item.setPhysicsDisable(); }

size_t World::wakeItems(const Tempest::Vec3& min, const Tempest::Vec3& max, size_t budget) {
  const auto center=(min+max)*.5f;
  const float radius=(max-min).length()*.5f;
  size_t created=0;
  detectItem(center,radius,[&](Item& item) {
    const auto p=item.position();
    if(p.x<min.x || p.x>max.x || p.y<min.y || p.y>max.y || p.z<min.z || p.z>max.z)
      return;
    if(item.isDynamic() || created<budget)
      created+=item.wakePhysics() ? 1 : 0;
    });
  return created;
  }

void World::itemHit(Item& item, asset::MaterialGroup material, float impulse, float mass) {
  auto  snd = addLandHitEffect(ItemMaterial(item.handle().material),material,item.transform());
  float v   = impulse/mass;
  float vol = snd.volume()*std::min(v/10.f,1.f);
  snd.setVolume(vol);
  snd.play();
  }
