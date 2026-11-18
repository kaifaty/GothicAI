#pragma once

#include "aiqueue.h"
#include <string_view>

class Interactive;

// Passive engine observations, installed only by an explicitly requested day probe.
// This interface deliberately knows neither the VM nor the activity executor.
class NpcActivityProbe {
  public:
    virtual ~NpcActivityProbe() = default;
    inline static NpcActivityProbe* instance = nullptr;
    virtual void phaseBegin(const Npc* npc, std::string_view symbol) = 0;
    virtual void phaseEnd(int result) = 0;
    virtual void random(const Npc* npc, uint32_t bound, uint32_t result, bool script) = 0;
    virtual void queued(const Npc* npc, const AiQueue::AiAction& action) = 0;
    virtual void point(const Npc& npc, const WayPoint* from, const WayPoint* to) = 0;
    virtual void mob(const Npc& npc, const Interactive& object, bool occupied) = 0;

    struct Phase {
      NpcActivityProbe* probe = instance;
      int result = 0;
      Phase(const Npc* npc, std::string_view symbol) { if(probe) probe->phaseBegin(npc,symbol); }
      ~Phase() { if(probe) probe->phaseEnd(result); }
      };
  };
