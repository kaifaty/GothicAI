#version 460

#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable
#extension GL_EXT_control_flow_attributes : enable

// subgroups share the probe lookups of a cage (gather); SPIR-V 1.3, shader/CMakeLists.txt
#extension GL_KHR_shader_subgroup_basic   : enable
#extension GL_KHR_shader_subgroup_vote    : enable
#extension GL_KHR_shader_subgroup_ballot  : enable
#extension GL_KHR_shader_subgroup_shuffle : enable

#include "lighting/rt/probe_common.glsl"
#include "lighting/tonemapping.glsl"
#include "lighting/purkinje_shift.glsl"
#include "scene.glsl"
#include "common.glsl"
#if defined(MATERIAL_MAPS)
#include "lighting/material_brdf.glsl"
#endif

// No SSAO on the light of the probes: they trace what hides the sky and the lit surfaces from them, SSAO (1 m
// around a pixel) would take that occlusion away a second time. The path tracer has no SSAO either.

layout(binding  = 0, std140) uniform UboScene {
  SceneDesc scene;
  };
layout(binding  = 1) uniform sampler2D  gbufDiffuse;
layout(binding  = 2) uniform usampler2D gbufNormal;
layout(binding  = 3) uniform sampler2D  ssao;
layout(binding  = 4) uniform sampler2D  depth;
layout(binding  = 5, std430) readonly buffer Hbo0 { uint hashTable[]; };
layout(binding  = 6, std430) readonly buffer Pbo  { ProbesHeader probeHeader; Probe probe[]; };
layout(binding  = 7) uniform sampler2D  probesLighting;
#if defined(MATERIAL_MAPS)
layout(binding = 12) uniform sampler2D gbufOrm;
layout(binding = 13) uniform sampler2D skyViewLut;
#endif

layout(location = 0) out vec4 outColor;

vec4 colorSum  = vec4(0);
bool err       = false;
#if defined(MATERIAL_MAPS)
bool ownMaterial = false;
float skyVisibilitySum = 0.0;
#endif

vec3 unprojectDepth(const float z) {
  const ivec2 fragCoord  = ivec2(gl_FragCoord.xy);
  const vec2  screenSize = ivec2(textureSize(depth,0));

  const vec2  inPos      = vec2(2*fragCoord+ivec2(1,1))/vec2(screenSize)-vec2(1,1);
  const vec4  pos        = vec4(inPos.xy,z,1);
  const vec4  ret        = scene.viewProjectInv*pos;
  return (ret.xyz/ret.w);
  }

int probeGridComputeLod() {
  ivec2 fragCoord  = ivec2(gl_FragCoord.xy);
  ivec2 screenSize = textureSize(depth,0);

  // must be exactly same as in allocation !
  const float z = texelFetch(depth,fragCoord,0).x;
  return probeGridComputeLod(fragCoord, screenSize, z, scene.viewProjectInv, scene.probeGridBias);
  }

float texLinearDepth(vec2 uv) {
  float d = textureLod(depth, uv, 0).x;
  return linearDepth(d, scene.clipInfo);
  }

Probe mkZeroProbe() {
  Probe px;
  px.bits = UNUSED_BIT;
  return px;
  }

Probe readProbe(const ivec3 gridPos, const vec3 wpos, out uint id) {
  const uint h       = probeGridPosHash(gridPos) % hashTable.length();
  uint       probeId = hashTable[h];

  [[loop]]
  for(int i=0; i<8; ++i) {
    if(probeId>=probeHeader.count)
      return mkZeroProbe();

    const Probe p = probe[probeId];
    if((p.bits & UNUSED_BIT)!=0)
      err = true;

    if((p.bits & TRACED_BIT)==0) {
      probeId = p.pNext;
      continue;
      }

    if(!probeIsSame(p.pos, wpos, 0)) {
      probeId = p.pNext;
      continue;
      }
    id = probeId;
    return p;
    }

  return mkZeroProbe();
  }

void addProbe(vec3 wpos, uint probeId, uint bits, vec3 pixelPos, vec3 pixelNorm) {
  const vec3 ldir = wpos-pixelPos;
  if(dot(ldir, pixelNorm)<=0)
    return;
  if((bits & TRACED_BIT)==0)
    return;

  float weight = 1;
  // cosine weight from https://advances.realtimerendering.com/s2015/SIGGRAPH_2015_Remedy_Notes.pdf
  weight *= max(0.01, dot(normalize(ldir), pixelNorm));
  // distnace based weight
  weight *= 1.0/(dot(ldir,ldir) + 0.00001);
  if((bits & BAD_BIT)!=0)
    weight *= 0.00001;

  colorSum.rgb += probeReadAmbient(probesLighting, probeId, pixelNorm, vec3(0)) * weight;
  colorSum.w   += weight;
#if defined(MATERIAL_MAPS)
  if(ownMaterial)
    skyVisibilitySum += probeReadSkyVisibility(bits, pixelNorm) * weight;
#endif
  }

void processProbe(ivec3 gridPos, vec3 wpos, int lod, vec3 pixelPos, vec3 pixelNorm) {
  const vec3 ldir = wpos-pixelPos;
  if(dot(ldir, pixelNorm)<=0)
    return;

  uint probeId = 0;
  const Probe p = readProbe(gridPos, wpos, probeId);
  addProbe(wpos, probeId, p.bits, pixelPos, pixelNorm);
  }

