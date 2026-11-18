#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "impostor_common.glsl"

layout(location = 0) out vec3 centeredPosition;
layout(location = 1) flat out vec3 centeredEye;
layout(location = 2) flat out uint instanceId;

void main() {
  const vec2 corners[6] = vec2[6](
    vec2(-1,-1), vec2(1,-1), vec2(1,1),
    vec2(-1,-1), vec2(1,1), vec2(-1,1));
  instanceId = uint(gl_InstanceIndex);
  const mat4 transform = instances[instanceId].objectToWorld;
  const vec3 center = (transform * vec4(push.centerRadius.xyz, 1)).xyz;
  const vec3 eye = inverse(mat3(transform)) * (scene.camPos - center) / push.centerRadius.w;
  const mat3 basis = impostorBasis(normalize(eye));
  const vec2 corner = corners[gl_VertexIndex];
  centeredPosition = basis[0]*corner.x + basis[1]*corner.y;
  centeredEye = eye;
  const vec3 world = center + mat3(transform) * (centeredPosition * push.centerRadius.w);
  gl_Position = scene.viewProject * vec4(world, 1);
  }
