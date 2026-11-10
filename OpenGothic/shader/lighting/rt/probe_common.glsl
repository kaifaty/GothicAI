#ifndef PROBE_COMMON_GLSL
#define PROBE_COMMON_GLSL

const uint UNUSED_BIT = 0x1;
const uint TRACED_BIT = 0x2;
const uint REUSE_BIT  = 0x4;
const uint BAD_BIT    = 0x8;
const uint NEW_BIT    = 0x10; //for debug view
const uint SEEN_BIT   = 0x20; // a pixel of this frame takes the probe (REUSE_BIT of the vote, kept through the frame)

#if defined(MATERIAL_MAPS)
uint probePackSkyVisibility(uint bits, vec3 negativeVisibility, vec3 positiveVisibility) {
  // The low byte belongs to the existing probe lifecycle flags. Six 4-bit
  // coefficients share its upper bytes without changing the probe layout.
  uint ret = bits & 0xFFu;
  for(int axis=0; axis<3; ++axis) {
    const uint negative = uint(clamp(negativeVisibility[axis], 0.0, 1.0)*15.0 + 0.5);
    const uint positive = uint(clamp(positiveVisibility[axis], 0.0, 1.0)*15.0 + 0.5);
    ret |= negative << (8u + 8u*uint(axis));
    ret |= positive << (12u + 8u*uint(axis));
    }
  return ret;
  }

float probeReadSkyVisibility(uint bits, vec3 norm) {
  float ret = 0.0;
  for(int axis=0; axis<3; ++axis) {
    const uint shift = 8u + 8u*uint(axis) + (norm[axis]>=0.0 ? 4u : 0u);
    ret += float((bits >> shift) & 0xFu) * (1.0/15.0) * norm[axis]*norm[axis];
    }
  return ret;
  }
#endif

// The probes keep their light scaled down by this: the samples' radiance (RGB9E5, up to 65408) and the cubes (RGBA16F,
// up to 65504). By day a lamp lights a surface next to it with 10^5..10^6: the path tracer's factor max(1,exposure)/
// exposure is about 1/8e-6 at noon (sky_exposure.comp). Clipped, the plank hut at noon lost half of the lamps' bounce,
// a quarter of its reflected light (step-2b.md, «2б-1: пересмотр»). 2^-8 holds up to 1,7e7; a dark corner at night
// (1e-3) keeps a precision of about 1,5 %.
const float probeStoreScale = 1.0/256.0;

// RGB9E5 (shared exponent) in an R32UI texel, rounded to nearest: the radiance of a probe's sample
uint packRGB9E5(vec3 rgb) {
  const float maxValue = 65408.0; // (511/512) * 2^16
  rgb = clamp(rgb, vec3(0), vec3(maxValue));
  const float maxc = max(rgb.r, max(rgb.g, rgb.b));
  int   e     = max(-16, int(floor(log2(max(maxc, 1e-30))))) + 1;
  float denom = exp2(float(e - 9));
  if(uint(floor(maxc/denom + 0.5))==512u) {
    denom *= 2.0;
    e     += 1;
    }
  const uvec3 m = min(uvec3(floor(rgb/denom + 0.5)), uvec3(511u));
  return m.r | (m.g << 9) | (m.b << 18) | (uint(e + 15) << 27);
  }

vec3 unpackRGB9E5(uint v) {
  const float scale = exp2(float(int(v >> 27) - 15 - 9));
  return vec3(float(v & 0x1FFu), float((v >> 9) & 0x1FFu), float((v >> 18) & 0x1FFu)) * scale;
  }

struct ProbesHeader { // 64 bytes
  uint count;
  uint iterator;
  uint iterator2;
  uint tracedCount;
  uint watch; //debug
  uint newCount; // probes allocated this frame, listed for probe_trace.comp and probe_relight_new.comp
  uint iterator3;
  uint frame;      // the frame number: the stamp of a probe's use (Probe::lastUse)
  uint evictAge;   // a probe unused for more frames is freed (probe_prune): the expiry, or less when places run out
  uint reqCount;   // requests of probes at hits, for the next frame (probe_relight*, probe_hit_alloc)
  uint hitCount;   // probes allocated at hits this frame (counter)
  uint freedCount; // probes freed this frame (counter)
  uint expireAge;  // frames: the expiry T of an unused probe; the width of the age histogram's buckets follows it
  uint lightReset; // 1: the time of day jumped, every live probe is lit anew as a new one (probe_prune)
  uint padd1[2];
  };

struct Probe { // 32 bytes
  vec3 pos;
  uint pNext;
  uint normal;  // packSnorm4x8: the normal of the pixel or the hit it was made for (debug)
  uint lastUse; // ProbesHeader::frame when a pixel or a sample of another probe last took it
  uint turn;    // the relights of the probe so far (probe_lighting.comp): which of its samples are relit next
  uint bits;
  };

// A request for a probe at a hit (probe_lighting.comp), allocated in the next frame through the vote table
struct ProbeRequest {
  ivec3 gridPos; // probeQueryGridPos: in steps of lod 0
  uint  normal;
  };

const uint  probeAgeBuckets = 64;   // the histogram of ages in probe_prune: which probes to free when places run out
const uint  probeRequestKey = 0x80000000u; // voteTable: a request at a hit, above any pixel's vote
const uint  probeRequestBusy = 0xFFFFFFFEu; // voteTable: a bucket a request is being written for

const float dbgViewRadius    = 3;
const float probeGridStep    = 25;
const float probeCageBias    = 2.5;
const float probeBadHitT     = 25/5.0;
const float probeRayDistance = 200*100; // Lumen rt-probe uses 200-meters range
const float SKY_DEPTH        = 0.999995;

