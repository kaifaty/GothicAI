#pragma once

#include <Tempest/Vec>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "worldchunksruntime.h"

// Desired presentation residency only. World logic and every presentation
// consumer remain loaded in this step; nothing here changes the world mesh.
class ChunkResidency {
  public:
    enum class State : uint8_t { Outside, Far, Near };

    struct Stats {
      size_t   near = 0;
      size_t   far = 0;
      size_t   outside = 0;
      size_t   transitionsLastSecond = 0;
      uint64_t transitionsNear = 0;
      uint64_t transitionsFar = 0;
      uint64_t totalTransitions = 0;
      uint64_t peakNear = 0;
      double   updateMs = 0;
      double   workMs = 0;
      };

    using Clock = std::chrono::steady_clock;
    using Listener = std::function<void(ChunkId,State,State)>;
    // A job performs a cooperative slice before deadline and returns true
    // when complete. Consumers must yield by the deadline (no blocking I/O).
    using Work = std::function<bool(Clock::time_point deadline)>;

    // Constants will be tuned with the flythrough measurements. Distances
    // are centimetres; Chebyshev radii are in 64 m cells.
    static constexpr int32_t NearRadius = 2;
    static constexpr int32_t NearExitRadius = 3;
    static constexpr float FarExitMargin = float(ChunkId::Size);
    static constexpr auto WorkBudget = std::chrono::milliseconds(2);

    explicit ChunkResidency(const ChunkTable& table);
    static std::unique_ptr<ChunkResidency> create(const ChunkTable& table);

    void update(const Tempest::Vec3& hero, const Tempest::Vec3& camera, float zFar);
    State desired(size_t index) const { return states.at(index); }
    State desired(ChunkId id) const;
    bool actualLoaded(size_t index) const { return index<chunks.entries.size(); }
    const ChunkTable& table() const { return chunks; }
    const Stats& stats() const { return counters; }

    uint64_t subscribe(Listener listener);
    void unsubscribe(uint64_t subscription);
    void enqueue(Work work);

  private:
    const ChunkTable& chunks;
    std::vector<State> states;
    Stats counters;
    uint64_t nextSubscription = 0;
    std::map<uint64_t,std::shared_ptr<Listener>> listeners;
    std::deque<Work> work;
    std::deque<std::pair<Clock::time_point,size_t>> recentTransitions;
  };
