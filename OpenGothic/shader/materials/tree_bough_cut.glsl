#ifndef TREE_BOUGH_CUT_GLSL
#define TREE_BOUGH_CUT_GLSL

#include "tree_bough_dissolve.glsl"

struct BoughNode {
  vec4 sphere;
  vec3 boxMin;
  float diameter;
  vec3 boxMax;
  float thresholdPx;
  float lowPx;
  float highPx;
  uint parent;
  uint subtreeEnd;
  };

struct BoughProjection {
  vec3 forwardLocal;
  float originDepth;
  float focalScale;
  float nearZ;
  };

BoughNode boughGetNode(uint i);
void boughEmit(uint i, uint transition, float weight);

float boughScreenSize(BoughNode node, BoughProjection projection) {
  vec3 nearest = mix(node.boxMin, node.boxMax, lessThan(projection.forwardLocal, vec3(0)));
  float depth = projection.originDepth + dot(projection.forwardLocal, nearest);
  return depth<=projection.nearZ ? uintBitsToFloat(0x7f800000u) : node.diameter * projection.focalScale / depth;
  }

// Preorder lets a selected proxy skip all its descendants. Stop at the first proxy for the fast-path query.
bool boughWalk(uint root, BoughProjection projection, bool emit) {
  const uint end = boughGetNode(root).subtreeEnd;
  uint band = 0u, bandEnd = 0u;
  float bandWeight = 0.0;
  for(uint i = root; i<end;) {
    BoughNode node = boughGetNode(i);
    if(i>=bandEnd) {
      band = 0u;
      bandWeight = 0.0;
      }
    if(node.subtreeEnd==i+1u) {
      if(emit)
        boughEmit(i, band, bandWeight);
      ++i;
      continue;
      }
    if(node.highPx<=0.0) {
      ++i;
      continue;
      }

    // Nested boxes and ordered thresholds guarantee that the replaced cut has no active descendant band.
    if(band!=0u) {
      boughEmit(i, band, bandWeight);
      i = node.subtreeEnd;
      continue;
      }

    const float weight = 1.0 - smoothstep(node.lowPx, node.highPx, boughScreenSize(node, projection));
    if(weight>0.0 && !emit)
      return true;
    if(weight>=1.0) {
      boughEmit(i, 0u, 0.0);
      i = node.subtreeEnd;
      continue;
      }
    if(weight>0.0) {
      boughEmit(i, i+1u, weight);
      band = BoughChildSide | (i+1u);
      bandEnd = node.subtreeEnd;
      bandWeight = weight;
      }
    ++i;
    }
  return false;
  }

#endif
