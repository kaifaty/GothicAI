#pragma once

#include <Tempest/Log>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "graphics/foliagewind.h"
#include "settings.h"

// The light of a tree's crown (reports/engine/2026-11-16-modern-trees/crown-light.md; BK_CROWN of scene.glsl,
// shader/lighting/crown_light.glsl): a material with `"foliage": {"anchor": "tree"}` darkens into the crown by the
// openness its vertices carry (COLOR_0 A of the own GLB), and the sky and the lamps shine through its leaves as the
// sun does. [INTERNAL] crownLight=0|1; OPENGOTHIC_CROWN_LIGHT=0|1 wins, `mask` paints the crown by its openness in
// place of the light (the measure of the note). Read once
namespace CrownLight {

enum Mode : uint8_t {
  Off  = 0,
  On   = 1,
  Mask = 2,
  };

inline Mode parse(std::string_view env, bool ini) {
  if(env.empty())
    return ini ? On : Off;
  if(env=="mask")
    return Mask;
  return env=="0" ? Off : On;
  }

inline Mode mode() {
  static const Mode m = [] {
    const char* env = std::getenv("OPENGOTHIC_CROWN_LIGHT");
    const bool  set = env!=nullptr && *env!='\0';
    const Mode  v   = parse(set ? std::string_view(env) : std::string_view(), ::Settings::getI("INTERNAL","crownLight")!=0);
    Tempest::Log::i("render: crown light = ", v==Mask ? "mask" : (v==On ? "1" : "0"),
                    set ? " by OPENGOTHIC_CROWN_LIGHT" : " by [INTERNAL] crownLight");
    return v;
    }();
  return m;
  }

inline bool enabled() {
  return mode()!=Off;
  }

// The processor's copy of the numbers of shader/lighting/crown_light.glsl and shader/common.glsl, and of the ride
// of the openness in the normal's length (shader/materials/vertex_process.glsl)
static constexpr float ShadeFloor = 0.25f; // CrownShadeFloor
static constexpr float OpenTop    = 0.9f;  // CrownOpenTop
static constexpr float SkyLobe    = 0.4f;  // CrownSkyLobe
static constexpr float OrmCode    = 0.49f; // EmissiveOrmCode

// the length the vertex shader gives the normal of a vertex with this alpha of COLOR_0
inline float normalLength(uint8_t alpha) {
  return 0.5f + 0.5f*(float(alpha)/255.f);
  }

// crownOpenness: the openness out of that length; its top tenth reads as 1 (normals of different directions come
// out shorter between the vertices)
inline float openness(float normalLength) {
  return std::clamp((normalLength*2.f - 1.f)/OpenTop, 0.f, 1.f);
  }

// crownShade: what the crown leaves of the light of a pixel at this openness
inline float shade(float openness) {
  return ShadeFloor + (1.f - ShadeFloor)*openness;
  }

// gbufCrownShadeCode and gbufCrownShade through the eight bits of the G-buffer's ORM alpha
inline uint8_t shadeCode(float shade) {
  return uint8_t((1.f - OrmCode*(1.f - shade))*255.f + 0.5f);
  }

inline float shadeOfCode(uint8_t code) {
  return std::clamp(1.f - (1.f - float(code)/255.f)/OrmCode, 0.f, 1.f);
  }

// crownSkyThrough: the share of the irradiance behind a leaf that comes through it; facing: the cosine between the
// leaf's normal and the eye
inline float skyThrough(float translucency, float facing) {
  return SkyLobe * translucency * std::clamp(1.f + 1.5f*facing, 0.f, 1.f);
  }

// BK_CROWN of a material: a tree's, while the light of the foliage is on (FoliageWind::enabled). A glowing material
// is none: the glow's share and the crown's shade ride in the same alpha of the G-buffer
inline bool lit(const FoliageWind::Params& p, uint8_t halves, bool on, float emissive = 0.f) {
  return on && emissive<=0.f && p.on && p.anchor==FoliageWind::AnchorTree && (halves & FoliageWind::Light)!=0;
  }

}
