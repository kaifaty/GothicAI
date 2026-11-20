#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "common.glsl"
#include "materials/foliage_wind.glsl"
#include "grass/grass_common.glsl"

// A blade of grass (reports/engine/2026-11-14-grass-system): a tapering strip of up to four links of one length,
// made here from 16 bytes of the cell's buffer (Grass::Blade, common/graphics/grass/grassplace.h) and the vertex's
// number. No vertex buffer, no meshlets: the same path with mesh shaders and without.

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };
layout(binding = 1, std430) readonly buffer Blades { uvec4 blade[]; };
// the ground material's painted texture, by the mesh's uv
layout(binding = 2) uniform sampler2D ground;
// of a ground with its own layers the sward's map, by the world's x and z (grass_layers.glsl); else the same texture
layout(binding = 3) uniform sampler2D sward;
#if defined(GRASS_GPU_CULL)
layout(binding = 4, std430) readonly buffer Compacted { uint compact[]; };
layout(binding = 5, std430) readonly buffer Windows { uvec2 windows[]; };
#else
// the frame's table of the draws' spans
layout(binding = 4, std430) readonly buffer Spans { uint span[]; };
#endif

layout(push_constant, std430) uniform Push {
  vec4  origin;       // cm: the corner of the cell less its margin, its lowest root in y
  float wind;         // the record's
  float translucency;
  uint  table;        // of the draw's spans, or the draw index with GRASS_GPU_CULL
  uint  segments;     // links of a blade in this cell: 4, 3, 2 or 1
  // The fine blades of the short sward (GrassField): x the width at the root, cm, y per step of the blade's bits;
  // z their own ring's full count to here, w into the ground from here to a quarter further. 0: a blade of the
  // scene's ring and widths
  vec4  shape;
  float swardK;       // repeats of the sward's map in a cm; 0: a ground without its own layers
  uint  pack;         // blades per instance
  uint  kind;         // GrassKindEar: stalks with an ear (grass_ear.glsl); 0: blades of grass
  } push;

layout(location = 0) out vec4 outColor;      // the painted ground under the root; w: 1 on a ground with its own layers,
                                             // 2 a stalk with an ear
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outShade;      // the share of the blade's length, of the sun through, the dryness, the sheen
layout(location = 3) out vec3 outPrevStatic; // as Varyings of materials_common.glsl
layout(location = 4) out vec3 outPrevDelta;
layout(location = 5) out vec3 outRoot;       // on a ground with its own layers: the sward under the root
layout(location = 6) out vec4 outEar;        // a stalk's ear (grassEarCard): cm across, the share of its length, how close, its length

out gl_PerVertex {
  vec4 gl_Position;
  };

// The tilt of the blade off the vertical grows along it: tilt(t) = a + b*t, in the world's x and z
struct Tilt {
  vec2 a;
  vec2 b;
  vec2 lean; // of b: the wind's lean alone
  };

