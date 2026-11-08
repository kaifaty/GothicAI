#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include <cstdint>

#include "graphics/dynamic/frustrum.h"

class Npc;

// Animation LOD (world streaming, optimisation row 1, part 1): NPCs outside the
// camera frustum update their pose every `rate` frames instead of every frame.
// The skipped frames' dt is accumulated and handed to the next update; the pose
// itself is a function of the world clock, so animation time, sfx/pfx ranges and
// effect lifetimes are neither lost nor doubled. Visible NPCs are not touched.
//
// OPENGOTHIC_ANIM_LOD=0 turns it off (every NPC every frame, as before);
// OPENGOTHIC_ANIM_LOD_RATE=N (1..16, default 4) sets the out-of-frustum rate.
namespace AnimationLod {

struct Config {
  bool     enabled = true;
  uint32_t rate    = 4;
  };
const Config& config();  // read from the environment once per process

// NPCs closer than this to the camera always update: the camera-behind and
// near-shadow cases stay exact.
constexpr float CameraRadius = 1000.f;
// A shadow is followed along the light at most this far from the body.
constexpr float ShadowReach  = 3000.f;

// The view an NPC must stay out of to be deferred: the camera frustum (no
// occlusion) and, when the sun is known, the shadow the body casts along it.
class View final {
  public:
    void make(const Tempest::Matrix4x4& viewProj, const Tempest::Vec3& cameraOrigin,
              const Tempest::Vec3* sunDir);
    // Conservative body sphere at the NPC's current position (not the visual's,
    // which a deferred NPC has not moved yet), swept along its shadow.
    bool visible(const Npc& npc) const;
    bool nearCamera(const Npc& npc) const;
    // The old probe's sphere test against the frustum only, for the
    // tier/policy/frustum split of the frame profile.
    bool probeVisible(const Npc& npc) const;

  private:
    Frustrum      frustum;
    Tempest::Vec3 camera;
    Tempest::Vec3 shadowDir;
    bool          shadow = false;
  };

// Stable stagger of the deferred NPCs over the `rate` frames.
uint32_t phase(const Npc& npc);
// The seed of that stagger, one per NPC in creation order. The stagger decides
// which frames apply the deferred dt and so changes the world: it must replay
// exactly. Creation order is the same in every run, a heap address is not (ASLR).
uint32_t nextSeed();

// The parallel animation pass is running: pose readers must not catch up an NPC
// from inside it (another worker may be updating that NPC).
void     beginPass();
void     endPass();
// Catching up on read is allowed: the main thread, outside the parallel pass.
bool     canSyncOnRead();
void     noteSync();
uint64_t takeSyncs();

}
