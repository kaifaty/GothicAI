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
// its distance from the root. A branch gives way to the gust where it stands: the pressure is the front's at the
// vertex's place in the world, so a gust goes through the crown from its windward side to its lee one, and the
// gusts within it are waves that run the same way. The end goes by the branch's length, a long one far and slowly,
// a short one little and quicker; up and down it goes by the same push, the lee branch up and the windward one down.
// The needles tremble on the free ends, in patches that run behind the front.

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
const float FoliageTreeBranchGive   = 0.08;
const float FoliageTreeBranchLong   = 460.0;
const float FoliageTreeBranchShort  = 45.0;
const float FoliageTreeBranchTopK   = 1.25;
const float FoliageTreeBranchReach  = 0.3;
const float FoliageTreeBranchRoot   = 0.05;
const float FoliageTreeBranchLift   = 0.6;
const float FoliageTreeBranchGust   = 0.5;
const float FoliageTreeBranchLinger = 600.0;
const float FoliageTreeBranchOwn    = 0.12;
const float FoliageTreeBranchAxis   = 0.02;
const float FoliageTreeWaveRun      = 30000.0;
const vec4  FoliageTreeWaveRate     = vec4(17.0, 29.0, 47.0, 83.0);
const vec4  FoliageTreeWaveTilt     = vec4(0.35, -0.5, 0.6, -0.4);
const float FoliageTreeWaveWidth    = 1.5;
const float FoliageTreeLeafMax    = 2.0;
const float FoliageTreeLeafInner  = 0.3;
const float FoliageTreeLeafEndFrom  = 0.25;
const float FoliageTreeLeafEndFull  = 0.8;
const float FoliageTreePatchFrom  = -0.2;
const float FoliageTreePatchFull  = 0.5;
const float FoliageTreeLeafUp     = 0.8;
const float FoliageTreeLeafSide   = 0.6;
const vec3  FoliageTreeLeafPlace  = vec3(0.045, 0.031, 0.053);

// p: the vertex in the mesh's axes; color: its colour, R in the low byte; reach: the mesh's height
// (Bucket::foliageReach); mat: the object's matrix; wind, phase, swingOf: as foliageBend takes them, the pressure
// that of the tree's place; frontPh, frontK: the front of that frame (foliageFrontAt)
vec3 foliageTreeBend(vec3 p, uint color, uint bits, float reach, mat4x3 mat, vec4 wind, vec4 phase, vec4 swingOf,
                     vec2 frontPh, vec4 frontK) {
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
    }

  const bool branch = c.r>0.0 && windK>0.0, leaf = c.b>0.0 && flutK>0.0;
  if(!branch && !leaf)
    return r;

  // the gust where the vertex stands: the front's pressure now, and what lingers of it; the gusts within it, four
  // waves that the wind carries over the world
  const vec3  at   = mat*vec4(p,1.0);
  const float ql   = clamp(foliageFrontAt(at.z, frontPh, frontK, wind, swingOf).x, 0.0, 1.0);
  const float el   = max(ql, clamp(foliageFrontAt(at.z + FoliageTreeBranchLinger, frontPh, frontK, wind, swingOf).x, 0.0, 1.0));
  const vec4  run  = vec4(swingOf.x) - (vec4(at.z) + FoliageTreeWaveTilt*at.x)/FoliageTreeWaveRun;
  const vec4  wave = sin(6.2831853*fract(FoliageTreeWaveRate*run + FoliageTreeBranchOwn*(c.g - 0.5)));

  // the branch: gives way to the gust and comes back, but not along itself; the lee one rises, the windward dips
  const float rl = length(p.xz);
  if(branch && rl>1e-4) {
    const float soft  = height*FoliageTreeBranchAxis;
    const float firm  = rl*rl/(rl*rl + soft*soft);
    const float along = (wx*p.x + wz*p.z)/rl;
    const float len   = al.y*rl/max(c.r, FoliageTreeBranchRoot);
    const float span  = 2.0*clamp(log2(FoliageTreeBranchLong/len)/log2(FoliageTreeBranchLong/FoliageTreeBranchShort), 0.0, 1.0);
    const vec4  take  = max(vec4(0.0), vec4(1.0) - abs(vec4(0.0,1.0,2.0,3.0) - vec4(span + 0.5))/FoliageTreeWaveWidth);
    const float gusts = dot(take,wave)/(take.x + take.y + take.z + take.w);
    const float give  = FoliageTreeBranchGive*min(sqrt(al.y*rl*c.r/FoliageTreeBranchLong), FoliageTreeBranchTopK);
    const float push  = windK*min(rl, height*FoliageTreeBranchReach)*give*(ql + FoliageTreeBranchGust*el*gusts);
    r.x += push*(wx - firm*along*p.x/rl);
    r.z += push*(wz - firm*along*p.z/rl);
    r.y += push*FoliageTreeBranchLift*firm*along;
    }

  // the needles: the free edge trembles where a gust's crest is, the ends of the branches most; each corner of a
  // card in its own step
  if(leaf) {
    const float crest = smoothstep(FoliageTreePatchFrom, FoliageTreePatchFull, 0.5*(wave.z + wave.w));
    const float end   = mix(FoliageTreeLeafInner, 1.0, smoothstep(FoliageTreeLeafEndFrom, FoliageTreeLeafEndFull, c.r));
    const float amp   = flutK*c.b*FoliageTreeLeafMax*el*crest*end;
    const float place = dot(p, FoliageTreeLeafPlace);
    const float s1    = sin(12.5663706*(phase.z + c.g + h) + place);
    const float s2    = sin(12.5663706*(phase.w + c.g + fract(h*1.618034)) + place*1.3 + 1.7);
    r.y += amp*FoliageTreeLeafUp*s1;
    r.x -= amp*FoliageTreeLeafSide*s2*wz;
    r.z += amp*FoliageTreeLeafSide*s2*wx;
    }
  return r;
  }

// a vertex of a BK_FOLIAGE bucket, by the anchor of its material: a tree or a tuft (FoliageTree::move). frontPh,
// frontK: the front of that frame, which a tree reads at every vertex
vec3 foliageMove(vec3 p, uint color, uint bits, float reach, mat4x3 mat, vec4 wind, vec4 phase, vec4 swingOf,
                 vec2 frontPh, vec4 frontK) {
  if((bits >> 24)==FoliageAnchorTree)
    return foliageTreeBend(p, color, bits, reach, mat, wind, phase, swingOf, frontPh, frontK);
  return foliageBend(p, bits, reach, mat, wind, phase, swingOf);
  }

#endif
