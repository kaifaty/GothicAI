#pragma once

#include <Tempest/Vec>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "worldchunksruntime.h"

// Desired presentation residency. World logic remains loaded independently.
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
      std::vector<double> publicationTimesMs;
      std::vector<double> farPublicationTimesMs;
      uint64_t published = 0;
      uint64_t released = 0;
      size_t   pending = 0;
      size_t   landscapePageUsedBytes = 0;
      size_t   landscapePageFreeBytes = 0;
      size_t   landscapePageRetiredBytes = 0;
      size_t   landscapePageCount = 0;
      size_t   vobNear = 0, vobFar = 0, vobOutside = 0;
      uint64_t vobCreated = 0, vobReleased = 0, vobDrawInstances = 0;
      size_t   vobPending = 0, vobNearMissing = 0;
      double   vobPublicationMs = 0, vobCreateMs = 0;
      double   vobNearMissingSeconds = 0, vobReadyMs = -1;
      std::string vobProbeFields() const;
      };

    using Clock = std::chrono::steady_clock;
    using Listener = std::function<void(ChunkId,State,State)>;
    using FocusListener = std::function<void()>;
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
    State desired(ChunkId id, State previous) const;
    // Returns size_t(-1) for cells without a landscape entry.
    size_t indexOf(ChunkId id) const;
    uint32_t textureDistance(ChunkId id) const;
    bool actualLoaded(size_t index) const { return index<chunks.entries.size(); }
    const ChunkTable& table() const { return chunks; }
    const Stats& stats() const { return counters; }
    bool idle() const { return work.empty(); }

    uint64_t subscribe(Listener listener);
    uint64_t subscribeFocus(FocusListener listener);
    void unsubscribe(uint64_t subscription);
    void enqueue(Work work);
    void recordPublication(double ms, bool far);
    void recordRelease() { ++counters.released; }
    void setPending(size_t count) { counters.pending=count; }
    Stats& vobStats() { return counters; }
    void setLandscapePages(size_t used, size_t free, size_t retired, size_t count) {
      counters.landscapePageUsedBytes=used;
      counters.landscapePageFreeBytes=free;
      counters.landscapePageRetiredBytes=retired;
      counters.landscapePageCount=count;
      }

  private:
    const ChunkTable& chunks;
    std::map<std::pair<int32_t,int32_t>,size_t> indices;
    std::vector<State> states;
    Stats counters;
    uint64_t nextSubscription = 0;
    std::map<uint64_t,std::shared_ptr<Listener>> listeners;
    std::map<uint64_t,std::shared_ptr<FocusListener>> focusListeners;
    ChunkId heroCell{}, cameraCell{};
    bool focused = false;
    Tempest::Vec3 cameraPosition;
    float farDistance = 0;
    std::deque<Work> work;
    std::deque<std::pair<Clock::time_point,size_t>> recentTransitions;
  };
