#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class Npc;

class BenchProbe {
  public:
    virtual ~BenchProbe() = default;

    static void install(BenchProbe* probe) { instance = probe; }
    static BenchProbe* get() { return instance; }

    virtual bool active() const = 0;
    virtual bool orderedVisibility() const = 0;
    virtual int bodyBlockVariant(const Npc& npc, int fallback) const = 0;
    virtual void destructionBreak(uintptr_t body, size_t pieces, float milliseconds, bool remnant) = 0;
    virtual void destructionRetire(uintptr_t body, size_t piece, bool expired, float observerDistanceCm) = 0;
    virtual std::string bodyJson(std::string_view text) const = 0;
    virtual std::string_view who(const Npc* npc) const = 0;
    virtual void ragdollBirthPeak(const Npc& npc, const Npc* culprit, const std::string& fields) = 0;

  private:
    inline static BenchProbe* instance = nullptr;
  };
