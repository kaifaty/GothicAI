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

float boughParentWeight(BoughNode node, BoughProjection projection) {
  return 1.0 - smoothstep(node.lowPx, node.highPx, boughScreenSize(node, projection));
  }

// The fast-path query: no proxy of the slice [first, last) of the preorder array replaces its children yet.
bool boughMerges(uint first, uint last, BoughProjection projection) {
  for(uint i = first; i<last; ++i) {
    BoughNode node = boughGetNode(i);
    if(node.subtreeEnd!=i+1u && node.highPx>0.0 && boughParentWeight(node, projection)>0.0)
      return true;
    }
  return false;
  }

// Preorder lets a selected proxy skip all its descendants. The band is the one that is active at node i.
void boughWalkFrom(uint i, uint end, uint band, uint bandEnd, float bandWeight, BoughProjection projection) {
  while(i<end) {
    BoughNode node = boughGetNode(i);
    if(i>=bandEnd) {
      band = 0u;
      bandWeight = 0.0;
      }
    if(node.subtreeEnd==i+1u) {
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

    const float weight = boughParentWeight(node, projection);
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
  }

// Emits the part of the cut that lies in the slice [first, last) of the preorder array. The ancestors of the first
// node give the state the whole walk has there, so the slices of one crown run in parallel and emit each node once.
void boughWalkSlice(uint first, uint last, BoughProjection projection) {
  if(first>=last)
    return;
  // The topmost ancestor proxy that replaces its children decides: a full one hides its subtree, one in a band
  // leaves its nearest proxies and source cards.
  uint  band = 0u, bandEnd = 0u, belowEnd = 0u, proxyEnd = 0u;
  float bandWeight = 0.0;
  for(uint p = boughGetNode(first).parent; p!=0xffffffffu;) {
    BoughNode node = boughGetNode(p);
    if(node.highPx>0.0) {
      const float weight = boughParentWeight(node, projection);
      if(weight>0.0) {
        band       = BoughChildSide | (p+1u);
        bandEnd    = node.subtreeEnd;
        bandWeight = weight;
        belowEnd   = proxyEnd;
        }
      proxyEnd = node.subtreeEnd;
      }
    p = node.parent;
    }
  if(bandWeight>=1.0) {
    first      = bandEnd;
    band       = 0u;
    bandWeight = 0.0;
    }
  else if(band!=0u) {
    first = max(first, belowEnd);
    }
  boughWalkFrom(first, last, band, bandEnd, bandWeight, projection);
  }

#endif
