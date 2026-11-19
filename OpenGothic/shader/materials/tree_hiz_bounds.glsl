#ifndef TREE_HIZ_BOUNDS_GLSL
#define TREE_HIZ_BOUNDS_GLSL

#include "foliage_tree.glsl"

struct TreeHiZBounds {
  vec3 min;
  uint firstMeshlet;
  vec3 max;
  uint meshletCount;
  };

layout(binding = 10, std430) readonly buffer TreeBounds { TreeHiZBounds treeBounds[]; };

bool treeHiZBox(Cluster cluster, Bucket bx, mat4x3 mat, vec3 camPos, float pxPerTan, out vec3 lo, out vec3 hi) {
  for(uint i=bx.treeHiZFirst; i<bx.treeHiZFirst+bx.treeHiZCount; ++i) {
    const TreeHiZBounds box = treeBounds[i];
    if(box.firstMeshlet!=cluster.firstMeshlet || box.meshletCount!=uint(cluster.meshletCount))
      continue;

    float grow = 0.0;
    if((bx.flags & BK_FOLIAGE)!=0u && foliageMoves(bx.foliage)) {
      // Match FoliageTree::travel with the same three fades as the vertex shader, in mesh units.
      const vec3 fade = foliageTreeFade(bx.foliage, bx.foliageReach, mat, camPos, pxPerTan);
      const float trunk = 2.0*(FoliageTreeLean + (FoliageTreeSwayCalm+FoliageTreeSwayWind)*(1.0+FoliageTreeAcross));
      const float branch = FoliageTreeBranchReach*FoliageTreeBranchGive*FoliageTreeBranchTopK*(1.0+FoliageTreeBranchGust);
      const float leaf = FoliageTreeLeafMax*(FoliageTreeLeafUp+FoliageTreeLeafSide);
      grow = bx.foliageReach*foliagePackedWind(bx.foliage)*(trunk*fade.x + branch*fade.y) +
             foliagePackedFlutter(bx.foliage)*leaf*fade.z;
      }
    // Both boxes enclose the moved vertices. Their intersection preserves containment and nesting at full wind.
    lo = max(box.min-vec3(grow), bx.bbox[0].xyz);
    hi = min(box.max+vec3(grow), bx.bbox[1].xyz);
    return true;
    }
  return false;
  }

#endif
