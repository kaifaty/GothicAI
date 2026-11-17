#pragma once

#include "foliagewind.h"

// The tree's wind (reports/engine/2026-11-16-modern-trees/tree-wind.md): a material with
// `"foliage": {"anchor": "tree"}` moves as a tree does and not as a tuft. This header is the processor's side of
// shader/materials/foliage_tree.glsl: the numbers, the rule for the tests, and the bounds. Keep the two in step.
//
// The tree stands in its mesh's axes: the root at the origin, the trunk up Y; Bucket::foliageReach is the height of
// the whole mesh, so the bark and the leaves of one tree bend as one body. The vertex colour of the own GLB
// (COLOR_0) says what a vertex is a part of: R the weight of its branch (0 on the trunk, 1 at the branch's end),
// G the branch's phase, B the weight of the leaf's flutter (0 at the stalk), A the openness of the crown, which this
// rule does not read.
//
// Three motions, as measured on trees (the note's reference). The trunk leans by the wind's pressure, the square of
// the height up, and sways about the lean at its own rate, which falls with the tree's height; a point keeps its
// distance from the root. A branch nods and gives way to the wind at a rate of its own, quicker than the trunk's.
// A leaf trembles, quicker still, by a length that does not depend on the tree's size.
namespace FoliageTree {

// The trunk. At the peak of the world's gust the top goes TreeLean of the height: a real tree's at 7 Beaufort
constexpr float TreeLean      = 0.03f;
constexpr float TreeSwayCalm  = 0.001f;   // the sway about the lean, of the height at the top: at no pressure,
constexpr float TreeSwayWind  = 0.007f;   // and more of it per unit of pressure
constexpr float TreeAcross    = 0.35f;    // the part of the sway across the wind
constexpr float Tree1Share    = 0.6f;     // two close rates beat: the sway rises and falls
constexpr float Tree2Share    = 0.4f;
// f0 = 0.0948 + 3.4317*DBH/H^2 Hz of a conifer (Moore, Maguire 2004), with the height 62 diameters as in a forest:
// 0.0948 + 5.535/H(m). In swings a SwingPeriod: 0.56 Hz at 12 m, 0.28 Hz at 30 m
constexpr float TreeRateBase  = 5.688f;
constexpr float TreeRateByCm  = 33210.f;
constexpr float TreeHeightMin = 300.f;    // cm: the heights the rate is taken between
constexpr float TreeHeightMax = 6000.f;
constexpr float TreeSpread    = 0.2f;     // trees of one height differ by this share of the rate, by their place
constexpr float TreeBeatMin   = 2.f;      // the second rate is this many swings a period over the first, and up to
constexpr float TreeBeatVar   = 3.f;      // this many more: the sway rises and falls every 15..30 s
// The branch, by its weight R; all of the tree's height at the branch's end
constexpr float BranchLean    = 0.010f;   // gives way to the wind per unit of pressure
constexpr float BranchSway    = 0.006f;   // swings along the wind
constexpr float BranchNod     = 0.008f;   // and up and down
constexpr float BranchCalm    = 0.15f;    // the share of the swing and the nod at no pressure: in a breeze it nearly stands
constexpr float BranchRate    = 1.6f;     // its rate is from this many of the trunk's
constexpr float BranchRateVar = 1.2f;     // to this many more, by its phase G
constexpr float BranchAxis    = 0.02f;    // of the height: nearer the trunk's axis than this a branch has no direction
// The leaf, by its weight B: the free edge goes this many cm of the mesh at twice the rates of the flutter's two
// phases, 2.6 and 3.8 Hz
constexpr float LeafCalm      = 0.3f;
constexpr float LeafWind      = 1.2f;     // more of it per unit of the flutter's pressure
constexpr float LeafMaxQ      = 1.5f;     // the pressure it stops growing at
constexpr float LeafUp        = 0.8f;     // the part up and down, and
constexpr float LeafSide      = 0.6f;     // the part across the wind
constexpr float LeafPlaceX    = 0.045f;   // rad per cm of the mesh: the corners of one card are out of step, and the
constexpr float LeafPlaceY    = 0.031f;   // card twists
constexpr float LeafPlaceZ    = 0.053f;

inline bool isTree(const FoliageWind::Params& p) { return p.on && p.anchor==FoliageWind::AnchorTree; }
inline bool isTree(uint32_t bits)                { return (bits >> 24)==FoliageWind::AnchorTree; }

// Bucket::foliageReach: the tree's height over its root, or the tuft's largest arm
inline float reach(const FoliageWind::Params& p, const Tempest::Vec3& boxMin, const Tempest::Vec3& boxMax) {
  return isTree(p) ? std::max(boxMax.y, 1e-3f) : FoliageWind::reach(boxMin,boxMax);
  }

// how far a vertex can go from its place at rest, in any wind: the bounds the culling tests grow by it
inline float travel(uint32_t bits, float reach) {
  if(!isTree(bits))
    return FoliageWind::travel(bits,reach);
  // keeping the distance from the root moves a vertex no farther than the lean itself does: twice the lean
  const float trunk  = 2.f*(TreeLean + (TreeSwayCalm+TreeSwayWind)*(1.f+TreeAcross));
  const float branch = BranchLean + BranchSway + BranchNod;
  return reach*FoliageWind::packedWind(bits)*(trunk + branch) +
         FoliageWind::packedFlutter(bits)*(LeafCalm + LeafWind*LeafMaxQ)*(LeafUp + LeafSide);
  }

// the same for a material's mesh, by the switch
inline float travel(const FoliageWind::Params& p, const Tempest::Vec3& boxMin, const Tempest::Vec3& boxMax) {
  return p.on ? travel(FoliageWind::pack(p,FoliageWind::enabled()), reach(p,boxMin,boxMax)) : 0.f;
  }

// the two rates of a tree's sway, in swings a SwingPeriod: by its height in the world and by its place
inline FoliageWind::SwingRates swayRates(float hash, float heightCm) {
  const float own = TreeRateBase + TreeRateByCm/std::clamp(heightCm, TreeHeightMin, TreeHeightMax);
  FoliageWind::SwingRates r;
  r.first  = std::floor(own*(1.f + TreeSpread*(FoliageWind::fract(hash*2.236068f)-0.5f)) + 0.5f);
  r.second = r.first + TreeBeatMin + std::floor(FoliageWind::fract(hash*3.302776f)*TreeBeatVar);
  return r;
  }

// The vertex of a tree in its mesh's axes, moved by the wind. color: Resources::Vertex::color, R in the low byte;
// reach: the mesh's height; axis: the columns of the object's matrix; place: its translation
inline Tempest::Vec3 bend(const Tempest::Vec3& p, uint32_t color, uint32_t bits, float reach,
                          const Tempest::Vec3 axis[3], const Tempest::Vec3& place, const FoliageWind::Scene& sc) {
  using FoliageWind::fract;
  const float windK = FoliageWind::packedWind(bits), flutK = FoliageWind::packedFlutter(bits);
  if(sc.wind.w<=0.f || (windK<=0.f && flutK<=0.f))
    return p;
  const float height = std::max(reach, 1e-3f);
  const float cR = float( color      & 0xFFu)/255.f;
  const float cG = float((color>>8)  & 0xFFu)/255.f;
  const float cB = float((color>>16) & 0xFFu)/255.f;

  // the wind across the tree's axes: along the trunk it does nothing
  const Tempest::Vec3 w(sc.wind.x, 0.f, sc.wind.y);
  float al[3] = {};
  for(int i=0; i<3; ++i)
    al[i] = axis[i].length();
  const float wx = al[0]>1e-6f ? Tempest::Vec3::dotProduct(w,axis[0])/al[0] : 0.f;
  const float wz = al[2]>1e-6f ? Tempest::Vec3::dotProduct(w,axis[2])/al[2] : 0.f;

  const float q  = std::clamp(sc.wind.z, 0.f, 1.f);
  const float h  = FoliageWind::placeHash(place);
  const FoliageWind::SwingRates n = swayRates(h, height*al[1]);
  Tempest::Vec3 r = p;

  if(windK>0.f) {
    // the trunk: the lean and the sway about it, the square of the height up; the distance from the root is kept
    const float u = std::clamp(p.y/height, 0.f, 1.f);
    if(u>0.f) {
      const float a1    = 6.2831853f*(fract(n.first *sc.swing.x) + fract(h*1.618034f));
      const float a2    = 6.2831853f*(fract(n.second*sc.swing.x) + h);
      const float sa    = Tree1Share*std::sin(a1) + Tree2Share*std::sin(a2);
      const float sb    = Tree1Share*std::cos(a1) - Tree2Share*std::cos(a2);
      const float sway  = TreeSwayCalm + TreeSwayWind*q;
      const float along = height*u*u*windK*(TreeLean*q + sway*sa);
      const float side  = height*u*u*windK*sway*TreeAcross*sb;
      const Tempest::Vec3 t(p.x + wx*along - wz*side, p.y, p.z + wz*along + wx*side);
      const float pl = p.length(), tl = t.length();
      r = tl>1e-6f ? t*(pl/tl) : p;
      }

    // the branch: gives way to the wind and swings along it, but not along itself; nods
    if(cR>0.f) {
      const float rl   = std::sqrt(p.x*p.x + p.z*p.z);
      const float soft = height*BranchAxis;
      const float keep = rl>1e-4f ? (rl*rl/(rl*rl + soft*soft))*(wx*p.x + wz*p.z)/(rl*rl) : 0.f;
      const float nb   = std::floor(n.first*(BranchRate + BranchRateVar*fract(cG*7.31f + h*3.7f)) + 0.5f);
      const float ab   = 6.2831853f*(fract(nb*sc.swing.x) + cG + h);
      const float live = BranchCalm + (1.f-BranchCalm)*q;
      const float amp  = height*windK*cR;
      const float push = amp*(BranchLean*q + BranchSway*live*std::sin(ab + 1.9f));
      r.x += push*(wx - keep*p.x);
      r.z += push*(wz - keep*p.z);
      r.y += amp*BranchNod*live*std::sin(ab);
      }
    }

  // the leaf: its free edge trembles, each corner of a card in its own step
  if(cB>0.f && flutK>0.f) {
    const float amp = flutK*cB*(LeafCalm + LeafWind*std::min(sc.swing.z, LeafMaxQ));
    const float at  = p.x*LeafPlaceX + p.y*LeafPlaceY + p.z*LeafPlaceZ;
    const float s1  = std::sin(12.5663706f*(sc.phase.z + cG + h) + at);
    const float s2  = std::sin(12.5663706f*(sc.phase.w + cG + fract(h*1.618034f)) + at*1.3f + 1.7f);
    r.y += amp*LeafUp*s1;
    r.x -= amp*LeafSide*s2*wz;
    r.z += amp*LeafSide*s2*wx;
    }
  return r;
  }

// what the vertex shader does to a vertex of a BK_FOLIAGE bucket (foliageMove, foliage_tree.glsl)
inline Tempest::Vec3 move(const Tempest::Vec3& p, uint32_t color, uint32_t bits, float reach,
                          const Tempest::Vec3 axis[3], const Tempest::Vec3& place, const FoliageWind::Scene& sc) {
  return isTree(bits) ? bend(p,color,bits,reach,axis,place,sc) : FoliageWind::bend(p,bits,reach,axis,place,sc);
  }

}
