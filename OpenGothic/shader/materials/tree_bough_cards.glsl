#ifndef TREE_BOUGH_CARDS_GLSL
#define TREE_BOUGH_CARDS_GLSL

#include "tree_bough_dissolve.glsl"

#if !defined(GL_VERTEX_SHADER)
layout(local_size_x = 64) in;
layout(triangles, max_vertices = MaxVert, max_primitives = MaxPrim) out;
layout(location = 7) flat out uvec3 boughFadeOut[];
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

uvec3 processPrimitive(uint firstIndex, uint bucketId, uint primitive) {
  const uint at   = firstIndex + 1u + primitive*3u;
  const uint base = boughFirstVertex(firstIndex, bucketId);
  return uvec3(boughIndex(bucketId, at), boughIndex(bucketId, at+1u), boughIndex(bucketId, at+2u))-base;
  }

uvec3 boughFade(uvec4 task, out vec3 mask) {
  const uint band = task.z >> 16u;
  const Instance instance = pullInstance(task.x);
#if defined(MATERIAL_MAPS)
  const uint joint = boughJointFade(instance.animPtr & ~CanopyVertexNormal);
#else
  const uint joint = boughJointFade(instance.animPtr);
#endif
  mask = vec3(0);
  if(band==0u && joint==0u)
    return uvec3(0);
  const uint seed = pcgHash(task.x) ^ (band & ~BoughChildSide);
  // During the joint all representations project the same raw pivot in the fragment stage.
  mask = joint!=0u ? instance.mat[3] : boughMaskCoordinates(instance.mat[3], scene);
  const uint side = (band & BoughChildSide)!=0u ? BoughFadeChild : 0u;
  const uint fade = band!=0u ? (task.w & BoughWeightMask) | BoughFadeActive | side : 0u;
  return uvec3(fade, seed, joint);
  }

#endif
