#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include "world/collisionzone.h"
#include "physics/dynamicworld.h"
#include "graphics/meshobjects.h"
#include "graphics/pfx/pfxslot.h"

class World;
class ParticleFx;
class PfxObjects;
class PfxEmitterMesh;

class PfxEmitter {
  public:
    PfxEmitter()=default;
    PfxEmitter(World& world, std::string_view name);
    PfxEmitter(World& world, const ParticleFx* decl);
    PfxEmitter(PfxObjects& obj, const ParticleFx* vob);
    PfxEmitter(World& world, const asset::VirtualObject& vob);
    ~PfxEmitter();
    PfxEmitter(PfxEmitter&&);
    PfxEmitter& operator=(PfxEmitter&& b);
    PfxEmitter(const PfxEmitter&)=delete;

    bool     isEmpty() const { return slot.isEmpty() && cpuDecl==nullptr; }
    void     setPosition (float x,float y,float z);
    void     setPosition (const Tempest::Vec3& pos);
    void     setDirection(const Tempest::Matrix4x4& pos);
    void     setObjMatrix(const Tempest::Matrix4x4& mt);
    bool     renderPosition(Tempest::Vec3& out) const;
    void     setActive(bool act);
    bool     isActive() const;
    void     setLooped(bool loop);
    // skin: the mesh's own bone matrices (a render rig's palette, graphics/mesh/renderrig), else the pose's
    void     setMesh(const MeshObjects::Mesh* mesh, const Pose* pose,
                     const std::vector<Tempest::Matrix4x4>* skin = nullptr);

    void     setTarget(const Npc* tg);

    void     setPhysicsEnable (World& physic, std::function<void(Npc& npc)> cb);
    void     setPhysicsDisable();

    uint64_t effectPrefferedTime() const;
    bool     isAlive() const;

  private:
    // The headless adapter retains the declared effect's gameplay collision zone;
    // it never constructs a PfxObjects bucket or emits rendering particles.
    const ParticleFx* cpuDecl = nullptr;
    World*            cpuWorld = nullptr;
    Tempest::Vec3    cpuPosition;
    bool              cpuActive = false;
    bool              cpuLooped = false;
    uint64_t          cpuStarted = 0;
    uint64_t          cpuStopped = 0;

    CollisionZone      zone;
    // the place in the particle bucket; declared last, so it is freed first, as before
    PfxSlot            slot;
  };

