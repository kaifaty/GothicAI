#ifndef TREE_NANITE_GEOMETRY_GLSL
#define TREE_NANITE_GEOMETRY_GLSL

#if defined(TREE_GAME)
#include "scene.glsl"

layout(set=0, binding=7, std140) uniform TreeFrame {
  uvec4 control; // subpixel bits, source meshlets, bones, instance slots
  vec4 params;  // near plane, far plane, cut error in pixels, tree height in cm
  vec4 previousEye; // previous camera and pixels per tangent (0 on a reset)
  vec4 lower, upper; // wind-expanded whole-tree bounds in model centimetres
  uvec4 materialBuckets; // bindless bucket IDs of bark and needles
  vec4 levelPx;
  vec4 restBounds; // stock tree's bound center and height used for screen LOD
  uvec4 contourOffsets; // first source face of the four authored contour levels
  uvec4 contourControl; // total faces, largest level's face count, frame serial, history valid
  } frame;
layout(set=0, binding=16, std140) uniform TreeScene { SceneDesc scene; };

struct TreeInstance { mat4x3 current, previous; };
struct TreeStoredInstance { mat3x4 packedTransform; uvec4 attributes; };

layout(set=0, binding=4, std430) readonly buffer TreeInstanceKeys { uvec4 instanceKeys[]; };
layout(set=0, binding=18, std430) readonly buffer TreeCurrentInstances { TreeStoredInstance currentInstances[]; };
layout(set=0, binding=19, std430) readonly buffer TreePreviousInstances { TreeStoredInstance previousInstances[]; };

const uint TreeImpostorOnly = 0x20000000u;

uint treeImpostorJoint(uint slot) {
  return currentInstances[instanceKeys[slot].x].attributes.y;
  }

mat4x3 treeUnpackTransform(mat3x4 columns) {
  // InstanceStorage packs four float3 columns as three aligned float4 loads.
  return mat4x3(columns[0].xyz, vec3(columns[0].w, columns[1].xy),
                vec3(columns[1].zw, columns[2].x), columns[2].yzw);
  }

mat4x3 treeCurrentTransformAt(uint slot) {
  return treeUnpackTransform(currentInstances[instanceKeys[slot].x].packedTransform);
  }

mat4x3 treePreviousTransformAt(uint slot) {
  return treeUnpackTransform(previousInstances[instanceKeys[slot].x].packedTransform);
  }

mat4x3 treeTransformAt(uint slot, bool previous) {
  return previous ? treePreviousTransformAt(slot) : treeCurrentTransformAt(slot);
  }

TreeInstance treeInstance(uint slot) {
  const mat4x3 current = treeCurrentTransformAt(slot);
  return TreeInstance(current, frame.previousEye.w > 0.0 && instanceKeys[slot].w != 0u ?
                               treePreviousTransformAt(slot) : current);
  }

vec3 treeWorldPoint(vec3 p, mat4x3 transform) {
  precise vec3 world = transform * vec4(p, 1.0);
  return world;
  }

vec4 treeClipPosition(vec3 world) {
  precise vec4 clip = scene.viewProject * vec4(world, 1.0);
  return clip;
  }

vec4 treeViewport() {
  return vec4(vec2(scene.screenRes), scene.screenResInv);
  }
#endif

#if !defined(TREE_FRAME_ONLY)
// The isolated Vulkan pilot shares this layout with its CPU upload.
struct TreeVertex {
  vec4 position;
  vec4 normal; // w: crown openness, from the asset's vertex alpha
  vec4 uv;
  };

#if !defined(TREE_GAME)
struct TreeInstance {
  vec4 current;  // xyz: translation in cm, w: uniform scale
  vec4 previous;
  };
#endif

layout(set=0, binding=0, std430) readonly buffer TreeVertexBuffer   { TreeVertex vertices[]; };
layout(set=0, binding=1, std430) readonly buffer TreeMeshletBuffer  { uvec4 meshlets[]; };
layout(set=0, binding=2, std430) readonly buffer TreeGeometryBuffer { uint geometry[]; };
#if defined(TREE_GAME)
struct TreeSource {
  vec4 lower, upper;
  uvec4 lod;
  };
layout(set=0, binding=3, std430) readonly buffer TreeSourceBuffer { TreeSource treeSources[]; };
#if defined(TREE_GAME_ADAPTIVE)
layout(set=0, binding=20, std430)
#if !defined(TREE_CONTOUR_PREPARE)
readonly
#endif
buffer TreeContourCuts { uvec2 contourCuts[]; };
layout(set=0, binding=22, std430) readonly buffer TreeContourFaces { TreeSource contourFaces[]; };

uint treeDenseCount() {
  return frame.control.y*frame.control.w;
  }

uint treeContourCutIndex(uint slot, uint face, uint level) {
  return slot*frame.contourControl.y + face - frame.contourOffsets[level-1u];
  }

uint treeAdaptiveId(uint slot, uint face, uint part) {
  return treeDenseCount() + ((slot*frame.contourControl.x + face) << 6u) + part;
  }

