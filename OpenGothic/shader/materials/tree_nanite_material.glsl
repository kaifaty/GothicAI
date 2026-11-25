#ifndef TREE_NANITE_MATERIAL_GLSL
#define TREE_NANITE_MATERIAL_GLSL

#include "common.glsl"
#include "scene.glsl"
#include "lighting/crown_light.glsl"

struct TreeMaterial {
  vec4 uvScaleFlags; // xy: texture scale, z: bucket flag bits, w: translucency
  vec4 tint;
  };

layout(set=0, binding=10, std430) readonly buffer TreeMaterialBuffer { TreeMaterial materials[]; };

vec3 treeVertexNormal(vec4 normal, uint flags) {
  if((flags & BK_CROWN) != 0u)
    return normal.xyz * inversesqrt(max(dot(normal.xyz, normal.xyz), 1e-12)) * (0.5 + 0.5*normal.w);
  return normal.xyz;
  }

#if defined(TREE_MATERIAL_FRAGMENT)
layout(set=0, binding=5) uniform sampler2DArray treeAlbedo;
layout(set=0, binding=8) uniform sampler2DArray treeNormal;
layout(set=0, binding=9) uniform sampler2DArray treeOrm;

layout(location=0) out vec4 outDiffuse;
layout(location=1) out uint outNormal;
layout(location=2) out vec4 outOrm;
layout(location=3) out vec2 outMotion;

struct TreeGradients {
  vec2 uvDx, uvDy;
  vec3 posDx, posDy;
  };

vec3 treeSafeNormal(vec3 normal) {
  const float len2 = dot(normal, normal);
  return len2 > 1e-20 ? normal * inversesqrt(len2) : vec3(0, 1, 0);
  }

vec3 treeMappedNormal(vec3 normal, TreeGradients grad, vec4 texel) {
  // The G-buffer cotangent frame, using DXT5nm's X in alpha and Y in green.
  const vec3 n = treeSafeNormal(normal);
  const vec3 p = cross(grad.posDy, n);
  const vec3 q = cross(n, grad.posDx);
  const vec3 t = p * grad.uvDx.x + q * grad.uvDy.x;
  const vec3 b = p * grad.uvDx.y + q * grad.uvDy.y;
  const float len2 = max(dot(t, t), dot(b, b));
  if(len2 <= 1e-20)
    return n;
  const vec2 xy = texel.ag * 2.0 - 1.0;
  return treeSafeNormal((t*xy.x + b*xy.y) * inversesqrt(len2) + n*sqrt(max(0.0, 1.0 - dot(xy, xy))));
  }

vec2 treeMotion(vec3 previousStatic, vec3 previousDelta) {
  const float w0 = previousStatic.z;
  const float w1 = w0 + previousDelta.z;
  if(w0 <= 0.0 || w1 <= 0.0)
    return vec2(0);
  return (previousDelta.xy*w0 - previousStatic.xy*previousDelta.z) / (w0*w1) * 0.5;
  }

void treeWriteMaterial(uint layer, TreeMaterial material, vec2 uv, vec3 normal, bool front, TreeGradients grad) {
  const uint flags = floatBitsToUint(material.uvScaleFlags.z);
  const vec3 texCoord = vec3(uv, float(layer));
  const vec3 facingNormal = front && (flags & BK_TWO_SIDED_FLIP) != 0u ? -normal : normal;
  vec3 shadedNormal = treeSafeNormal(facingNormal);
  vec4 orm = vec4(1, 1, 0, (flags & BK_MATERIAL_MAPS) != 0u ? 1.0 : 0.0);
  int hints = 0; // Stage 1 rasterizes opaque triangles.
  if((flags & BK_NORMAL) != 0u)
    shadedNormal = treeMappedNormal(facingNormal, grad, textureGrad(treeNormal, texCoord, grad.uvDx, grad.uvDy));
  if((flags & BK_ORM) != 0u)
    orm.rgb = textureGrad(treeOrm, texCoord, grad.uvDx, grad.uvDy).rgb;
  if((flags & BK_FOLIAGE) != 0u && material.uvScaleFlags.w > 0.0) {
    orm.b = material.uvScaleFlags.w;
    hints |= GBufFoliageBit;
    }
  if((flags & BK_CROWN) != 0u && orm.a > 0.5) {
    orm.a = gbufCrownShadeCode(crownShade(crownOpenness(normal)));
    hints |= GBufCrownBit;
    }

  outDiffuse = vec4(textureGrad(treeAlbedo, texCoord, grad.uvDx, grad.uvDy).rgb * material.tint.rgb, float(hints)/255.0);
  outNormal  = encodeNormal(shadedNormal);
  outOrm     = orm;
  }
#endif

#endif
