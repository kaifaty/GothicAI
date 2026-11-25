#ifndef TREE_BOUGH_HIZ_GLSL
#define TREE_BOUGH_HIZ_GLSL

const uint BoundsUnknown = 0u, BoundsOutside = 1u, BoundsProjected = 2u;
const float Epsilon = 1.1920928955078125e-7;

mat4 absMatrix(mat4 m) {
  return mat4(abs(m[0]), abs(m[1]), abs(m[2]), abs(m[3]));
  }

bool finiteValues(vec4 v) {
  return all(notEqual(floatBitsToUint(v) & 0x7f800000u, uvec4(0x7f800000u)));
  }

uint projectBounds(mat4 clipTransform, mat4 clipError, vec3 lo, vec3 hi, float nearZ, out vec4 rect, out float nearest) {
  // Bound rounding in the two affine transforms before taking the extrema of clip.xyz / clip.w.
  const vec4 error = clipError * vec4(max(abs(lo), abs(hi)), 1.0) + 32.0 * Epsilon;
  const float negInf = uintBitsToFloat(0xff800000u);
  vec4 sideUpper = vec4(negInf);
  vec2 depthUpper = vec2(negInf);
  bool canProject = true;
  rect = vec4(2.0, 2.0, -2.0, -2.0);
  nearest = 2.0;
  for(uint corner=0u; corner<8u; ++corner) {
    const vec3 pos = mix(lo, hi, notEqual(uvec3(corner) & uvec3(1,2,4), uvec3(0)));
    const vec4 clip = clipTransform * vec4(pos, 1.0);
    const vec4 low = clip - error, high = clip + error;
    if(!finiteValues(low) || !finiteValues(high))
      return BoundsUnknown;
    // All corners outside one Vulkan clip plane prove exclusion even if some w cannot be divided.
    sideUpper = max(sideUpper, high.w + vec4(high.xy, -low.xy));
    depthUpper = max(depthUpper, vec2(high.z, high.w - low.z));
    if(low.w<=nearZ || low.z<=0.0) {
      canProject = false;
      continue;
      }
    const vec3 lower = min(low.xyz / low.w, low.xyz / high.w);
    const vec2 upper = max(high.xy / low.w, high.xy / high.w);
    rect = vec4(min(rect.xy, lower.xy), max(rect.zw, upper));
    nearest = min(nearest, lower.z);
    }
  if(any(lessThan(sideUpper, vec4(0))) || any(lessThan(depthUpper, vec2(0))))
    return BoundsOutside;
  return canProject && finiteValues(rect) ? BoundsProjected : BoundsUnknown;
  }

bool coveredByDepth(sampler2D depthPyramid, ivec2 screenRes, vec4 rect, float nearest) {
  const ivec2 base = textureSize(depthPyramid, 0);
  const ivec2 pot = ivec2(1) << (findMSB(uvec2(screenRes - 1)) + 1);
  if(any(lessThanEqual(base, ivec2(0))) || any(greaterThan(base, pot)))
    return false;

  // The R32F pyramid stores the furthest source depth of each POT tile. Test all base-level
  // cells touching the projected box, including a pixel of slack for rasterization at the boundary.
  const ivec2 tile = pot / base;
  const vec4 pixels = (clamp(rect, -1.0, 1.0) * 0.5 + 0.5) * vec2(screenRes).xyxy;
  const vec2 last = vec2(screenRes - 1);
  const ivec2 lo = ivec2(clamp(pixels.xy - 1.0, vec2(0), last)) / tile;
  const ivec2 hi = ivec2(clamp(pixels.zw + 1.0, vec2(0), last)) / tile;
  // One D16 step also covers D24 and D32F quantization of the depth attachment.
  const float guarded = min(nearest, 1.0) - 1.0/65535.0;
  for(int y=lo.y; y<=hi.y; ++y) {
    for(int x=lo.x; x<=hi.x; ++x) {
      const float depth = texelFetch(depthPyramid, ivec2(x,y), 0).r;
      if(!(depth>0.0 && guarded>depth))
        return false; // Includes missing or clear (depth 1) samples.
      }
    }
  return true;
  }

bool boughBoundsValid(float windPad, float animPtr, vec3 x, vec3 z) {
  // The loaded wind arms are bounded for rotation and scale. A sheared horizontal basis can amplify them.
  return windPad>=0.0 && finiteValues(vec4(windPad)) && animPtr==0.0 &&
         abs(dot(x,z))<=8.0*Epsilon*length(x)*length(z);
  }

#endif
