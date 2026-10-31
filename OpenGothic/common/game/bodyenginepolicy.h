#pragma once

#include "bodypolicy.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

// Per-lease P30 driver. The immutable network is shared by all leases.
class BodyEnginePolicy final {
  public:
    using Apply = std::function<std::string(int,const rapidjson::Value&)>;
    // P30 waits out a passive opponent. After this long without a swing by the target
    // and without damage either way, its choice is limited to approach, turn and attacks,
    // each attack tried once before any repeats.
    static constexpr uint64_t StallGameMs = 4000;
    static void initialize();
    static std::string netStatusJson();
    static bool requested(const rapidjson::Value& request);
    static const char* validateAcquire(const rapidjson::Value& request);

    BodyEnginePolicy(uint64_t session,uint64_t world,uint64_t entity,uint64_t generation,
                     uint64_t initialGameMs,uint64_t minimumSequence);
    // An empty result retains ownership; otherwise use the ordinary release path.
    std::string process(const rapidjson::Value& row,uint64_t nowGameMs,const Apply& apply);
    std::string report(bool force=false);

  private:
    using Clock = std::chrono::steady_clock;
    BodyPolicyRuntime runtime;
    uint64_t session,world,minimumSequence,streamSequence=0,lastSequence=0,acceptedSimStep=0;
    unsigned transientRejections=0;
    bool needsReset=false;
    std::optional<uint64_t> engagedGameMs;
    std::optional<int64_t> ownHp, targetHp;
    uint32_t stallAttacks=0;
    BodyNet::Identity identity;
    uint64_t decisions=0;
    std::array<uint64_t,BodyNet::actionCount> selectedActions{},acceptedActions{};
    uint64_t hiddenResets=0;
    std::map<std::string,uint64_t> fallbacks;
    std::vector<double> stepTimes;
    Clock::time_point reportAt=Clock::now();
  };
