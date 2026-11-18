#pragma once

#include "foliagefront.h"
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
// Three motions, by the note's reference. The trunk leans by the wind's pressure, the square of the height up, and
// sways about the lean at its own rate, which falls with the tree's height; a point keeps its distance from the
// root. A branch gives way to the gust where it stands: the pressure is the front's at the vertex's place in the
// world, so a gust goes through the crown from its windward side to its lee one, and the gusts within it are waves
// that run the same way. The end goes by the branch's length, a long one far and slowly, a short one little and
// quicker; up and down it goes by the same push, the lee branch up and the windward one down. The needles tremble
// on the free ends, in patches that run behind the front, by a length that does not depend on the tree's size.
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
// The branch, by its weight R (0 at the trunk, 1 at its end) and the vertex's distance r from the trunk's axis: its
// length is about r/R. At full pressure the end of a branch BranchLong long gives way BranchGive of its length
// (37 cm of 4.6 m), and the share goes as the root of the length: 11 cm of 2 m, 1.1 cm of 0.45 m
constexpr float BranchGive    = 0.08f;
constexpr float BranchLong    = 460.f;    // cm: the lower bough of the spruce
constexpr float BranchShort   = 45.f;     // cm: the bough under its top
constexpr float BranchTopK    = 1.25f;    // the share grows no more past this many of BranchGive (a branch of 7.2 m)
constexpr float BranchReach   = 0.3f;     // of the height: r is taken no farther than this, so the bounds stay a share of it
constexpr float BranchRoot    = 0.05f;    // under this R the length is not taken longer
constexpr float BranchLift    = 0.6f;     // of the push goes up on a branch that points down the wind, and down on one against it
constexpr float BranchGust    = 0.5f;     // the gusts within a gust swing the branch by this share of the push
constexpr float BranchLinger  = 600.f;    // cm of the front's run, 1.2 s: the gusts stay this long after the pressure falls
constexpr float BranchOwn     = 0.12f;    // of a turn: the branches differ in step by their phase G within this
constexpr float BranchAxis    = 0.02f;    // of the height: nearer the trunk's axis than this a branch has no direction
// The gusts within a gust: four waves the wind carries along +Z at the front's speed, WaveRate turns in a
// SwingPeriod each (0.28, 0.48, 0.78 and 1.38 Hz; 17.6, 10.3, 6.4 and 3.6 m long), each turned a little to the
// side. A branch takes the two or three next to its length: BranchLong the first two, BranchShort the last two
constexpr float WaveRun       = float(FoliageFront::Speed*double(FoliageWind::SwingPeriod)/1000.0); // cm in a SwingPeriod
constexpr float WaveRate[4]   = {17.f, 29.f, 47.f, 83.f};
constexpr float WaveTilt[4]   = {0.35f, -0.5f, 0.6f, -0.4f}; // cm of the wave's run per cm of the world's x
constexpr float WaveWidth     = 1.5f;     // waves on each side of the branch's own it still takes
// The needles, by the weight B: the free edge goes up to LeafMax cm of the mesh at twice the rates of the flutter's
// two phases, 2.6 and 3.8 Hz, where a crest of the two quick waves is
constexpr float LeafMax       = 2.f;
constexpr float LeafInner     = 0.3f;     // the share of it by the trunk; all of it from R = LeafEndFull out
constexpr float LeafEndFrom   = 0.25f;
constexpr float LeafEndFull   = 0.8f;
constexpr float PatchFrom     = -0.2f;    // the half sum of the two quick waves a patch begins at
constexpr float PatchFull     = 0.5f;     // and is whole from
constexpr float LeafUp        = 0.8f;     // the part up and down, and
constexpr float LeafSide      = 0.6f;     // the part across the wind
constexpr float LeafPlaceX    = 0.045f;   // rad per cm of the mesh: the corners of one card are out of step, and the
constexpr float LeafPlaceY    = 0.031f;   // card twists
constexpr float LeafPlaceZ    = 0.053f;
// The fade with the distance (the note's section 4), in pixels of the main view: a motion is whole while the most
// of it is *Full pixels or more and gone under *None. The branches' most is the end of the longest one the rule
// lets a tree have, which goes four times as far as a middle one, so their numbers are above a pixel
constexpr float FadeLeafFull   = 0.8f;
constexpr float FadeLeafNone   = 0.5f;
constexpr float FadeBranchFull = 5.f;
constexpr float FadeBranchNone = 3.f;
constexpr float FadeTrunkFull  = 4.f;
constexpr float FadeTrunkNone  = 2.5f;
// The bounds of a tree whose wind fades (fadeBound): the fade is taken this share of the way to the camera and
// up to a whole one of FadeBoundSteps, so the sphere is never less than the motion and is not set anew each frame
constexpr float FadeBoundNear  = 0.98f;
constexpr float FadeBoundSteps = 8.f;

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
  // the branch's push is no longer than the wind's direction: what goes up is taken off what goes along
  const float branch = BranchReach*BranchGive*BranchTopK*(1.f + BranchGust);
  return reach*FoliageWind::packedWind(bits)*(trunk + branch) +
         FoliageWind::packedFlutter(bits)*LeafMax*(LeafUp + LeafSide);
  }

