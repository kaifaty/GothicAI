// Mesh stage of the compact foliage cards (main.vert). A workgroup draws push.meshletCount cut nodes of one command:
// a node alone left most of its group idle, and the frame paid for every group. The mesh path keeps the vertex
// count of a node above its first index (TreeBoughRender::uploadData): the layout of a group comes from the payload alone.
const uint BoughFirstIndexBits = 26u;

uint boughFirstIndex(uvec4 task) {
  return task.y & ((1u << BoughFirstIndexBits)-1u);
  }

uint boughVertexCount(uvec4 task) {
  return task.y >> BoughFirstIndexBits;
  }

void main() {
  const uint laneID = gl_LocalInvocationIndex;
  const uint size   = uint(push.meshletCount);
  const uint first  = gl_WorkGroupID.x*size + push.firstMeshlet;

  // Vertices and primitives of the nodes follow each other. An empty node pads the last group.
  uint  vertCount = 0u, primCount = 0u;
  uint  vertLane  = 0u, primLane  = 0u, primBase = 0u;
  uvec4 vertTask  = uvec4(0), primTask = uvec4(0);
  bool  hasVert   = false, hasPrim = false;
  for(uint i=0u; i<size; ++i) {
    const uvec4 task  = payload[first+i];
    const uint  verts = boughVertexCount(task);
    const uint  prims = boughIndexCount(task)/3u;
    if(vertCount<=laneID && laneID<vertCount+verts) {
      vertTask = task;
      vertLane = laneID-vertCount;
      hasVert  = true;
      }
    if(primCount<=laneID && laneID<primCount+prims) {
      primTask = task;
      primLane = laneID-primCount;
      primBase = vertCount;
      hasPrim  = true;
      }
    vertCount += verts;
    primCount += prims;
    }
  SetMeshOutputsEXT(vertCount, primCount);

  if(hasPrim) {
    const uvec3 prim = processPrimitive(boughFirstIndex(primTask), boughBucket(primTask), primLane);
    gl_PrimitiveTriangleIndicesEXT[laneID] = prim + primBase;
    }
  if(hasVert) {
    const uint bucketId = boughBucket(vertTask);
    Varyings   var;
    gl_MeshVerticesEXT[laneID].gl_Position = processVertex(var, vertTask.x, boughFirstIndex(vertTask), bucketId, vertLane);
#if defined(MATERIAL_MAPS)
    bucketIdOut[laneID] = bucketId | canopyNormalTag(vertTask.x);
#else
    bucketIdOut[laneID] = bucketId;
#endif
    boughFadeOut[laneID] = boughFade(vertTask, boughMaskOut[laneID]);
#include "mesh_varyings.glsl"
    }
  }
