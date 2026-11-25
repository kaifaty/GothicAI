#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_control_flow_attributes : enable

#if defined(VIRTUAL_SHADOW)
#include "virtual_shadow/vsm_common.glsl"
#endif

#include "materials_common.glsl"
#include "vertex_process.glsl"

#if !defined(GL_VERTEX_SHADER)
#extension GL_EXT_mesh_shader : enable
#endif
#if defined(TREE_BOUGH_CARDS)
#include "tree_bough_cards.glsl"
#elif !defined(GL_VERTEX_SHADER)
layout(local_size_x = 64) in;
layout(triangles, max_vertices = MaxVert, max_primitives = MaxPrim) out;
#endif
#if defined(TREE_BOUGH_JOINT)
#include "tree_bough_dissolve.glsl"
#endif
#if defined(TREE_BOUGH_JOINT) && defined(DEPTH_ONLY) && defined(GL_VERTEX_SHADER)
layout(location = 9) out vec2 boughBandCellOut;
layout(location = 10) flat out uvec2 boughBandOut;
#elif defined(TREE_BOUGH_JOINT) && defined(DEPTH_ONLY)
layout(location = 9) out vec2 boughBandCellOut[];
layout(location = 10) flat out uvec2 boughBandOut[];
#endif

#if defined(VIRTUAL_SHADOW)
layout(push_constant, std430) uniform Push {
  uint      commandId;
  } push;
#else
layout(push_constant, std430) uniform Push {
  uint      firstMeshlet;
  int       meshletCount;
  } push;
#endif

#if defined(VIRTUAL_SHADOW)
// the four planes of the page's rect; objects have a fifth on a lamp's faces: the fixture plane (processVertexVsm)
#if defined(LVL_OBJECT)
#define VSM_CLIP_COUNT 5
#else
#define VSM_CLIP_COUNT 4
#endif
#endif

// These passes share the main depth buffer. Keep their position invariant
// across specializations with and without material and motion varyings.
#if defined(GBUFFER_MOTION) || defined(LANDSCAPE_GBUFFER) || defined(HIZ_OCCLUDER) || defined(HISTORY_PAYLOAD)
#define INVARIANT_MAIN_POSITION
#endif

#if defined(GL_VERTEX_SHADER)
out gl_PerVertex {
#if defined(INVARIANT_MAIN_POSITION)
  invariant vec4 gl_Position;
#else
  vec4  gl_Position;
#endif
#if defined(VIRTUAL_SHADOW)
  float gl_ClipDistance[VSM_CLIP_COUNT];
#endif
  };
#else
out gl_MeshPerVertexEXT {
#if defined(INVARIANT_MAIN_POSITION)
  invariant vec4 gl_Position;
#else
  vec4  gl_Position;
#endif
#if defined(VIRTUAL_SHADOW)
  float gl_ClipDistance[VSM_CLIP_COUNT];
#endif
  } gl_MeshVerticesEXT[];
#endif

#if defined(GL_VERTEX_SHADER) && defined(MAT_VARYINGS)
layout(location = 0) out flat uint bucketIdOut;
layout(location = 1) out Varyings  shOut;
#elif defined(MAT_VARYINGS)
layout(location = 0) out flat uint bucketIdOut[]; //TODO: per-primitive
layout(location = 1) out Varyings  shOut[];
#endif

#if defined(GL_VERTEX_SHADER) && defined(VIRTUAL_SHADOW)
layout(location = 3) out flat uint vsmMipIdOut;
#elif defined(VIRTUAL_SHADOW)
layout(location = 3) out flat uint vsmMipIdOut[];
#endif

#if defined(VIRTUAL_SHADOW)
uint shadowPageId = 0;
#endif

#if defined(VIRTUAL_SHADOW) && !defined(GL_VERTEX_SHADER)
// the meshlet's vertices in clip space (x, y, w): the facing of its triangles (meshShader)
shared vec3 vsmClip[MaxVert];
#if defined(ATEST) && (MESH_TYPE==T_OBJ)
#define SUN_CROWN_PAGE_CULL
shared uint vsmOutside[MaxVert];
#endif
#endif

