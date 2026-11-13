#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <mutex>
#include <vector>

#include "graphics/meshobjects.h"
#include "graphics/pfx/pfxmotion.h"

class Material;
class ParticleFx;
class ParticleTarget;
class PfxBucket;
class PfxObjects;
class Pose;

namespace asset { struct VirtualObject; }

// An emitter's place in a particle bucket: the bucket, the emitter's index in it and the effect's shape mesh.
// The platform core of the game's PfxEmitter; the bucket itself owns the nested emitters (ppsCreateEm) as slots.
class PfxSlot final {
  public:
    PfxSlot()=default;
    PfxSlot(PfxObjects& owner, const ParticleFx* decl);
    // a layer emitter of a vob: its effect and the flame's signal (nullptr - none)
    PfxSlot(PfxObjects& owner, const ParticleFx& decl, const PfxMotion::Flicker* flicker);
    // a decal vob
    PfxSlot(PfxObjects& owner, const Material& mat, const asset::VirtualObject& vob);
    ~PfxSlot();
    PfxSlot(PfxSlot&&);
    PfxSlot& operator=(PfxSlot&& b);
    PfxSlot(const PfxSlot&)=delete;

    bool     isEmpty() const { return bucket==nullptr; }
    // the owner's lock: the slot's own calls take it too (a recursive mutex)
    std::unique_lock<std::recursive_mutex> lock() const;
    const ParticleFx& decl() const;
    Tempest::Vec3 position() const;

    void     setPosition (float x,float y,float z);
    void     setPosition (const Tempest::Vec3& pos);
    void     setDirection(const Tempest::Matrix4x4& pos);
    void     setShapeMatrix(const Tempest::Matrix4x4& mt);
    bool     renderPosition(Tempest::Vec3& out) const;
    void     setActive(bool act);
    bool     isActive() const;
    void     setLooped(bool loop);
    void     setMesh(const MeshObjects::Mesh* mesh, const Pose* pose,
                     const std::vector<Tempest::Matrix4x4>* skin = nullptr);
    void     setTarget(const ParticleTarget* tg);
    bool     isAlive() const;

  private:
    PfxBucket* bucket = nullptr;
    size_t     id     = size_t(-1);

    MeshObjects::Mesh  shpMesh;

  friend class PfxBucket;
  };
