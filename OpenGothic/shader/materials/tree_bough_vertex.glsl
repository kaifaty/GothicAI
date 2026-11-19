layout(location = 7) flat out uvec2 boughFadeOut;

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
  bucketIdOut  = materialRef;
  shOut        = var;
  boughFadeOut = boughFade(task);
  }
