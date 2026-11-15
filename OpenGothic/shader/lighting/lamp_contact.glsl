#ifndef LAMP_CONTACT_GLSL
#define LAMP_CONTACT_GLSL

// The screen ray of a lamp (reports/render-features/2026-11-15-night-interiors, "Шаг 4"): a lamp's cube face has
// texels of half a centimetre and more and a soft filter over them, so what is a few centimetres deep casts no
// shadow of its own - a nose on a cheek, a chin on a neck, a collar on a chest. A short ray from the lit point
// towards the lamp through the depth buffer finds them. It only darkens the light of its own lamp, only where the
// lamp's shadow map said "lit", and what it cannot see (behind the screen's edge, behind another thing) stays lit.
// Needs scene.glsl and common.glsl. [INTERNAL] lampContact=0 or OPENGOTHIC_LAMP_CONTACT=0 turn it off (VF_LAMP_CONTACT);
// bench `vsm study 32` shows the frame without it.

const float LampContactLength = 20.0; // cm: how far towards the lamp a thing still shadows the point
const int   LampContactSteps  = 12;   // at most; a ray that is short on the screen takes fewer, one per StepPx
const float LampContactStepPx = 4.0;
// a lamp's light under this share of white before the tonemapper shows no small shadow: no ray for it. Most of a
// lamp's pixels are such, and a room of many lamps pays for every one of them
const float LampContactFaint  = 0.03;
// cm: a surface this much nearer to the eye than the ray is still the thing in its way; nearer than that it is
// something else in front of both (a weapon before the camera, an arm before a far wall)
const float LampContactThick  = 10.0;
const float LampContactMinPx  = 12.0; // a ray shorter than this on the screen sees nothing the shadow map does not
// the ray leaves the surface along its plane's normal by this, cm, and by a share of the distance to the eye: the
// depth of the pixel is not the depth of its neighbours
const float LampContactLift   = 0.3;
const float LampContactLiftK  = 0.0015;

// clip: the point in clip space, (ndc, 1)*w; dir: the unit direction to the lamp in the world; dist: the distance to
// the lamp, cm; planeN: the surface's plane; view: the unit direction to the eye; noise: 0..1 of this pixel and
// frame. Returns the light left, 0..1
float lampContact(in texture2D depthTex, const vec4 clip, const vec3 dir, const float dist, const vec3 planeN,
                  const vec3 view, const float noise) {
  // a lamp's own fixture casts no shadow (scene.vsmLampNear): the ray stops before it
  const float len = min(LampContactLength, dist - scene.vsmLampNear);
  if(len<=1.0)
    return 1.0;

  const float lift = LampContactLift + LampContactLiftK*clip.w;
  const vec4  c0   = clip + scene.viewProject*vec4(planeN*lift, 0.0);
  const vec4  dc   = scene.viewProject*vec4(dir*len, 0.0);
  const vec4  c1   = c0 + dc;
  if(c0.w<=0.0 || c1.w<=0.0)
    return 1.0;

  const vec2  size = vec2(textureSize(depthTex, 0));
  const vec2  px0  = (c0.xy/c0.w*0.5 + 0.5)*size;
  const vec2  px1  = (c1.xy/c1.w*0.5 + 0.5)*size;
  const float pxLen = length(px1-px0);
  if(pxLen < LampContactMinPx)
    return 1.0;

  // A facet that stands edge-on or with its back to the lamp while its smooth normal still takes the light: the
  // ray runs under the facet's own plane, and the facet would shadow itself whole, a low mesh's facets drawn in
  // shadow. What is as near to the eye as that plane is the surface itself, not a thing in the ray's way
  const float rise  = dot(dir, planeN)*len;         // the ray's height over the plane at its end, cm
  const float slant = 1.0/max(dot(view, planeN), 0.15);
  const int   steps = clamp(int(pxLen/LampContactStepPx), 4, LampContactSteps);
  const ivec2 self  = ivec2(px0);
  for(int i=0; i<steps; ++i) {
    // denser near the point: the thing that touches it is the one the shadow map misses
    const float s = (float(i) + noise)/float(steps);
    const float t = s*sqrt(s);
    const vec4  c = c0 + dc*t;
    const vec3  n = c.xyz/c.w;
    if(any(greaterThanEqual(abs(n.xy), vec2(1.0))))
      break;
    const ivec2 at = ivec2((n.xy*0.5 + 0.5)*size);
    if(at==self)
      continue;
    const float ray   = linearDepth(n.z, scene.clipInfo);
    const float there = linearDepth(texelFetch(depthTex, at, 0).x, scene.clipInfo);
    const float own   = max(0.0, -(lift + rise*t))*slant;
    const float under = ray - there - own;
    if(under > 0.2 + 0.002*ray && under < LampContactThick) {
      // towards its end the ray hands over to the shadow map
      return smoothstep(0.75, 1.0, t);
      }
    }
  return 1.0;
  }

#endif
