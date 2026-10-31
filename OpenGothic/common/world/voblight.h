#pragma once

#include "world/objects/vob.h"
#include "graphics/lightgroup.h"

class VobLight : public Vob {
  public:
    VobLight(Vob* parent, World& world, const asset::VLight& vob, Flags flags);

  private:
    void  moveEvent() override;
    void  fillAttachmentSnapshot(AttachmentSnapshot& out) const override;

    LightGroup::Light light;
  };

