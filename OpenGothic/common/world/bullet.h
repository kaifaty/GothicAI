#pragma once

#include <cstdint>

#include "physics/dynamicworld.h"

#include "game/damagecalculator.h"
#include "graphics/meshobjects.h"
#include "graphics/effect.h"
#include "graphics/mesh/hitzones.h"

class World;
class Item;
class Npc;

class Bullet final : public DynamicWorld::BulletCallback {
  public:
    Bullet()=default;
    Bullet(World &owner, const Item &itm, const Tempest::Vec3& pos);
    Bullet(Bullet&&)=default;
    ~Bullet() override;
    Bullet& operator=(Bullet&&)=default;

    enum Flg:uint8_t {
      NoFlags = 0,
      Stopped = 1,
      };

    void     setPosition  (const Tempest::Vec3& p);
    void     setPosition  (float x,float y,float z);

    void     setDirection (const Tempest::Vec3& dir);
    auto     position()  const -> Tempest::Vec3;
    auto     direction() const -> Tempest::Vec3;
    void     setTargetRange(float tgRange);

    void     setView      (MeshObjects::Mesh&&   m);
    void     setView      (Effect&& p);

    bool     isSpell() const;
    int32_t  spellId() const;

    void     setOrigin(Npc* n);
    Npc*     origin() const override;

    void     setTarget(const Npc* n);

    Flg      flags()     const { return flg;  }
    ItemMaterial itemMaterial() const;

    auto     damage() const -> const DamageCalculator::Damage& { return dmg; }
    void     setDamage(DamageCalculator::Damage d) { dmg=d; }

    float    critChance() const { return critCh; }
    void     setCritChance(float v) { critCh=v; }
    float    hitChance() const { return hitCh; }
    void     setHitChance(float v) { hitCh=v; }

    // an arrow aimed by hand (game/archery): no dice, it hits where it flies; draw: how far the string
    // was drawn (the damage), the zone it hit
    void     setPrecise(float draw) { precise=true; drawK=draw; }
    bool     isPrecise() const override { return precise; }
    float    draw() const { return drawK; }
    auto     hitZone() const -> HitZones::Zone { return zone; }
    size_t   impactBone() const { return impactOwner; }
    auto     impactPoint() const -> Tempest::Vec3 { return impactKnown ? impactAt : position(); }

    bool     isFinished() const;
    float    pathLength() const;

    bool     onEffectCollide(Npc& other);

  protected:
    bool     stopsAtWorldHit() const override;
    void     onWorldHit(const DynamicWorld::BulletWorldHit& hit) override;
    void     onSpellWorldHit(const DynamicWorld::BulletWorldHit& hit) override;
    void     onStop() override;
    void     onMove() override;
    void     onCollide(asset::MaterialGroup matId) override;
    bool     onCollide(Npc& other) override;
    bool     onCollidePrecise(Npc& other, const Tempest::Vec3& from, const Tempest::Vec3& to) override;

  private:
    DynamicWorld::BulletBody* obj=nullptr;
    World*                    wrld=nullptr;
    Npc*                      ow=nullptr;

    DamageCalculator::Damage  dmg        = {};
    float                     hitCh      = 1.f;
    float                     critCh     = 0.f;
    bool                      precise    = false;
    float                     drawK      = 1.f;
    HitZones::Zone            zone       = HitZones::None;
    bool                      landed     = false; // came down to the ground (a miss)
    size_t                    impactOwner = size_t(-1);
    Tempest::Vec3             impactAt;
    bool                      impactKnown = false;
    bool                      spellPhysicsApplied = false;

    MeshObjects::Mesh         view;
    std::string               visualName;
    Effect                    vfx;

    uint8_t                   material=0;
    Flg                       flg=NoFlags;

    void updateMatrix();
    void applySpellPhysics(Npc* target);
  };

