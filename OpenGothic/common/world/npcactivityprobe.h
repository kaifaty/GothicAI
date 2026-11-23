#pragma once

#include "aiqueue.h"
#include <string_view>

class Interactive;
namespace ActivityEvents { enum class Stage : uint8_t; }

// Passive engine observations, installed only for a requested trace or day probe.
class NpcActivityProbe {
  public:
    virtual ~NpcActivityProbe() = default;
    inline static NpcActivityProbe* instance = nullptr;
    virtual void phaseBegin(const Npc* npc, std::string_view symbol) = 0;
    virtual void phaseEnd(int result) = 0;
    // The current phase ran from its record instead of the script (counters only, not the trace).
    virtual void phaseRecord() {}
    virtual void random(const Npc* npc, uint32_t bound, uint32_t result, bool script) = 0;
    virtual void queued(const Npc* npc, const AiQueue::AiAction& action) = 0;
    virtual void point(const Npc& npc, const WayPoint* from, const WayPoint* to) = 0;
    virtual void mob(const Npc& npc, const Interactive& object, bool occupied) = 0;
    virtual void activityTransition(const Npc&, uint16_t, ActivityEvents::Stage) {}
    virtual void physicsNoise(const Npc&, const Npc*, bool) {}

    struct Phase {
      NpcActivityProbe* probe = instance;
      int result = 0;
      Phase(const Npc* npc, std::string_view symbol) { if(probe) probe->phaseBegin(npc,symbol); }
      ~Phase() { if(probe) probe->phaseEnd(result); }
      void fromRecord() { if(probe) probe->phaseRecord(); }
      };
  };
