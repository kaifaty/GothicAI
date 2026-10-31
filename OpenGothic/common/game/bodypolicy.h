#pragma once

#include "nets/smallnet.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Immutable P30 actor weights. Recurrent state belongs to BodyPolicyRuntime.
class BodyNet final {
  public:
    static constexpr size_t featureCount = 184, hiddenSize = 256, actionCount = 12;
    using Features = std::array<float,featureCount>;
    using Mask = std::array<bool,actionCount>;
    using Identity = std::array<uint64_t,4>;
    struct Step final {
      std::array<float,actionCount> logits{};
      std::vector<float> hidden;
      };

    explicit BodyNet(const std::string& path);
    const std::string& checkpointSha256() const { return checkpoint; }
    static std::vector<std::string> featureNames();
    static Identity actorIdentity(const rapidjson::Value& row);
    static Mask actionMask(const rapidjson::Value& row);
    Features packFeatures(const rapidjson::Value& row, uint64_t sequenceDtGameMs) const;
    Step step(const Features& features, const Mask& mask, const std::vector<float>& hidden = {}) const;

  private:
    std::string checkpoint;
    SmallNet::Linear encoder, gruInput, gruHidden, head, output;
    Features mean{}, scale{};
  };

class BodyPolicyRuntime final {
  public:
    struct Result final {
      std::string status = "fallback", reason, error;
      std::optional<int> actionId, selectedActionId;
      bool hiddenReset = false, modelProcessed = false, memoryOnly = false, newDecision = false, accepted = false;
      std::optional<uint64_t> sequenceDtGameMs;
      uint64_t streamSeq = 0, observationSeq = 0, gameMs = 0;
      BodyNet::Features features{};
      std::array<float,BodyNet::actionCount> logits{};
      BodyNet::Mask effectiveActionMask{}, rawActionMask{};
      };

    explicit BodyPolicyRuntime(const BodyNet& network, uint64_t maxAgeGameMs = 100,
                               uint64_t maxDtGameMs = 100, bool allowStaleMemoryOnly = false);
    void beginEpisode(uint64_t sessionEpoch, uint64_t worldEpoch, uint64_t initialGameMs = 0);
    Result endEpisode(uint64_t sessionEpoch, uint64_t worldEpoch);
    void resetActor(const BodyNet::Identity& identity);
    Result process(const rapidjson::Value& request);
    Result arbitrate(const rapidjson::Value& row, uint64_t streamSeq, uint64_t maxAnchorAgeGameMs = 67) const;
    std::vector<float> hiddenState(const rapidjson::Value& row) const;

  private:
    struct ActorState final {
      uint64_t sequence = 0, gameMs = 0;
      std::vector<float> hidden;
      std::optional<std::array<float,BodyNet::actionCount>> scores;
      Result response;
      rapidjson::Document observation;
      };
    const BodyNet& network;
    uint64_t maxAgeGameMs, maxDtGameMs;
    bool allowStaleMemoryOnly, episodeClosed = false;
    std::map<BodyNet::Identity,ActorState> states;
    std::map<uint64_t,BodyNet::Identity> active;
    std::map<uint64_t,std::array<uint64_t,3>> actorClocks;
    std::optional<std::array<uint64_t,2>> expectedEpochs;
    std::optional<uint64_t> initialGameMs;
  };