#if defined(VIRTUAL_SHADOW)
vec4 mapViewportProj(const uint data, vec4 pos, out float clipDistance[4]) {
  const ivec2 page    = unpackVsmPageInfoProj(data);
  const ivec2 sz      = unpackVsmPageSize(data);

  pos.xy = (pos.xy*0.5+0.5*pos.w); // [0..1]
  pos.xy = (pos.xy*sz - page.xy*pos.w);

  {
    clipDistance[0] = 0         +pos.x;
    clipDistance[1] = sz.x*pos.w-pos.x;
    clipDistance[2] = 0         +pos.y;
    clipDistance[3] = sz.y*pos.w-pos.y;
  }

  const vec2 pageId = vec2(unpackVsmPageId(shadowPageId));
  pos.xy = (pos.xy + pageId*pos.w)/VSM_PAGE_PER_ROW;

  pos.xy = pos.xy*2.0-1.0*pos.w; // [-1..1]
  return pos;
  }

vec4 mapViewportOrtho(const uint data, vec4 pos, out float clipDistance[4]) {
  const ivec3 page = unpackVsmPageInfo(data);
  const ivec2 sz   = unpackVsmPageSize(data);
  pos.xy /= float(1u << page.z);

  pos.xy = (pos.xy*0.5+0.5); // [0..1]
  pos.xy = (pos.xy*VSM_PAGE_TBL_SIZE - page.xy);

  {
    clipDistance[0] = 0   +pos.x;
    clipDistance[1] = sz.x-pos.x;
    clipDistance[2] = 0   +pos.y;
    clipDistance[3] = sz.y-pos.y;
  }

  const vec2 pageId = vec2(unpackVsmPageId(shadowPageId));
  pos.xy = (pos.xy + pageId)/VSM_PAGE_PER_ROW;

  pos.xy = pos.xy*2.0-1.0; // [-1..1]
  return pos;
  }

vec4 mapViewport(vec4 pos, out float clipDistance[4]) {
  const uint  data = vsm.pageList[shadowPageId];
  if(vsmPageIsOmni(data))
    return mapViewportProj(data, pos, clipDistance);
  return mapViewportOrtho(data, pos, clipDistance);
  }

vec4 processVertexVsm(out Varyings var, const Vertex vert, const uint bucketId, uint instanceId, const uint vboOffset, const uint laneID) {
  vec3       wpos = processVertexCommon(var, vert, bucketId, instanceId, vboOffset);
  const uint data = vsm.pageList[shadowPageId];
#if (MESH_TYPE==T_OBJ)
  if(strandVx.halfWidth>0.0) {
    // a ribbon faces the light of its page
    const vec3 toLight = vsmPageIsOmni(data) ? normalize(lights[unpackLightId(data).x].pos - wpos) : scene.sunDir;
    if(!strandShadowRibbon(var, wpos, toLight))
      wpos = vec3(uintBitsToFloat(0x7fc00000));
    }
#endif

  vec4  pos4;
  float fixture = 1.0;
  if(vsmPageIsOmni(data)) {
    const uvec2       page = unpackLightId(data);
    const LightSource lx   = lights[page.x];
    const vec3        pos  = vsmMapDirToFace((wpos - lx.pos)/lx.range, page.y);

    pos4 = vsmApplyProjective(pos);
    // a lamp's own fixture (a brazier's basket, a lantern's frame, a candle holder) casts no shadow of its light:
    // an object's part nearer than scene.vsmLampNear to the light along the face's axis is clipped away here, so the
    // face keeps what stands behind it. Left out at the lookup instead, it took the wall behind it out of the map
    // too, and the light passed the planks of a hut (reports/render-features/2026-11-10-lamp-wall-leak). The world
    // mesh is never a fixture: it keeps the projection's near plane alone
    fixture = pos.z - scene.vsmLampNear/lx.range;
    } else {
    pos4 = scene.viewProject*vec4(wpos,1.0);
    }

  float clip[4];
  pos4 = mapViewport(pos4, clip);
#if defined(SUN_CROWN_PAGE_CULL)
  vsmOutside[laneID] = (clip[0]<0.0 ? 1u : 0u) | (clip[1]<0.0 ? 2u : 0u) |
                       (clip[2]<0.0 ? 4u : 0u) | (clip[3]<0.0 ? 8u : 0u);
#endif
#if defined(GL_VERTEX_SHADER)
  [[unroll]] for(int i=0; i<4; ++i)
    gl_ClipDistance[i] = clip[i];
#if VSM_CLIP_COUNT>4
  gl_ClipDistance[4] = fixture;
#endif
#else
  [[unroll]] for(int i=0; i<4; ++i)
    gl_MeshVerticesEXT[laneID].gl_ClipDistance[i] = clip[i];
#if VSM_CLIP_COUNT>4
  gl_MeshVerticesEXT[laneID].gl_ClipDistance[4] = fixture;
#endif
#endif
  return pos4;
  }
