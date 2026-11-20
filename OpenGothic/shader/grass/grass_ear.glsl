#ifndef GRASS_EAR_GLSL
#define GRASS_EAR_GLSL

// A stalk with an ear: the rye of a field (reports/content/2026-11-19-crop-fields/stalk.md; the record's
// "kind": "ear", common/graphics/grass/grassear.h). The same 16 bytes and the same strip of up to four links as a
// blade of grass, but the links are not of one length: a thin stalk to GrassEarFrom of the height, then the ear,
// which hangs over to the side the stalk leans to. The straw is stiff: it leans as a rod, with no bend along it, and
// the bend is the neck's under the ear, over three links. The strip's width is across the view round the stalk
// itself, so a stalk is seen from straight above as well. Seen close the ear is a card with its grains and awns
// drawn in the pixel (grassEarCard); with the distance the card narrows to the plain strip. The numbers of the look:
// grass_common.glsl

const uint  GrassKindEar       = 2u;    // Push::kind of grass.vert (Grass::KindEar)
const float GrassEarHeightStep = 0.75;  // cm of the height's byte (Grass::EarHeightStep)
const float GrassEarFrom       = 0.86;  // of the height: the ear starts
const float GrassEarNeck       = 0.78;  // of the height: the straight straw ends, the neck starts to bend
const float GrassEarDroopFrom  = 0.70;  // the direction turns over from here to the tip
const float GrassEarBody       = 0.72;  // of the ear's length are the grains, the rest is the awns' tuft

// The tip's tone of a stalk's pixel at `t` of its height (grass.frag)
vec3 grassEarTip(float t, float dry) {
  return mix(GrassEarStalk, mix(GrassEarGold, GrassEarPale, dry), smoothstep(GrassEarFrom, GrassEarFrom+0.04, t));
  }

#if defined(GRASS_EAR_PIXEL)
// An awn through the pixel, of the family that starts every `pitch` cm up the ear between `from` and `to`, x0 cm
// off the axis, and goes up and out by the tangent tanA. x: cm across, y: cm from the ear's start, len: cm
bool grassEarAwn(float x, float y, float x0, float tanA, float pitch, float from, float to, vec2 len, float seed) {
  const float ax    = abs(x);
  const float shift = x<0.0 ? 0.5 : 0.0;
  const float at    = (y - (ax - x0)/tanA)/pitch + shift;
  const float index = floor(at + 0.5);
  const float start = (index - shift)*pitch;
  const float own   = fract(sin(index*12.9898 + seed*78.233 + tanA*311.7)*43758.5453);
  const float off   = abs(at - index)*pitch*tanA/sqrt(1.0 + tanA*tanA); // cm across to the awn's line
  return ax>=x0 && start>from && start<to && y>start && y-start<mix(len.x, len.y, own) &&
         off<max(0.04, 0.4*fwidth(x));
  }

// The ear seen close (grass.frag). e: cm across from the ear's axis, the share of the ear's length, how close
// (0 the plain strip .. 1 the card whole), the ear's length in cm. False where the card is empty; shade: the tone
// of the grain or of the awn there
bool grassEarCard(vec4 e, float seed, out float shade) {
  shade = 1.0;
  const float close = smoothstep(0.0, 0.6, e.z);
  if(close<=0.0)
    return true;
  const float ax   = abs(e.x);
  if(e.y<=0.0) // the neck under the ear: the card widens there, the straw does not
    return ax<0.5*GrassEarStalkWidth + 0.05 + (1.0-close)*2.0;
  const float y    = e.y*e.w;                       // cm from the ear's start
  const float body = e.w*GrassEarBody;
  const float u    = clamp(y/body, 0.0, 1.0);
  // the grains: a spindle, two rows of spikelets as a herringbone that opens upwards
  const float hw    = y<body ? 0.62*smoothstep(0.0, 0.14, u)*(1.0 - 0.85*smoothstep(0.55, 1.0, u)) + 0.08 : 0.0;
  const float row   = fract(y/0.85 - ax*0.9 + (e.x<0.0 ? 0.5 : 0.0));
  const float grain = smoothstep(0.0, 0.2, row)*(1.0 - smoothstep(0.8, 1.0, row));
  if(ax<hw + (1.0-close)*2.0) {
    shade = mix(1.0, mix(0.74, 1.0, grain), close*step(ax, hw));
    return true;
    }
  // the awns: from every other spikelet, up and out by some 12 degrees, so that they stay apart at a few metres;
  // the upper spikelets' stand nearly along the ear and make the tuft over it
  if(grassEarAwn(e.x, y, 0.25, 0.22, 1.70, 0.06*body, body, e.w*vec2(0.26, 0.42), seed) ||
     grassEarAwn(e.x, y, 0.03, 0.10, 1.60, 0.45*body, body, e.w*vec2(0.24, 0.40), seed)) {
    shade = 1.12;
    return true;
    }
  return false;
  }
#endif

#if defined(GRASS_EAR_SHAPE)
// The share of the height at node k of n links. Four: the neck, the ear's start, its full width (`top`:
// GrassEarTopFar..GrassEarTopNear by the distance), the tip; three: no neck apart
float grassEarAt(int k, int n, float top) {
  if(k<=0)
    return 0.0;
  if(k>=n)
    return 1.0;
  if(n==2)
    return GrassEarFrom;
  if(n==3)
    return k==1 ? 0.84 : 0.92;
  return k==1 ? GrassEarNeck : (k==2 ? GrassEarFrom : top);
  }

// The stalk's direction at t of its height. fwd: the side it leans to; droop: the tangent the ear hangs over by.
// The straw is a rod: it has the lean of a blade's middle all along, and turns over under the ear
vec3 grassEarDir(Tilt tilt, vec2 fwd, float droop, float t) {
  tilt.a += tilt.b*0.5 + fwd*(droop*clamp((t-GrassEarDroopFrom)/(1.0-GrassEarDroopFrom), 0.0, 1.0));
  tilt.b  = vec2(0);
  return bladeDir(tilt, t);
  }

// The node k of n from the root: the links keep their lengths
vec3 grassEarPoint(Tilt tilt, vec2 fwd, float droop, int k, int n, float top, float height) {
  vec3  p  = vec3(0);
  float t0 = 0.0;
  for(int i=1; i<=k; ++i) {
    const float t1 = grassEarAt(i, n, top);
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

// vertex: of the strip's 2n+1, two at each node and a single one last. widths: of the stalk, of the ear where it
// starts and of the ear, cm. With one link the strip is a wedge as wide as the ear at the top: the pair is up, the
// single one at the root
GrassEarVertex grassEarVertex(int vertex, int n, float top, vec3 widths) {
  GrassEarVertex r;
  const bool single = (vertex >> 1)>=n;
  r.k     = n==1 ? (single ? 0 : 1) : min(vertex >> 1, n);
  r.t     = grassEarAt(r.k, n, top);
  r.side  = single ? 0.0 : ((vertex & 1)==0 ? -0.5 : 0.5);
  r.width = r.t>GrassEarFrom+0.01 ? widths.z : (r.t>GrassEarFrom-0.01 && n==4 ? widths.y : widths.x);
  return r;
  }

// The strip's width at a node: across the view, round the stalk. toEye: from the node; level: the width of a blade
// of grass, for a stalk seen along itself
vec3 grassEarAxis(vec3 dir, vec3 toEye, vec3 level) {
  return normalize(cross(dir, normalize(toEye)) + level*1e-3);
  }
#endif

#endif
