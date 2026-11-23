#include "animationlod.h"

#include "world/objects/npc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <thread>

using namespace Tempest;

namespace {
std::atomic<bool>     inPass{false};
std::atomic<uint64_t> syncs{0};
std::atomic<uint32_t> seeds{0};
// The thread that runs the animation pass is the one that runs world logic.
std::atomic<std::thread::id> mainThread{};

// A body may outgrow its bind-pose box when a limb swings out, and a deferred
// NPC's pose is up to rate-1 frames old: widen the sphere a little.
constexpr float PoseMargin = 50.f;
}

const AnimationLod::Config& AnimationLod::config() {
  static const Config cfg = [] {
    Config c;
    if(const char* v=std::getenv("OPENGOTHIC_ANIM_LOD"); v!=nullptr && v[0]=='0')
      c.enabled = false;
    if(const char* v=std::getenv("OPENGOTHIC_ANIM_LOD_RATE"); v!=nullptr && v[0]!='\0') {
      const long n = std::strtol(v,nullptr,10);
      c.rate = uint32_t(std::clamp<long>(n,1,16));
      }
    if(c.rate<=1)
      c.enabled = false;
    return c;
    }();
  return cfg;
  }

void AnimationLod::View::make(const Matrix4x4& viewProj, const Vec3& cameraOrigin, const Vec3* sunDir) {
  frustum.make(viewProj,1,1);
  camera = cameraOrigin;
  shadow = false;
  if(sunDir!=nullptr) {
    // A shadow falls away from the light, downwards; whichever way the sky
    // stores its direction, take the downward one.
    Vec3 d = *sunDir;
    if(d.y>0)
      d = d*(-1.f);
    const float len = d.length();
    if(len>0.f && std::isfinite(len)) {
      shadowDir = d*(1.f/len);
      shadow    = true;
      }
    }
  }

static void bodySphere(const Npc& npc, Vec3& center, float& radius) {
  const auto bounds = npc.bounds();
  const auto model  = npc.transform();
  Vec3 c = (bounds.bbox[0]+bounds.bbox[1])*0.5f;
  model.project(c);
  // A deferred NPC's visual still stands where it was last posed: move the
  // sphere to the logical position.
  const Vec3 shown(model.at(3,0),model.at(3,1),model.at(3,2));
  center = c + (npc.position()-shown);
  radius = bounds.rConservative + PoseMargin;
  }

bool AnimationLod::View::visible(const Npc& npc) const {
  Vec3  c;
  float r = 0;
  bodySphere(npc,c,r);
  if(frustum.testPoint(c,r))
    return true;
  if(!shadow)
    return false;
  // Sweep the sphere along the light down to the feet: the box around both
  // ends holds the whole shadow of the body.
  const float feet  = npc.position().y;
  const float drop  = std::max(c.y-feet+r,0.f);
  const float down  = std::max(-shadowDir.y,0.05f);
  const float reach = std::min(drop/down,ShadowReach);
  const Vec3  e     = c + shadowDir*reach;
  const Vec3  lo(std::min(c.x,e.x)-r,std::min(c.y,e.y)-r,std::min(c.z,e.z)-r);
  const Vec3  hi(std::max(c.x,e.x)+r,std::max(c.y,e.y)+r,std::max(c.z,e.z)+r);
  return frustum.testBbox(lo,hi)!=Frustrum::T_Invisible;
  }

bool AnimationLod::View::nearCamera(const Npc& npc) const {
  return (npc.position()-camera).quadLength()<CameraRadius*CameraRadius;
  }

bool AnimationLod::View::probeVisible(const Npc& npc) const {
  const auto bounds = npc.bounds();
  auto center = (bounds.bbox[0]+bounds.bbox[1])*0.5f;
  npc.transform().project(center);
  return frustum.testPoint(center,bounds.rConservative);
  }

uint32_t AnimationLod::phase(const Npc& npc) {
  // splitmix64 of the creation seed: uniform over the frames, stable for the NPC's life.
  uint64_t z = uint64_t(npc.animationLodSeed()) + 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  z =  z ^ (z >> 31);
  return uint32_t(z % config().rate);
  }

uint32_t AnimationLod::nextSeed() {
  return seeds.fetch_add(1,std::memory_order_relaxed);
  }

void AnimationLod::beginPass() {
  mainThread.store(std::this_thread::get_id(),std::memory_order_relaxed);
  inPass.store(true,std::memory_order_release);
  }

void AnimationLod::endPass() {
  inPass.store(false,std::memory_order_release);
  }

bool AnimationLod::canSyncOnRead() {
  return !inPass.load(std::memory_order_acquire) &&
         mainThread.load(std::memory_order_relaxed)==std::this_thread::get_id();
  }

void AnimationLod::noteSync() {
  syncs.fetch_add(1,std::memory_order_relaxed);
  }

uint64_t AnimationLod::takeSyncs() {
  return syncs.exchange(0,std::memory_order_relaxed);
  }
