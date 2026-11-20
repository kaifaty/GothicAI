#ifndef GRASS_EAR_GLSL
#define GRASS_EAR_GLSL

// A stalk with an ear: the rye of a field (reports/content/2026-11-19-crop-fields/stalk.md; the record's
// "kind": "ear", common/graphics/grass/grassear.h). The same 16 bytes and the same strip of up to four links as a
// blade of grass, but the links are not of one length: a thin stalk to GrassEarFrom of the height, then the ear,
// which hangs over to the side the stalk leans to. The strip's width is across the view round the stalk itself, so
// a stalk is seen from straight above as well. The numbers of the look: grass_common.glsl

const uint  GrassKindEar       = 2u;    // Push::kind of grass.vert (Grass::KindEar)
const float GrassEarHeightStep = 0.75;  // cm of the height's byte (Grass::EarHeightStep)
const float GrassEarFrom       = 0.86;  // of the height: the ear starts
const float GrassEarFull       = 0.90;  // and is as wide as it gets
const float GrassEarDroopFrom  = 0.78;  // the stalk bends under the ear from here

// The tip's tone of a stalk's pixel at `t` of its height (grass.frag)
vec3 grassEarTip(float t, float dry) {
  return mix(GrassEarStalk, mix(GrassEarGold, GrassEarPale, dry), smoothstep(GrassEarFrom, GrassEarFrom+0.04, t));
  }

#if defined(GRASS_EAR_SHAPE)
// The share of the height at node k of n links. Four: the stalk's middle, the ear's start, its full width, the tip;
// fewer links drop the nodes from below
float grassEarAt(int k, int n) {
  if(k<=0)
    return 0.0;
  if(k>=n)
    return 1.0;
  if(n==2)
    return 0.86;
  const int i = k + 4 - n;
  return i==1 ? 0.45 : (i==2 ? GrassEarFrom : GrassEarFull);
  }

// The stalk's direction at t of its height. fwd: the side it leans to; droop: the tangent the ear hangs over by
vec3 grassEarDir(Tilt tilt, vec2 fwd, float droop, float t) {
  tilt.a += fwd*(droop*clamp((t-GrassEarDroopFrom)/(1.0-GrassEarDroopFrom), 0.0, 1.0));
  return bladeDir(tilt, t);
  }

// The node k of n from the root: the links keep their lengths
vec3 grassEarPoint(Tilt tilt, vec2 fwd, float droop, int k, int n, float height) {
  vec3  p  = vec3(0);
  float t0 = 0.0;
  for(int i=1; i<=k; ++i) {
    const float t1 = grassEarAt(i, n);
    p += grassEarDir(tilt, fwd, droop, 0.5*(t0+t1))*(t1-t0);
    t0 = t1;
    }
  return p*height;
  }

struct GrassEarVertex {
  int   k;     // its node
  float t;     // the node's share of the height
  float side;  // -0.5, 0.5 or 0 at the single one
  float width; // cm, of the strip at the node
  };

// vertex: of the strip's 2n+1, two at each node and a single one last. widths: of the stalk and of the ear, cm.
// With one link the strip is a wedge as wide as the ear at the top: the pair is up, the single one at the root
GrassEarVertex grassEarVertex(int vertex, int n, vec2 widths) {
  GrassEarVertex r;
  const bool single = (vertex >> 1)>=n;
  r.k     = n==1 ? (single ? 0 : 1) : min(vertex >> 1, n);
  r.t     = grassEarAt(r.k, n);
  r.side  = single ? 0.0 : ((vertex & 1)==0 ? -0.5 : 0.5);
  r.width = r.t>GrassEarFrom+0.01 ? widths.y : widths.x;
  return r;
  }

// The strip's width at a node: across the view, round the stalk. toEye: from the node; level: the width of a blade
// of grass, for a stalk seen along itself
vec3 grassEarAxis(vec3 dir, vec3 toEye, vec3 level) {
  return normalize(cross(dir, normalize(toEye)) + level*1e-3);
  }
#endif

#endif
