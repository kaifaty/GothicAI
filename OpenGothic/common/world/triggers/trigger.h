#pragma once

#include "abstracttrigger.h"

class Trigger : public AbstractTrigger {
  public:
    Trigger(Vob* parent, World& world, const asset::VirtualObject& data, Flags flags);

    void onTrigger(const TriggerEvent& evt) override;
  };
