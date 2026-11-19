#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "common.glsl"
#include "impostor_common.glsl"
#include "lighting/crown_light.glsl"

layout(binding = 2) uniform sampler2D atlasAlbedo;
layout(binding = 3) uniform sampler2D atlasSurface;
#if defined(MATERIAL_MAPS)
layout(binding = 4) uniform sampler2D atlasOrm;
#endif

layout(location = 0) in vec3 centeredPosition;
layout(location = 1) flat in vec3 centeredEye;
layout(location = 2) flat in uint instanceId;
layout(location = 3) flat in mat3 normalTransform;

layout(location = 0) out vec4 outDiffuse;
layout(location = 1) out uint outNormal;
#if defined(MATERIAL_MAPS)
layout(location = 2) out vec4 outOrm;
layout(location = 3) out vec2 outMotion;
#if defined(HISTORY_OWNER)
layout(location = 4) out uint outHistoryOwner;
#endif
#else
layout(location = 2) out vec2 outMotion;
#if defined(HISTORY_OWNER)
layout(location = 3) out uint outHistoryOwner;
#endif
#endif

const int   Grid = 12;
const float Tile = 128.0;
const float Border = 16.0;
const float Frame = Tile - 2.0*Border;

vec2 nonZeroSign(vec2 v) {
  return mix(vec2(-1), vec2(1), greaterThanEqual(v, vec2(0)));
  }

vec2 octEncode(vec3 v) {
  v /= dot(abs(v), vec3(1));
  return v.y<0.0 ? (1.0 - abs(v.zx))*nonZeroSign(v.xz) : v.xz;
  }

vec3 octDecode(vec2 p) {
  vec3 v = vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
  if(v.y<0.0)
    v.xz = (1.0 - abs(v.zx))*nonZeroSign(v.xz);
  return normalize(v);
  }

vec2 frameUV(mat3 basis, vec3 centered) {
  return vec2(dot(basis[0], centered), -dot(basis[1], centered))*0.5 + 0.5;
  }

vec2 atlasUV(ivec2 frame, vec2 uv) {
  return (vec2(frame)*Tile + Border + uv*Frame) / (float(Grid)*Tile);
  }

struct ViewSample {
  vec4  albedo;
  vec3  normal;
  vec3  orm;
  float openness;
  float depth;
  int   materialClass;
  };

ViewSample sampleView(ivec2 frame, vec3 ray, vec3 dx, vec3 dy, float lastMip) {
  ViewSample result = ViewSample(vec4(0), vec3(0), vec3(0), 0.0, 0.0, 0);
  const vec3 direction = octDecode(vec2(frame)*(2.0/float(Grid-1)) - 1.0);
  const mat3 basis = impostorBasis(direction);
  const vec2 uvDx = (frameUV(basis, dx) - 0.5)*Frame;
  const vec2 uvDy = (frameUV(basis, dy) - 0.5)*Frame;
  const float lod = clamp(0.5*log2(max(1.0, max(dot(uvDx,uvDx), dot(uvDy,uvDy)))), 0.0, lastMip);

  // First intersect the ray with this capture's central plane. One depth lookup moves that intersection
  // onto the surface in the capture. Padded, dilated texels support the lookup just outside the silhouette.
  const float rayDot = dot(ray, direction);
  if(rayDot>=-0.1)
    return result;
  const float planeDot = dot(centeredPosition, direction);
  const vec3 plane = centeredPosition - ray*(planeDot/rayDot);
  const float padding = (Border - 0.5*exp2(ceil(lod)))/Frame;
  const vec2 initialUV = clamp(frameUV(basis, plane), vec2(-padding), vec2(1.0+padding));
  const float depth = textureLod(atlasSurface, atlasUV(frame, initialUV), lod).a*2.0 - 1.0;
  const vec2 corrected = frameUV(basis, centeredPosition + ray*((depth-planeDot)/rayDot));
  if(any(lessThan(corrected, vec2(0))) || any(greaterThan(corrected, vec2(1))))
    return result;

  const vec2 uv = atlasUV(frame, corrected);
  result.albedo = textureLod(atlasAlbedo, uv, lod);
  if(result.albedo.a<=0.0)
    return result;
  const vec4 surface = textureLod(atlasSurface, uv, lod);
  result.normal = octDecode(surface.rg*2.0 - 1.0);
  result.openness = surface.b;
  result.depth = (surface.a*2.0 - 1.0 - planeDot)/rayDot;
#if defined(MATERIAL_MAPS)
  result.orm = textureLod(atlasOrm, uv, lod).rgb;
  const int mip = int(floor(lod+0.5));
  const ivec2 texel = ivec2(uv*vec2(textureSize(atlasOrm, mip)));
  result.materialClass = int(round(texelFetch(atlasOrm, texel, mip).a*4.0));
#endif
  return result;
  }