#endif

#if defined(VIRTUAL_SHADOW)
void initVsm(uvec4 task) {
  shadowPageId = task.w & 0xFFFF;
  }
#endif

#if defined(TREE_BOUGH_JOINT) && defined(DEPTH_ONLY)
// The shadow side of a level band (tree_bough_dissolve.glsl): the instance's word and seed, and the vertex's cell
// across the light of the view: the sun, or a lamp of the virtual shadow map's page
uvec2 boughBandShadow(uint instanceId, uint bucketId, vec3 pos, out vec2 cell) {
  const Instance obj  = pullInstance(instanceId);
  const uint     word = boughBandWord(obj);
  cell = vec2(0);
  if(!boughBandActive(word))
    return uvec2(0);
  vec3 toLight = scene.sunDir;
#if defined(VIRTUAL_SHADOW)
  const uint data = vsm.pageList[shadowPageId];
  if(vsmPageIsOmni(data))
    toLight = normalize(lights[unpackLightId(data).x].pos - obj.mat[3]);
#endif
  const float height = (bucket[bucketId].bbox[1].y - bucket[bucketId].bbox[0].y)*length(obj.mat[1]);
  cell = boughBandShadowCell(pos, obj.mat[3], toLight, height);
  return uvec2(word, boughJointSeed(obj.mat[3]));
  }
#endif

#if !defined(TREE_BOUGH_CARDS)
uvec2 processMeshlet(const uint meshletId, const uint bucketId) {
#if defined(BINDLESS)
  nonuniformEXT uint bId = bucketId;
#else
  const         uint bId = 0;
#endif

  const uint iboOffset = meshletId * MaxPrim + MaxPrim - 1;
  const uint bits      = ibo[bId].indexes[iboOffset];
  uvec4 prim;
  prim.x = ((bits >>  0) & 0xFF);
  prim.y = ((bits >>  8) & 0xFF);

  uint vertCount = MaxVert;
  uint primCount = MaxPrim;
  if(prim.x==prim.y) {
    // last dummy triangle encodes primitive count
    prim.z = ((bits >> 16) & 0xFF);
    prim.w = ((bits >> 24) & 0xFF);

    primCount = prim.z;
    vertCount = prim.w;
    }
  return uvec2(vertCount, primCount);
  }

uvec3 processPrimitive(const uint meshletId, const uint bucketId, const uint outId) {
#if defined(BINDLESS)
  nonuniformEXT uint bId = bucketId;
#else
  const         uint bId = 0;
#endif

  const uint iboOffset = meshletId * MaxPrim + outId;
  const uint bits      = ibo[bId].indexes[iboOffset];
  uvec3 prim;
  prim.x = ((bits >>  0) & 0xFF);
  prim.y = ((bits >>  8) & 0xFF);
  prim.z = ((bits >> 16) & 0xFF);
  return prim;
  }
#endif

