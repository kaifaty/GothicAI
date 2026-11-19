#ifndef TREE_BOUGH_CARDS_GLSL
#define TREE_BOUGH_CARDS_GLSL

#include "tree_bough_dissolve.glsl"

#if !defined(GL_VERTEX_SHADER)
layout(local_size_x = 32) in;
layout(triangles, max_vertices = 32, max_primitives = 16) out;
layout(location = 7) flat out uvec2 boughFadeOut[];
layout(location = 8) flat out vec3 boughMaskOut[];
#endif

// Each range starts with its first vertex and vertex count, followed by scalar indices.
uint boughIndex(uint bucketId, uint at) {
#if defined(BINDLESS)
  return ibo[nonuniformEXT(bucketId)].indexes[at];
#else
  return ibo[0].indexes[at];
#endif
  }

uint boughBucket(uvec4 task) {
  return task.z & 0xffffu;
  }

uint boughIndexCount(uvec4 task) {
  return task.w >> 24u;
  }

uint boughFirstVertex(uint firstIndex, uint bucketId) {
  return boughIndex(bucketId, firstIndex) >> 16u;
  }

uvec2 boughMeshlet(uvec4 task, uint bucketId) {
  const uint vertices = boughIndex(bucketId, task.y) & 0xffffu;
  const uint prims    = boughIndexCount(task)/3u;
  return uvec2(vertices, prims);
  }

uvec3 processPrimitive(uint firstIndex, uint bucketId, uint primitive) {
  const uint at   = firstIndex + 1u + primitive*3u;
  const uint base = boughFirstVertex(firstIndex, bucketId);
  return uvec3(boughIndex(bucketId, at), boughIndex(bucketId, at+1u), boughIndex(bucketId, at+2u))-base;
  }

uvec2 boughFade(uvec4 task, out vec3 mask) {
  const uint band = task.z >> 16u;
  mask = vec3(0);
  if(band==0u)
    return uvec2(0);
  const uint seed = pcgHash(task.x) ^ (band & ~BoughChildSide);
  vec4 pivot = scene.view * vec4(pullInstance(task.x).mat[3], 1.0);
  pivot.z = max(pivot.z, scene.clipInfo.x / scene.clipInfo.z);
  const vec4 clip = scene.project * pivot;
  mask.xy = (clip.xy / clip.w * 0.5 + 0.5) * vec2(scene.screenRes);
  // Map offset pixels to centimetres in the plane at the pivot's depth.
  mask.z = 2.0 * clip.w * scene.screenResInv.y / abs(scene.project[1][1]);
  const uint fade = (task.w & BoughWeightMask) | BoughFadeActive |
                    ((band & BoughChildSide)!=0u ? BoughFadeChild : 0u);
  return uvec2(fade, seed);
  }

#endif
