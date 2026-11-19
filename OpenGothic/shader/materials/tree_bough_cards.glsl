#ifndef TREE_BOUGH_CARDS_GLSL
#define TREE_BOUGH_CARDS_GLSL

#if !defined(GL_VERTEX_SHADER)
layout(local_size_x = 32) in;
layout(triangles, max_vertices = 32, max_primitives = 16) out;
layout(location = 7) flat out uvec2 boughFadeOut[];
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

uvec2 boughFade(uvec4 task) {
  // The band owner (including its child-side bit) and the shared 24-bit parent strength.
  return uvec2(task.z >> 16u, task.w & 0xffffffu);
  }

#endif