vec4  processVertex(out Varyings var, uint instanceOffset, const uint meshletId, const uint bucketId, const uint laneID) {
#if defined(TREE_BOUGH_CARDS)
  const uint vboOffset = boughFirstVertex(meshletId, bucketId) + laneID;
#else
  const uint vboOffset = meshletId * MaxVert + laneID;
#endif
#if (MESH_TYPE==T_PFX)
  const Vertex vert = pullVertex(instanceOffset);
#else
  const Vertex vert = pullVertex(bucketId, vboOffset);
#endif
#if defined(VIRTUAL_SHADOW)
  return processVertexVsm(var, vert, bucketId, instanceOffset, vboOffset, laneID);
#else
  return processVertex(var, vert, bucketId, instanceOffset, vboOffset);
#endif
  }

#if defined(GL_VERTEX_SHADER) && defined(TREE_BOUGH_CARDS)
#include "tree_bough_vertex.glsl"
#elif defined(GL_VERTEX_SHADER)
void vertexShader(const uvec4 task, const uint materialRef) {
  const uint  instanceId = task.x;
  const uint  meshletId  = task.y;
  const uint  bucketId   = task.z;

  const uvec2 mesh       = processMeshlet(meshletId, bucketId);
  const uint  vertCount  = mesh.x;
  const uint  primCount  = mesh.y;

  const uint  laneID     = gl_VertexIndex/3;
  if(laneID>=primCount) {
    gl_Position = MeshletPaddingPosition;
    return;
    }

#if defined(MAT_VARYINGS)
  bucketIdOut = materialRef;
#endif
  Varyings var;
  uint idx = processPrimitive(meshletId, bucketId, laneID)[gl_VertexIndex%3];
  vec4 pos = processVertex(var, instanceId, meshletId, bucketId, idx);
  gl_Position = pos;
#if defined(MAT_VARYINGS)
  shOut       = var;
#endif
#if defined(TREE_BOUGH_JOINT) && defined(DEPTH_ONLY)
  boughBandOut = boughBandShadow(instanceId, bucketId, var.pos, boughBandCellOut);
#endif
  }
#elif !defined(TREE_BOUGH_CARDS)
void meshShader(const uvec4 task, const uint materialRef) {
  const uint  instanceId = task.x;
  const uint  meshletId  = task.y;

  const uint  bucketId   = task.z;
  const uvec2 mesh       = processMeshlet(meshletId, bucketId);
  const uint  vertCount  = mesh.x;
  const uint  primCount  = mesh.y;

  const uint  laneID     = gl_LocalInvocationIndex;

  // Alloc outputs
  SetMeshOutputsEXT(vertCount, primCount);

  Varyings var;
  uvec3 prim = uvec3(0);
  if(laneID<primCount) {
    prim = processPrimitive(meshletId, bucketId, laneID);
    gl_PrimitiveTriangleIndicesEXT[laneID] = prim;
    }
  if(laneID<vertCount) {
#if defined(MAT_VARYINGS)
    bucketIdOut[laneID] = materialRef;
#endif
    vec4 pos = processVertex(var, instanceId, meshletId, bucketId, laneID);
    gl_MeshVerticesEXT[laneID].gl_Position = pos;
#if defined(TREE_BOUGH_JOINT) && defined(DEPTH_ONLY)
    vec2 bandCell;
    boughBandOut[laneID] = boughBandShadow(instanceId, bucketId, var.pos, bandCell);
    boughBandCellOut[laneID] = bandCell;
#endif
#if defined(VIRTUAL_SHADOW)
    vsmClip[laneID] = pos.xyw;
#endif

#include "mesh_varyings.glsl"
    }

#if defined(VIRTUAL_SHADOW)
  // The pipeline draws both sides here (Shaders::materialPipeline); the culling it did is done per primitive. A sun
  // page keeps the side to the sun, as the rest of the frame does. A lamp's cube face is mirrored against the camera,
  // so the pipeline's culling kept only the side away from the light, and a one-sided surface lit by the lamp (a floor
  // without an underside, a wall of one layer) was missing from it: the light passed it. A lamp's face keeps both sides
  // of the world mesh (scene.vsmLampSides 2) and of objects too (3, the default): a carpet lit by a hearth, a fireplace's
  // hood, a net on a wall are one-sided as well (reports/render-features/2026-11-10-lamp-one-sided,
  // reports/render-features/2026-11-11-lamp-objects-two-sided). One page per workgroup: the branch is uniform.
  barrier();
  if(laneID<primCount) {
    const bool omni = vsmPageIsOmni(vsm.pageList[shadowPageId]);
    // the sign of the homogeneous determinant is the facing as rasterized, also for a triangle cut by w=0; Tempest's
    // front face is clockwise, CullMode::Front culled a positive one
    const float det = dot(vsmClip[prim.x], cross(vsmClip[prim.y], vsmClip[prim.z]));
#if defined(LVL_OBJECT)
    const bool twoSided = scene.vsmLampSides>=3u;
#else
    const bool twoSided = scene.vsmLampSides>=2u;
#endif
#if (MESH_TYPE==T_OBJ)
    // a material of two sides (materials.json "two_sided") is in every page with both, as its pipelines draw it
    const bool bothSides = (bucket[bucketId].flags & BK_TWO_SIDED)!=0u;
#else
    const bool bothSides = false;
#endif
    gl_MeshPrimitivesEXT[laneID].gl_CullPrimitiveEXT = det>0.0 && (!omni || !twoSided) && !bothSides;
#if defined(SUN_CROWN_PAGE_CULL)
    // All three vertices must lie strictly outside the same edge of the page.
    if(!omni && (bucket[bucketId].flags & BK_CROWN)!=0u &&
       (vsmOutside[prim.x] & vsmOutside[prim.y] & vsmOutside[prim.z])!=0u)
      gl_MeshPrimitivesEXT[laneID].gl_CullPrimitiveEXT = true;
#endif
    }
#endif
  }
