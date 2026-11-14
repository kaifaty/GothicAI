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
layout(binding = 2) uniform sampler2D ground; // the ground material's own texture: the colour under the root

layout(push_constant, std430) uniform Push {
  vec4  origin;       // cm: the corner of the cell less its margin, its lowest root in y
  float wind;         // the record's
  float translucency;
  uint  first;        // the batch's first blade in the buffer
  uint  segments;     // links of a blade in this cell: 4, 3, 2 or 1
  } push;

layout(location = 0) out vec3 outColor;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outShade;      // AO, the share of the sun through
layout(location = 3) out vec3 outPrevStatic; // as Varyings of materials_common.glsl
layout(location = 4) out vec3 outPrevDelta;

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

void main() {
  const uvec4 b = blade[push.first + uint(gl_InstanceIndex)];

  const vec3  root   = push.origin.xyz + vec3(float(b.x & 0x3FFFu), float((b.y & 0xFFFFu)*2u), float((b.x >> 14) & 0x3FFFu))*(1.0/16.0);
  const float lean0  = radians(8.0 + float(b.x >> 28)*(40.0/15.0));
  const float rank   = (float(b.y >> 24) + 0.5)/256.0;
  const float facing = float(b.z & 0xFFu)*(6.2831853/256.0);
  const float tuft   = float((b.z >> 8) & 0xFFu)/256.0;
  const vec2  gxz    = vec2(float((b.z >> 16) & 0xFFu) - 128.0, float(b.z >> 24) - 128.0)/127.0;
  const vec3  gn     = vec3(gxz.x, sqrt(max(1.0 - dot(gxz,gxz), 0.04)), gxz.y);
  const vec2  uv     = vec2(float(b.w & 0xFFFu), float((b.w >> 12) & 0xFFFu))/4096.0;
  const float dry    = float(b.w >> 28)/15.0;
  float       width  = 0.8 + float((b.w >> 24) & 0xFu)*0.08;
  float       height = float((b.y >> 16) & 0xFFu)*0.25;

  // The ring: past `full` a share full/d of the blades is left, each d/full wider; a blade of a rank over the
  // share has shrunk into the ground before it is dropped, so nothing pops while walking
  const vec4  ring = scene.grassRing;
  const float d    = distance(root, scene.camPos);
  const float grow = clamp((ring.x/max(d,1.0)*1.12 - rank)/0.12, 0.0, 1.0);
  height *= grow*(1.0 - smoothstep(ring.y, ring.z, d));
  if(height<=0.05 || ring.w<0.75) {
    gl_Position   = vec4(uintBitsToFloat(0x7fc00000));
    outColor      = vec3(0);
    outNormal     = vec3(0,1,0);
    outShade      = vec2(0);
    outPrevStatic = vec3(0,0,1);
    outPrevDelta  = vec3(0);
    return;
    }
  const float depth = (scene.viewProject*vec4(root,1.0)).w;
  const float pixel = 2.0*max(depth,1.0)/(abs(scene.project[1][1])*float(scene.screenRes.y));
  width = max(width*clamp(d/ring.x, 1.0, 8.0), 1.3*pixel);

  // the vertex: two at each link, one at the tip
  const int   n    = int(push.segments);
  const int   k    = min(gl_VertexIndex >> 1, n);
  const float t    = float(k)/float(n);
  const float side = k==n ? 0.0 : ((gl_VertexIndex & 1)==0 ? -0.5 : 0.5);

  // the strip's width is never edge-on to the eye
  const vec2  fwd   = vec2(cos(facing), sin(facing));
  const vec2  eye   = normalize(scene.camPos.xz - root.xz + vec2(1e-3,0.0));
  const vec2  perp  = vec2(-eye.y, eye.x);
  const vec2  s0    = vec2(-fwd.y, fwd.x);
  const float sp    = dot(s0,perp);
  const vec2  s1    = normalize(eye*(dot(s0,eye)*0.3) + perp*((sp>=0.0 ? 1.0 : -1.0)*max(abs(sp),0.35)));
  const vec3  sideV = vec3(s1.x, 0.0, s1.y);

  const float still = 1.0 - smoothstep(GrassStillFrom, GrassStillTo, d);
  const vec3  loc   = foliageFrontAt(root.z, scene.foliageFront.xy, scene.foliageFrontK, scene.foliageWind, scene.foliageSwing);
  const Tilt  tilt  = bladeTilt(fwd, lean0, height, tuft, rank, still, scene.foliageWind, scene.foliagePhase,
                                scene.foliageSwing, loc, root);
  const vec3  off   = sideV*(side*width*(1.0 - 0.75*t));
  const vec3  pos   = root + bladePoint(tilt, k, n, height) + off;

  const vec4 clip = scene.viewProject*vec4(pos,1.0);
  gl_Position = clip;
  if(scene.motionVectors!=0u) {
    // where the previous frame's wind had the vertex; what depends on the camera is this frame's in both
    const vec3 locP  = foliageFrontAt(root.z, scene.foliageFront.zw, scene.foliageFrontK, scene.foliageWindPrev,
                                      scene.foliageSwingPrev);
    const Tilt tiltP = bladeTilt(fwd, lean0, height, tuft, rank, still, scene.foliageWindPrev, scene.foliagePhasePrev,
                                 scene.foliageSwingPrev, locP, root);
    const vec3 prev  = root + bladePoint(tiltP, k, n, height) + off;
    outPrevStatic = (scene.reprojectClip*clip).xyw;
    outPrevDelta  = (scene.viewProjectPrev*vec4(prev - pos,0.0)).xyw;
    } else {
    outPrevStatic = vec3(0,0,1);
    outPrevDelta  = vec3(0);
    }

  // The colour of the ground under the root, from a coarse level of its texture: the strokes of the painted grass
  // pass into the blades as patches. Darker at the root, lighter and greener or more like straw at the tip
  const float lod    = max(float(textureQueryLevels(ground)) - 6.0, 0.0);
  const vec3  earth  = textureLod(ground, uv, lod).rgb;
  const float lean   = length(tilt.lean);
  const float sheen  = 1.0 + GrassLeanSheen*min(lean/(0.5*GrassLeanMaxTan), 1.0)*t;
  outColor = min(earth*mix(vec3(GrassRootShade), mix(GrassTipGreen, GrassTipStraw, dry), t)*sheen, vec3(1.0));
  outShade = vec2(mix(GrassRootAo, 1.0, t), push.translucency);

  // Three quarters the ground's normal, a quarter the blade's own, rounded across it; the lean turns it against
  // the wind, as the faces of the blades that lie over turn up: grass under a gust differs in tone from grass
  // that stands, also where the lean is under a pixel
  vec3 face = cross(sideV, bladeDir(tilt, t));
  if(face.y<0.0)
    face = -face;
  face = normalize(face + sideV*(side*1.2));
  outNormal = normalize(gn*0.75 + face*0.25 - vec3(tilt.lean.x, 0.0, tilt.lean.y)*(t*GrassNormalLean*0.5));
  }
