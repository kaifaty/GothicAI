#include "pfxmotion.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <string_view>

using namespace Tempest;

namespace PfxMotion {
namespace {

struct Wave {
  Vec3  k;      // unit direction of the wave
  Vec3  c;      // unit direction of its curl, across k: the term is divergence-free
  float scale;  // multiple of the wave number, so the sum does not repeat
  float phase;  // turns
  };

constexpr size_t WaveCount = 6;

std::array<Wave,WaveCount> makeWaves() {
  // the six axes of an icosahedron: evenly spread directions
  const float phi = 1.6180340f;
  const Vec3 axes[WaveCount] = {{0,1,phi},{0,-1,phi},{1,phi,0},{-1,phi,0},{phi,0,1},{-phi,0,1}};
  const Vec3 refs[WaveCount] = {{1,0,0},{0.6f,0.8f,0},{0,0,1},{0.8f,0,0.6f},{0,1,0},{0,0.6f,0.8f}};
  const float scales[WaveCount] = {0.71f,1.f,1.37f,0.86f,1.19f,1.58f};
  const float phases[WaveCount] = {0.05f,0.33f,0.70f,0.19f,0.89f,0.52f};  // turns
  std::array<Wave,WaveCount> ret;
  for(size_t i=0; i<WaveCount; ++i) {
    Vec3 k = axes[i]/axes[i].length();
    Vec3 c = Vec3::crossProduct(k,refs[i]);
    ret[i] = {k, c/c.length(), scales[i], phases[i]};
    }
  return ret;
  }

const std::array<Wave,WaveCount> waves = makeWaves();

uint32_t hash(uint64_t i, uint32_t seed) {
  uint64_t h = i*0x9E3779B97F4A7C15ull ^ (uint64_t(seed)*0xC2B2AE3D27D4EB4Full + 0x165667B19E3779F9ull);
  h ^= h>>31; h *= 0xBF58476D1CE4E5B9ull;
  h ^= h>>29; h *= 0x94D049BB133111EBull;
  h ^= h>>32;
  return uint32_t(h);
  }

// smooth value noise in -1..1: random values at integer knots joined by the quintic fade
float noise(double x, uint32_t seed) {
  const double   fl = std::floor(x);
  const uint64_t i  = uint64_t(int64_t(fl));
  const float    t  = float(x-fl);
  const float    v0 = float(hash(i,  seed)&0xFFFFu)/32767.5f - 1.f;
  const float    v1 = float(hash(i+1,seed)&0xFFFFu)/32767.5f - 1.f;
  const float    s  = t*t*t*(t*(t*6.f-15.f)+10.f);
  return v0 + (v1-v0)*s;
  }

// cos(2*pi*turns), |error| < 3e-5: the curl does not need libm's precision, and it runs six times a substep
float cosTurns(float turns) {
  float r    = std::abs(turns - std::floor(turns+0.5f));  // 0..0.5
  float sign = 1.f;
  if(r>0.25f) {
    r    = 0.5f-r;
    sign = -1.f;
    }
  const float t = r*6.2831853f, t2 = t*t;
  return sign*(1.f + t2*(-0.5f + t2*(1.f/24.f + t2*(-1.f/720.f + t2*(1.f/40320.f)))));
  }

// knots per period: neighbouring knots differ in sign half the time, so the noise crosses its mean once in two
// knots; four knots per period give pulse_hz cycles a second
constexpr double KnotsPerPeriod = 4.0;

}

Vec3 curl(const Params& p, const Vec3& at, float timeMs) {
  Vec3 sum;
  const float turnsPerCm = p.curlWave/6.2831853f;
  for(auto& w:waves) {
    const float a = turnsPerCm*w.scale*(Vec3::dotProduct(w.k,at) - p.curlDrift*timeMs) + w.phase;
    sum += w.c*cosTurns(a);
    }
  // each term's rms is 1/sqrt(2), the directions are unrelated: the sum's rms is sqrt(N/2)
  return sum*(p.curlStrength*std::sqrt(2.f/float(WaveCount)));
  }

void step(const Params& p, const Vec3& gravity, Vec3& pos, Vec3& vel, const Vec3& origin, float timeMs, float dtMs) {
  if(dtMs<=0)
    return;
  const int   n  = std::max(1, int(std::ceil(dtMs/MaxSubstepMs - 1e-4f)));
  const float h  = dtMs/float(n);
  const float dh = p.drag*h;
  // exact over a substep for a constant acceleration a: x += v*f1 + a*f2, v = v*e + a*f1
  float e, f1, f2;
  if(dh<1e-2f) {
    e  = 1.f - dh + dh*dh*0.5f - dh*dh*dh/6.f;
    f1 = h*(1.f - dh*0.5f + dh*dh/6.f);
    f2 = h*h*(0.5f - dh/6.f + dh*dh/24.f);
    } else {
    e  = std::exp(-dh);
    f1 = (1.f-e)/p.drag;
    f2 = (h-f1)/p.drag;
    }
  for(int i=0; i<n; ++i) {
    Vec3 a = gravity;
    if(p.curlStrength>0)
      a += curl(p, origin+pos, timeMs+float(i)*h);
    pos += vel*f1 + a*f2;
    vel  = vel*e  + a*f1;
    }
  }

float flicker(const Flicker& f, uint64_t timeMs) {
  if(!f.active())
    return 1.f;
  const double t = double(timeMs)/1000.0;
  float s = 0.7f*noise(t*f.pulseHz*KnotsPerPeriod, f.seed);
  if(f.jitterHz>0)
    s += 0.3f*noise(t*f.jitterHz*KnotsPerPeriod, f.seed^0x68E31DA4u);
  return 1.f + f.amount*s;
  }

Vec3 jitter(const Flicker& f, uint64_t timeMs, float cm) {
  if(cm<=0)
    return Vec3();
  const double t  = double(timeMs)/1000.0;
  const double hz = (f.jitterHz>0 ? f.jitterHz : f.pulseHz)*KnotsPerPeriod;
  return Vec3(noise(t*hz, f.seed^0x1B56C4E9u),
              noise(t*hz, f.seed^0x7F4A7C15u),
              noise(t*hz, f.seed^0x2545F491u))*cm;
  }

bool logEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("OPENGOTHIC_FLICKER_LOG");
    return e!=nullptr && std::string_view(e)=="1";
    }();
  return on;
  }

} // namespace PfxMotion