#endif

#if defined(TREE_BOUGH_CARDS) && !defined(GL_VERTEX_SHADER)
#include "tree_bough_mesh.glsl"
#else
void main() {
#if defined(GL_VERTEX_SHADER)
  const uint workIndex = gl_InstanceIndex;
#else
  const uint workIndex = gl_WorkGroupID.x;
#endif

#if defined(VIRTUAL_SHADOW)
  const uint firstMeshlet = cmd[push.commandId].writeOffset;
#else
  const uint firstMeshlet = push.firstMeshlet;
#endif

  const uint payloadId = workIndex + firstMeshlet;
  const uvec4 task     = payload[payloadId];
#if defined(TREE_BOUGH_CARDS)
#if defined(MATERIAL_MAPS)
  uint materialRef = boughBucket(task);
#else
  const uint materialRef = boughBucket(task);
#endif
#elif defined(HISTORY_PAYLOAD)
  // Both history phases use this same vertex shader and carry the unchanged payload index.
#if defined(TREE_BOUGH_JOINT) || (MESH_TYPE==T_OBJ && defined(MATERIAL_MAPS))
  uint materialRef = payloadId;
#else
  const uint materialRef = payloadId;
#endif
#else
  uint materialRef     = task.z;
#if defined(MAT_CLUSTER_LEVEL)
  materialRef |= task.w << 16u;
#endif
#if defined(MAT_MOTION) || (MESH_TYPE==T_LANDSCAPE && defined(MAT_CLUSTER_LEVEL))
  // The probe uses the existing flat output to recover both the bucket and cluster.
  if((scene.viewFlags & VF_TREE_OCCLUSION_PROBE)!=0u)
    materialRef = payloadId;
#endif
#endif

#if defined(TREE_BOUGH_JOINT) && !defined(DEPTH_ONLY)
  {
    // the impostor joint or a level band: the fragment stage masks the instance's side
    const Instance obj = pullInstance(task.x);
    if((obj.animPtr & BoughJointTag)!=0u || boughBandActive(boughBandWord(obj)))
      materialRef = payloadId | BoughJointTag;
  }
#endif

#if MESH_TYPE==T_OBJ && defined(GBUFFER_MOTION) && defined(MATERIAL_MAPS)
  materialRef |= canopyNormalTag(task.x);
#endif

#if defined(VIRTUAL_SHADOW)
  initVsm(task);
#endif

#if defined(GL_VERTEX_SHADER)
  vertexShader(task, materialRef);
#else
  meshShader(task, materialRef);
#endif
  }
#endif
