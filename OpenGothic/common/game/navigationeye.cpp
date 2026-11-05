#include "navigationeye.h"

// The eye core: no engine headers here (see navigationeye.h). The nav-sim stand
// compiles this file as is, with only Tempest math on its include path.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace {
using Tempest::Vec3;
constexpr float Deg = 3.14159265358979323846f/180.f;

void setHit(NavigationEye::Ray& ray, const NavigationEye::RayHit& hit, float distanceLimit) {
  ray.hit = hit.hit;
  ray.kind = hit.kind;
  ray.point = hit.hit ? hit.point : ray.origin+ray.direction*distanceLimit;
  ray.distanceCm = hit.hit ? (hit.point-ray.origin).length() : distanceLimit;
  ray.normalY = hit.normalY;
  }
}

NavigationEye::NavigationEye():NavigationEye(Config{}) {
  }

NavigationEye::NavigationEye(const Config& config):cfg(config) {
  if((cfg.layout!=250 && cfg.layout!=500 && cfg.layout!=1000) ||
     !std::isfinite(cfg.centerRangeCm) || cfg.centerRangeCm<=0.f || cfg.centerRangeCm>10000.f ||
     !std::isfinite(cfg.peripheralRangeCm) || cfg.peripheralRangeCm<=0.f || cfg.peripheralRangeCm>3000.f ||
     !std::isfinite(cfg.rangeMultiplier) || cfg.rangeMultiplier<=0.f || cfg.rangeMultiplier>1.f ||
     !std::isfinite(cfg.nearRangeCm) || cfg.nearRangeCm<=0.f || cfg.nearRangeCm>cfg.centerRangeCm ||
     cfg.nearPeriodMs==0 || cfg.farPeriodMs<cfg.nearPeriodMs ||
     (cfg.floorSampling!=FloorSampling::Baseline && cfg.floorSampling!=FloorSampling::WalkingGazeDown &&
      cfg.floorSampling!=FloorSampling::ForwardRows))
    throw std::invalid_argument("invalid navigation eye configuration");

  const size_t centerIntervals = cfg.layout==250 ? 12 : cfg.layout==500 ? 20 : 28;
  const size_t verticalIntervals = cfg.layout==250 ? 13 : cfg.layout==500 ? 19 : 27;
  std::vector<float> yaw;
  for(size_t i=0; i<=centerIntervals; ++i)
    yaw.push_back(-60.f+120.f*float(i)/float(centerIntervals));
  const float peripheralStep = cfg.layout==1000 ? 10.f : 20.f;
  for(float angle=60.f+peripheralStep; angle<=100.f; angle+=peripheralStep) {
    yaw.push_back(-angle);
    yaw.push_back(angle);
    }
  std::sort(yaw.begin(),yaw.end());
  const size_t extra = cfg.floorSampling==FloorSampling::ForwardRows ? 35 : 0;
  rays.reserve(yaw.size()*(verticalIntervals+1)+extra);
  localDirections.reserve(rays.capacity());
  const auto addRay = [&](float angle, float pitch) {
    Ray ray;
    ray.yawDeg = angle;
    ray.pitchDeg = pitch;
    const bool central = std::abs(angle)<=60.f;
    ray.maxRangeCm = (central ? cfg.centerRangeCm : std::min(cfg.centerRangeCm,cfg.peripheralRangeCm))*cfg.rangeMultiplier;
    ray.distanceCm = ray.maxRangeCm;
    const float h = angle*Deg, v = pitch*Deg;
    localDirections.push_back(Vec3(std::cos(v)*std::cos(h),std::sin(v),std::cos(v)*std::sin(h)));
    ray.direction = localDirections.back(); // preview geometry in the canonical head frame
    ray.point = ray.direction*ray.maxRangeCm;
    rays.push_back(ray);
    };
  for(float angle:yaw)
    for(size_t row=0; row<=verticalIntervals; ++row)
      addRay(angle,-70.f+130.f*float(row)/float(verticalIntervals));
  // Extra density stays inside the original field and range. At human eye
  // height these rows observe floor at roughly 5.5, 6, 8.5, 13 and 17 metres.
  if(cfg.floorSampling==FloorSampling::ForwardRows)
    for(float pitch:{-6.f,-8.f,-12.f,-16.f,-18.f})
      for(float angle=-30.f; angle<=30.f; angle+=10.f)
        addRay(angle,pitch);
  far.resize(rays.size());
  memoryRays.resize(2*rays.size());
  }

