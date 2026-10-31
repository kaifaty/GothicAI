#pragma once

#include <Tempest/Vec>
#include "asset/modeltypes.h"

#include <cstdint>
#include <string_view>
#include <vector>

#include "animation.h"

class Serialize;

// Transitions between animations by inertialization (D. Bollo, "Inertialization: High-Performance
// Animation Transitions in Gears of War", GDC 2018). When a bone switches to another animation, the
// new one plays at once; the difference to the pose the bone was shown in, and to its velocity, is an
// offset that fades to zero over the blend time on a quintic curve. The curve ends with zero velocity
// and acceleration and does not overshoot zero. Where the two poses match, nothing changes.
//
// Beyond Bollo: the velocity is kept across the offset too and away from zero (a bit), not dropped,
// so a limb that moves fast at a switch does not jerk; all bones that switch in one frame share one
// blend time, so a leg stays a leg on the way.
//
// Replaces the cross-fade of Pose: it started from a frozen pose (the body stopped for the first
// frames of every switch) and did nothing where the new animation has no blend time (most T_
// transitions): the legs jumped up to a meter in a frame when a run stopped. Switched off, that
// cross-fade comes back, to compare.
class AnimBlend final {
  public:
    using Sample = asset::AnimationSample;

    // Tuned on the bench (anim, quad-zoo): longer or with more swing out, the feet of a creature that
    // stops from a gallop stay in the air too long
    struct Params final {
      float minTime   = 0.15f;  // seconds: a switch blends at least this long (MDS gives 0 for most T_)
      float scale     = 1.f;    // of the MDS blend times
      float maxTime   = 0.3f;
      // a large offset takes longer: its fastest turn (1.875 * angle / time on the quintic) is not
      // faster than this, rad/s; the same for a move, cm/s. All bones switching together share the
      // longest time
      float turnSpeed = 12.f;
      float moveSpeed = 400.f;
      // an offset that grows at the switch (the new animation moves away fast: a landing, the next
      // hit of a combo) keeps growing for a moment, up to this * offset / blend time: the curve
      // then goes out up to ~11% further (27% at 2). Bollo drops that velocity: the limbs jerked
      float outSpeed  = 1.f;
      // the velocity across the offset (Bollo drops it too) swings the bone aside and back, up to
      // this * offset / blend time: aside up to ~20% of the offset
      float sideSpeed = 1.f;
      };

    // For the test bench
    struct Probe final {
      bool     valid   = false;
      uint32_t active  = 0;   // bones blending
      uint32_t started = 0;   // switches since the last probe
      float    angle   = 0;   // the largest rotation offset now, degrees
      float    time    = 0;   // the longest blend time started since the last probe, seconds
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);
    static bool    setParam(std::string_view name, float v);
    static const Params& params();

    void           reset(size_t numBones);
    void           setLog(bool l) { log = l; }  // switches to the log (the hero, OPENGOTHIC_ANIBLEND_LOG=1)

    // What drives a bone this frame: the animation and when it started (a switch is a change of
    // either), and the key frames around the time, a then b (the velocity of the animation)
    void           drive(size_t bone, const Animation::Sequence& seq, uint64_t sAnim,
                         const Sample& a, const Sample& b, float fpsRate) {
      auto& d = cur[bone];
      d.seq   = &seq;
      d.sAnim = sAnim;
      d.in    = seq.blendIn;
      d.out   = seq.blendOut;
      d.a     = &a;
      d.b     = &b;
      d.rate  = fpsRate;
      }

    // out = what the animations give (raw) with the offsets of the bones that switched; bones nothing
    // ever drove are left as they are. True while something blends
    bool           apply(const Sample* raw, Sample* out, size_t numBones, uint64_t now);
    // An external pose (a settled physical body), without a synthetic animation driver. The next
    // apply starts the same inertial transition for every bone, including bones not keyed by an
    // animation. This is separate from the normal driver's switch detection.
    void           transitionFrom(const Sample* shown, size_t numBones, uint64_t now,
                                  const Sample* previous=nullptr, uint64_t previousTime=0);
    bool           hasExternalSeed() const { return !external.empty(); }
    // Finished local targets after root-motion cancellation and procedural passes, sampled ahead.
    void           setExternalTarget(const Sample* current, const Sample* next, size_t numBones, float rate);
    // The common driverless transition curve, also used for an affine bone's stretch/shear.
    float          externalWeight(uint64_t now) const;
    // Only the driverless external transition; written in the optional ragdoll save entry.
    void           saveExternal(Serialize& fout) const;
    void           loadExternal(Serialize& fin, size_t expectedBones, bool hasVelocityState=true);
    void           probe(Probe& p);

  private:
    struct Driver final {
      const Animation::Sequence* seq   = nullptr;
      uint64_t                   sAnim = 0;
      uint64_t                   in    = 0;
      uint64_t                   out   = 0;
      const Sample*              a     = nullptr;
      const Sample*              b     = nullptr;
      float                      rate  = 0;
      };

    // x(t) from x0 with velocity v0 to 0 at t1, zero velocity and acceleration there
    struct Curve final {
      float t1 = 0, x0 = 0, v0 = 0, a0 = 0, A = 0, B = 0, C = 0;
      void  init(float x0, float v0, float t1, float vMax);
      float at(float t) const;
      };

    struct Offset final {
      bool          active = false;
      uint64_t      start  = 0;
      float         end    = 0;
      // rotation (as a rotation vector) and move, each along the offset and across it
      Curve         rot, rotSide, pos, posSide;
      Tempest::Vec3 axis, axisSide, dir, dirSide;
      // switched off: the old cross-fade from the pose shown, frozen, over the new animation's
      // longer blend time
      bool          fade   = false;
      Sample        from;
      float         fadeTime = 0;
      };

    // a bone that switched this frame: its offset and the offset's velocity, turn and move
    struct Switch final {
      size_t        bone  = 0;
      Tempest::Vec3 turn, dturn, move, dmove;
      float         angle = 0, len = 0;
      float         time  = 0;  // from the MDS
      };

    void           prepare(size_t bone, const Sample& target, const Sample& shown, float dtShown, float dtNow);
    void           begin(uint64_t now);

    std::vector<Driver> cur;    // this frame
    std::vector<Driver> drv;    // the frame before
    std::vector<Offset> off;
    std::vector<Sample> last;   // shown at lastTime
    std::vector<Switch> pend;   // switches of this frame
    std::vector<Sample> external;
    std::vector<Sample> externalPrevious;
    uint64_t            externalPreviousTime = 0;
    uint64_t            outTime  = 0;  // when the current out was made
    uint64_t            lastTime = 0;
    uint32_t            started  = 0;
    bool                log      = false;
    float               lastBlend = 0;
    bool                externalDriven = false;
    Curve               externalFade;
    uint64_t            externalStart = 0;
    bool                externalClockPending = false;
  };
