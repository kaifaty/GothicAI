#pragma once

#include <Tempest/CommandBuffer>
#include <Tempest/ComputePipeline>
#include <Tempest/StorageBuffer>
#include <Tempest/Texture2d>
#include <array>
#include <string>

#include "benchprobe.h"

namespace Tempest { class ComputePipeline; }

class HiZHistoryVerify final {
  public:
    void compare(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const Tempest::ComputePipeline& pipeline,
                 const BenchProbe::HistoryVerifyRequest& request,
                 const Tempest::Texture2d& earlyDepth, const Tempest::Texture2d& earlyOwners,
                 const Tempest::Texture2d& referenceDepth, const Tempest::Texture2d& referenceOwners,
                 const Tempest::Texture2d& enabledDepth, const Tempest::Texture2d& enabledOwners,
                 const Tempest::StorageBuffer& flags, uint32_t clusters, bool reset);
    bool finish();

  private:
    static constexpr uint32_t PairCount = 248;
    using Counters = std::array<uint32_t, 16>;
    struct Sample {
      std::string segment;
      uint32_t    frame = 0;
      uint64_t    elapsedMs = 0;
      uint32_t    width = 0;
      uint32_t    height = 0;
      bool        reset = false;
      };

    Tempest::StorageBuffer       results, seen;
    std::array<Sample,PairCount>  samples;
    uint32_t                     recorded = 0;
  };

class BoughHiZVerify final {
  public:
    void compare(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const BenchProbe::HistoryVerifyRequest& request,
                 const Tempest::Texture2d& referenceDepth, const Tempest::Texture2d& referenceOwners,
                 const Tempest::Texture2d& enabledDepth, const Tempest::Texture2d& enabledOwners, bool reset,
                 bool optimized);
    bool finish();

  private:
    static constexpr uint32_t MaxPairs = 248;
    using Counters = std::array<uint32_t, 7>;
    struct Sample {
      std::string segment;
      uint32_t frame = 0;
      uint64_t elapsedMs = 0;
      uint32_t width = 0, height = 0;
      bool reset = false;
      bool optimized = false;
      };

    Tempest::ComputePipeline pipeline;
    Tempest::StorageBuffer results;
    std::array<Sample, MaxPairs> samples;
    uint32_t recorded = 0;
    bool failed = false;
  };
