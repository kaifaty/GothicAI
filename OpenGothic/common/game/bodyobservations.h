#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class Npc;

// Passive CPU observations shared by the native runner and the ordinary AI bridge. All entry
// points belong to the simulation thread. This object never acquires control of an NPC.
class BodyObservations final {
  public:
    struct Config final {
      uint64_t periodMs = 50;
      size_t maxActors = 4;
      size_t queueCapacity = 256;
      float rayRangeCm = 300;
      };

    // Generation changes invalidate runtime entity IDs, history, events and queued rows.
    static void reset(uint64_t generation);
    static void configure(const Config& config);
    static void watch(const std::vector<Npc*>& actors);
    // Same-period subscription edit: retain identity, history and rows of remaining actors.
    static void updateWatch(const std::vector<Npc*>& actors);
    static void disable();
    static bool enabled();
    static size_t watchedCount();
    static size_t queuedCount();
    static size_t queueCapacity();
    static uint64_t samplePeriodMs();
    static void setSink(std::function<void(std::string_view)> sink);
    static uint64_t lastStepCostUs();
    static size_t lastStepActorCount();
    static uint64_t entityId(const Npc& actor);
    static uint64_t sessionEpoch();
    static uint64_t worldEpoch();
    static Npc* findEntity(uint64_t id);
    static bool watched(const Npc& actor);
    // Recorder-only causal view. Clones the sampled actor's history, reads current native
    // state before the executor, and leaves live sampling/IPC sequence and actor history intact.
    static std::string captureDecision(Npc& actor);
    static uint64_t latestSequence(const Npc& actor);
    static uint64_t latestGameMs(const Npc& actor);
    static bool latestPeriodic(const Npc& actor);
    static uint64_t latestWallMs(const Npc& actor);
    struct SentObservation final {
      uint64_t sequence = 0, gameMs = 0, wallMs = 0;
      };
    static SentObservation latestSent(const Npc& actor);
    static void forget(const Npc* actor);
    // Explicit teleport/fixture relocation; ordinary MoveAlgo correction is not a discontinuity.
    static void discontinuity(const Npc& actor);

    static void beginStep(uint64_t simStep);
    static uint64_t currentStep();
    // Call once after world movement and animation updates, before benchmark mutations.
    // A terminal episode may request a final sample between periodic buckets. The native
    // builder still reads actual state, and force never duplicates an already sampled game time.
    static void sampleAll(bool force = false);
    static void sample(Npc& actor, uint64_t gameMs, bool force = false);
    // Native hooks carry a fixed event vocabulary; arbitrary debug fields never enter actor input.
    static void noteEvent(const Npc& actor, const Npc* other, std::string_view event, uint64_t gameMs);

    // The packet is valid JSON even when empty. Rows share one generation-wide sequence;
    // dropped counters account for bounded-queue losses. Draining never advances game time.
    static std::string drainJson();
    // Ordinary bridge only: acknowledge the last drained packet after transport accepts it.
    static void markDrainedSent();
    static std::string statsJson();

    // Decision-relevant fields of one queued row, kept beside the serialized queue. The runner's
    // arena_advance stops on them exactly where a per-tick client would make its next decision.
    struct QueuedRow final {
      uint64_t entityId = 0, sessionEpoch = 0, worldEpoch = 0, controlGeneration = 0;
      uint64_t enqueueIndex = 0; // queue cursor, including rows removed by subscription edits
      SentObservation observation;
      bool periodic = false, canDecide = false, actionMaskValid = false;
      std::array<bool,12> actionMask = {};
      };
    // Rows queued in this generation so far, and the still-queued ones queued after `total`.
    static uint64_t queuedTotal();
    static std::vector<QueuedRow> queuedSince(uint64_t total);
  };