void main() {
  const vec2 grid = (octEncode(normalize(centeredEye))*0.5 + 0.5)*float(Grid-1);
  const ivec2 cell = min(ivec2(grid), ivec2(Grid-2));
  const vec2 v = grid - vec2(cell);
  const bool alongX = v.x>=v.y;
  const ivec2 middle = alongX ? ivec2(1,0) : ivec2(0,1);
  const vec3 angular = vec3(1.0-max(v.x,v.y), abs(v.x-v.y), min(v.x,v.y));

  const vec3 ray = normalize(centeredPosition - centeredEye);
  const vec3 dx = dFdx(centeredPosition)*scene.mipGradScale;
  const vec3 dy = dFdy(centeredPosition)*scene.mipGradScale;
  const float lastMip = float(min(4, textureQueryLevels(atlasSurface)-1));
  const ViewSample a = sampleView(cell, ray, dx, dy, lastMip);
  const ViewSample b = sampleView(cell + middle, ray, dx, dy, lastMip);
  const ViewSample c = sampleView(cell + 1, ray, dx, dy, lastMip);
  const ImpostorInstance instance = instances[instanceId];
  vec3 weights = angular*vec3(a.albedo.a, b.albedo.a, c.albedo.a);
  const float coverage = weights.x + weights.y + weights.z;
  float threshold = 0.5;
  if((scene.viewFlags & VF_TAA)!=0u) {
    // The mips keep fractional coverage. As with the source's masked material, TAA can resolve the
    // texels below 0.5, including gaps where more than one tree falls on the same pixel.
    const float layer = float(pcgHash(instance.info.x) >> 8)*(1.0/16777216.0);
    const float step = float(scene.taaFrame % 64u)*0.61803399;
    threshold = max(fract(interleavedGradientNoise(gl_FragCoord.xy) + layer + step), 0.5/255.0);
    }
  if(coverage<threshold)
    discard;
  weights /= coverage;

  const mat4 transform = instance.objectToWorld;
  const float depth = dot(weights, vec3(a.depth, b.depth, c.depth));
  const vec3 point = push.centerRadius.xyz + (centeredPosition + ray*depth)*push.centerRadius.w;
  const vec4 clip = scene.viewProject * (transform * vec4(point, 1));
  if(clip.w<=0.0 || clip.z<0.0 || clip.z>clip.w)
    discard;
  gl_FragDepth = clip.z/clip.w;

  vec3 albedo = a.albedo.rgb*weights.x + b.albedo.rgb*weights.y + c.albedo.rgb*weights.z;
  vec3 normal = a.normal*weights.x + b.normal*weights.y + c.normal*weights.z;
  const ViewSample dominant = weights.x>=max(weights.y,weights.z) ? a : (weights.y>=weights.z ? b : c);
  if(dot(normal,normal)<1e-8)
    normal = dominant.normal;
  normal = normalize(normalTransform * normal);
  int hints = 1 << 2; // the usual G-buffer alpha-test hint
#if defined(MATERIAL_MAPS)
  const int materialClass = dominant.materialClass;
  // Metalness and foliage transmission share a channel. Average each material class only with its own.
  const vec3 sameClass = vec3(a.materialClass==materialClass, b.materialClass==materialClass,
                             c.materialClass==materialClass);
  const vec3 materialWeights = weights*sameClass;
  const float materialWeight = dot(materialWeights, vec3(1));
  const vec3 orm = (a.orm*materialWeights.x + b.orm*materialWeights.y + c.orm*materialWeights.z)/materialWeight;
  outOrm = vec4(orm, materialClass!=0 ? 1.0 : 0.0);
  const bool foliage = materialClass==2 || materialClass==4;
  const bool crown = materialClass==3 || materialClass==4;
  const uint disabled = instance.info.y;
  const bool foliageLight = (disabled & 2u)==0u;
  if(foliage) {
    if(foliageLight && outOrm.b>0.0)
      hints |= GBufFoliageBit;
    if(!foliageLight)
      outOrm.b = 0.0;
    }
  if(crown && foliageLight && (disabled & 1u)==0u) {
    const float openness = dot(materialWeights, vec3(a.openness, b.openness, c.openness))/materialWeight;
    if((scene.viewFlags & VF_CROWN_MASK)!=0u) {
      albedo = crownMask(openness, outOrm.b);
      } else {
      outOrm.a = gbufCrownShadeCode(crownShade(openness));
      hints |= GBufCrownBit;
      }
    }
#endif
  outDiffuse = vec4(albedo, float(hints)/255.0);
  outNormal = (scene.viewFlags & VF_TREE_OCCLUSION_PROBE)!=0u ? instance.info.x+1u : encodeNormal(normal);
#if defined(HISTORY_OWNER)
  // Keep the instance ID for parity with the 3a probe, outside the sampled
  // cluster ID range. Impostors never seed the history.
  outHistoryOwner = 0x80000000u | (instance.info.x+1u);
#endif
  // The camera's motion is reconstructed from G-buffer depth. Distant static objects have zero residual motion.
  outMotion = vec2(0);
  }
