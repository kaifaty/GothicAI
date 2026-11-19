#ifndef TREE_BOUGH_CUT_GLSL
#define TREE_BOUGH_CUT_GLSL

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
bool boughWasCoarse(uint i);
void boughSetCoarse(uint i, bool coarse);
void boughEmit(uint i, uint transition, float weight);

float boughScreenSize(BoughNode node, BoughProjection projection) {
  vec3 nearest = mix(node.boxMin, node.boxMax, lessThan(projection.forwardLocal, vec3(0)));
  float depth = projection.originDepth + dot(projection.forwardLocal, nearest);
  return depth<=projection.nearZ ? uintBitsToFloat(0x7f800000u) : node.diameter * projection.focalScale / depth;
  }

// Preorder lets a selected proxy skip all its descendants. For the ordinary meshlet fast path, stop as soon as
// there is any proxy or active band. That query leaves the previous cut intact for the emitting walk.
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

    const float size = boughScreenSize(node, projection);
    const float limit = node.thresholdPx * (boughWasCoarse(i) ? 1.1 : 1.0);
    const bool coarse = size<limit;
    const bool inBand = size>node.lowPx && size<node.highPx;
    if(!emit) {
      if(coarse || inBand)
        return true;
      ++i;
      continue;
      }
    boughSetCoarse(i, coarse);

    // Nested boxes and threshold ordering keep descendant bands outside the active ancestor's band.
    const float weight = inBand ? 1.0 - smoothstep(node.lowPx, node.highPx, size) : 0.0;
    if(coarse) {
      boughEmit(i, band!=0u ? band : (inBand ? i+1u : 0u), band!=0u ? bandWeight : weight);
      i = node.subtreeEnd;
      continue;
      }
    if(inBand && band==0u) {
      band = 0x8000u | (i+1u);
      bandEnd = node.subtreeEnd;
      bandWeight = weight;
      }
    ++i;
    }
  return false;
  }

#endif
