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
#include "lighting/rt/probe_pixel_cage.glsl"
#include "lighting/tonemapping.glsl"
#include "lighting/purkinje_shift.glsl"
#include "scene.glsl"
#include "common.glsl"
#if defined(MATERIAL_MAPS)
#include "lighting/material_brdf.glsl"
#include "lighting/eye_brdf.glsl"
#include "lighting/crown_light.glsl"
#endif

// No SSAO of a metre on the light of the probes: they trace what hides the sky and the lit surfaces from them, it
// would take that occlusion away a second time. What they do not see - anything nearer than their 25 cm grid, the
// hero, the people, the things - is the short occlusion of ssao.comp (VF_CONTACT_AO, graphics/contactao.h).

layout(binding  = 0, std140) uniform UboScene {
  SceneDesc scene;
  };
layout(binding  = 1) uniform sampler2D  gbufDiffuse;
layout(binding  = 2) uniform usampler2D gbufNormal;
layout(binding  = 3) uniform sampler2D  contactAo;
layout(binding  = 4) uniform sampler2D  depth;
layout(binding  = 5, std430) readonly buffer Hbo0 { uint hashTable[]; };
layout(binding  = 6, std430) readonly buffer Pbo  { ProbesHeader probeHeader; Probe probe[]; };
layout(binding  = 7) uniform sampler2D  probesLighting; // the L2 harmonics of irradiance of the probes
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
// a leaf of a tree's crown (crown_light.glsl): the light of the same probes on a surface that faces away from the eye
bool crownThrough = false;
vec3 throughDir   = vec3(0);
vec3 throughSum   = vec3(0);
#endif

