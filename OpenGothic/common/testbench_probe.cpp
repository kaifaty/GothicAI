#include "testbench.h"
#include "benchprobe.h"

namespace {
class Probe final : public BenchProbe {
  public:
    bool active() const override { return TestBench::isActive(); }
    bool orderedVisibility() const override { return TestBench::orderedVisibility(); }
    int bodyBlockVariant(const Npc& npc, int fallback) const override {
      return TestBench::bodyBlockVariant(npc,fallback);
      }
    void destructionBreak(uintptr_t body, size_t pieces, float milliseconds, bool remnant) override {
      TestBench::destructionBreak(body,pieces,milliseconds,remnant);
      }
    void destructionRetire(uintptr_t body, size_t piece, bool expired, float observerDistanceCm) override {
      TestBench::destructionRetire(body,piece,expired,observerDistanceCm);
      }
    std::string bodyJson(std::string_view text) const override { return TestBench::bodyJson(text); }
    std::string_view who(const Npc* npc) const override { return TestBench::who(npc); }
    void ragdollBirthPeak(const Npc& npc, const Npc* culprit, const std::string& fields) override {
      TestBench::ragdollBirthPeak(npc,culprit,fields);
      }
  };
}

void TestBench::install() {
  static Probe probe;
  BenchProbe::install(&probe);
  }
