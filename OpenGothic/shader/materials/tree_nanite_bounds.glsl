#ifndef TREE_NANITE_BOUNDS_GLSL
#define TREE_NANITE_BOUNDS_GLSL

const float Epsilon = 1.1920928955078125e-7;

struct ClipBox {
  mat4 project;
  vec4 roundoff;
  vec3 low, high;
  };

bool finiteValues(vec4 value) {
  // Exponent test also works on backends compiled with finite math assumptions.
  return all(notEqual(floatBitsToUint(value) & 0x7f800000u, uvec4(0x7f800000u)));
  }

ClipBox clipBox(vec3 lower, vec3 upper, mat4x3 object) {
  const vec3 magnitude = max(abs(lower), abs(upper));
  const vec3 padding = (magnitude + 1.0) * (64.0 * Epsilon);
  const vec3 low = lower - padding;
  const vec3 high = upper + padding;
  const vec3 bound = max(abs(low), abs(high));
  const vec3 world = abs(object[0]) * bound.x + abs(object[1]) * bound.y +
                     abs(object[2]) * bound.z + abs(object[3]);
  const mat4 camera = scene.viewProject;
  // Budget separately rounded vertex and corner transforms, including the
  // precomposition below. The magnitude uses the uncombined terms to cover cancellation.
  const vec4 roundoff = (abs(camera[0]) * world.x + abs(camera[1]) * world.y +
                        abs(camera[2]) * world.z + abs(camera[3]) + 1.0) * (64.0 * Epsilon);
  const mat4 transform = mat4(vec4(object[0], 0.0), vec4(object[1], 0.0),
                              vec4(object[2], 0.0), vec4(object[3], 1.0));
  return ClipBox(camera * transform, roundoff, low, high);
  }

bool outsidePlane(ClipBox box, vec4 plane) {
  const vec4 local = transpose(box.project) * plane;
  const vec3 corner = mix(box.low, box.high, greaterThanEqual(local.xyz, vec3(0.0)));
  const float support = dot(local, vec4(corner, 1.0)) + dot(abs(plane), box.roundoff);
  return finiteValues(vec4(support, 0.0, 0.0, 0.0)) && support < 0.0;
  }

bool inFrustum(ClipBox box) {
  // Vulkan clip depth is [0,w]. Boxes that straddle a plane still reach the raster.
  return !outsidePlane(box, vec4( 1, 0, 0, 1)) && !outsidePlane(box, vec4(-1, 0, 0, 1)) &&
         !outsidePlane(box, vec4( 0, 1, 0, 1)) && !outsidePlane(box, vec4( 0,-1, 0, 1)) &&
         !outsidePlane(box, vec4( 0, 0, 1, 0)) && !outsidePlane(box, vec4( 0, 0,-1, 1));
  }

#endif