vec3 unprojectDepth(const float z) {
  const ivec2 fragCoord  = ivec2(gl_FragCoord.xy);
  const vec2  screenSize = ivec2(textureSize(depth,0));

  const vec2  inPos      = vec2(2*fragCoord+ivec2(1,1))/vec2(screenSize)-vec2(1,1);
  const vec4  pos        = vec4(inPos.xy,z,1);
  const vec4  ret        = scene.viewProjectInv*pos;
  return (ret.xyz/ret.w);
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

  colorSum.rgb += probeReadIrradiance(probesLighting, probeId, pixelNorm) * weight;
  colorSum.w   += weight;
#if defined(MATERIAL_MAPS)
  if(ownMaterial)
    skyVisibilitySum += probeReadSkyVisibility(bits, pixelNorm) * weight;
  if(crownThrough)
    throughSum += probeReadIrradiance(probesLighting, probeId, throughDir) * weight;
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


void main() {
  const float minW    = uintBitsToFloat(0x00000008);
  const float z       = texelFetch(depth,ivec2(gl_FragCoord.xy),0).x;

  const vec4  diff4   = texelFetch(gbufDiffuse, ivec2(gl_FragCoord.xy), 0);
  const vec3  diff    = diff4.rgb;
  const vec3  norm    = normalFetch(gbufNormal, ivec2(gl_FragCoord.xy));
#if defined(MATERIAL_MAPS)
  const vec4 ormG = texelFetch(gbufOrm, ivec2(gl_FragCoord.xy), 0);
  const vec4 orm  = gbufOrmMaterial(ormG, diff4.a);
  // hair cards keep their own values in the ORM channels (common.glsl, GBufHairBit): the probes light them as a
  // matte surface of the hair's colour, like the pixels of the original
  ownMaterial = orm.a>0.5 && !isGBufHair(diff4.a);
#endif

  const vec3  basePos = unprojectDepth(z);
  const int   lod     = probePixelLod(basePos, scene.camPos, scene.probeLodScale, isGBufGrassBlade(diff4.a));
  const vec3  pos     = basePos + norm*probeCageBias;
#if defined(MATERIAL_MAPS)
  const bool  crown   = ownMaterial && isGBufCrown(diff4.a);
  float       through = 0.0;
  if(crown && isGBufFoliage(diff4.a)) {
    throughDir   = normalize(basePos - scene.camPos);
    through      = crownSkyThrough(ormG.b, norm, -throughDir);
    crownThrough = through>0.0;
    }
#endif

  gather(basePos, pos, norm, lod);

  if(colorSum.w<=minW) {
    // debug
    // colorSum = vec4(vec3(1,0,0)*scene.GSunIntensity,1);
    colorSum.rgb = probeReadIrradiance(probesLighting, 0, norm);
    colorSum.w   = 1;  // * Fd_Lambert is accounted in integration
#if defined(MATERIAL_MAPS)
    skyVisibilitySum = 1.0;
    if(crownThrough)
      throughSum = probeReadIrradiance(probesLighting, 0, throughDir);
#endif
    } else {
    colorSum.rgb = colorSum.rgb/max(colorSum.w, minW);
#if defined(MATERIAL_MAPS)
    skyVisibilitySum /= max(colorSum.w, minW);
    throughSum       /= max(colorSum.w, minW);
#endif
    }

  // const vec3  linear = vec3(1);
  const vec3  linear  = textureAlbedo(diff);

  // the night sky of the path tracer is in the probes' misses (probe_lighting.comp): no NightAmbient on top,
  // it would light the corners the probes see no sky from
  vec3 luminance  = vec3(0);
  luminance += colorSum.rgb;

  float ao = 1.0;
  if((scene.viewFlags & VF_CONTACT_AO)!=0u)
    ao = textureLod(contactAo, gl_FragCoord.xy*scene.screenResInv, 0).r;
#if defined(MATERIAL_MAPS)
  // a leaf and a blade of grass carry their own occlusion (the root's shade of grass.frag, the crown's of the
  // leaves): cards in the depth buffer would take it away a second time, and as noise
  if(orm.a>0.5 && isGBufFoliage(diff4.a))
    ao = 1.0;
#endif

  vec3 color = linear;
  color *= luminance * ao;
#if defined(MATERIAL_MAPS)
  if(ownMaterial && isGBufEye(diff4.a)) {
    // an eyeball (eye_brdf.glsl): the lids' shade takes the light of the probes and the sky's reflection in full
    const float lid = orm.b;
    color *= orm.r * lid;
    const vec3 view = normalize(scene.camPos - basePos);
    const float noV = max(dot(norm, view), 0.0);
    const vec3 sharpSky = textureSkyLUT(skyViewLut, vec3(0,RPlanet,0), reflect(-view, norm), scene.sunDir) * scene.GSunIntensity;
    const vec3 sky = probeReadIrradiance(probesLighting, 0, norm);
    color += mix(sharpSky, sky, orm.g*orm.g) * materialEnvBrdf(vec3(EyeF0), orm.g, noV) * lid *
             clamp(skyVisibilitySum, 0.0, 1.0);
    } else
  if(ownMaterial) {
    color *= orm.r * (1.0 - orm.b);
    const vec3 view = normalize(scene.camPos - basePos);
    const float noV = max(dot(norm, view), 0.0);
    const vec3 reflection = reflect(-view, norm);
    const vec3 sharpSky = textureSkyLUT(skyViewLut, vec3(0,RPlanet,0), reflection, scene.sunDir) * scene.GSunIntensity;
    // Probe zero contains unoccluded sky only, unlike the local GI radiance.
    const vec3 sky = probeReadIrradiance(probesLighting, 0, norm);
    const vec3 reflectedSky = mix(sharpSky, sky, orm.g*orm.g);
    color += reflectedSky * materialEnvBrdf(materialF0(linear, orm.b), orm.g, noV) *
             materialSpecularWeight(orm.g, orm.b) * clamp(skyVisibilitySum, 0.0, 1.0) * ao;
    // a tree's crown (crown_light.glsl): the light of the probes behind a leaf comes through it under the map's
    // occlusion, and the shade of the crown's depth takes its share of all of it
    if(through>0.0)
      color += linear * FoliageThroughTint * throughSum * (through * orm.r);
    if(crown)
      color *= gbufCrownShade(ormG, diff4.a);
    }
#endif
  color *= scene.exposure;
#if defined(MATERIAL_MAPS)
  // own glow (embers, burning wood): common.glsl, emissiveMeshGlow - the same as without the probes
  color += emissiveMeshGlow(orm, diff4.a, scene.GSunIntensity, scene.exposure);
#endif
  outColor = vec4(color, 1);

  // outColor = vec4(srgbEncode(linear), 0);
  // outColor = vec4(0.001 * vec3(harmonicR.r/max(harmonicR.g, 0.000001)), 0);
  // if(err)
  //   outColor = vec4(1,0,0,0);
  }
