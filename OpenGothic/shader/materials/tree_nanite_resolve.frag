#version 460
#extension GL_GOOGLE_include_directive : require
#if defined(TREE_GAME)
#extension GL_EXT_nonuniform_qualifier : require
#endif

#define TREE_MATERIAL_FRAGMENT
#include "tree_nanite_geometry.glsl"
#include "tree_nanite_material.glsl"

#if defined(TREE_GAME)
layout(set=0, binding=15, std430) readonly buffer TreeHistoryState { uvec2 treeState[]; };
#if defined(TREE_GAME_ADAPTIVE)
layout(set=0, binding=24, std430) readonly buffer TreeAdaptiveHistory { uvec2 contourHistoryShape[]; };

bool treeSameContour(uint id, uvec4 record) {
  const uvec3 key = treeAdaptiveKey(id);
  const uint level = treeDrawLevel(record);
  const uint index = treeContourCutIndex(key.x,key.y,level);
  const uvec2 history = contourHistoryShape[index];
  const uint stamp = ((level-1u) << 30u) | (frame.contourControl.z-1u);
  return history.y == stamp && history.x == record.x-key.z;
  }
#endif
#define TREE_VIEWPORT treeViewport()
#else
#define TREE_VIEWPORT frame.viewport
// Cut the resolve at cumulative boundaries while keeping the same attachments.
// 0: complete, 1: visibility lookup, 2: attributes, 3: attributes and gradients.
layout(constant_id=0) const uint resolveMode = 0u;
#endif
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

bool treeUnclipped(vec4 v) {
  return v.w > 0.0 && v.z >= 0.0 && v.z <= v.w && all(lessThanEqual(abs(v.xy), vec2(v.w)));
  }

