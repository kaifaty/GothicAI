#ifndef HAIR_BRDF_GLSL
#define HAIR_BRDF_GLSL

#include "common.glsl"

// Hair lit as fibres, not as a surface (reports/content/2026-11-npc-hair): Marschner's three paths of light through
// a hair in the form Karis fitted for Unreal (Physically Based Hair Shading in Unreal, SIGGRAPH 2016):
//   R   - reflected off the cuticle: the white highlight, shifted towards the root by the tilt of the scales;
//   TT  - through the hair: the glow of an edge lit from behind, in the hair's colour;
//   TRT - into the hair, off its back wall and out: the second highlight, coloured, shifted towards the tip;
// and a wrapped diffuse term for the light scattered between hairs.
//
// The albedo of our hair cards is the colour the hair is seen in, not the pigment: the scatter term returns it as
// it is, and the paths through the hair take its square as the pigment (Karis' scatter is sqrt of the base colour).
//
// One set of numbers for every light: the look of dull, unwashed hair
const float HairRoughness = 0.52; // the width of the highlights: sqr() is the longitudinal deviation, radians
const float HairShift     = 0.035; // the tilt of the cuticle, radians; R sits at -2x, TT at +1x, TRT at +4x
const float HairShiftVary = 0.035; // how far a strand's own tilt goes either way, by its number
const float HairScatter   = 2.0;   // the scattered light against Karis' unit: head-on it matches a Lambert surface

float hairLobe(float b, float theta) {
  return exp(-0.5*theta*theta/(b*b)) / (sqrt(2.0*M_PI)*b);
  }

float hairFresnel(float cosTheta) {
  const float n  = 1.55;
  const float f0 = ((1.0-n)/(1.0+n))*((1.0-n)/(1.0+n));
  const float f  = 1.0 - cosTheta;
  const float f2 = f*f;
  return f0 + (1.0-f0)*f2*f2*f;
  }

// albedo - linear; strand - the hair's direction, root to tip; view, light - towards the eye and the light;
// strandId - 0..1, the strand's number from the hair map; backlit - how much of the light from behind gets through.
// Returns the radiance per unit of the light's irradiance: the cosine is inside
vec3 hairBrdf(vec3 albedo, vec3 strand, vec3 view, vec3 light, float strandId, float backlit) {
  const vec3  n         = -strand; // Karis' frame: along the hair, towards the root
  const vec3  pigment   = albedo*albedo;
  const float voL       = dot(view, light);
  const float sinThetaL = clamp(dot(n, light), -1.0, 1.0);
  const float sinThetaV = clamp(dot(n, view),  -1.0, 1.0);
  const float cosThetaD = cos(0.5*abs(asin(sinThetaV) - asin(sinThetaL)));

  const vec3  lp         = light - sinThetaL*n;
  const vec3  vp         = view  - sinThetaV*n;
  const float cosPhi     = dot(lp,vp) * inversesqrt(dot(lp,lp)*dot(vp,vp) + 1e-4);
  const float cosHalfPhi = sqrt(clamp(0.5 + 0.5*cosPhi, 0.0, 1.0));

  const float nPrime = 1.19/cosThetaD + 0.36*cosThetaD;
  const float shift  = HairShift + (strandId - 0.5)*2.0*HairShiftVary;
  const float r2     = HairRoughness*HairRoughness;

  vec3 s = vec3(0);
  { // R
    const float alpha = -2.0*shift;
    const float sa = sin(alpha), ca = cos(alpha);
    const float sh = 2.0*sa*(ca*cosHalfPhi*sqrt(max(1.0 - sinThetaV*sinThetaV, 0.0)) + sa*sinThetaV);
    const float mp = hairLobe(r2*sqrt(2.0)*max(cosHalfPhi, 1e-3), sinThetaL + sinThetaV - sh);
    const float np = 0.25*cosHalfPhi;
    const float fp = hairFresnel(sqrt(clamp(0.5 + 0.5*voL, 0.0, 1.0)));
    s += vec3(mp*np*fp * mix(1.0, backlit, clamp(-voL, 0.0, 1.0)));
  }
  { // TT
    const float mp = hairLobe(r2*0.5, sinThetaL + sinThetaV - shift);
    const float a  = 1.0/nPrime;
    const float h  = cosHalfPhi*(1.0 + a*(0.6 - 0.8*cosPhi));
    const float f  = hairFresnel(cosThetaD*sqrt(clamp(1.0 - h*h, 0.0, 1.0)));
    const float fp = (1.0-f)*(1.0-f);
    const vec3  tp = pow(pigment, vec3(0.5*sqrt(max(1.0 - h*h*a*a, 0.0))/cosThetaD));
    const float np = exp(-3.65*cosPhi - 3.98);
    s += mp*np*fp*tp*backlit;
  }
  { // TRT
    const float mp = hairLobe(r2*2.0, sinThetaL + sinThetaV - 4.0*shift);
    const float f  = hairFresnel(cosThetaD*0.5);
    const float fp = (1.0-f)*(1.0-f)*f;
    const vec3  tp = pow(pigment, vec3(0.8/cosThetaD));
    const float np = exp(17.0*cosPhi - 16.78);
    s += mp*np*fp*tp;
  }
  { // light scattered between the hairs: Kajiya-Kay's diffuse, softened, round the fibre as the eye sees it
    const float kajiya = 1.0 - abs(sinThetaL);
    const vec3  facing = vp * inversesqrt(max(dot(vp,vp), 1e-6));
    const float wrap   = clamp((dot(facing, light) + 1.0)*0.25, 0.0, 1.0);
    s += albedo * (Fd_Lambert*HairScatter*mix(wrap, kajiya, 0.33));
  }
  return max(s, vec3(0));
  }

// How much of a light reaches hair whose surface normal makes this cosine with it: the far side of a head of hair
// is in the head's own shade even where a shadow map is too coarse to tell, the rim still catches the light
float hairShell(float noL) {
  return clamp(noL*0.5 + 0.5, 0.0, 1.0);
  }

// the direction of the strands as a colour, for OPENGOTHIC_HAIR_DEBUG
vec3 hairDebugColor(vec3 strand) {
  return strand*0.5 + 0.5;
  }

#endif
