#pragma once

#include "asset/material.h"
#include "graphics/mesh/hitzones.h"
#include "records/definitions.h"

#include <Tempest/Matrix4x4>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class Npc;
class Item;

class PhysicsHost {
  public:
    enum class PhysicalSpell : uint8_t { None, Fireball, WindFist };

    virtual ~PhysicsHost() = default;
    virtual std::string_view name() const = 0;
    virtual uint64_t tickCount() const = 0;
    virtual void setOriginalClothVisible(bool visible) = 0;
    virtual PhysicalSpell physicalSpell(int32_t spellId) const = 0;

    // The script instance owns the handle; resolve its live NPC only at the call site.
    virtual std::weak_ptr<void> npcHandle(const Npc& npc) const = 0;
    virtual Npc* resolveNpc(const std::shared_ptr<void>& handle) const = 0;
    virtual std::string npcTraceName(const Npc& npc) const = 0;
    virtual Tempest::Vec3 npcPosition(const Npc& npc) const = 0;
    virtual uint64_t npcTickCount(const Npc& npc) const = 0;
    virtual bool npcUsesSimplifiedMovement(const Npc& npc) const = 0;
    virtual uint32_t npcCreationSeq(const Npc& npc) const = 0;
    virtual void finishNpcSupport(Npc& npc, uint64_t dt) = 0;
    virtual HitZones::Zone npcRayHitZone(const Npc& npc, const Tempest::Vec3& from,
                                       const Tempest::Vec3& to, float& distance) const = 0;
    virtual bool npcBodySinks(const Npc& npc) const = 0;

    virtual ItemMaterial itemMaterial(const Item& item) const = 0;
    virtual std::string_view itemVisual(const Item& item) const = 0;
    virtual bool isPlacedItem(const Item& item) const = 0;
    virtual uint32_t placedItemZenId(const Item& item) const = 0;
    virtual void setPhysicsMatrix(Item& item, const Tempest::Matrix4x4& matrix) = 0;
    virtual void setPhysicsDisable(Item& item) = 0;
    virtual size_t wakeItems(const Tempest::Vec3& min, const Tempest::Vec3& max, size_t budget) = 0;
    virtual void itemHit(Item& item, asset::MaterialGroup material, float impulse, float mass) = 0;
  };
