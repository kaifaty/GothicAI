#ifndef CLUSTER_LOD_GLSL
#define CLUSTER_LOD_GLSL

struct ClusterLevel {
  vec4 bounds;
  vec4 parentBounds;
  float errorCm;
  float parentErrorCm;
  uint level;
  uint flags;
  };

layout(binding = L_ClusterLevelIndex, std430) readonly buffer ClusterLevelIndices { uint clusterLevelIndex[]; };
layout(binding = L_ClusterLevel, std430) readonly buffer ClusterLevels { ClusterLevel clusterLevels[]; };

// Match ClusterLod::projectError, including the order of float operations.
float clusterProjectError(vec4 sphere, float errorCm) {
  precise vec3 offset = sphere.xyz - push.lodCamera.xyz;
  precise float distanceSq = offset.x*offset.x + offset.y*offset.y + offset.z*offset.z;
  precise float nearDistance = max(sqrt(distanceSq) - sphere.w, 1.0);
  precise float scale = 0.5 * push.lodProjection * push.lodHeight;
  precise float errorPx = errorCm / nearDistance * scale;
  return errorPx;
  }

// For an instanced mesh the transition records are in object space. The maximum absolute row sum of the
// linear transform's Gram matrix bounds the squared scale even when the instance stretches or shears.
float clusterLodScale(mat4x3 mat) {
  vec3 lengthsSq = vec3(dot(mat[0], mat[0]), dot(mat[1], mat[1]), dot(mat[2], mat[2]));
  vec3 products = abs(vec3(dot(mat[0], mat[1]), dot(mat[1], mat[2]), dot(mat[2], mat[0])));
  vec3 rowSums = lengthsSq + products + products.zxy;
  return sqrt(max(max(rowSums.x, rowSums.y), rowSums.z));
  }

bool clusterLodVisible(uint clusterId, uint instanceId, out uint level) {
  level = 0u;
  if(push.lodHeight <= 0.0 || clusterId >= clusterLevelIndex.length())
    return true;

  const uint index = clusterLevelIndex[clusterId];
  if(index == 0u)
    return true;
  ClusterLevel record = clusterLevels[index-1u];
  if((record.flags & 1u) == 0u)
    return true;

  level = record.level + 1u; // zero is reserved for a cluster without hierarchy
  const bool leaf = record.level == 0u;
  // The recorded sentinel is positive infinity. Test its bits on backends that compile with fast math.
  const uint infinity = 0x7f800000u;
  const bool terminal = floatBitsToUint(record.parentErrorCm) == infinity;
  if(push.lodPx <= 0.0)
    return leaf;
  if(floatBitsToUint(push.lodPx) == infinity)
    return terminal;

  if(instanceId != 0xffffffffu) {
    const mat4x3 mat = pullInstance(instanceId).mat;
    const float scale = clusterLodScale(mat);
    record.bounds = vec4(mat * vec4(record.bounds.xyz, 1.0), record.bounds.w * scale);
    record.parentBounds = vec4(mat * vec4(record.parentBounds.xyz, 1.0), record.parentBounds.w * scale);
    record.errorCm *= scale;
    record.parentErrorCm *= scale;
    }

  const float ownError = leaf ? 0.0 : clusterProjectError(record.bounds, record.errorCm);
  return ownError <= push.lodPx &&
         (terminal || clusterProjectError(record.parentBounds, record.parentErrorCm) > push.lodPx);
  }

#endif