void NavigationEye::reset() {
  lastNear = lastFar = 0;
  sampled = false;
  gazePitch = 0;
  previousPosition = Vec3();
  std::fill(memoryRays.begin(),memoryRays.end(),Ray{});
  for(size_t i=0; i<rays.size(); ++i) {
    auto& ray = rays[i];
    ray.origin = ray.direction = ray.point = Vec3();
    ray.distanceCm = ray.maxRangeCm;
    ray.normalY = 0;
    ray.kind = HitKind::None;
    ray.sampledAtMs = ray.nearSampledAtMs = 0;
    ray.hit = ray.complete = false;
    far[i].valid = false;
    }
  }

NavigationEye::Update NavigationEye::update(const Pose& pose, const RayCaster& caster,
                                            const Npc* except, uint64_t gameMs) {
  const auto begin = std::chrono::steady_clock::now();
  Update result;
  if(sampled && gameMs<lastNear)
    reset();
  if(sampled && gameMs>=lastNear && gameMs-lastNear<cfg.nearPeriodMs)
    return result;
  const bool farDue = !sampled || gameMs<lastFar || gameMs-lastFar>=cfg.farPeriodMs;
  const auto moved = pose.position-previousPosition;
  const bool walking = sampled && moved.x*moved.x+moved.z*moved.z>1.f;
  gazePitch = cfg.floorSampling==FloorSampling::WalkingGazeDown && walking ? -27.f : 0.f;
  const float gazeSin = std::sin(gazePitch*Deg), gazeCos = std::cos(gazePitch*Deg);
  const Vec3 forward = pose.forward*gazeCos+pose.up*gazeSin;
  const Vec3 up = pose.up*gazeCos-pose.forward*gazeSin;
  const float nearRange = cfg.nearRangeCm;
  for(size_t i=0; i<rays.size(); ++i) {
    auto& ray = rays[i];
    const auto& local = localDirections[i];
    Ray next = ray;
    next.origin = pose.origin;
    next.direction = Vec3::normalize(forward*local.x+up*local.y+pose.right*local.z);
    next.sampledAtMs = next.nearSampledAtMs = gameMs;
    const float nearEnd = std::min(nearRange,next.maxRangeCm);
    const auto nearHit = caster.cast(next.origin,next.origin+next.direction*nearEnd,except);
    ++result.nearCasts;
    setHit(next,nearHit,nearEnd);
    next.complete = next.hit || nearEnd>=next.maxRangeCm;
    if(!next.complete && farDue) {
      const auto farHit = caster.cast(next.origin+next.direction*nearEnd,
                                      next.origin+next.direction*next.maxRangeCm,except);
      ++result.farCasts;
      setHit(next,farHit,next.maxRangeCm);
      next.complete = true;
      far[i].ray = next;
      far[i].valid = true;
      }
    ray = next;
    memoryRays[i] = next;
    auto& cached = memoryRays[rays.size()+i];
    cached = far[i].valid ? far[i].ray : Ray{};
    if(!far[i].valid)
      cached.sampledAtMs = gameMs;
    }
  previousPosition = pose.position;
  lastNear = sampled ? lastNear+((gameMs-lastNear)/cfg.nearPeriodMs)*cfg.nearPeriodMs : gameMs;
  if(farDue)
    lastFar = sampled ? lastFar+((gameMs-lastFar)/cfg.farPeriodMs)*cfg.farPeriodMs : gameMs;
  sampled = true;
  result.updated = true;
  result.farUpdated = farDue;
  result.casts = result.nearCasts+result.farCasts;
  result.costUs = std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();
  return result;
  }

const char* NavigationEye::hitKindName(HitKind kind) {
  switch(kind) {
    case HitKind::None: return "none";
    case HitKind::World: return "world";
    case HitKind::StaticVob: return "static_vob";
    case HitKind::MovableVob: return "movable_vob";
    case HitKind::Npc: return "npc";
    case HitKind::Water: return "water";
    }
  return "none";
  }
