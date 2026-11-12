#pragma once

#include <Tempest/StorageBuffer>
#include <Tempest/Vec>

#include <cstdint>
#include <cstdio>

#include "resources.h"

// The eye's adaptation under GI (-gi 1): a multiplier of the frame in the tone mapping pass, after all the light,
// bloom and TAA, so lamps, particles, probes and walls brighten together, as with a camera; scene.exposure stays.
// reports/render-features/2026-11-08-rt-revisit/step-2-shade.md, «Привыкание глаза»: замысел, сделано.
//  - the measure: the geometric mean of the luminance of the frame before the tone curve, without the sky (by the
//    depth), over a grid of 128x72 bilinear taps (eye_meter.comp), read back a frame slot later;
//  - the key: the level of the frame of an open place at this time of day, in EV from the grey 0.18: the market at
//    noon by day, an open field without lamps at night; between them by the night's share of the scene's exposure
//    (sky_exposure.comp: 1/(lum*1.25 + 1.5), the 1.5 holds the night);
//  - only up: 2^(key - measure), at least 1 and at most 2^MaxEv; up (into the dark) at UpEvPerS, down (into the
//    light) at DownEvPerS of the world's time;
//  - no way, a jump to the target: the first frames, another world, the clock going back, a jump of the time of day
//    over 10 game minutes, the camera jumping more than 3 m in a frame (a teleport, a cut).
// -eye 0|1, OPENGOTHIC_EYE=0|1 (the flag wins); on by default. Without GI the multiplier is exactly 1.
// OPENGOTHIC_EYE_LOG=<file> (a relative name: in the bench's out dir) writes a row a frame (experiments).
class EyeAdaptation final {
  public:
    static constexpr uint32_t Bins        = 64;
    static constexpr uint32_t ResultWords = 4*Bins + 2;

    // EV: market-day at noon under GI after the fog's veil went (-2.98; -2.9 with the veil, step-2-shade.md З1)
    static constexpr float KeyDay     = -3.0f;
    // EV: an open place without lamps at night under GI, the light of the time of day and not of lamps, as by day
    // (the lead's decision 12.11.2026, Н1): the fields of Lobart and Onar at 23:00, -6.35 and -6.88. The open streets
    // (-4.5, lit by lamp posts) lifted the cave at night by 2.4 stops (step-2-shade.md, Э1)
    static constexpr float KeyNight   = -6.6f;
    static constexpr float MaxEv      = 2.5f;  // the ceiling: the cave's 2.1 of the decomposition, up to half a stop
    static constexpr float UpEvPerS   = 1.f;   // into the dark: UE's default
    static constexpr float DownEvPerS = 3.f;   // into the light: UE's default
    static constexpr float NightL     = 1.5f;  // the night's term of the scene's exposure (Renderer::prepareExposure)

    EyeAdaptation() = default;
    ~EyeAdaptation();

    static bool enabledByDefault();

    // the buffer eye_meter.comp writes in frame slot fId
    Tempest::StorageBuffer& result(uint8_t fId);
    void  markWritten(uint8_t fId) { written[fId] = true; }

    // once a frame, before the passes: reads the measure the slot wrote (its fence has been waited for) and moves the
    // multiplier; tick and dayTime: the world's (ms), cam: the camera's position (cm)
    void  update(uint8_t fId, uint64_t tick, int64_t dayTime, const Tempest::Vec3& cam);
    // inactive (no GI, the path tracer, the switch): the multiplier is 1, the next active frame starts over
    void  off();
    void  reset() { snap = SnapFrames; }

    float mul() const { return mulV; }

  private:
    static constexpr uint8_t SnapFrames = Resources::MaxFramesInFlight + 1;

    struct Measure {
      bool  valid     = false;
      float scene     = 0; // EV of the frame without the sky: what the eye takes
      float sceneTrim = 0; // the same, the darkest and brightest 10 % left out
      float all       = 0; // with the sky
      float allTrim   = 0;
      float skyShare  = 0;
      float exposure  = 0;
      float lumMed    = 0;
      };
    Measure read(uint8_t fId);
    void    log(uint64_t tick, int64_t dayTime, const Tempest::Vec3& cam, const Measure& m, float dayW, float key,
                float target);

    Tempest::StorageBuffer buf[Resources::MaxFramesInFlight];
    bool                   written[Resources::MaxFramesInFlight] = {};

    float         ev       = 0;
    float         target   = 0;
    float         mulV     = 1;
    uint8_t       snap     = SnapFrames;
    bool          hasLast  = false;
    uint64_t      lastTick = 0;
    int64_t       lastDay  = -1;
    Tempest::Vec3 lastCam;

    std::FILE*    logFile  = nullptr;
    bool          logTried = false;
    uint64_t      logFlush = 0;
  };
