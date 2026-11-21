// Local cull boxes enclose the animated source and proxy geometry. Test internal nodes so a
// rejected box removes a whole range of the preorder array with no per-card depth lookup.
mat4 boughClip, boughClipError;
float boughPad;
bool boughCanCull;

mat4 boughAbs(mat4 m) {
  return mat4(abs(m[0]), abs(m[1]), abs(m[2]), abs(m[3]));
  }

void boughCullTransform(mat4 object, float pad, float fatness) {
  // Wind arms are bounded for rotation and scale. A sheared horizontal basis can amplify them.
  const vec3 x = object[0].xyz, z = object[2].xyz;
  boughCanCull = fatness==0.0 && abs(dot(x,z))<=8.0*FoliageCullEpsilon*length(x)*length(z);
  boughPad = pad;
  boughClip = scene.viewProject*object;
  // Bound rounding in both affine products, including cancellation at large world coordinates.
  boughClipError = (boughAbs(scene.viewProject)*boughAbs(object))*(32.0*FoliageCullEpsilon);
  }

bool boughHidden(BoughNode node) {
  if(!boughCanCull)
    return false;
  const vec3 lo = node.boxMin - boughPad, hi = node.boxMax + boughPad;
  const vec4 error = boughClipError*vec4(max(abs(lo),abs(hi)), 1.0) + 32.0*FoliageCullEpsilon;
  vec4 rect;
  float nearest;
  if(!foliageProjectBox(boughClip, lo, hi, error, push.nearZ, rect, nearest))
    return false;
  if(foliageOutsideView(rect, nearest))
    return true;
  // A full D16 depth step also covers D24 and D32F rounding in either current-frame pyramid.
  return (push.flags & 16u)!=0u && !foliageHiZVisible(rect, nearest - 1.0/65535.0, (push.flags & 8u)!=0u);
  }
