#pragma once

#include "abstracttrigger.h"

class MessageFilter : public AbstractTrigger {
  public:
    MessageFilter(Vob* parent, World& world, const asset::VMessageFilter& data, Flags flags);

  private:
    void onTrigger(const TriggerEvent& evt) override;
    void onUntrigger(const TriggerEvent& evt) override;

    void exec(asset::MessageFilterAction type);

    asset::MessageFilterAction onUntriggerA;
    asset::MessageFilterAction onTriggerA;
  };
