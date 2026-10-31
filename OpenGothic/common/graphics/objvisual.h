#pragma once

#include "world/objects/pfxemitter.h"
#include "world/objects/vobbundle.h"
#include "physics/physicmesh.h"
#include "graphics/mdlvisual.h"

class Npc;
class World;
class ClothVisual;

class ObjVisual {
  public:
    ObjVisual();
    ObjVisual(ObjVisual&& other);
    ObjVisual& operator = (ObjVisual&& other);
    ~ObjVisual();

    void save(Serialize& fout, const Interactive& mob) const;
    void load(Serialize& fin,  Interactive &mob);

    void setVisual(const zenkit::IItem& visual, World& world, bool staticDraw);
    void setVisual(const asset::VirtualObject& visual, World& world, bool staticDraw, bool enablePhysics = true);
    void setObjMatrix(const Tempest::Matrix4x4& obj);
    // Preload the cached cut surfaces and their draw instances before impacts.
    void setupFracture(World& world, DynamicWorld::Item& body);
    bool renderPosition(Tempest::Vec3& out) const;

    void setSlotItem(MeshObjects::Mesh&& itm, std::string_view slot);

    void setInteractive(Interactive* it);

    const Animation::Sequence* startAnimAndGet(std::string_view name, uint64_t tickCount, bool force = false);
    bool hasAnim(std::string_view name) const;

    bool updateAnimation(Npc* npc, Interactive* mobsi, World& world, uint64_t dt, bool force);
    void processLayers(World& world);
    void syncPhysics();

    const ProtoMesh* protoMesh() const;
    const Tempest::Matrix4x4& bone(size_t i) const;
    std::string animNames() const;

  private:
    enum Type : uint8_t {
      M_None = 0,
      M_Mdl,
      M_Mesh,
      M_Pfx,
      M_Bundle,
      };

    struct Mdl {
      PhysicMesh       physic;
      MdlVisual        view;
      const ProtoMesh* proto = nullptr;
      };

    struct Mesh {
      PhysicMesh        physic;
      MeshObjects::Mesh view;
      const ProtoMesh*  proto = nullptr;
      };

    union {
      Mdl        mdl;
      Mesh       mesh;
      PfxEmitter pfx;
      VobBundle  bundle;
      };
    Type type = M_None;

    struct FractureView;
    std::shared_ptr<FractureView> fracture;
    std::unique_ptr<ClothVisual> cloth;
    Tempest::Matrix4x4 renderMatrix = Tempest::Matrix4x4::mkIdentity();

    void cleanup();
    void setType(Type t);
  };