// loc: the lean's pressure, the swing's share and the flutter's pressure of the place (foliageFrontAt)
Tilt bladeTilt(vec2 fwd, float lean0, float height, float tuft, float own, float still,
               vec4 wind, vec4 phase, vec4 swingOf, vec3 loc, vec3 root) {
  Tilt r;
  const float rest = tan(lean0);
  r.a    = fwd*(rest*0.35);
  r.b    = fwd*(rest*0.9);
  r.lean = vec2(0);
  if(wind.w<=0.0 || push.wind<=0.0)
    return r;
  const vec2 dir = wind.xy, across = vec2(-wind.y, wind.x);

  // the lean: the place's pressure on a rod of this length, and the two waves as a ripple on it
  const float w1 = sin(6.2831853*(fract(root.z/FoliageWave1Length) - phase.x));
  const float w2 = sin(6.2831853*(fract(dot(root.xz,FoliageWave2Dir)/FoliageWave2Length) - phase.y));
  const float g  = FoliageWave1Share*w1 + FoliageWave2Share*w2;
  const float sw = min(push.wind*loc.y*GrassSwingK, FoliageSwingMaxTan)*2.0*still;
  const float tl = min(2.0*tan(min(grassLeanAngle(height)*loc.x*push.wind*(FoliageWaveSteady + FoliageWaveSwing*g), 1.2)),
                       GrassLeanMaxTan - sw);
  r.lean = dir*max(tl,0.0);
  r.b   += r.lean;

  // the swing: the blade's own about its lean, by its length; a tuft swings as one body, each blade a little late
  if(sw>0.0) {
    const float len = height<16.0 ? 12.0 : (height<32.0 ? 20.0 : 40.0);
    const float n1  = floor(FoliageSwingRate/sqrt(len)*(1.0 + FoliageSwingSpread*(fract(tuft*2.236068)-0.5)) + 0.5);
    const float n2  = n1 + FoliageSwingBeatMin + floor(fract(tuft*3.302776)*FoliageSwingBeatVar);
    const float a1  = 6.2831853*(fract(n1*swingOf.x) + fract(tuft*1.618034)) + own*0.9;
    const float a2  = 6.2831853*(fract(n2*swingOf.x) + tuft)                 - own*0.7;
    r.b += dir   *(sw*(FoliageSwing1Share*sin(a1) + FoliageSwing2Share*sin(a2)));
    r.b += across*(sw*FoliageSwingAcross*(FoliageSwing1Share*cos(a1) - FoliageSwing2Share*cos(a2)));
    }
  // the flutter: small and quick, each blade in its own step
  const float amp = 2.0*still*push.wind*(FoliageFlutterCalm + FoliageFlutterWind*min(loc.z,FoliageFlutterMaxQ));
  r.b += dir   *(amp*0.8*sin(6.2831853*(phase.z + tuft) + own*6.0));
  r.b += across*(amp*0.6*sin(6.2831853*(phase.w + fract(tuft*1.618034)) + own*7.8 + 1.7));
  return r;
  }

// The link's direction at t of the blade's length
vec3 bladeDir(Tilt tilt, float t) {
  vec2 v = tilt.a + tilt.b*t;
  const float l = length(v);
  if(l>2.4)
    v *= 2.4/l;
  return normalize(vec3(v.x, 1.0, v.y));
  }

// The point at link k of n: the links keep their length, so does the blade
vec3 bladePoint(Tilt tilt, int k, int n, float height) {
  vec3 p = vec3(0);
  const float step = 1.0/float(n);
  for(int i=0; i<k; ++i)
    p += bladeDir(tilt, (float(i)+0.5)*step);
  return p*(height*step);
  }

#define GRASS_EAR_SHAPE 1
#include "grass/grass_ear.glsl"
#include "grass/grass_blade.glsl"

void hideBlade() {
  gl_Position   = vec4(uintBitsToFloat(0x7fc00000));
  outColor      = vec4(0);
  outNormal     = vec3(0,1,0);
  outShade      = vec4(0);
  outPrevStatic = vec3(0,0,1);
  outPrevDelta  = vec3(0);
  outRoot       = vec3(0);
  outEar        = vec4(0);
  }

