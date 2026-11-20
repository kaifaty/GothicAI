#ifndef GRASS_BLADE_GLSL
#define GRASS_BLADE_GLSL

#include "grass/grass_common.glsl"
#include "grass/grass_ear.glsl"

vec3 grassBladeRoot(uvec4 b, vec3 origin) {
  return origin + vec3(float(b.x & 0x3FFFu), float((b.y & 0xFFFFu)*2u), float((b.x >> 14) & 0x3FFFu))*(1.0/16.0);
  }

float grassBladeRank(uvec4 b) {
  return (float(b.y >> 24) + 0.5)/256.0;
  }

vec4 grassBladeRing(vec4 shape, vec4 sceneRing) {
  return shape.x>0.0 ? vec4(shape.z, shape.w, shape.w*1.25, sceneRing.w) : sceneRing;
  }

// Past the full ring a share full/d of the blades is left. A blade shrinks into the ground before it is dropped.
float grassBladeHeight(uvec4 b, bool ear, float rank, vec4 ring, float d) {
  const float height = float((b.y >> 16) & 0xFFu)*(ear ? GrassEarHeightStep : 0.25);
  const float grow = clamp((ring.x/max(d,1.0)*1.12 - rank)/0.12, 0.0, 1.0);
  return height*(grow*(1.0 - smoothstep(ring.y, ring.z, d)));
  }

bool grassBladeGrown(float height, vec4 ring, uint partsOff) {
  return !(height<=0.05 || ring.w<0.75 || (partsOff & GrassPartHide)!=0u);
  }

float grassBladeWidth(uvec4 b, vec4 shape) {
  const float width = float((b.w >> 24) & 0xFu);
  return shape.x>0.0 ? shape.x + width*shape.y : GrassWidth + width*GrassWidthStep;
  }

float grassBladeWiden(float d, float full) {
  return clamp(d/full, 1.0, 8.0);
  }

float grassBladeThin(float depth, float projectY, int screenHeight) {
  const float pixel = 2.0*max(depth,1.0)/(abs(projectY)*float(screenHeight));
  return 1.3*pixel;
  }

#endif
