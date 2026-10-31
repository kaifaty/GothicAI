#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

class Npc;

// Opt-in diagnostic elapsed clocks. Worker elapsed sums are separate from
// the enclosing main-thread wall intervals; no timer changes gameplay or waits.
namespace FrameProfile {
enum class Zone : size_t {
  Tick, FinishStep, Camera, Observations, Bench, FencePoll, Recycle, Ui,
  Encode, Submit, Present, Limiter, WorldLogic, Prepare, Physics, Sync, WorldMisc,
  ParallelAnimation, NativeExport, ForcedSyncAnimation, ActorAnimationCpu,
  PoseCpu, PaletteCpu, StorageJoin, StorageCommit, StoragePack, StorageDispatch,
  StorageGrow, StorageUploadWorker,
  BodyNet, Cloth,
  // Main-thread buoyancy preparation/finish plus the synchronous Jolt step
  // listener (possibly a worker): elapsed sum, overlapping Physics wall time.
  Water, Destruction, AiBridge, Sound,
  EncodeShadows, EncodeMain, EncodeWater, EncodePost, EncodeUi, Count
  };
enum class Amount : size_t { PaletteCalls, PaletteBytes, IdenticalCalls, IdenticalBytes,
  PatchCalls, PatchBytes, PatchBlocks, GrowBytes, UploadBytes, DrawCalls, Count };
enum class Outcome : uint8_t { Unfinished, Submitted, FenceBusy, Capture, Error };
enum class Flag : uint32_t { SubmitCompleted=1, PresentCompleted=2,
  CaptureRequested=4, CaptureReadCompleted=8, FenceBusy=16, Error=32 };
#if defined(OPENGOTHIC_ENABLE_FRAME_PROFILE) && OPENGOTHIC_ENABLE_FRAME_PROFILE
bool enabled();
bool watchedActor(const Npc* actor);
bool actorContext();
void setActors(const std::vector<Npc*>& actors);
void observer(uint64_t dt, std::chrono::steady_clock::time_point now);
void sample(std::string_view segment, uint64_t frame);
void add(Amount amount, uint64_t value);
void flush();
// Capture the attempt at submission, never look up a reused cmdId on completion.
void* gpuSubmitted();
void gpuCompleted(void* record, double startSeconds, double endSeconds);
void* geometrySubmitted();
void geometryCompleted(void* record, uint64_t triangles, bool valid);

class Scope final {
  public:
    explicit Scope(Zone zone, bool include=true);
    ~Scope();
    void finish();
    void add(Amount amount, uint64_t value);
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
  private:
    void* record=nullptr;
    uint64_t begin=0;
    Zone zone;
    bool inActor=false;
  };
class Attempt final {
  public:
    explicit Attempt(uint8_t commandId);
    ~Attempt();
    void simulation(uint64_t dt);
    void outcome(Outcome value);
    void flag(Flag value);
  private:
    void* record=nullptr;
  };
#else
// Normal builds have no recorder, clocks, TLS, lazy initialization or diagnostic calls.
constexpr bool enabled() noexcept { return false; }
constexpr bool watchedActor(const Npc*) noexcept { return false; }
constexpr bool actorContext() noexcept { return false; }
constexpr void setActors(const std::vector<Npc*>&) noexcept {}
constexpr void observer(uint64_t, std::chrono::steady_clock::time_point) noexcept {}
constexpr void sample(std::string_view, uint64_t) noexcept {}
constexpr void add(Amount, uint64_t) noexcept {}
constexpr void flush() noexcept {}
constexpr void* gpuSubmitted() noexcept { return nullptr; }
constexpr void gpuCompleted(void*, double, double) noexcept {}
constexpr void* geometrySubmitted() noexcept { return nullptr; }
constexpr void geometryCompleted(void*, uint64_t, bool) noexcept {}

class Scope final {
  public:
    constexpr explicit Scope(Zone, bool=true) noexcept {}
    constexpr ~Scope() noexcept {}
    constexpr void finish() noexcept {}
    constexpr void add(Amount, uint64_t) noexcept {}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
  };
class Attempt final {
  public:
    constexpr explicit Attempt(uint8_t) noexcept {}
    constexpr ~Attempt() noexcept {}
    constexpr void simulation(uint64_t) noexcept {}
    constexpr void outcome(Outcome) noexcept {}
    constexpr void flag(Flag) noexcept {}
  };
#endif
}
