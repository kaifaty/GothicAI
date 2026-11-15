#ifndef WINDOW_GLOW_GLSL
#define WINDOW_GLOW_GLSL

// Windows that glow at night (reports/render-features/2026-11-15-night-interiors): a material with `emissive` and
// `window` in materials.json (BK_WINDOW). The ORM's alpha marks what glows; every repeat of the texture is one
// window with its own evening. The numbers are those of common/graphics/windowglow.h: keep them in step.

const float WindowOnFrom    = 21.25;
const float WindowOnSpread  = 1.0;
const float WindowOffSpread = 2.0;
const float WindowFade      = 0.2;
const float WindowDawn      = 28.75;
const float WindowMinUntil  = 18.0;
const float WindowMaxUntil  = 36.0;
const float WindowDimmest   = 0.55;
// A window's numbers drift from wall to wall over this many cm, so that two walls with the same repeats differ
const float WindowWallCell  = 700.0;

float windowPackedLit(uint bits) {
  return float(bits & 0xFFu)/255.0;
  }

float windowPackedUntil(uint bits) {
  return WindowMinUntil + float((bits>>8) & 0xFFu)*((WindowMaxUntil-WindowMinUntil)/255.0);
  }

uint windowHash(uint x) {
  x ^= x >> 15; x *= 0x2C1B3C6Du;
  x ^= x >> 12; x *= 0x297A2D39u;
  x ^= x >> 15;
  return x;
  }

vec3 windowHash3(ivec3 p) {
  const uint h = windowHash(uint(p.x)*0x9E3779B1u ^ windowHash(uint(p.y)*0x85EBCA77u ^ windowHash(uint(p.z)*0xC2B2AE3Du + 0x27D4EB2Fu)));
  return vec3(float(h & 0x3FFu), float((h>>10) & 0x3FFu), float((h>>20) & 0x3FFu))/1024.0;
  }

// smooth over the lattice: a small error of the place moves the numbers a little, never to another window's
vec3 windowWallNoise(vec3 p) {
  const ivec3 i = ivec3(floor(p));
  vec3  f = fract(p);
  f = f*f*(3.0-2.0*f);
  const vec3 a = mix(mix(windowHash3(i+ivec3(0,0,0)), windowHash3(i+ivec3(1,0,0)), f.x),
                     mix(windowHash3(i+ivec3(0,1,0)), windowHash3(i+ivec3(1,1,0)), f.x), f.y);
  const vec3 b = mix(mix(windowHash3(i+ivec3(0,0,1)), windowHash3(i+ivec3(1,0,1)), f.x),
                     mix(windowHash3(i+ivec3(0,1,1)), windowHash3(i+ivec3(1,1,1)), f.x), f.y);
  return mix(a, b, f.z);
  }

// The light of the window a pixel belongs to, 0..1 (WindowGlow::light with a slow flicker of the candle behind it).
// uv: of the mesh, a repeat is a window; pos: the pixel in the world, cm; the derivatives are a pixel's steps of both;
// dayHour: 0..24; tick: ms
float windowLight(uint bits, vec2 uv, vec3 pos, vec2 duvdx, vec2 duvdy, vec3 dpdx, vec3 dpdy, float dayHour, uint tick) {
  // by day no window burns: nothing below is worth its cost
  const float e = dayHour<12.0 ? dayHour+24.0 : dayHour;
  if(e<WindowOnFrom || e>WindowDawn+WindowFade)
    return 0.0;
  // the middle of the repeat in the world: pos is linear in uv over a flat wall
  const vec2  f   = fract(uv) - vec2(0.5);
  const float det = duvdx.x*duvdy.y - duvdx.y*duvdy.x;
  vec3 centre = pos;
  if(abs(det)>1e-12) {
    const vec3 dpdu = (dpdx*duvdy.y - dpdy*duvdx.y)/det;
    const vec3 dpdv = (dpdy*duvdx.x - dpdx*duvdy.x)/det;
    centre = pos - dpdu*f.x - dpdv*f.y;
    }
  const ivec2 tile = ivec2(floor(uv));
  const vec3  r    = fract(windowHash3(ivec3(tile, 7)) + windowWallNoise(centre/WindowWallCell));

  const float lit = windowPackedLit(bits), until = windowPackedUntil(bits);
  if(r.x>=lit)
    return 0.0;
  const float on = WindowOnFrom + r.y*WindowOnSpread;
  float off = until + (r.z-0.5)*WindowOffSpread;
  if(until>=WindowDawn)
    off = WindowDawn - r.z*0.5;
  off = clamp(off, on+0.5, WindowDawn);
  const float s = smoothstep(on, on+WindowFade, e)*(1.0 - smoothstep(off, off+WindowFade, e));
  if(s<=0.0)
    return 0.0;
  // a candle behind a window: a slow sway of a tenth, each window in its own time; the clock wraps at 20 pi s
  const float t = float(tick % 62832u)*0.001;
  const float flicker = 1.0 - 0.06*(1.0 + sin(t*2.3 + r.y*40.0)) - 0.03*(1.0 + sin(t*5.9 + r.z*40.0));
  return s*mix(WindowDimmest, 1.0, fract(r.y*7.31 + r.z*3.17))*flicker;
  }

#endif
