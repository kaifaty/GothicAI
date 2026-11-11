#pragma once

#include <Tempest/Point>
#include <vector>
#include <string>
#include "asset/math.h"
#include "graphics/pfx/pfxmotion.h"

class LightSource final {
  public:
    LightSource();

    Tempest::Vec3        dir() const { return ldir; }
    void                 setDir(const Tempest::Vec3& d);
    void                 setDir(float x,float y,float z);

    const Tempest::Vec3& color() const { return clr; }
    void                 setColor(const Tempest::Vec3& cl);
    void                 setColor(const std::vector<uint32_t>&      cl, float fps, bool smooth);
    void                 setColor(const std::vector<asset::Color>& cl, float fps, bool smooth);
    void                 setColor(const std::vector<Tempest::Vec3>& cl, float fps, bool smooth);

    void                 setPosition(const Tempest::Vec3& p) { pos = p; }
    const Tempest::Vec3& position() const { return pos; }

    float                range() const { return rgn; }
    void                 setRange(float r);
    void                 setRange(const std::vector<float>& rangeAniScale, float base, float fps, bool smooth);

    void                 setEnabled(bool e);

    void                 update(uint64_t time);
    bool                 isDynamic() const;
    bool                 isEnabled() const;
    float                currentRange() const { return curRgn; }
    const Tempest::Vec3& currentColor() const { return curClr; }
    const Tempest::Vec3& currentPosition() const { return flicker.active() ? curPos : pos; }

    // the brightness follows a flame's signal at the world time and the position trembles by jitterCm
    // (vobs.d "flicker_from", "jitter_cm"): the same signal as the emitter's particles
    void                 setFlicker(const PfxMotion::Flicker& f, float jitterCm);

    void                 setTimeOffset(uint64_t t);

    uint64_t             effectPrefferedTime() const;

    void                 setDebugName(std::string_view hint);
    std::string_view     debugName() const;

  private:
#ifndef NDEBUG
    std::string        dbgHint;
#endif

    Tempest::Vec3      ldir;
    Tempest::Vec3      clr;
    Tempest::Vec3      pos;
    float              rgn = 0;

    uint64_t           timeOff = 0;

    std::vector<float> rangeAniScale;
    uint64_t           rangeAniFPSInv = 0;
    bool               rangeSmooth = false;

    std::vector<Tempest::Vec3> colorAniList;
    uint64_t                   colorAniListFpsInv = 0;
    bool                       colorSmooth = false;

    Tempest::Vec3      curClr;
    float              curRgn = 0;
    Tempest::Vec3      curPos;

    PfxMotion::Flicker flicker;
    float              flickerJitterCm = 0;

    bool               enable = true;
  };

