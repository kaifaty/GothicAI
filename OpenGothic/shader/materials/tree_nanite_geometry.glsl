#ifndef TREE_NANITE_GEOMETRY_GLSL
#define TREE_NANITE_GEOMETRY_GLSL

#if !defined(TREE_FRAME_ONLY)
// The isolated Vulkan pilot shares this layout with its CPU upload.
struct TreeVertex {
  vec4 position;
  vec4 normal; // w: crown openness, from the asset's vertex alpha
  vec4 uv;
  };

struct TreeInstance {
  vec4 current;  // xyz: translation in cm, w: uniform scale
  vec4 previous;
  };

layout(set=0, binding=0, std430) readonly buffer TreeVertexBuffer   { TreeVertex vertices[]; };
layout(set=0, binding=1, std430) readonly buffer TreeMeshletBuffer  { uvec4 meshlets[]; };
layout(set=0, binding=2, std430) readonly buffer TreeGeometryBuffer { uint geometry[]; };
layout(set=0, binding=3, std430) readonly buffer TreeDrawBuffer     { uvec4 records[]; };
layout(set=0, binding=4, std430) readonly buffer TreeInstanceBuffer { TreeInstance instances[]; };
#endif

layout(set=0, binding=7, std140) uniform TreeFrame {
  mat4  viewProject;
  mat4  prevViewProject;
  vec4  viewport; // width, height, inverse width, inverse height
  uvec4 control; // x: subpixel precision bits of the pilot's Vulkan rasterizer
#if defined(TREE_WIND)
  mat4 shadowProject;
  vec4 eye, right, up, forward, sun;
  vec4 wind, previousWind; // seconds, pressure, horizontal direction
  vec4 windFade, previousWindFade; // trunk, whorl, shoot, asset height in cm
#endif
  } frame;

#if defined(TREE_WIND)
struct TreePose { vec4 x, y, z; };
#if !defined(TREE_FRAME_ONLY)
layout(set=0, binding=11, std430) readonly buffer TreeSkinBuffer { uvec2 skin[]; };
layout(set=0, binding=12, std430) readonly buffer TreePoseBuffer { TreePose poses[]; };
#endif
#endif

#if !defined(TREE_FRAME_ONLY)
const uint TreePrimitiveMask = (1u << 13) - 1u;
const uint TreeSideBit       =  1u << 13;

uvec3 treeTriangle(uint address) {
  const uint packed = geometry[address];
  return (uvec3(packed) >> uvec3(0, 8, 16)) & 255u;
  }

TreeVertex treeVertex(uint address) {
  return vertices[geometry[address]];
  }

#if defined(TREE_WIND)
vec3 treeTransform(TreePose p, vec4 v) {
  return vec3(dot(p.x, v), dot(p.y, v), dot(p.z, v));
  }

TreeVertex treeDeform(uint address, bool previous) {
  TreeVertex v = treeVertex(address);
  const uvec2 binding = skin[geometry[address]];
  const uint offset = previous ? frame.control.z : 0u;
  const TreePose a = poses[offset + (binding.x & 65535u)];
  const TreePose b = poses[offset + (binding.x >> 16)];
  const float weight = float(binding.y & 255u) / 255.0;
  v.position.xyz = mix(treeTransform(b, v.position), treeTransform(a, v.position), weight);
  const vec4 n = vec4(v.normal.xyz, 0.0);
  v.normal.xyz = mix(treeTransform(b, n), treeTransform(a, n), weight);
  return v;
  }

vec3 treePreviousDeformedDelta(uint address, vec3 world, TreeInstance instance) {
  const vec3 previous = treeDeform(address, true).position.xyz * instance.previous.w + instance.previous.xyz;
  return (frame.prevViewProject * vec4(previous - world, 0.0)).xyw;
  }
#endif

vec3 treeWorldPosition(TreeVertex v, TreeInstance instance) {
  precise vec3 pos = v.position.xyz * instance.current.w + instance.current.xyz;
  return pos;
  }

vec4 treeClipPosition(vec3 world) {
  // The two independent mesh modules must produce the same clip coordinates.
  precise vec4 clip = frame.viewProject * vec4(world, 1.0);
  return clip;
  }

vec3 treePreviousStatic(vec3 world) {
  return (frame.prevViewProject * vec4(world, 1.0)).xyw;
  }

vec3 treePreviousDelta(TreeVertex v, TreeInstance instance) {
  const vec4 delta = instance.previous - instance.current;
  return (frame.prevViewProject * vec4(v.position.xyz * delta.w + delta.xyz, 0.0)).xyw;
  }

uvec2 treeVisibility(uint record, uint primitive, bool front) {
  // Bit 14 is zero for the triangle type.
  return uvec2(record + 1u, primitive | (front ? TreeSideBit : 0u));
  }
#endif

#endif
