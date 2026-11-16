#ifndef FOLIAGE_TREE_GLSL
#define FOLIAGE_TREE_GLSL

#include "foliage_wind.glsl"

// The tree's wind (reports/engine/2026-11-16-modern-trees/tree-wind.md): the vertices of a material with
// `"foliage": {"anchor": "tree"}` move as a tree does. The numbers and the same rule on the processor, for the
// tests and the bounds: common/graphics/foliagetree.h. Keep the two files in step.
//
// The tree stands in its mesh's axes: the root at the origin, the trunk up Y; Bucket::foliageReach is the height of
// the whole mesh, so the bark and the leaves of one tree bend as one body. The vertex colour of the own GLB says
// what a vertex is a part of: R the weight of its branch, G the branch's phase, B the weight of the leaf's flutter.
// The trunk leans by the wind's pressure and sways about the lean at a rate from the tree's height, a point keeps
// its distance from the root; a branch nods and gives way to the wind at its own, quicker rate; a leaf trembles.

const uint  FoliageAnchorTree = 1u;

const float FoliageTreeLean      = 0.03;
const float FoliageTreeSwayCalm  = 0.001;
const float FoliageTreeSwayWind  = 0.007;
const float FoliageTreeAcross    = 0.35;
const float FoliageTree1Share    = 0.6;
const float FoliageTree2Share    = 0.4;
const float FoliageTreeRateBase  = 5.688;
const float FoliageTreeRateByCm  = 33210.0;
const float FoliageTreeHeightMin = 300.0;
const float FoliageTreeHeightMax = 6000.0;
const float FoliageTreeSpread    = 0.2;
const float FoliageTreeBeatMin   = 2.0;
const float FoliageTreeBeatVar   = 3.0;
const float FoliageTreeBranchLean    = 0.010;
const float FoliageTreeBranchSway    = 0.006;
const float FoliageTreeBranchNod     = 0.008;
const float FoliageTreeBranchCalm    = 0.15;
const float FoliageTreeBranchRate    = 1.6;
const float FoliageTreeBranchRateVar = 1.2;
const float FoliageTreeBranchAxis    = 0.02;
const float FoliageTreeLeafCalm   = 0.3;
const float FoliageTreeLeafWind   = 1.2;
const float FoliageTreeLeafMaxQ   = 1.5;
const float FoliageTreeLeafUp     = 0.8;
const float FoliageTreeLeafSide   = 0.6;
const vec3  FoliageTreeLeafPlace  = vec3(0.045, 0.031, 0.053);

// p: the vertex in the mesh's axes; color: its colour, R in the low byte; reach: the mesh's height
// (Bucket::foliageReach); mat: the object's matrix; wind, phase, swingOf: as foliageBend takes them
vec3 foliageTreeBend(vec3 p, uint color, uint bits, float reach, mat4x3 mat, vec4 wind, vec4 phase, vec4 swingOf) {
  const float windK = foliagePackedWind(bits), flutK = foliagePackedFlutter(bits);
  if(wind.w<=0.0 || (windK<=0.0 && flutK<=0.0))
    return p;
  const float height = max(reach, 1e-3);
  const vec4  c      = unpackUnorm4x8(color);

  // the wind across the tree's axes: along the trunk it does nothing
  const vec3  w  = vec3(wind.x, 0.0, wind.y);
  const vec3  al = vec3(length(mat[0]), length(mat[1]), length(mat[2]));
  const float wx = al.x>1e-6 ? dot(w,mat[0])/al.x : 0.0;
  const float wz = al.z>1e-6 ? dot(w,mat[2])/al.z : 0.0;

  const float q   = clamp(wind.z, 0.0, 1.0);
  const float h   = foliagePlaceHash(mat[3]);
  const float own = FoliageTreeRateBase + FoliageTreeRateByCm/clamp(height*al.y, FoliageTreeHeightMin, FoliageTreeHeightMax);
  const float n1  = floor(own*(1.0 + FoliageTreeSpread*(fract(h*2.236068)-0.5)) + 0.5);
  const float n2  = n1 + FoliageTreeBeatMin + floor(fract(h*3.302776)*FoliageTreeBeatVar);
  vec3 r = p;

  if(windK>0.0) {
    // the trunk: the lean and the sway about it, the square of the height up; the distance from the root is kept
    const float u = clamp(p.y/height, 0.0, 1.0);
    if(u>0.0) {
      const float a1    = 6.2831853*(fract(n1*swingOf.x) + fract(h*1.618034));
      const float a2    = 6.2831853*(fract(n2*swingOf.x) + h);
      const float sa    = FoliageTree1Share*sin(a1) + FoliageTree2Share*sin(a2);
      const float sb    = FoliageTree1Share*cos(a1) - FoliageTree2Share*cos(a2);
      const float sway  = FoliageTreeSwayCalm + FoliageTreeSwayWind*q;
      const float along = height*u*u*windK*(FoliageTreeLean*q + sway*sa);
      const float side  = height*u*u*windK*sway*FoliageTreeAcross*sb;
      const vec3  t     = vec3(p.x + wx*along - wz*side, p.y, p.z + wz*along + wx*side);
      const float tl    = length(t);
      r = tl>1e-6 ? t*(length(p)/tl) : p;
      }

    // the branch: gives way to the wind and swings along it, but not along itself; nods
    if(c.r>0.0) {
      const float rl   = length(p.xz);
      const float soft = height*FoliageTreeBranchAxis;
      const float keep = rl>1e-4 ? (rl*rl/(rl*rl + soft*soft))*(wx*p.x + wz*p.z)/(rl*rl) : 0.0;
      const float nb   = floor(n1*(FoliageTreeBranchRate + FoliageTreeBranchRateVar*fract(c.g*7.31 + h*3.7)) + 0.5);
      const float ab   = 6.2831853*(fract(nb*swingOf.x) + c.g + h);
      const float live = mix(FoliageTreeBranchCalm, 1.0, q);
      const float amp  = height*windK*c.r;
      const float push = amp*(FoliageTreeBranchLean*q + FoliageTreeBranchSway*live*sin(ab + 1.9));
      r.x += push*(wx - keep*p.x);
      r.z += push*(wz - keep*p.z);
      r.y += amp*FoliageTreeBranchNod*live*sin(ab);
      }
    }

  // the leaf: its free edge trembles, each corner of a card in its own step
  if(c.b>0.0 && flutK>0.0) {
    const float amp = flutK*c.b*(FoliageTreeLeafCalm + FoliageTreeLeafWind*min(swingOf.z, FoliageTreeLeafMaxQ));
    const float at  = dot(p, FoliageTreeLeafPlace);
    const float s1  = sin(12.5663706*(phase.z + c.g + h) + at);
    const float s2  = sin(12.5663706*(phase.w + c.g + fract(h*1.618034)) + at*1.3 + 1.7);
    r.y += amp*FoliageTreeLeafUp*s1;
    r.x -= amp*FoliageTreeLeafSide*s2*wz;
    r.z += amp*FoliageTreeLeafSide*s2*wx;
    }
  return r;
  }

// a vertex of a BK_FOLIAGE bucket, by the anchor of its material: a tree or a tuft (FoliageTree::move)
vec3 foliageMove(vec3 p, uint color, uint bits, float reach, mat4x3 mat, vec4 wind, vec4 phase, vec4 swingOf) {
  if((bits >> 24)==FoliageAnchorTree)
    return foliageTreeBend(p, color, bits, reach, mat, wind, phase, swingOf);
  return foliageBend(p, bits, reach, mat, wind, phase, swingOf);
  }

#endif
