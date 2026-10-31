#pragma once

#include "graphics/objvisual.h"
#include "vob.h"

class StaticObj : public Vob {
  public:
    StaticObj(Vob* parent, World& world, const asset::VirtualObject& vob, Flags flags);

  private:
    void  moveEvent() override;
    void  physicsMoveEvent() override;
    bool  hasDynamicBody() const override;
    void  fillAttachmentSnapshot(AttachmentSnapshot& out) const override;
    bool  setMobState(std::string_view scheme,int32_t st) override;

    ObjVisual   visual;
    // The authored Vob transform remains the save identity and toggle-OFF pose.
    // DynamicWorld persists the simulated transform in an optional entry.
    DynamicWorld::Item movable;
    std::string scheme;
  };

