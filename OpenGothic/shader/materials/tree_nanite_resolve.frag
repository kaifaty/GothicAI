#version 460
#extension GL_GOOGLE_include_directive : require

#define TREE_MATERIAL_FRAGMENT
#include "tree_nanite_geometry.glsl"
#include "tree_nanite_material.glsl"

// Cut the resolve at cumulative boundaries while keeping the same attachments.
// 0: complete, 1: visibility lookup, 2: attributes, 3: attributes and gradients.
layout(constant_id=0) const uint resolveMode = 0u;
layout(set=0, binding=6) uniform usampler2D treeVisibilityBuffer;
#if defined(VERIFY)
layout(location=4) out uint outEdgeMask;
#endif

struct TreeInterpolation {
  vec3 q;
  vec3 dqDx, dqDy;
  };

float sum3(vec3 v) {
  return v.x + v.y + v.z;
  }

TreeInterpolation treeInterpolation(vec4 a, vec4 b, vec4 c, vec2 pixel) {
  // Perspective-correct weights solve sum(w_i*clip_i.xy) = p*sum(w_i*clip_i.w),
  // with sum(w_i)=1. The cross product solves both homogeneous equations
  // before the final normalization. It also works for vertices behind the eye.
  const vec2 p = (pixel - 0.5*frame.viewport.xy) * (2.0*frame.viewport.zw);
  const vec3 w = vec3(a.w, b.w, c.w);
  const vec3 x = fma(vec3(-p.x), w, vec3(a.x, b.x, c.x));
  const vec3 y = fma(vec3(-p.y), w, vec3(a.y, b.y, c.y));
  return TreeInterpolation(cross(x, y), cross(-2.0*frame.viewport.z*w, y), cross(x, -2.0*frame.viewport.w*w));
  }

vec3 treeQuadDerivative(vec3 q, vec3 dq, float oddPixel) {
  // Fine derivatives use the left/right or top/bottom sample centers of the
  // raster quad. Evaluate that difference on this triangle, including helpers
  // outside the primitive. Resolving another triangle's UVs would diverge at seams.
  const vec3 low = q - oddPixel*dq;
  const float lowSum = sum3(low);
  const float highSum = lowSum + sum3(dq);
  if(lowSum == 0.0 || highSum == 0.0)
    return vec3(0);
  return (dq - (low/lowSum)*sum3(dq))/highSum;
  }

#if defined(VERIFY)
float clippedTreeArea(vec4 a, vec4 b, vec4 c) {
  // Clip z=0 before dividing; a triangle crossing the eye may have a vertex
  // with w<=0, even though the visible polygon has positive w everywhere.
  const vec4 triangle[3] = vec4[3](a, b, c);
  vec4 polygon[4];
  uint count = 0;
  for(uint i=0; i<3; ++i) {
    const vec4 u = triangle[i];
    const vec4 v = triangle[(i+1)%3];
    if(u.z >= 0.0)
      polygon[count++] = u;
    if((u.z < 0.0 && v.z > 0.0) || (u.z > 0.0 && v.z < 0.0))
      polygon[count++] = mix(u, v, u.z/(u.z-v.z));
    }
  if(count < 3u || polygon[0].w <= 0.0)
    return 0.0;
  const vec2 origin = polygon[0].xy/polygon[0].w;
  float twiceArea = 0.0;
  for(uint i=1; i+1<count; ++i) {
    if(polygon[i].w <= 0.0 || polygon[i+1].w <= 0.0)
      return 0.0;
    const vec2 u = (polygon[i].xy/polygon[i].w - origin) * frame.viewport.xy*0.5;
    const vec2 v = (polygon[i+1].xy/polygon[i+1].w - origin) * frame.viewport.xy*0.5;
    twiceArea += u.x*v.y - u.y*v.x;
    }
  return 0.5*abs(twiceArea);
  }