void gather(vec3 basePos, vec3 pos, vec3 norm, int lod) {
  colorSum = vec4(0);

  probeQuery pq;
  probeQueryInitialize(pq, pos, lod);

  // The cage of a probe cell spans 14-28 pixels: most subgroups share it. Then lane i finds corner i in the hash
  // once for all lanes, and every lane weighs the eight probes for its own pixel (the same probes and weights).
  const bool sameCage = gl_SubgroupSize>=8 && subgroupAllEqual(lod) && subgroupAllEqual(pq.pLow) &&
                      subgroupAllEqual(pq.pHigh) && (subgroupBallot(true).x & 0xFFu)==0xFFu;
  if(sameCage) {
    uint myId   = 0;
    uint myBits = UNUSED_BIT;
    if(gl_SubgroupInvocationID<8) {
      probeQuery q = pq;
      probeQueryProceed(q, int(gl_SubgroupInvocationID));
      const Probe p = readProbe(probeQueryGridPos(q), probeQueryWorldPos(q), myId);
      myBits = p.bits;
      }
    for(uint i=0; i<8; ++i) {
      probeQuery q = pq;
      probeQueryProceed(q, int(i));
      const uint id   = subgroupShuffle(myId,   i);
      const uint bits = subgroupShuffle(myBits, i);
      addProbe(probeQueryWorldPos(q), id, bits, basePos, norm);
      }
    return;
    }

  while(probeQueryProceed(pq)) {
    vec3  wpos = probeQueryWorldPos(pq);
    ivec3 gPos = probeQueryGridPos(pq);
    processProbe(gPos, wpos, lod, basePos, norm);
    }
  }

#if defined(SSAO)
float textureSsao() {
  vec2  uv = (gl_FragCoord.xy / vec2(textureSize(depth,0)));
  return textureLod(ssao, uv, 0).r;
  }
#else
float textureSsao() { return 1; }
#endif

void main() {
  const float minW    = uintBitsToFloat(0x00000008);
  const float z       = texelFetch(depth,ivec2(gl_FragCoord.xy),0).x;

  const vec3  diff    = texelFetch(gbufDiffuse, ivec2(gl_FragCoord.xy), 0).rgb;
  const vec3  norm    = normalFetch(gbufNormal, ivec2(gl_FragCoord.xy));
#if defined(MATERIAL_MAPS)
  const vec4 orm = texelFetch(gbufOrm, ivec2(gl_FragCoord.xy), 0);
  ownMaterial = orm.a>0.5;
#endif

  const int   lod     = probeGridComputeLod();
  const vec3  basePos = unprojectDepth(z);
  const vec3  pos     = basePos + norm*probeCageBias;

  gather(basePos, pos, norm, lod);

  if(colorSum.w<=minW) {
    // debug
    // colorSum = vec4(vec3(1,0,0)*scene.GSunIntensity,1);
    colorSum.rgb = probeReadAmbient(probesLighting, 0, norm, vec3(0,1,0));
    colorSum.w   = 1;  // * Fd_Lambert is accounted in integration
#if defined(MATERIAL_MAPS)
    skyVisibilitySum = 1.0;
#endif
    } else {
    colorSum.rgb = colorSum.rgb/max(colorSum.w, minW);
#if defined(MATERIAL_MAPS)
    skyVisibilitySum /= max(colorSum.w, minW);
#endif
    }

  // const vec3  linear = vec3(1);
  const vec3  linear  = textureAlbedo(diff);
  const float ao      = textureSsao();

  // the night sky of the path tracer is in the probes' misses (probe_lighting.comp): no NightAmbient on top,
  // it would light the corners the probes see no sky from
  vec3 luminance  = vec3(0);
  luminance += colorSum.rgb;

  vec3 color = linear;
  color *= luminance;
#if defined(MATERIAL_MAPS)
  if(ownMaterial) {
    color *= orm.r * (1.0 - orm.b);
    const vec3 view = normalize(scene.camPos - basePos);
    const float noV = max(dot(norm, view), 0.0);
    const vec3 reflection = reflect(-view, norm);
    const vec3 sharpSky = textureSkyLUT(skyViewLut, vec3(0,RPlanet,0), reflection, scene.sunDir) * scene.GSunIntensity;
    // Probe zero contains unoccluded sky only, unlike the local GI radiance.
    const vec3 sky = probeReadAmbient(probesLighting, 0, norm, vec3(0,1,0));
    const vec3 reflectedSky = mix(sharpSky, sky, orm.g*orm.g);
    color += reflectedSky * materialEnvBrdf(materialF0(linear, orm.b), orm.g, noV) *
             materialSpecularWeight(orm.g, orm.b) * clamp(skyVisibilitySum, 0.0, 1.0);
    }
#endif
  color *= ao;
  color *= scene.exposure;
  outColor = vec4(color, 1);

  // outColor = vec4(srgbEncode(linear), 0);
  // outColor = vec4(0.001 * vec3(harmonicR.r/max(harmonicR.g, 0.000001)), 0);
  // if(err)
  //   outColor = vec4(1,0,0,0);
  }
