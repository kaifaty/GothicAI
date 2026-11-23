#pragma once

#include "graphics/objvisual.h"
#include "vob.h"

class StaticObj : public Vob {
  public:
    StaticObj(Vob* parent, World& world, const asset::VirtualObject& vob, Flags flags);
    ~StaticObj();

  private:
    void  moveEvent() override;
    void  physicsMoveEvent() override;
    bool  hasDynamicBody() const override;
    void  fillAttachmentSnapshot(AttachmentSnapshot& out) const override;
    bool  setMobState(std::string_view scheme,int32_t st) override;

    Tempest::Matrix4x4 visualMatrix() const;

    ObjVisual   visual;
    bool        flameOut = false; // a flame's decal whose lamps the house has put out (graphics/houselamps.h)
    // The authored Vob transform remains the save identity and toggle-OFF pose.
    // DynamicWorld persists the simulated transform in an optional entry.
    DynamicWorld::Item movable;
    std::string scheme;
  };