// What is left of a tree's three motions far off (foliageTreeFade): 1 near, where the rule is what it was; never
// trunk < branch < leaf, so the needles stop first and the trunk last
struct Fade final {
  float trunk  = 1.f;
  float branch = 1.f;
  float leaf   = 1.f;
  bool  none() const { return trunk<=0.f; }  // no wind at all: the vertex is not moved
  };

// SceneDesc::foliageFadeScale: the pixels of the main view a unit of the tangent is. projY: the projection's [1][1]
inline float fadeScale(float projY, int screenH) {
  return 0.5f*float(std::max(screenH,0))*std::abs(projY);
  }

// The fade of a tree `distance` cm from the main camera, by how many pixels the most of each motion is there: the
// needles' LeafMax, the end of the longest branch a tree of this height has by the rule, the lean of the top, all
// at a wind and a flutter of one, so the bark and the leaves of a tree fade as one body. scale: of the object's
// matrix up the trunk; pxPerTan: fadeScale, 0 without the fade. Not a tree: no fade
inline Fade fade(uint32_t bits, float reach, float scale, float distance, float pxPerTan) {
  Fade f;
  if(!isTree(bits) || pxPerTan<=0.f)
    return f;
  const auto  smooth = [](float from, float full, float v) {
    const float t = std::clamp((v-from)/(full-from), 0.f, 1.f);
    return t*t*(3.f-2.f*t);
    };
  const float height = std::max(reach, 1e-3f);
  const float px     = scale*pxPerTan/std::max(distance, 1.f); // pixels a cm of the mesh is
  const float arm    = height*BranchReach;
  const float bough  = arm*BranchGive*std::min(std::sqrt(scale*arm/BranchLong), BranchTopK);
  f.leaf   = smooth(FadeLeafNone, FadeLeafFull, LeafMax*px);
  f.branch = std::max(f.leaf,   smooth(FadeBranchNone, FadeBranchFull, bough*px));
  f.trunk  = std::max(f.branch, smooth(FadeTrunkNone,  FadeTrunkFull,  TreeLean*height*px));
  return f;
  }

// the fade the bounds are grown by: no less than the shader's of the same frame
inline Fade fadeBound(uint32_t bits, float reach, float scale, float distance, float pxPerTan) {
  Fade f = fade(bits, reach, scale, distance*FadeBoundNear, pxPerTan);
  f.trunk  = std::ceil(f.trunk *FadeBoundSteps)/FadeBoundSteps;
  f.branch = std::ceil(f.branch*FadeBoundSteps)/FadeBoundSteps;
  f.leaf   = std::ceil(f.leaf  *FadeBoundSteps)/FadeBoundSteps;
  return f;
  }

// a fade of fadeBound in twelve bits: what a tree's sphere is grown by now
inline uint16_t fadeCode(const Fade& f) {
  const auto step = [](float v) { return uint16_t(std::lround(std::clamp(v, 0.f, 1.f)*FadeBoundSteps)); };
  return uint16_t(step(f.trunk) | (step(f.branch) << 4) | (step(f.leaf) << 8));
  }