uint treeEdgeMask(TreeInterpolation interp, vec4 a, vec4 b, vec4 c, float qSum) {
  // Each unnormalized weight is zero along the corresponding screen-space
  // line. Divide by its pixel gradient to get perpendicular distance in pixels.
  const vec3 norms = sqrt(interp.dqDx*interp.dqDx + interp.dqDy*interp.dqDy);
  bool nearEdge = any(lessThanEqual(abs(interp.q), norms));
  const vec3 z = vec3(a.z, b.z, c.z);
  const bool clipped = any(lessThan(z, vec3(0))) && any(greaterThanEqual(z, vec3(0)));
  if(clipped) {
    const vec2 dz = vec2(dot(interp.dqDx, z), dot(interp.dqDy, z));
    nearEdge = nearEdge || abs(dot(interp.q, z)) <= length(dz);
    }
  return (nearEdge ? 1u : 0u) | (clippedTreeArea(a, b, c)<1.0 ? 2u : 0u) |
         (clipped ? 4u : 0u) | (qSum == 0.0 ? 8u : 0u);
  }
#endif

void main() {
  const ivec2 pixel = ivec2(gl_FragCoord.xy);
  const uvec2 id = texelFetch(treeVisibilityBuffer, pixel, 0).rg;
  if(id.x == 0u)
    discard; // Preserve the existing G-buffer over world geometry and background.

  if(resolveMode == 1u) {
    outDiffuse = vec4(1);
    outNormal = 0u;
    outOrm = vec4(0);
    outMotion = vec2(0);
    return;
    }

  const uvec4 record = records[id.x-1u];
  const uvec4 meshlet = meshlets[record.x];
  const uvec3 indices = treeTriangle(meshlet.y + (id.y & TreePrimitiveMask));
  const TreeVertex a = treeVertex(meshlet.x + indices.x);
  const TreeVertex b = treeVertex(meshlet.x + indices.y);
  const TreeVertex c = treeVertex(meshlet.x + indices.z);
  const TreeInstance instance = instances[record.y];
  const TreeMaterial material = materials[record.z];
  const uint flags = floatBitsToUint(material.uvScaleFlags.z);

  const mat3 position = mat3(treeWorldPosition(a, instance), treeWorldPosition(b, instance), treeWorldPosition(c, instance));
  const vec4 clipA = treeClipPosition(position[0]);
  const vec4 clipB = treeClipPosition(position[1]);
  const vec4 clipC = treeClipPosition(position[2]);
  const TreeInterpolation interp = treeInterpolation(clipA, clipB, clipC, gl_FragCoord.xy);
  const float qSum = sum3(interp.q);
  const vec3 weights = qSum == 0.0 ? vec3(1, 0, 0) : interp.q/qSum;
  const mat3x2 texCoords = mat3x2(a.uv.xy, b.uv.xy, c.uv.xy);
  const vec2 uv = (texCoords * weights) * material.uvScaleFlags.xy;
  const mat3 normals = mat3(treeVertexNormal(a.normal, flags), treeVertexNormal(b.normal, flags),
                            treeVertexNormal(c.normal, flags));
  const vec3 normal = normals * weights;
  const mat3 prevStatic = mat3(treePreviousStatic(position[0]), treePreviousStatic(position[1]), treePreviousStatic(position[2]));
  const mat3 prevDelta = mat3(treePreviousDelta(a, instance), treePreviousDelta(b, instance), treePreviousDelta(c, instance));
  outMotion = treeMotion(prevStatic * weights, prevDelta * weights);

  if(resolveMode == 2u) {
    outDiffuse = vec4(fract(uv), 0, 0);
    outNormal = encodeNormal(treeSafeNormal(normal));
    outOrm = vec4(0);
    return;
    }

  const vec3 dx = treeQuadDerivative(interp.q, interp.dqDx, float(pixel.x & 1));
  const vec3 dy = treeQuadDerivative(interp.q, interp.dqDy, float(pixel.y & 1));
  const TreeGradients grad = TreeGradients((texCoords * dx) * material.uvScaleFlags.xy,
                                          (texCoords * dy) * material.uvScaleFlags.xy,
                                          position * dx, position * dy);
  if(resolveMode == 3u) {
    outDiffuse = vec4(fract(uv), length(grad.uvDx), length(grad.uvDy));
    outNormal = encodeNormal(treeSafeNormal(normal));
    outOrm = vec4(length(grad.posDx), length(grad.posDy), 0, 0);
    return;
    }

  treeWriteMaterial(record.z, material, uv, normal, (id.y & TreeSideBit) != 0u, grad);
#if defined(VERIFY)
  outEdgeMask = treeEdgeMask(interp, clipA, clipB, clipC, qSum);
#endif
  }
