#ifndef VSM_CULL_GLSL
#define VSM_CULL_GLSL

// The tests of a sphere against the shadow pages, the same for a cluster (vsm_visibility_pass.comp) and for a meshlet
// of it (vsm_cull_meshlets.comp). Needs scene.glsl and vsm_common.glsl.

// The sphere in the sun's view: the box it covers in mip 0's [-1..1] and its farthest depth; false past the depth range
bool vsmProjectSphere(const mat4 viewProject, const vec4 sphere, out vec4 aabb, out float depthMax) {
  const vec3  c = (viewProject * vec4(sphere.xyz, 1)).xyz;
  const float R = sphere.w;

  const float smWidthInv = 1.0/(1024.0); // NOTE: from camera.cpp
  const float zScale     = 1.0/(10*5120.0);

  float Rz = R * zScale;
  if(c.z - Rz > 1 || c.z + Rz < 0)
    return false;

  float Rx = R * smWidthInv;
  float Ry = R * smWidthInv;
  aabb     = vec4(c.xy-vec2(Rx,Ry), c.xy+vec2(Rx,Ry));
  depthMax = c.z + Rz;
  return true;
  }

// The box of vsmProjectSphere in the units of the page table of a mip
vec4 vsmPageTableBox(vec4 aabb, uint mip) {
  aabb /= (1u << mip);
  aabb = aabb*0.5+0.5; // [0..1]
  return aabb*VSM_PAGE_TBL_SIZE;
  }

// The box in the units of the page table touches the entry's rect of sz pages at page.xy
bool vsmEntryOverlap(vec4 aabb, const ivec3 page, const ivec2 sz) {
  aabb -= page.xyxy;
  if(aabb.z < 0 || sz.x <= aabb.x)
    return false;
  if(aabb.w < 0 || sz.y <= aabb.y)
    return false;
  return true;
  }

// A face of a lamp's cube in the face's frame (vsmMapDirToFace): the sphere at pos of radius r is inside its frustum
bool vsmFrustumOmniTest(vec3 pos, float r) {
  const vec3  plane[] = {vec3(0,-1,1),  vec3(0,+1,1), vec3(-1,0,1), vec3(+1,0,1)};
  const float scale   = 1.41421356237; //sqrt(2);

  [[unroll]]
  for(int i=0; i<4; ++i) {
    if(dot(pos, plane[i]) < -r*scale)
      return false;
    }
  return true;
  }

// The sphere is in the lamp's range and inside its face
bool vsmOmniFaceTest(const vec4 sphere, const vec3 lightPos, const float range, const uint face) {
  const vec3  dvec = sphere.xyz - lightPos;
  const float r    = sphere.w   + range;
  if(dot(dvec,dvec) > r*r)
    return false;
  return vsmFrustumOmniTest(vsmMapDirToFace(dvec, face), sphere.w);
  }

#endif
