#pragma once

#include <Tempest/StorageBuffer>
#include <Tempest/Vec>

#include <cstdint>
#include <cstdio>

#include "resources.h"

// The eye's adaptation under GI (-gi 1): a multiplier of the frame in the tone mapping pass, after all the light,
// bloom and TAA, so lamps, particles, probes and walls brighten together, as with a camera; scene.exposure stays.
// reports/render-features/2026-11-08-rt-revisit/step-2-shade.md, «Привыкание глаза»: замысел, сделано.
//  - the measure: the geometric mean of the illuminance of the frame (luminance / albedo, after the fog, before the
//    translucent) without the sky, in a window of the histogram, over a grid of 128x72 bilinear taps (eye_meter.comp),
//    read back a frame slot later;
//  - the key: the level of the frame of an open place at this time of day, in EV from the grey 0.18: the market at
//    noon by day, an open field without lamps at night; between them by the night's share of the scene's exposure
//    (sky_exposure.comp: 1/(lum*1.25 + 1.5), the 1.5 holds the night);
//  - only up: 2^(key - measure), at least 1 and at most 2^MaxEv; up (into the dark) at UpEvPerS, down (into the
//    light) at DownEvPerS of the world's time;
//  - no way, a jump to the target: the first frames, the clock going back, a jump of the time of day
//    over 10 game minutes, the camera jumping more than 3 m in a frame (a teleport, a cut that moves the camera; a
//    cut that only turns it goes the usual way; another world shows as a jump of the camera or of the clock).
// -eye 0|1, OPENGOTHIC_EYE=0|1 (the flag wins); on by default. Without GI the multiplier is exactly 1.
// OPENGOTHIC_EYE_LOG=<file> (a relative name: in the bench's out dir) writes a row a frame (experiments).
class EyeAdaptation final {
  public:
    static constexpr uint32_t Bins        = 64;
    static constexpr uint32_t ResultWords = 4*Bins + 3;

    // The keys in EV of the illuminance (eye_meter.comp: every surface an 18 % grey diffuse one) in the window below,
    // by the same rule as before (step-2-shade.md, «Замер по правилам индустрии»): by day the market at noon, +1.59
    // (the frame's luminance, all samples: -2.98); at night the fields of Lobart and Onar at 23:00, -1.61 and -2.43
    // (luminance: -6.35 and -6.88, the key -6.6, the lead's decision Н1)
    static constexpr float KeyDay     = 1.6f;
    static constexpr float KeyNight   = -2.0f;
    static constexpr float MaxEv      = 2.5f;  // the ceiling: the cave's 2.1 of the decomposition, up to half a stop
    static constexpr float UpEvPerS   = 1.f;   // into the dark: UE's default
    static constexpr float DownEvPerS = 3.f;   // into the light: UE's default
    static constexpr float NightL     = 1.5f;  // the night's term of the scene's exposure (Renderer::prepareExposure)
    // the window of the histogram the eye takes, shares of the samples (step-2-shade.md, «Замер по правилам
    // индустрии», Б)
    // 50-95 %: the darkest half and the brightest 5 % left out (Narkowicz). Chosen by the rule named beforehand: the
    // least swing on a 360 turn in the forest (0.21 stops; 10-90 %: 0.34, 0-100 %: 0.51) among the windows that keep
    // the forest and the cave lifted
    static constexpr float WindowLo   = 0.5f;
    static constexpr float WindowHi   = 0.95f;

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
      float scene     = 0; // EV of the illuminance without the sky in the window [WindowLo, WindowHi]: what the eye takes
      float full      = 0; // the same over all samples (0-100 %)
      float trim      = 0; // the darkest and brightest 10 % left out (10-90 %, the engines' default)
      float hi        = 0; // the darkest half and the brightest 5 % left out (50-95 %, Narkowicz)
      float all       = 0; // with the sky, 0-100 %
      float skyShare  = 0;
      float exposure  = 0;
      float lumMed    = 0;
      float skyVis    = 1; // scene.fogSkyVis: the sky seen from the camera for the fog (renderer_fogsky.cpp)
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