ivec2 gbufferCoord(const uint probeId, const uint sampleId) {
  uint x = (probeId     ) & 0xFF;
  uint y = (probeId >> 8);
  x = (x << 4) + ((sampleId     ) & 0xF);
  y = (y << 4) + ((sampleId >> 4) & 0xF);
  return ivec2(x,y);
  }

// The irradiance of a probe (step-2b.md, «2б-2: сделано»): L2 spherical harmonics of irradiance/pi with the cosine's
// convolution in them (Ramamoorthi, Hanrahan 2001), 27 numbers in 7 RGBA16F texels, 4x2 a probe (one spare): 4 MB.
// The cube of six faces it replaced read a slanted normal as a mix of faces that took the hemispheres round the axes,
// directions behind the pixel's plane among them. Against the integral of the 256 samples by the pixel's normal (E5),
// cell by cell in the reflected light: the cube 3,9 % off by day and 8,2 % by night, L2 1,4 % and 2,2 %, an
// octahedron of 6x6 texels 2,3 % and 4,3 % for 0,1 ms more and 32 MB; a window against the ringing (Lanczos) 4,9 %.
const int probeIrrTileW = 4;
const int probeIrrTileH = 2;

ivec2 probeIrrTileCoord(const uint probeId) {
  uint x = (probeId     ) & 0xFF;
  uint y = (probeId >> 8) & 0xFF;
  return ivec2(x*probeIrrTileW, y*probeIrrTileH);
  }

ivec2 probeIrrTexel(const uint probeId, int i) {
  return probeIrrTileCoord(probeId) + ivec2(i%probeIrrTileW, i/probeIrrTileW);
  }

void probeSh9(vec3 d, out float y[9]) {
  y[0] = 0.282095;
  y[1] = 0.488603*d.y; y[2] = 0.488603*d.z; y[3] = 0.488603*d.x;
  y[4] = 1.092548*d.x*d.y; y[5] = 1.092548*d.y*d.z; y[6] = 0.315392*(3.0*d.z*d.z-1.0);
  y[7] = 1.092548*d.x*d.z; y[8] = 0.546274*(d.x*d.x-d.y*d.y);
  }

// irradiance/pi for a normal from the 27 coefficients (rgb of 9, in a row) packed into 7 texels; the ringing of a
// bright patch in the dark may go below zero on the far side
vec3 probeShEval(const vec4 t[7], vec3 n) {
  float y[9];
  probeSh9(n, y);
  float f[28];
  for(int i=0; i<7; ++i) {
    f[4*i+0] = t[i].x; f[4*i+1] = t[i].y; f[4*i+2] = t[i].z; f[4*i+3] = t[i].w;
    }
  vec3 e = vec3(0);
  for(int k=0; k<9; ++k)
    e += vec3(f[3*k], f[3*k+1], f[3*k+2])*y[k];
  return max(e, vec3(0));
  }

uint probeGridPosHash(ivec3 gridPos) {
  return (gridPos.x * 18397) + (gridPos.y * 20483) + (gridPos.z * 29303);
  }

// irradiance/pi of the probe for a normal: 7 reads
vec3 probeReadIrradiance(in sampler2D irradiance, uint id, vec3 n) {
  vec4 t[7];
  for(int i=0; i<7; ++i)
    t[i] = texelFetch(irradiance, probeIrrTexel(id, i), 0);
  return probeShEval(t, n) * (1.0/probeStoreScale);
  }

// The lod of the probe grid at a point: by its distance to the camera, not by the depth of the view, so a probe off
// the screen or behind the camera has the lod it had on the screen. Spherical shells around the camera: lod 0 up to
// 2/lodScale, each next one twice as far and twice as coarse (scene.probeLodScale, SceneGlobals::setResolution).
// The one function for the pixels (allocation, probe_ambient.frag), the hits (probe_lighting.comp) and the lookups.
int probeGridLod(vec3 pos, vec3 camPos, float lodScale) {
  const float d = distance(pos, camPos)*lodScale;
  return clamp(int(log2(max(d, 1e-6))), 0, 12);
  }

bool probeIsSame(vec3 ppos, vec3 expectedPos, int lod) {
  // NOTE: reconstructed position may jitter a little
  vec3 dp = (ppos - expectedPos);
  if(dot(dp,dp)>1.0)
    return false;
  return true;
  // const float step = probeGridStep*(1 << lod);
  // const vec3  dp   = abs(ppos - expectedPos);
  // return all(lessThan(dp, vec3(step)));
  }

struct probeQuery {
  int   lod;
  float step;
  ivec3 pLow, pHigh;

  int   iterator;
  ivec3 px;
  };

void probeQueryInitialize(out probeQuery q, vec3 pos, int lod) {
  const float step    = probeGridStep*(1 << lod);
  const vec3  gridPos = pos/step;

  q.pLow     = ivec3(floor(gridPos));
  q.pHigh    = ivec3(ceil (gridPos));
  q.iterator = 0;
  q.step     = step;
  q.lod      = lod;
  }

bool probeQueryProceed(inout probeQuery q) {
  bool x = (q.iterator & 0x1)!=0;
  bool y = (q.iterator & 0x2)!=0;
  bool z = (q.iterator & 0x4)!=0;

  ivec3 px = ivec3(x ? q.pLow.x : q.pHigh.x,
                   y ? q.pLow.y : q.pHigh.y,
                   z ? q.pLow.z : q.pHigh.z);

  q.px      = px;
  q.iterator++;
  return q.iterator<=8;
  }

bool probeQueryProceed(inout probeQuery q, int id) {
  q.iterator = id;
  return probeQueryProceed(q);
  }

vec3 probeQueryWorldPos(in probeQuery q) {
  return q.px*q.step;
  }

ivec3 probeQueryGridPos(in probeQuery q) {
  return q.px*(1 << q.lod);
  }

#endif
