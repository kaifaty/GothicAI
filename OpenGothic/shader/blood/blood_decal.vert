#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "blood/blood_decal.glsl"

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };
layout(binding = 4, std430) readonly buffer Decals { BloodDecal decal[]; };

layout(location = 0) out flat uint outId;

out gl_PerVertex {
  vec4 gl_Position;
  };

// 12 triangles of a unit cube [-1,1]^3, wound so that the inner (back) faces of the box face the camera.
const int cubeIndex[36] = int[](
  0,2,1, 1,2,3,  4,5,6, 5,7,6,
  0,1,4, 1,5,4,  2,6,3, 3,6,7,
  0,4,2, 2,4,6,  1,3,5, 3,7,5);

void main() {
  const int  v = cubeIndex[gl_VertexIndex];
  const vec3 c = vec3((v&1)!=0 ? 1.0 : -1.0, (v&2)!=0 ? 1.0 : -1.0, (v&4)!=0 ? 1.0 : -1.0);
  const BloodDecal d = decal[gl_InstanceIndex];
  const vec3 pos = d.world.xyz + d.axisU.xyz*c.x + d.axisV.xyz*c.y + d.axisW.xyz*c.z;
  outId       = uint(gl_InstanceIndex);
  gl_Position = scene.viewProject*vec4(pos,1.0);
  }
