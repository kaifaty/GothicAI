#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstddef>
#include <cstdint>

#include "graphics/effect.h"
#include "graphics/meshobjects.h"

class World;
class WorldView;
class ProtoMesh;
class VisualFx;
struct ItemRecord;

// The view of an item in the world (reports/engine/2026-11-19-item-effects): the item's mesh and its own effect
// (C_ITEM.effect) as one thing. It is the only way an item's mesh gets onto the ground (Item), onto a body
// (MdlVisual's item slots) or into an object's slot (Interactive): as Gothic's oCItem::InsertEffect, the effect is
// born on the item's mesh, follows the mesh's matrix and goes with the mesh. Not in an inventory, a chest, the side
// quiver or a worn armour: those never make one.
class ItemView final {
  public:
    ItemView() = default;
    // the item's mesh and, with [GAME] itemEffects, its effect; shown=false: the item is known, nothing is made
    // until restore() (a view the world streams in and out)
    ItemView(World& world, const ItemRecord& item, bool shown = true);
    // a mesh that never has an effect: a stuck arrow loaded from a save (saves keep no effects)
    explicit ItemView(MeshObjects::Mesh&& mesh);
    ItemView(ItemView&& other) noexcept;
    ItemView& operator=(ItemView&& other) noexcept;
    ItemView(const ItemView&) = delete;
    ItemView& operator=(const ItemView&) = delete;
    ~ItemView();

    // [GAME] itemEffects, 1 by default as in Gothic; OPENGOTHIC_ITEM_EFFECTS=0 sets it to 0 at start
    static bool enabled();
    // the live effects of items in the world, and of them those nearer the camera than the particles' range
    struct Census {
      size_t alive = 0;
      size_t near  = 0;
      };
    static Census census(const World& world);

    bool                     isEmpty() const { return mesh.isEmpty(); }
    bool                     hasEffect() const { return effect.handle()!=nullptr; }
    // its particles are being born (the effect active)
    bool                     isEmitting() const { return hasEffect() && effect.isEmitting(); }
    // the mesh the effect's particles are born on (its own one), nullptr: no effect
    const MeshObjects::Mesh* effectMesh() const { return hasEffect() ? effect.emitterMesh() : nullptr; }
    // where the effect's emitter is now; false: no effect
    bool                     effectPosition(Tempest::Vec3& out) const;
    MeshObjects::Mesh&       view()       { return mesh; }
    const MeshObjects::Mesh& view() const { return mesh; }
    // the item's mesh, also while the view is released
    const ProtoMesh*         protoMesh() const { return proto; }
    size_t                   nodesCount() const { return mesh.nodesCount(); }

    // the effect at level n, Gothic's oCVisualFX::SetLevel: its KEY_INVEST_n (an arrow: 2 shot, 3 hit); kept for restore()
    void                     setLevel(int32_t level);
    int32_t                  level() const { return keyLevel; }
    // the key the effect plays now, nullptr: its base or no effect
    const VisualFx::Key*     effectKey() const { return hasEffect() ? effect.currentKey() : nullptr; }
    // the effect goes ms of the world's time from now (World::tickCount), the mesh stays; tick() drops it then and
    // restore() does not bring it back. A stuck arrow's: Gothic's arrow fades in 20 s
    void                     fadeEffectIn(uint64_t ms);

    // the mesh and its effect move together
    void                     setObjMatrix(const Tempest::Matrix4x4& m);
    float                    setTextureOwner(const Tempest::Vec3& position, bool spatial);
    // and drops the effect once its fade is due (fadeEffectIn)
    void                     tick(uint64_t dt);

    // the mesh and its effect go, the item stays known; restore() makes both again where the view was last put
    void                     release();
    void                     restore(WorldView& view);

  private:
    // a mesh with no item and no world behind it, never an effect (tests of the slots and streaming, ItemViewTest);
    // an empty mesh: released, restore() makes it of proto
    ItemView(const ProtoMesh* proto, int32_t material, MeshObjects::Mesh&& mesh);
    void                     startEffect();
    void                     stopEffect();
    void                     moveFrom(ItemView& other);

    World*                   world    = nullptr;
    const ProtoMesh*         proto    = nullptr;
    int32_t                  material = 0;
    const VisualFx*          fx       = nullptr;
    int32_t                  keyLevel = 0;
    uint64_t                 fadeAt   = 0;
    Tempest::Matrix4x4       pos      = Tempest::Matrix4x4::mkIdentity();
    MeshObjects::Mesh        mesh;
    Effect                   effect;

  friend struct ItemViewTest;
  friend struct MeshGltfTest;
  };
