#ifndef IMPOSTOR_COMMON_GLSL
#define IMPOSTOR_COMMON_GLSL

#include "scene.glsl"

layout(binding = 0, std140) uniform UboScene { SceneDesc scene; };

struct ImpostorInstance {
  mat4  objectToWorld;
  uvec4 info;
  };
layout(binding = 1, std430) readonly buffer Instances { ImpostorInstance instances[]; };

layout(push_constant, std430) uniform Push {
  vec4 centerRadius; // the object-space center and radius of the capture sphere
  } push;

// These axes are also used by the orthographic baker, with image rows running down.
mat3 impostorBasis(vec3 direction) {
  vec3 side = cross(vec3(0,1,0), direction);
  vec3 right = dot(side, side)>1e-12 ? normalize(side) : vec3(1,0,0);
  return mat3(right, cross(direction, right), direction);
  }

#endif
