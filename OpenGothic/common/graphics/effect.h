#pragma once

#include "world/objects/pfxemitter.h"
#include "world/objects/globalfx.h"
#include "world/objects/sound.h"
#include "graphics/lightgroup.h"
#include "graphics/meshobjects.h"
#include "graphics/visualfx.h"

#include "game/rpgvocabulary.h"

#include <memory>
#include <optional>

class World;
class Bullet;
class Npc;
class Pose;
class Skeleton;

class Effect final {
  public:
    Effect() = default;
    Effect(Effect&&) = default;
    Effect(PfxEmitter&& pfx, std::string_view node);
    // an animation event's effect (World::animationPfx): its emitter on the node and its particles' own light
    Effect(World& owner, const ParticleFx* decl, std::string_view node);
    Effect(const VisualFx& vfx, World& owner, const Npc& src,           SpellFxKey key = SpellFxKey::Count);
    Effect(const VisualFx& vfx, World& owner, const Tempest::Vec3& pos, SpellFxKey key = SpellFxKey::Count);
    ~Effect();

    Effect&  operator = (Effect&&) = default;
    bool     is(const VisualFx& vfx) const;
    const VisualFx* handle() const { return root; }
    // the mesh its particles are born on (setMesh), where it is now (its last matrix), where its particles are emitted
    // from and whether they are
    const MeshObjects::Mesh* emitterMesh() const { return meshEmitter; }
    Tempest::Vec3   position() const { return {pos.at(3,0),pos.at(3,1),pos.at(3,2)}; }
    bool            emitterPosition(Tempest::Vec3& out) const { return pfx.position(out); }
    bool            isEmitting() const { return pfx.isActive(); }
    // the key it plays now (setKey), nullptr: its base
    const VisualFx::Key* currentKey() const { return key; }
    // the light of its own particles (particles.d "light") as it gave it to the world's: the range, the colour and
    // their keys, whether the keys go round and where it put the light last; false - it set up none. A headless world
    // has no lights, the effect still keeps what it gave
    struct OwnLight {
      asset::LightPreset preset;
      bool               loop = false;
      Tempest::Vec3      at;
      };
    bool            ownLight(OwnLight& out) const;

    void     tick(uint64_t dt);

    void     setActive(bool e);
    void     setLooped(bool l);

    void     setTarget   (const Npc* npc);
    void     setOrigin   (Npc* npc);

    void     setObjMatrix(const Tempest::Matrix4x4& mt);
    void     setKey      (World& owner, SpellFxKey key, int32_t keyLvl=0);
    // skin: the mesh's own bone matrices when it is on a render rig (MdlVisual), else the pose's (bindAttaches)
    void     setMesh     (const MeshObjects::Mesh* mesh, const std::vector<Tempest::Matrix4x4>* skin = nullptr);
    void     setBullet   (Bullet* b, World& owner);
    void     setSpellId  (int32_t splId, World& owner);

    uint64_t effectPrefferedTime() const;
    bool     isAlive() const;
    void     setPhysicsDisable();

    void     bindAttaches  (const Pose& pose, const Skeleton& to);
    static void onCollide  (World& owner, const VisualFx* root, const Tempest::Vec3& pos, Npc* npc, Npc* other, int32_t splId);

  private:
    void               syncAttaches(const Tempest::Matrix4x4& pos);
    void               syncAttachesSingle(const Tempest::Matrix4x4& inPos);
    void               setupLight(World& owner);
    bool               setupOwnLight(World& owner);
    // the light's settings go through these, which keep what the own light got (ownLight)
    void               giveKeysOnce(bool once);
    void               givePosition(const Tempest::Vec3& at);
    void               setupPfx(World& owner);
    void               setupSfx(World& owner);
    void               setupCollision(World& owner);

    const VisualFx::Key*  key  = nullptr;
    const VisualFx*       root = nullptr;

    PfxEmitter            pfx;
    Sound                 sfx;
    GlobalFx              gfx;
    LightGroup::Light     light;

    Npc*                  origin = nullptr;
    const Npc*            target = nullptr;

    Bullet*               bullet = nullptr;
    int32_t               splId  = 0;

    std::string_view      nodeSlot;
    size_t                boneId   = size_t(-1);
    const Skeleton*       skeleton = nullptr;
    const Pose*           pose     = nullptr;

    const MeshObjects::Mesh* meshEmitter = nullptr;
    const std::vector<Tempest::Matrix4x4>* meshSkin = nullptr;
    bool                  meshPfx   = false; // the particle effect is born on the mesh
    const ParticleFx* ownPfx = nullptr; // the particle effect of this one: its own light (particles.d "light")
    Tempest::Vec3         lightPos;          // where its light stands: its node with the own light's "at"
    std::optional<OwnLight> givenLight;      // what setupOwnLight and the settings since gave the own light

    Tempest::Matrix4x4    pos;
    Tempest::Vec3         selfRotation;

    bool                  active    = false;
    bool                  looped    = false;
    bool                  noPhysics = false;

    std::unique_ptr<Effect> next;
  };

