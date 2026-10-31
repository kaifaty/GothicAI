#pragma once

#include "abstracttrigger.h"

class MoverControler : public AbstractTrigger {
  public:
    MoverControler(Vob* parent, World& world, const asset::VMoverController& data, Flags flags);

  private:
    void onTrigger(const TriggerEvent& evt) override;
    void onUntrigger(const TriggerEvent& evt) override;

  public:
    asset::MoverMessageType message;
    uint32_t                 key;
  };


