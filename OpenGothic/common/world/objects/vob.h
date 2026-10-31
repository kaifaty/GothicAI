#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>
#include <string>
#include <vector>

#include "asset/worldtypes.h"

class World;
class Serialize;

class Vob {
  public:
    struct AttachmentSnapshot {
      uint32_t zenId = uint32_t(-1), parentZenId = uint32_t(-1);
      std::string visual;
      bool isLight = false, hasRenderedPosition = false;
      Tempest::Matrix4x4 transform;
      Tempest::Vec3 renderedPosition;
      };

    enum Flags:uint8_t {
      None    = 0,
      Startup = 0x1 << 0,
      Static  = 0x1 << 1,
      };
    friend Flags operator | (Flags a, Flags b) { return Flags(uint8_t(a) | uint8_t(b)); }
    friend Flags operator & (Flags a, Flags b) { return Flags(uint8_t(a) & uint8_t(b)); }
    friend Flags operator ~ (Flags a)          { return Flags(~uint8_t(a));             }

    Vob(World& owner);
    Vob(Vob* parent, World& owner, const asset::VirtualObject& vob, Flags flags);
    virtual ~Vob();
    static std::unique_ptr<Vob> load(Vob* parent, World& world, const asset::VirtualObject& vob, Flags flags);

    void          saveVobTree(Serialize& fin) const;
    virtual void  save(Serialize& fout) const;

    void          loadVobTree(Serialize& fin);
    virtual void  load(Serialize& fin);

    Tempest::Vec3 position() const;
    auto          transform() const -> const Tempest::Matrix4x4& { return hasPhysicsPos ? physicsPos : pos; }
    void          setGlobalTransform(const Tempest::Matrix4x4& p);

    auto          localTransform() const -> const Tempest::Matrix4x4& { return local; }
    void          setLocalTransform(const Tempest::Matrix4x4& p);
    virtual bool  setMobState(std::string_view scheme, int32_t st);

    virtual bool  isDynamic() const;
    virtual float extendedSearchRadius() const;
    bool          attachmentSnapshot(uint32_t zenId, std::vector<AttachmentSnapshot>& out) const;

  protected:
    World&                            world;
    asset::VirtualObjectType         vobType     = asset::VirtualObjectType::UNKNOWN;
    uint32_t                          vobObjectID = uint32_t(-1);

    virtual void  moveEvent();
    virtual void  physicsMoveEvent();
    virtual bool  hasDynamicBody() const;
    virtual void  fillAttachmentSnapshot(AttachmentSnapshot& out) const;
    void          setPhysicsTransform(const Tempest::Matrix4x4& p);

  private:
    friend struct WorldDataContract;
    std::vector<std::unique_ptr<Vob>> child;

    // Physics poses are transient: pos/local retain the authored save identity.
    Tempest::Matrix4x4                pos, local, physicsPos;
    bool                              hasPhysicsPos = false;
    bool                              layerObject = false;
    Vob*                              parent = nullptr;

    void          recalculateTransform();
    void          collectAttachmentSnapshots(std::vector<AttachmentSnapshot>& out) const;
  };

