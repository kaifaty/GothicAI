#pragma once

#include <Tempest/Vec>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "physics/worldwind.h"

// Living foliage (reports/engine/2026-11-14-living-foliage): a material with `foliage` in materials.json moves in
// the world's wind in the vertex shader and lets the low sun through its leaves. This header is the processor's
// side of shader/materials/foliage_wind.glsl: the record's numbers, the switch, what the scene gives the shaders
// each frame, and the same bend rule for the tests and for the bounds. Keep the two files in step.
//
// A leaf has three motions. The lean follows the pressure of the wind: small in the breeze, over and held while a
// gust blows, with the two travelling waves as a ripple on it. The swing is the leaf's own, a pendulum about its
// lean: its rate comes from its length, two close rates beat, and no two tufts keep step. The flutter is the small
// quick part.
namespace FoliageWind {

// materials.json "foliage" (Material::foliage)
struct Params final {
  bool    on           = false;
  float   wind         = 1.f;   // times the bend, 0..4
  float   flutter      = 1.f;   // times the flutter, 0..4
  float   translucency = 0.6f;  // the share of the sun a leaf lets through, 0..1
  uint8_t anchor       = 0;     // AnchorLine: the roots on the mesh's X axis
  bool    operator==(const Params&) const = default;
  };

enum : uint8_t { AnchorLine = 0 };

// the halves of the switch
enum : uint8_t { Wind = 1, Light = 2, Both = 3 };

constexpr float MaxScale     = 4.f;      // the range of wind and flutter in the record
constexpr float GustSpeed    = 5.65f;    // m/s: the peak of the world's gust at scale 1 (WorldWind::sample)
// The lean's pressure by the wind's speed, as a share of GustSpeed: the square of it in the breeze (1.55..2.85 m/s
// between gusts: 0.075..0.25), and from GustFrom up the leaf goes over and is held at 1 from GustFull on, so a gust
// reads as one event: over, held, back
constexpr float GustFrom     = 0.48f;
constexpr float GustFull     = 0.76f;
constexpr float EddyCurve    = 0.75f;    // the flutter grows as the speed to this power
constexpr float BendK        = 0.162f;   // tan of the lean per unit of pressure (1 at that peak), on the crest of the waves
constexpr float BendMaxTan   = 0.3093f;  // tan(0.30 rad): the cap of the lean and the swing together
constexpr float BendRoot     = 0.35f;    // the share of the lean and the swing a vertex at the root takes
constexpr float WaveSteady   = 0.875f;   // the lean is WaveSteady + WaveSwing*G, G in -1..1: the waves are a ripple on it,
constexpr float WaveSwing    = 0.125f;   // a quarter of its depth from trough to crest, so a gust holds the leaf over
constexpr float Wave1Length  = 2400.f;   // cm; along +Z, the mean direction of the wind
constexpr float Wave2Length  = 950.f;    // cm; turned 55 degrees to +X
constexpr float Wave1Share   = 0.55f;
constexpr float Wave2Share   = 0.45f;
constexpr float Wave2DirX    = 0.819152f;
constexpr float Wave2DirZ    = 0.573576f;
constexpr uint64_t Wave1Period    = 6000; // ms
constexpr uint64_t Wave2Period    = 3200;
constexpr uint64_t Flutter1Period = 770;  // 1.3 Hz
constexpr uint64_t Flutter2Period = 530;  // 1.9 Hz
constexpr float FlutterCalm  = 0.004f;   // of the arm, at no pressure
constexpr float FlutterWind  = 0.022f;   // more of it per unit of pressure
constexpr float FlutterMaxQ  = 1.5f;     // the pressure the flutter stops growing at
constexpr float FlutterLeaf  = 0.35f;    // rad per cm across the tuft: leaves side by side are out of step
// The swing: a hanging leaf of length L is a pendulum of (1/2pi)*sqrt(3g/2L) Hz, 0.66 at 85 cm and 0.97 at 40 cm.
// Every leaf swings a whole number of times in SwingPeriod, so the tick's remainder of it is all the scene gives
constexpr uint64_t SwingPeriod = 60000;  // ms
constexpr float SwingRate    = 366.3f;   // swings in SwingPeriod of a leaf 1 cm long; falls as the root of the length
constexpr float SwingLenMin  = 15.f;     // cm: the lengths the rate is taken between
constexpr float SwingLenMax  = 150.f;
constexpr float SwingSpread  = 0.3f;     // tufts of one size differ by this share of the rate, by their place
constexpr float SwingBeatMin = 4.f;      // the second rate is this many swings a period over the first, and up to
constexpr float SwingBeatVar = 6.f;      // this many more: the swing rises and falls every 6.7..15 s
constexpr float Swing1Share  = 0.6f;
constexpr float Swing2Share  = 0.4f;
constexpr float SwingK       = 0.085f;   // of the arm, by the root of the speed over GustSpeed: 4.4..6 % in the breeze
constexpr float SwingTop     = 0.065f;   // and no more in a gust: a leaf held 12..16 % over must not swing back to where
                                         // the breeze keeps it, or the gust reads as a wider swing and not as a push
constexpr float SwingMaxTan  = 0.12f;    // the cap of it; the lean keeps under BendMaxTan less the swing
constexpr float SwingAcross  = 0.3f;     // the part of the swing across the wind
constexpr float Swing1Leaf   = 0.012f;   // rad per cm across the tuft: its sides are a little out of step, and a card
constexpr float Swing2Leaf   = -0.009f;  // 40 cm wide changes its width by a few percent at most
constexpr float WallSign     = 0.04f;    // the wind a wall stops turns along it, to the side of its own part along
                                         // the wall; that side turns over smoothly within this share of the wind
constexpr float LeeLift      = 0.35f;    // of the wind that blows away from the rock lifts the leaf off it
constexpr float BoundsDir    = 1.1f;     // the wind's direction in the tuft's axes is no longer than 1; the rest is
                                         // the chord of the arm's turn

// The switch: on by default. OPENGOTHIC_FOLIAGE=0 turns both halves off, `wind` or `light` keeps that one only
// (test benches); [INTERNAL] foliage=0 of Gothic.ini is given by the renderer before the first bucket is built
// and yields to the variable.
inline uint8_t& switchBits() {
  static uint8_t bits = Both;
  return bits;
  }

inline bool envSwitch(uint8_t& bits) {
  const char* e = std::getenv("OPENGOTHIC_FOLIAGE");
  if(e==nullptr)
    return false;
  const std::string_view v = e;
  bits = v=="0" ? uint8_t(0) : (v=="wind" ? uint8_t(Wind) : (v=="light" ? uint8_t(Light) : uint8_t(Both)));
  return true;
  }

inline void setSwitch(bool ini) {
  uint8_t bits = ini ? uint8_t(Both) : uint8_t(0);
  envSwitch(bits);
  switchBits() = bits;
  }

inline uint8_t enabled() {
  static const bool once = [] { uint8_t b = Both; if(envSwitch(b)) switchBits() = b; return true; }();
  (void)once;
  return switchBits();
  }

// Bucket::foliage: 8 bits each of wind, flutter, translucency, anchor. A half that is off packs as zero
inline uint32_t pack(const Params& p, uint8_t halves) {
  if(!p.on)
    return 0;
  auto u8 = [](float v, float range) { return uint32_t(std::lround(std::clamp(v/range, 0.f, 1.f)*255.f)); };
  uint32_t ret = uint32_t(p.anchor) << 24;
  if(halves & Wind)
    ret |= u8(p.wind,MaxScale) | (u8(p.flutter,MaxScale) << 8);
  if(halves & Light)
    ret |= u8(p.translucency,1.f) << 16;
  return ret;
  }

inline float packedWind   (uint32_t bits) { return float( bits      & 0xFFu)*(MaxScale/255.f); }
inline float packedFlutter(uint32_t bits) { return float((bits>>8)  & 0xFFu)*(MaxScale/255.f); }
inline bool  moves        (uint32_t bits) { return (bits & 0xFFFFu)!=0; }

// SceneDesc::foliageWind and foliagePhase of scene.glsl
struct Scene final {
  Tempest::Vec4 wind;   // xy: the wind's direction in the world's x and z; z: the lean's pressure; w: 1 when it blows
  Tempest::Vec4 phase;  // 0..1 of the periods: the two waves, the two flutters
  Tempest::Vec4 swing;  // x: 0..1 of SwingPeriod; y: the swing as a share of the arm; z: the flutter's pressure
  };

inline float fract(float v) { return v - std::floor(v); }

// the lean's pressure, the flutter's and the swing's share of the arm by the wind's speed as a share of GustSpeed
inline float pressure(float x) {
  const float t    = std::clamp((x-GustFrom)/(GustFull-GustFrom), 0.f, 1.f);
  const float gust = t*t*(3.f-2.f*t);
  const float air  = std::min(x*x, 1.f);
  return air + (1.f-air)*gust;
  }
inline float eddy(float x)     { return std::pow(x, EddyCurve); }
inline float swingAmp(float x) { return std::min(SwingK*std::sqrt(std::max(x, 0.f)), SwingTop); }

// The same wind as the tilt of the original's vegetation (WorldWind::shaderMatrix): a steady 4 m/s without the
// cloth's world wind. Every phase is a whole remainder of the tick, so a fixed step repeats and a pause stands
inline Scene scene(uint64_t tick, uint64_t periodMs, bool windEnabled, float scale, bool legacy) {
  Scene s;
  s.phase = Tempest::Vec4(float(tick%Wave1Period)/float(Wave1Period),       float(tick%Wave2Period)/float(Wave2Period),
                          float(tick%Flutter1Period)/float(Flutter1Period), float(tick%Flutter2Period)/float(Flutter2Period));
  s.swing = Tempest::Vec4(float(tick%SwingPeriod)/float(SwingPeriod), 0.f, 0.f, 0.f);
  if(!windEnabled || periodMs==0)
    return s;
  const Tempest::Vec3 v = legacy ? Tempest::Vec3(0,0,4) : WorldWind::sample(tick,periodMs,true,scale);
  const float speed = std::sqrt(v.x*v.x + v.z*v.z);
  if(speed<=1e-4f)
    return s;
  const float x = speed/GustSpeed;
  s.wind    = Tempest::Vec4(v.x/speed, v.z/speed, pressure(x), 1.f);
  s.swing.y = swingAmp(x);
  s.swing.z = eddy(x);
  return s;
  }

// 0..1 by the tuft's place, the same on every frame and machine
inline float placeHash(const Tempest::Vec3& place) {
  uint32_t h = uint32_t(int32_t(std::floor(place.x)))*0x9E3779B1u ^ uint32_t(int32_t(std::floor(place.z)))*0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return float(h & 0xFFFFu)/65536.f;
  }

// the largest arm of the mesh: from its X axis to the far corner of its box
inline float reach(const Tempest::Vec3& boxMin, const Tempest::Vec3& boxMax) {
  const float y = std::max(std::abs(boxMin.y), std::abs(boxMax.y));
  const float z = std::max(std::abs(boxMin.z), std::abs(boxMax.z));
  return std::max(std::sqrt(y*y + z*z), 1e-3f);
  }

// The two rates of a tuft's swing, in swings a SwingPeriod: by the length of its longest leaf and by its place.
// Whole numbers: the swing is then a function of the tick's remainder of the period alone
struct SwingRates final { float first = 0.f, second = 0.f; };
inline SwingRates swingRates(float hash, float lengthCm) {
  const float own = SwingRate/std::sqrt(std::clamp(lengthCm, SwingLenMin, SwingLenMax));
  SwingRates r;
  r.first  = std::floor(own*(1.f + SwingSpread*(fract(hash*2.236068f)-0.5f)) + 0.5f);
  r.second = r.first + SwingBeatMin + std::floor(fract(hash*3.302776f)*SwingBeatVar);
  return r;
  }

// how far a vertex can go from its place at rest, in any wind: the bounds the culling tests grow by it
inline float travel(uint32_t bits, float reach) {
  const float bend = packedWind(bits)>0.f ? BoundsDir*BendMaxTan : 0.f;  // the lean and the swing
  const float flut = packedFlutter(bits)*(FlutterCalm + FlutterWind*FlutterMaxQ);
  return reach*(bend + flut);
  }

// the same for a material's mesh, by the switch
inline float travel(const Params& p, const Tempest::Vec3& boxMin, const Tempest::Vec3& boxMax) {
  return p.on ? travel(pack(p,enabled()), reach(boxMin,boxMax)) : 0.f;
  }

// The vertex of a tuft in its own axes (X along the wall, Y up the surface, Z out of the rock), moved by the wind.
// reach: of the mesh's box at rest; axis: the columns of the object's matrix; place: its translation. The arm from
// the X axis keeps its length: a hanging leaf swings about its root, a standing one bends
inline Tempest::Vec3 bend(const Tempest::Vec3& p, uint32_t bits, float reach,
                          const Tempest::Vec3 axis[3], const Tempest::Vec3& place, const Scene& sc) {
  const float windK = packedWind(bits), flutK = packedFlutter(bits);
  if(sc.wind.w<=0.f || (windK<=0.f && flutK<=0.f))
    return p;
  const float len = std::sqrt(p.y*p.y + p.z*p.z);
  if(len<1e-4f)
    return p;
  const float s = std::min(len/std::max(reach,1e-3f), 1.f);

  // the wind in the tuft's axes: what the wall stops turns along it and keeps its speed; none goes into the rock
  const Tempest::Vec3 w(sc.wind.x, 0.f, sc.wind.y);
  float wl[3] = {}, al[3] = {};
  for(int i=0; i<3; ++i) {
    al[i] = axis[i].length();
    wl[i] = al[i]>1e-6f ? Tempest::Vec3::dotProduct(w,axis[i])/al[i] : 0.f;
    }
  const float own   = wl[0];
  const float into  = std::max(-wl[2],0.f);
  const float along = own + (std::sqrt(own*own + into*into) - std::abs(own))*own/std::sqrt(own*own + WallSign*WallSign);
  const Tempest::Vec3 dir(along, wl[1], LeeLift*std::max(wl[2],0.f));

  // the lean: the wind's pressure, and two waves that run over the world as a ripple on it
  const float w1 = std::sin(6.2831853f*(fract(place.z/Wave1Length) - sc.phase.x));
  const float w2 = std::sin(6.2831853f*(fract((place.x*Wave2DirX + place.z*Wave2DirZ)/Wave2Length) - sc.phase.y));
  const float g     = Wave1Share*w1 + Wave2Share*w2;
  const float swing = std::min(windK*sc.swing.y, SwingMaxTan);
  const float tanT  = std::min(BendK*windK*sc.wind.z*(WaveSteady + WaveSwing*g), BendMaxTan - swing);
  const float arm   = len*(BendRoot + (1.f-BendRoot)*s);
  Tempest::Vec3 d   = dir*(arm*tanT);

  // the swing: the leaf's own, about its lean. Mostly along the wind as the wall has turned it, to both sides; a
  // smaller part across. Off the rock it goes one way only
  const float h = placeHash(place);
  if(swing>0.f) {
    const SwingRates n = swingRates(h, reach*al[1]);
    const float a1 = 6.2831853f*(fract(n.first *sc.swing.x) + fract(h*1.618034f)) + p.x*Swing1Leaf;
    const float a2 = 6.2831853f*(fract(n.second*sc.swing.x) + h)                  + p.x*Swing2Leaf;
    const float sa = Swing1Share*std::sin(a1) + Swing2Share*std::sin(a2);
    const float sb = Swing1Share*std::cos(a1) - Swing2Share*std::cos(a2);
    const float ux = std::sqrt(own*own + into*into), uz = std::max(wl[2],0.f);
    d.x += arm*swing*(ux*sa + SwingAcross*uz*sb);
    d.z += arm*swing*(LeeLift*uz*(0.5f+0.5f*sa) + SwingAcross*ux*(0.5f+0.5f*sb));
    }

  // the flutter: each tuft and each leaf of it in its own step, the tip most
  const float leaf = p.x*FlutterLeaf;
  const float amp  = len*s*flutK*(FlutterCalm + FlutterWind*std::min(sc.swing.z,FlutterMaxQ));
  d.x += amp*0.8f*std::sin(6.2831853f*(sc.phase.z + h) + leaf);
  d.z += amp*0.6f*std::sin(6.2831853f*(sc.phase.w + fract(h*1.618034f)) + leaf*1.3f + 1.7f);

  const Tempest::Vec3 r(d.x, p.y + d.y, p.z + d.z);
  const float rl = r.length();
  if(rl<1e-6f)
    return p;
  return Tempest::Vec3(p.x,0.f,0.f) + r*(len/rl);
  }

}
