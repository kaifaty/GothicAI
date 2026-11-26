#ifdef TREE_COVERAGE
layout(location = 11) flat out uint crownOwner;
#endif
layout(location = 7) flat out uvec3 boughFadeOut;
layout(location = 8) flat out vec3 boughMaskOut;

void vertexShader(uvec4 task, uint materialRef) {
  const uint lane = uint(gl_VertexIndex);
  if(lane>=boughIndexCount(task)) {
    gl_Position = MeshletPaddingPosition;
    return;
    }

  const uint bucketId = boughBucket(task);
  const uint index    = boughIndex(bucketId, task.y + 1u + lane);
  Varyings var;
  gl_Position  = processVertex(var, pullVertex(bucketId, index), bucketId, task.x, index);
#ifdef TREE_COVERAGE
  crownOwner = task.x;
#endif
  bucketIdOut  = materialRef;
  shOut        = var;
  boughFadeOut = boughFade(task, boughMaskOut);
  }