void main() {
  const uint inst     = uint(gl_InstanceIndex);
  const bool isPacked = push.pack!=1u;
  const int n = int(push.segments);
  int vertex = gl_VertexIndex;
  uint bladeIndex;
#if defined(GRASS_GPU_CULL)
  const uvec2 window = windows[push.table];
  uint at = inst*push.pack;
  if(isPacked) {
    const int verts = 2*n+1;
    const int copy  = vertex/verts;
    vertex -= copy*verts;
    at += uint(copy);
    if(at>=window.y) {
      hideBlade();
      return;
      }
    }
  bladeIndex = compact[window.x+at];
#else
  // Each of the draw's 16 spans stores the exclusive instance end and the blade offset.
  // Packed spans also store the exclusive blade end, so the last pack cannot read past it.
  const uint stride   = isPacked ? 3u : 2u;
  uint       at       = push.table;
  if(inst>=span[at + stride*7u]) at += stride*8u;
  if(inst>=span[at + stride*3u]) at += stride*4u;
  if(inst>=span[at + stride])    at += stride*2u;
  if(inst>=span[at])             at += stride;

  const uint bladeOffset = span[at+1u];
  bladeIndex = inst + bladeOffset;
  if(isPacked) {
    const int verts = 2*n+1;
    const int copy  = vertex/verts;
    vertex -= copy*verts;
    bladeIndex = inst*push.pack + uint(copy) + bladeOffset;
    if(bladeIndex>=span[at+2u]) {
      hideBlade();
      return;
      }
    }
#endif
  const uvec4 b = blade[bladeIndex];

  const vec3  root   = grassBladeRoot(b, push.origin.xyz);
  const float lean0  = radians(8.0 + float(b.x >> 28)*(40.0/15.0));
  const float rank   = grassBladeRank(b);
  const float facing = float(b.z & 0xFFu)*(6.2831853/256.0);
  const float tuft   = float((b.z >> 8) & 0xFFu)/256.0;
  const vec2  gxz    = vec2(float((b.z >> 16) & 0xFFu) - 128.0, float(b.z >> 24) - 128.0)/127.0;
  const vec3  gn     = vec3(gxz.x, sqrt(max(1.0 - dot(gxz,gxz), 0.04)), gxz.y);
  const vec2  uv     = vec2(float(b.w & 0xFFFu), float((b.w >> 12) & 0xFFFu))/4096.0;
  const float dry    = float(b.w >> 28)/15.0;
  const bool  ear    = push.kind==GrassKindEar;

  const vec4  ring = grassBladeRing(push.shape, scene.grassRing);
  const float d    = distance(root, scene.camPos);
  const float height = grassBladeHeight(b, ear, rank, ring, d);
  const uint  partsOff = grassPartsOff(ring.w);
#if !defined(GRASS_GPU_CULL)
  if(!grassBladeGrown(height, ring, partsOff)) {
    hideBlade();
    return;
    }
#endif
  const float depth = (scene.viewProject*vec4(root,1.0)).w;
  const float widen = grassBladeWiden(d, ring.x), thin = grassBladeThin(depth, scene.project[1][1], scene.screenRes.y);
  const float width = max(grassBladeWidth(b, push.shape)*widen, thin);

  // the vertex: two at each link, one at the tip
  int   k    = min(vertex >> 1, n);
  float t    = float(k)/float(n);
  float side = k==n ? 0.0 : ((vertex & 1)==0 ? -0.5 : 0.5);

  // the strip's width is never edge-on to the eye
  const vec2  fwd   = vec2(cos(facing), sin(facing));
  const vec2  eye   = normalize(scene.camPos.xz - root.xz + vec2(1e-3,0.0));
  const vec2  perp  = vec2(-eye.y, eye.x);
  const vec2  s0    = vec2(-fwd.y, fwd.x);
  const float sp    = dot(s0,perp);
  const vec2  s1    = normalize(eye*(dot(s0,eye)*0.3) + perp*((sp>=0.0 ? 1.0 : -1.0)*max(abs(sp),0.35)));
  vec3        sideV = vec3(s1.x, 0.0, s1.y);

  const float still = 1.0 - smoothstep(GrassStillFrom, GrassStillTo, d);
  const vec3  loc   = foliageFrontAt(root.z, scene.foliageFront.xy, scene.foliageFrontK, scene.foliageWind, scene.foliageSwing);
  const Tilt  tilt  = bladeTilt(fwd, lean0, height, tuft, rank, still, scene.foliageWind, scene.foliagePhase,
                                scene.foliageSwing, loc, root);
  // a stalk with an ear: its own nodes and widths, the ear hangs over by the blade's three bits
  const float droop = mix(GrassEarDroopMin, GrassEarDroopMax, float((b.w >> 24) & 0xFu)/7.0);
  vec3        dir   = bladeDir(tilt, t);
  vec3        stem  = bladePoint(tilt, k, n, height);
  float       across= width*(1.0 - GrassTaper*t); // cm: the strip at the vertex
  // seen close a stalk of four links has its ear as a card (grassEarCard)
  const float close = ear && n==4 ? 1.0 - smoothstep(GrassEarNearFrom, GrassEarNearTo, d) : 0.0;
  const float top   = mix(GrassEarTopFar, GrassEarTopNear, close);
  outEar = vec4(0);
  if(ear) {
    const vec3 widths = vec3(GrassEarStalkWidth, mix(GrassEarStalkWidth, GrassEarCardBase, close),
                             mix(GrassEarWidth, GrassEarCardWidth, close));
    const GrassEarVertex e = grassEarVertex(vertex, n, top, max(widths*widen, vec3(thin)));
    k     = e.k;
    t     = e.t;
    side  = e.side;
    dir   = grassEarDir(tilt, fwd, droop, t);
    stem  = grassEarPoint(tilt, fwd, droop, k, n, top, height);
    across= e.width;
    sideV = grassEarAxis(dir, scene.camPos - (root+stem), sideV);
    outEar = vec4(side*across, (t-GrassEarFrom)/(1.0-GrassEarFrom), close, height*(1.0-GrassEarFrom));
    }
  const vec3  off   = sideV*(side*across);
  const vec3  pos   = root + stem + off;

  const vec4 clip = scene.viewProject*vec4(pos,1.0);
  gl_Position = clip;
  if(scene.motionVectors!=0u) {
    // where the previous frame's wind had the vertex; what depends on the camera is this frame's in both
    const vec3 locP  = foliageFrontAt(root.z, scene.foliageFront.zw, scene.foliageFrontK, scene.foliageWindPrev,
                                      scene.foliageSwingPrev);
    const Tilt tiltP = bladeTilt(fwd, lean0, height, tuft, rank, still, scene.foliageWindPrev, scene.foliagePhasePrev,
                                 scene.foliageSwingPrev, locP, root);
    const vec3 prev  = root + (ear ? grassEarPoint(tiltP, fwd, droop, k, n, top, height) : bladePoint(tiltP, k, n, height)) + off;
    outPrevStatic = (scene.reprojectClip*clip).xyw;
    outPrevDelta  = (scene.viewProjectPrev*vec4(prev - pos,0.0)).xyw;
    } else {
    outPrevStatic = vec3(0,0,1);
    outPrevDelta  = vec3(0);
    }

  // The colour of the painted ground under the root, from a coarse level of its texture: the strokes of the
  // painted grass pass into the blades as patches. The pixel makes the blade's tone of it (grass.frag). On a ground
  // with its own layers the painted texture is not drawn, but the blades keep its colours: the root alone is of
  // the sward it stands in
  const bool  own  = push.swardK>0.0 && (partsOff & GrassPartLayers)==0u;
  const float lean = length(tilt.lean);
  outColor = vec4(textureLod(ground, uv, max(float(textureQueryLevels(ground)) - 6.0, 0.0)).rgb, ear ? 2.0 : (own ? 1.0 : 0.0));
  outRoot  = own ? textureLod(sward, root.xz*push.swardK, max(float(textureQueryLevels(sward)) - 6.0, 0.0)).rgb : outColor.rgb;
  outShade = vec4(t, push.translucency, dry, GrassLeanSheen*min(lean/(0.5*GrassLeanMaxTan), 1.0));

  // Three quarters the ground's normal, a quarter the blade's own, rounded across it; the lean turns it against
  // the wind, as the faces of the blades that lie over turn up: grass under a gust differs in tone from grass
  // that stands, also where the lean is under a pixel
  vec3 face = cross(sideV, dir);
  if(face.y<0.0)
    face = -face;
  face = normalize(face + sideV*(side*1.2));
  outNormal = normalize(gn*0.75 + face*0.25 - vec3(tilt.lean.x, 0.0, tilt.lean.y)*(t*GrassNormalLean*0.5));
  if((partsOff & GrassPartNormal)!=0u)
    outNormal = gn;
  // the vertex stage's cost by difference: everything above is done, and the vertex leaves by the right plane
  if((partsOff & GrassPartOffscreen)!=0u)
    gl_Position = vec4(4.0*(abs(clip.w)+1.0), clip.y, clip.z, abs(clip.w)+1.0);
  }