TreeInterpolation treeInterpolation(vec4 a, vec4 b, vec4 c, vec2 pixel) {
  // Perspective-correct weights solve sum(w_i*clip_i.xy) = p*sum(w_i*clip_i.w),
  // with sum(w_i)=1. The cross product solves both homogeneous equations
  // before the final normalization. It also works for vertices behind the eye.
  const vec3 w = vec3(a.w, b.w, c.w);
  const vec3 clipX = vec3(a.x, b.x, c.x);
  const vec3 clipY = vec3(a.y, b.y, c.y);
  if(frame.control.x != 0u && treeUnclipped(a) && treeUnclipped(b) && treeUnclipped(c)) {
    // On the pilot RTX the rasterizer interpolates on quantized framebuffer
    // vertices. Keep the displacement from the sample in fixed-point units
    // until after subtraction so thin triangles retain that displacement.
    const float grid = float(1u << frame.control.x);
    const vec2 halfViewportGrid = TREE_VIEWPORT.xy * (0.5*grid);
    const vec2 pixelGrid = (pixel - 0.5*TREE_VIEWPORT.xy)*grid;
    // The viewport center is an exact even grid coordinate for the pilot.
    // Rounding relative to it avoids losing fractional bits to that offset.
    const vec3 sx = roundEven((clipX/w)*halfViewportGrid.x);
    const vec3 sy = roundEven((clipY/w)*halfViewportGrid.y);
    const vec3 x = (sx - pixelGrid.x)*(w/grid);
    const vec3 y = (sy - pixelGrid.y)*(w/grid);
    return TreeInterpolation(cross(x, y), cross(-w, y), cross(x, -w));
    }

  // Clipping can add vertices. Use the original homogeneous triangle for
  // that case, including at the near plane and for w<=0.
  const vec2 p = (pixel - 0.5*TREE_VIEWPORT.xy) * (2.0*TREE_VIEWPORT.zw);
  const vec3 x = fma(vec3(-p.x), w, clipX);
  const vec3 y = fma(vec3(-p.y), w, clipY);
  return TreeInterpolation(cross(x, y), cross(-2.0*TREE_VIEWPORT.z*w, y), cross(x, -2.0*TREE_VIEWPORT.w*w));
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
  // The subpixel bit keeps the original triangle's projected area.
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

#if !defined(TREE_GAME)
  if(resolveMode == 1u) {
    outDiffuse = vec4(1);
    outNormal = 0u;
    outOrm = vec4(0);
    outMotion = vec2(0);
    return;
    }
#endif

#if defined(TREE_GAME)
  const uint recordId = id.x - 1u;
  const uvec4 record = treeRecord(recordId);
#else
  const uvec4 record = records[id.x-1u];
#endif
  const uvec4 meshlet = meshlets[record.x];
  const uvec3 indices = treeTriangle(meshlet.y + (id.y & TreePrimitiveMask));
#if defined(TREE_WIND)
  // The microtriangles are on the source plane. Use its derivatives even for
  // a thin contour ear, so its normal, color and motion match the card's chart.
  const uvec3 address = treeIsContour(record) ? treeContourFace(record) + uvec3(0, 1, 2) : meshlet.x + indices;
#if defined(TREE_GAME)
  const TreeVertex a = treeDeform(address.x, false, record.y);
  const TreeVertex b = treeDeform(address.y, false, record.y);
  const TreeVertex c = treeDeform(address.z, false, record.y);
#else
  const TreeVertex a = treeDeform(address.x, false);
  const TreeVertex b = treeDeform(address.y, false);
  const TreeVertex c = treeDeform(address.z, false);
#endif
#else
  const TreeVertex a = treeVertex(meshlet.x + indices.x);
  const TreeVertex b = treeVertex(meshlet.x + indices.y);
  const TreeVertex c = treeVertex(meshlet.x + indices.z);
#endif
#if defined(TREE_GAME)
  const TreeInstance instance = treeInstance(record.y);
  const TreeMaterial material = treeMaterial(record.z, treeDrawLevel(record));
#else
  const TreeInstance instance = instances[record.y];
#if defined(TREE_WIND)
  TreeMaterial material = materials[record.z];
  if(record.z == 1u && treeDrawLevel(record) != 0u)
    material.tint.rgb = vec3(1.0);
#else
  const TreeMaterial material = materials[record.z];
#endif
#endif
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
#if defined(TREE_GAME)
  const mat3 normalTransform = transpose(inverse(mat3(instance.current)));
  const mat3 normals = mat3(treeVertexNormal(vec4(treeSafeNormal(normalTransform*a.normal.xyz), a.normal.w), flags),
                            treeVertexNormal(vec4(treeSafeNormal(normalTransform*b.normal.xyz), b.normal.w), flags),
                            treeVertexNormal(vec4(treeSafeNormal(normalTransform*c.normal.xyz), c.normal.w), flags));
#else
  const mat3 normals = mat3(treeVertexNormal(a.normal, flags), treeVertexNormal(b.normal, flags),
                            treeVertexNormal(c.normal, flags));
#endif
  const vec3 normal = normals * weights;
#if defined(TREE_GAME)
  outMotion = vec2(0.0);
  if(scene.motionVectors != 0u && frame.previousEye.w > 0.0 && instanceKeys[record.y].w != 0u) {
    const mat3 prevStatic = mat3((scene.reprojectClip*clipA).xyw, (scene.reprojectClip*clipB).xyw,
                                 (scene.reprojectClip*clipC).xyw);
    const mat3 prevDelta = mat3(treePreviousDeformedDelta(address.x, a, instance, record.y),
                                treePreviousDeformedDelta(address.y, b, instance, record.y),
                                treePreviousDeformedDelta(address.z, c, instance, record.y));
    outMotion = treeMotion(prevStatic*weights, prevDelta*weights);
    }
  const uint denseCount = frame.control.y*frame.control.w;
  outReactive = 0.0;
  if(frame.previousEye.w > 0.0 && frame.materialBuckets.z != 0u) {
    bool sameCut = false;
    if(recordId < denseCount)
      sameCut = (treeState[recordId].x & 2u) != 0u;
#if defined(TREE_GAME_ADAPTIVE)
    else if(frame.contourControl.w != 0u)
      sameCut = treeSameContour(recordId,record);
#endif
    outReactive = sameCut ? 0.0 : 1.0;
    }
#else
  const mat3 prevStatic = mat3(treePreviousStatic(position[0]), treePreviousStatic(position[1]), treePreviousStatic(position[2]));
#if defined(TREE_WIND)
  const mat3 prevDelta = mat3(treePreviousDeformedDelta(address.x, position[0], instance),
                             treePreviousDeformedDelta(address.y, position[1], instance),
                             treePreviousDeformedDelta(address.z, position[2], instance));
#else
  const mat3 prevDelta = mat3(treePreviousDelta(a, instance), treePreviousDelta(b, instance), treePreviousDelta(c, instance));
#endif
  outMotion = treeMotion(prevStatic * weights, prevDelta * weights);

  if(resolveMode == 2u) {
    outDiffuse = vec4(fract(uv), 0, 0);
    outNormal = encodeNormal(treeSafeNormal(normal));
    outOrm = vec4(0);
    return;
    }
#endif

  const vec3 dx = treeQuadDerivative(interp.q, interp.dqDx, float(pixel.x & 1));
  const vec3 dy = treeQuadDerivative(interp.q, interp.dqDy, float(pixel.y & 1));
  const TreeGradients grad = TreeGradients((texCoords * dx) * material.uvScaleFlags.xy,
                                          (texCoords * dy) * material.uvScaleFlags.xy,
                                          position * dx, position * dy);
#if !defined(TREE_GAME)
  if(resolveMode == 3u) {
    outDiffuse = vec4(fract(uv), length(grad.uvDx), length(grad.uvDy));
    outNormal = encodeNormal(treeSafeNormal(normal));
    outOrm = vec4(length(grad.posDx), length(grad.posDy), 0, 0);
    return;
    }
#endif

  treeWriteMaterial(record.z, material, uv, normal, (id.y & TreeSideBit) != 0u, grad);
#if defined(VERIFY)
  outEdgeMask = treeEdgeMask(interp, clipA, clipB, clipC, qSum);
#endif
  }
