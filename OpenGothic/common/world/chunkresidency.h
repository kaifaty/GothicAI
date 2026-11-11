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
    // A larger move of the hero or the camera in one update is a jump (a teleport): metres
    static constexpr double JumpDistance = 50;

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

    // The hero's and camera's positions of the last update (centimetres) and the odometer: how far, in metres, the
    // nearer of them may have come to anything since the first update (the larger of their two moves, summed). An
    // owner's demand by distance holds until the odometer passes the margin it was evaluated with.
    bool viewKnown() const { return focused; }
    const Tempest::Vec3& viewHero() const { return heroPosition; }
    const Tempest::Vec3& viewCamera() const { return cameraPosition; }
    double viewOdometer() const { return odometer; }
    // The larger of the hero's and the camera's moves in the last update, metres; a jump past JumpDistance
    double viewMove() const { return lastMove; }
    bool   viewJumped() const { return lastMove>JumpDistance; }

    uint64_t subscribe(Listener listener);
    uint64_t subscribeFocus(FocusListener listener);
    // Called at every update, after the chunk and focus listeners.
    uint64_t subscribeView(FocusListener listener);
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
    std::map<uint64_t,std::shared_ptr<FocusListener>> viewListeners;
    ChunkId heroCell{}, cameraCell{};
    bool focused = false;
    Tempest::Vec3 heroPosition;
    Tempest::Vec3 cameraPosition;
    double odometer = 0;
    double lastMove = 0;
    float farDistance = 0;
    std::deque<Work> work;
    std::deque<std::pair<Clock::time_point,size_t>> recentTransitions;
  };