// travel of a tree whose wind has faded: each motion's part by what is left of it; all of it at Fade()
inline float travel(uint32_t bits, float reach, const Fade& f) {
  if(!isTree(bits))
    return FoliageWind::travel(bits,reach);
  const float trunk  = 2.f*(TreeLean + (TreeSwayCalm+TreeSwayWind)*(1.f+TreeAcross));
  const float branch = BranchReach*BranchGive*BranchTopK*(1.f + BranchGust);
  return reach*FoliageWind::packedWind(bits)*(trunk*f.trunk + branch*f.branch) +
         FoliageWind::packedFlutter(bits)*LeafMax*(LeafUp + LeafSide)*f.leaf;
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
// reach: the mesh's height; axis: the columns of the object's matrix; place: its translation; sc: the scene's wind
// with the pressure of the tree's place; front: the front the branches and the needles read at the vertex's place,
// `previous` its last frame's. Without a front every vertex has the tree's pressure. fd: what the distance leaves
// of the motions (fade); all of them without it
inline Tempest::Vec3 bend(const Tempest::Vec3& p, uint32_t color, uint32_t bits, float reach,
                          const Tempest::Vec3 axis[3], const Tempest::Vec3& place, const FoliageWind::Scene& sc,
                          const FoliageFront::Front& front = FoliageFront::Front(), bool previous = false,
                          const Fade& fd = Fade()) {
  using FoliageWind::fract;
  const float windK = FoliageWind::packedWind(bits), flutK = FoliageWind::packedFlutter(bits);
  if(sc.wind.w<=0.f || (windK<=0.f && flutK<=0.f) || fd.none())
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
      const float along = height*u*u*windK*(TreeLean*q + sway*sa)*fd.trunk;
      const float side  = height*u*u*windK*sway*TreeAcross*sb*fd.trunk;
      const Tempest::Vec3 t(p.x + wx*along - wz*side, p.y, p.z + wz*along + wx*side);
      const float pl = p.length(), tl = t.length();
      r = tl>1e-6f ? t*(pl/tl) : p;
      }

    }

  // a far tree has the trunk's motion alone: no front and no waves for its vertices
  const bool branch = cR>0.f && windK>0.f && fd.branch>0.f, leaf = cB>0.f && flutK>0.f && fd.leaf>0.f;
  if(!branch && !leaf)
    return r;

  // the gust where the vertex stands: the front's pressure now, and what lingers of it; the gusts within it, four
  // waves that the wind carries over the world
  const float atX = place.x + axis[0].x*p.x + axis[1].x*p.y + axis[2].x*p.z;
  const float atZ = place.z + axis[0].z*p.x + axis[1].z*p.y + axis[2].z*p.z;
  const float ql  = std::clamp(FoliageFront::atPlace(front,sc,atZ,previous).pressure, 0.f, 1.f);
  const float el  = std::max(ql, std::clamp(FoliageFront::atPlace(front,sc,atZ+BranchLinger,previous).pressure, 0.f, 1.f));
  float wave[4] = {};
  for(int i=0; i<4; ++i) {
    const float run = sc.swing.x - (atZ + WaveTilt[i]*atX)/WaveRun;
    wave[i] = std::sin(6.2831853f*fract(WaveRate[i]*run + BranchOwn*(cG - 0.5f)));
    }

  // the branch: gives way to the gust and comes back, but not along itself; the lee one rises, the windward dips
  const float rl = std::sqrt(p.x*p.x + p.z*p.z);
  if(branch && rl>1e-4f) {
    const float soft  = height*BranchAxis;
    const float firm  = rl*rl/(rl*rl + soft*soft);
    const float along = (wx*p.x + wz*p.z)/rl;
    const float len   = al[1]*rl/std::max(cR, BranchRoot);
    const float span  = 2.f*std::clamp(std::log2(BranchLong/len)/std::log2(BranchLong/BranchShort), 0.f, 1.f);
    float gusts = 0.f, sum = 0.f;
    for(int i=0; i<4; ++i) {
      const float take = std::max(0.f, 1.f - std::abs(float(i) - (span + 0.5f))/WaveWidth);
      gusts += take*wave[i];
      sum   += take;
      }
    gusts /= sum;
    const float give  = BranchGive*std::min(std::sqrt(al[1]*rl*cR/BranchLong), BranchTopK);
    const float push  = windK*std::min(rl, height*BranchReach)*give*(ql + BranchGust*el*gusts)*fd.branch;
    r.x += push*(wx - firm*along*p.x/rl);
    r.z += push*(wz - firm*along*p.z/rl);
    r.y += push*BranchLift*firm*along;
    }

  // the needles: the free edge trembles where a gust's crest is, the ends of the branches most; each corner of a
  // card in its own step
  if(leaf) {
    const auto  smooth = [](float from, float full, float v) {
      const float t = std::clamp((v-from)/(full-from), 0.f, 1.f);
      return t*t*(3.f-2.f*t);
      };
    const float crest = smooth(PatchFrom, PatchFull, 0.5f*(wave[2] + wave[3]));
    const float end   = LeafInner + (1.f-LeafInner)*smooth(LeafEndFrom, LeafEndFull, cR);
    const float amp   = flutK*cB*LeafMax*el*crest*end*fd.leaf;
    const float at    = p.x*LeafPlaceX + p.y*LeafPlaceY + p.z*LeafPlaceZ;
    const float s1    = std::sin(12.5663706f*(sc.phase.z + cG + h) + at);
    const float s2    = std::sin(12.5663706f*(sc.phase.w + cG + fract(h*1.618034f)) + at*1.3f + 1.7f);
    r.y += amp*LeafUp*s1;
    r.x -= amp*LeafSide*s2*wz;
    r.z += amp*LeafSide*s2*wx;
    }
  return r;
  }

// what the vertex shader does to a vertex of a BK_FOLIAGE bucket (foliageMove, foliage_tree.glsl)
inline Tempest::Vec3 move(const Tempest::Vec3& p, uint32_t color, uint32_t bits, float reach,
                          const Tempest::Vec3 axis[3], const Tempest::Vec3& place, const FoliageWind::Scene& sc,
                          const FoliageFront::Front& front = FoliageFront::Front(), bool previous = false,
                          const Fade& fd = Fade()) {
  return isTree(bits) ? bend(p,color,bits,reach,axis,place,sc,front,previous,fd) : FoliageWind::bend(p,bits,reach,axis,place,sc);
  }

}