uvec3 treeAdaptiveKey(uint id) {
  const uint contour = id-treeDenseCount();
  const uint face = (contour >> 6u)%frame.contourControl.x;
  const uint slot = (contour >> 6u)/frame.contourControl.x;
  return uvec3(slot, face, contour & 63u);
  }
#endif

uvec4 treeRecord(uint id) {
#if defined(TREE_GAME_ADAPTIVE)
  if(id >= treeDenseCount()) {
    const uvec3 key = treeAdaptiveKey(id);
    const uvec4 face = contourFaces[key.y].lod;
    const uint index = treeContourCutIndex(key.x,key.y,face.z);
    const uint flags = 0x80000000u | (face.x << 3u) | face.z;
    return uvec4(contourCuts[index].x + key.z, key.x, 1u, flags);
    }
#endif
  const uint source = id % frame.control.y;
  const uvec4 lod = treeSources[source].lod;
  return uvec4(source, id/frame.control.y, lod.zw);
  }
#else
layout(set=0, binding=3, std430) readonly buffer TreeDrawBuffer     { uvec4 records[]; };
layout(set=0, binding=4, std430) readonly buffer TreeInstanceBuffer { TreeInstance instances[]; };
#endif
#endif

#if !defined(TREE_GAME)
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
  vec4 impostorSphere; // bind center and radius in cm, zero radius outside the band
  vec4 lodCoverage[2]; // cumulative coarser coverage for thresholds L1–L4 and impostor
#endif
  } frame;
#endif

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

TreeVertex treeDeform(uint address, bool previous
#if defined(TREE_GAME)
                      , uint slot
#endif
                      ) {
  TreeVertex v = treeVertex(address);
  const uvec2 binding = skin[geometry[address]];
#if defined(TREE_GAME)
  const uint offset = (slot + (previous ? frame.control.w : 0u))*frame.control.z;
#else
  const uint offset = previous ? frame.control.z : 0u;
#endif
  const TreePose a = poses[offset + (binding.x & 65535u)];
  const TreePose b = poses[offset + (binding.x >> 16)];
  const float weight = float(binding.y & 255u) / 255.0;
  v.position.xyz = mix(treeTransform(b, v.position), treeTransform(a, v.position), weight);
  const vec4 n = vec4(v.normal.xyz, 0.0);
  v.normal.xyz = mix(treeTransform(b, n), treeTransform(a, n), weight);
  return v;
  }

bool treeIsContour(uvec4 record) {
  return (record.w & 0x80000000u) != 0u;
  }

uint treeDrawLevel(uvec4 record) {
  return record.w & 7u;
  }

uint treeContourFace(uvec4 record) {
  return (record.w & 0x7ffffff8u) >> 3u;
  }

TreeVertex treeRasterVertex(uvec4 record, uint address) {
#if defined(TREE_GAME)
  if(!treeIsContour(record))
    return treeDeform(address, false, record.y);
  const uint face = treeContourFace(record);
  const TreeVertex a = treeDeform(face + 0u, false, record.y);
  const TreeVertex b = treeDeform(face + 1u, false, record.y);
  const TreeVertex c = treeDeform(face + 2u, false, record.y);
#else
  if(!treeIsContour(record))
    return treeDeform(address, false);
  const uint face = treeContourFace(record);
  const TreeVertex a = treeDeform(face + 0u, false);
  const TreeVertex b = treeDeform(face + 1u, false);
  const TreeVertex c = treeDeform(face + 2u, false);
#endif
  const vec2 bary = unpackUnorm2x16(geometry[address]);
  const float first = 1.0 - bary.x - bary.y;
  return TreeVertex(a.position*first + b.position*bary.x + c.position*bary.y,
                    a.normal*first + b.normal*bary.x + c.normal*bary.y,
                    a.uv*first + b.uv*bary.x + c.uv*bary.y);
  }

#if defined(TREE_GAME)
vec3 treePreviousDeformedDelta(uint address, TreeVertex vertex, TreeInstance instance, uint slot) {
  const vec3 previous = treeDeform(address, true, slot).position.xyz;
  // Keep both translations out of the subtraction of the local positions.
  const vec3 delta = instance.previous * vec4(previous, 0.0) - instance.current * vec4(vertex.position.xyz, 0.0)
                     + (instance.previous[3] - instance.current[3]);
  return (scene.viewProjectPrev * vec4(delta, 0.0)).xyw;
  }
#else
vec3 treePreviousDeformedDelta(uint address, vec3 world, TreeInstance instance) {
  const vec3 previous = treeDeform(address, true).position.xyz * instance.previous.w + instance.previous.xyz;
  return (frame.prevViewProject * vec4(previous - world, 0.0)).xyw;
  }
#endif
#endif

vec3 treeWorldPosition(TreeVertex v, TreeInstance instance) {
#if defined(TREE_GAME)
  return treeWorldPoint(v.position.xyz, instance.current);
#else
  precise vec3 pos = v.position.xyz * instance.current.w + instance.current.xyz;
  return pos;
#endif
  }

#if !defined(TREE_GAME)
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
#endif

uvec2 treeVisibility(uint record, uint primitive, bool front) {
  // Bit 14 is zero for the triangle type.
  return uvec2(record + 1u, primitive | (front ? TreeSideBit : 0u));
  }
#endif

#endif
