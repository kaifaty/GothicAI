#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Step 4 of the texture levels (reports/engine/2026-11-10-texture-mips/README.md, docs/world-streaming-textures.md):
// where the shortfall goes. Every texture short of its wanted level is in one state, and the shortfall's
// texture-seconds are split by it; the time from a rise of demand to the publication that meets it, the wait for a
// load slot and the bursts of textures that become short are kept as distributions. Pure: no device, no Resources.
namespace TextureFlow {
  using Clock = std::chrono::steady_clock;

  // The state of a texture short of its wanted level.
  enum State : uint8_t {
    Waiting,     // not loading: waiting for a load slot
    Reading,     // its load reads and decodes the file on the worker
    Uploading,   // its load uploads on the worker, or waits for the upload to be ready (Texture2d::uploadReady)
    Publishing,  // loaded and ready, waiting for its publication (one a frame, through the residency's work queue)
    Stale,       // loading a level coarser than it now wants: its demand rose again after the load started
    States,
    NotShort = 0xff,
    };
  const char* name(State state);

  // A load as the main thread last saw it; the worker moves Queued -> Reading -> Uploading -> Done, and the main
  // thread marks Ready when the uploaded payload's uploadReady() turns true.
  enum class Phase : uint8_t { Queued, Reading, Uploading, Done, Ready };

  // The state of a short texture: `loading` whether a load of it is in flight, `stale` whether that load reads a level
  // coarser than now wanted, `phase` how far it is.
  State state(bool loading, bool stale, Phase phase);

  // A distribution of times in milliseconds: bucket 0 under 1 ms, then buckets an eighth of an octave wide, the last
  // one open. Copies of it taken earlier give what was added since (`before`).
  struct Histogram {
    static constexpr size_t Buckets = 136;  // the last one from 2^(134/8) ms, about 110 s
    std::array<uint32_t,Buckets> counts{};
    uint64_t total = 0;
    double   maxMs = 0;

    void   add(double ms);
    // The q-quantile (0..1) of what was added since `before`: the upper edge of its bucket, so at most an eighth of an
    // octave above the true value; -1 when nothing was added.
    double quantile(double q, const Histogram& before) const;
    double quantile(double q) const;
    // The largest time added since `before`: exact when it raised the maximum, else the upper edge of its bucket.
    double max(const Histogram& before) const;
    double max() const;
    uint64_t count(const Histogram& before) const { return total-before.total; }
    uint64_t count() const { return total; }
    static size_t bucket(double ms);
    static double upper(size_t bucket);
    };

  // How many textures became short within one second of the first of them (a burst), one count per burst; the last
  // bucket is open. A burst ends one second after its first texture, and the next texture starts a new one.
  struct Bursts {
    static constexpr size_t   Buckets = 257;
    static constexpr auto     Window  = std::chrono::seconds(1);
    std::array<uint32_t,Buckets> counts{};
    uint64_t total = 0;
    uint32_t maxSize = 0;
    // the open burst
    Clock::time_point since{};
    uint32_t          size = 0;

    // A texture became short at `now`.
    void   add(Clock::time_point now);
    // Closes the open burst when its second is over.
    void   tick(Clock::time_point now);
    double quantile(double q, const Bursts& before) const;
    double quantile(double q) const;
    uint32_t max(const Bursts& before) const;
    uint32_t max() const { return maxSize; }
    uint64_t count(const Bursts& before) const { return total-before.total; }
    uint64_t count() const { return total; }

    private:
      void close();
    };

  // Loads in flight and publications with levels (part B). A load holds its slot from its start to its publication.
  // The worker's own work is short (read p50 0.06 ms, p90 0.3 ms; upload p50 0.12 ms on Ryzen, t4), so a load takes the
  // frame until the main thread sees it done and, before step 4, another frame through the residency's work queue to
  // its publication: two slots made at most one load a frame.
  struct Limits {
    size_t   loads     = 4;               // loads in flight at most (OPENGOTHIC_TEXTURE_LOADS)
    uint64_t bytes     = 64ull*1048576;   // bytes their payloads will take at most; one load may always start
                                          // (OPENGOTHIC_TEXTURE_LOAD_MB)
    double   publishMs = 0.1;             // publications of a frame stop after this many milliseconds, one at least
    };
  // Whether a load of `bytes` may start beside `inFlight` loads making `bytesInFlight`.
  bool admits(size_t inFlight, uint64_t bytesInFlight, uint64_t bytes, const Limits& limits);
  // Whether a frame that published `published` ready loads in `elapsedMs` publishes one more.
  bool publishes(size_t published, double elapsedMs, const Limits& limits);

  // The counters Resources keeps: short textures by state now, the texture-seconds in each state, the visible
  // shortfall (against the level with reserve 0, what the screen needs), the loads' latencies and bursts, and the
  // bytes of the loads in flight.
  struct Tracker {
    std::array<uint64_t,States> count{};
    std::array<double,States>   seconds{};
    uint64_t  visible = 0;
    double    visibleSeconds = 0;
    uint64_t  entries = 0;        // textures that became short, since the start
    Histogram latency;            // demand rose -> the publication that met it, async loads
    Histogram wait;               // demand rose -> its first load started
    Bursts    bursts;
    uint64_t  bytesInFlight = 0;  // the payloads the loads in flight will make
    uint64_t  publications = 0;   // asynchronous publications and their cost on the main thread, ms
    double    publishMs = 0, publishMaxMs = 0;

    void integrate(double elapsed);
    uint64_t shortfall() const;
    };
  }
