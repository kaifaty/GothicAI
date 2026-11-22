#ifndef FOLIAGE_VISIBILITY_GLSL
#define FOLIAGE_VISIBILITY_GLSL

// Main-view bounds shared by grass blades and nodes of a tree crown. A box crossing the near
// plane is kept; only a box covered in full by current-frame depth can be rejected.
const float FoliageCullEpsilon = 1.1920928955078125e-7;

bool foliageProjectBox(mat4 project, vec3 lo, vec3 hi, vec4 clipPad, float znear,
                       out vec4 rect, out float nearDepth) {
  rect = vec4(2.0, 2.0, -2.0, -2.0);
  nearDepth = 2.0;
  for(uint corner=0u; corner<8u; ++corner) {
    const vec3 p = mix(lo, hi, notEqual(uvec3(corner) & uvec3(1,2,4), uvec3(0)));
    const vec4 clip = project*vec4(p,1.0);
    const vec4 clipLo = clip - clipPad, clipHi = clip + clipPad;
    if(clipLo.w<=znear || clipLo.z<=0.0)
      return false;
    const vec3 low = min(clipLo.xyz/clipLo.w, clipLo.xyz/clipHi.w);
    const vec2 high = max(clipHi.xy/clipLo.w, clipHi.xy/clipHi.w);
    rect = vec4(min(rect.xy, low.xy), max(rect.zw, high));
    nearDepth = min(nearDepth, low.z);
    }

  return true;
  }

bool foliageOutsideView(vec4 rect, float nearDepth) {
  return any(lessThan(rect.zw, vec2(-1))) || any(greaterThan(rect.xy, vec2(1))) || nearDepth>1.0;
  }

ivec2 foliageHiZTileSize(bool rawDepth) {
  if(!rawDepth)
    return scene.hiZTileSize;
  const ivec2 power = findMSB(uvec2(scene.screenRes - 1)) + 1;
  return (ivec2(1) << power) / textureSize(hiZ, 0);
  }

bool foliageHiZVisible(vec4 rect, float depth, bool rawDepth) {
  // The R16 pyramid packs depth below 0.95 into the same bin. Keep bounds touching that zone.
  if(depth<=0.95)
    return true;

  // Same precise mip selection and max-depth test as for clusters. Include a pixel for raster rounding.
  const vec4 pixels = (rect*0.5 + 0.5)*vec2(scene.screenRes).xyxy;
  const vec2 last = vec2(scene.screenRes - 1);
  const ivec2 tile = foliageHiZTileSize(rawDepth);
  ivec2 lo = ivec2(clamp(pixels.xy - 1.0, vec2(0), last)) / tile;
  ivec2 hi = ivec2(clamp(pixels.zw + 1.0, vec2(0), last)) / tile;
  const ivec2 span = hi - lo;
  int mip = findMSB(max(1, max(span.x, span.y)));
  lo >>= mip;
  hi >>= mip;
  if(any(greaterThan(hi - lo, ivec2(1)))) {
    ++mip;
    lo >>= 1;
    hi >>= 1;
    }

  float furthest = texelFetch(hiZ, lo, mip).x;
  if(lo.x!=hi.x)
    furthest = max(furthest, texelFetch(hiZ, ivec2(hi.x, lo.y), mip).x);
  if(lo.y!=hi.y)
    furthest = max(furthest, texelFetch(hiZ, ivec2(lo.x, hi.y), mip).x);
  if(lo.x!=hi.x && lo.y!=hi.y)
    furthest = max(furthest, texelFetch(hiZ, hi, mip).x);
  const float nearest = min(rawDepth ? depth : packHiZ(depth), 1.0);
  return !(nearest>furthest);
  }

#endif
