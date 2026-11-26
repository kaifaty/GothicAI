#ifndef TREE_NANITE_OCCLUSION_GLSL
#define TREE_NANITE_OCCLUSION_GLSL

bool projectBox(ClipBox box, float depthRoundoff, out vec4 rect, out float nearDepth) {
  if(!finiteValues(vec4(box.low, 0.0)) || !finiteValues(vec4(box.high, 0.0)) ||
     any(greaterThan(box.low, box.high)))
    return false;

  rect = vec4(1.0, 1.0, -1.0, -1.0);
  nearDepth = 1.0;
  for(uint i = 0u; i < 8u; ++i) {
    const bvec3 corner = notEqual(uvec3(i) & uvec3(1u, 2u, 4u), uvec3(0u));
    const vec4 clip = box.project * vec4(mix(box.low, box.high, corner), 1.0);
    const vec4 low = clip - box.roundoff;
    const vec4 high = clip + box.roundoff;
    if(!finiteValues(low) || !finiteValues(high) || low.w <= frame.params.x || low.z <= 0.0)
      return false;
    const vec3 a = min(low.xyz / low.w, low.xyz / high.w);
    const vec3 b = max(high.xyz / low.w, high.xyz / high.w);
    rect = vec4(min(rect.xy, a.xy), max(rect.zw, b.xy));
    nearDepth = min(nearDepth, a.z);
    }

  nearDepth -= depthRoundoff + 8.0 * Epsilon;
  return nearDepth > 0.0 && finiteValues(rect);
  }

int powerOfTwoCeil(int size) {
  return 1 << (findMSB(uint(size - 1)) + 1);
  }

bool fullyHidden(vec4 rect, float nearDepth) {
  const ivec2 base = textureSize(earlyHiZ, 0);
  const ivec2 tile = ivec2(powerOfTwoCeil(scene.screenRes.x), powerOfTwoCeil(scene.screenRes.y)) / base;
  // Visit every touched cell with an outward guard for raster and coordinate rounding.
  const vec4 pixels = (clamp(rect, -2.0, 2.0) * 0.5 + 0.5) * vec2(scene.screenRes).xyxy;
  const vec2 last = vec2(scene.screenRes - 1);
  ivec2 low = ivec2(clamp(pixels.xy - 1.0, vec2(0.0), last)) / tile;
  ivec2 high = ivec2(clamp(pixels.zw + 1.0, vec2(0.0), last)) / tile;
  int mip = 0;
  while(any(greaterThan(high - low, ivec2(7)))) {
    low >>= 1;
    high >>= 1;
    ++mip;
    }
  for(int y = low.y; y <= high.y; ++y)
    for(int x = low.x; x <= high.x; ++x)
      if(!(nearDepth > texelFetch(earlyHiZ, ivec2(x,y), mip).r))
        return false;
  return true;
  }

#endif
