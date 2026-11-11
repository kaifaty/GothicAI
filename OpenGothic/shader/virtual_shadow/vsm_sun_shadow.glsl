#ifndef VSM_SUN_SHADOW_GLSL
#define VSM_SUN_SHADOW_GLSL

#extension GL_EXT_control_flow_attributes : enable

#include "virtual_shadow/vsm_common.glsl"

// The sun from the virtual shadow map, for the deferred sun (direct_light.frag) and the forward pass (water,
// translucent meshes, particles: materials/main.frag) alike.
// The page and mip as vsm_mark_pages.comp picks them; a point no page was marked for (forward geometry marks none,
// or the pool overflowed) takes the next coarser mip that has one, up to four steps. Then a 4x4 texel tent
// (a 3-texel box over bilinear weights, as the cascades' four gathers) in virtual texels, each texel through the
// page table. The receiver's depth at every texel centre comes from its plane, so the filter does not shadow
// the surface by itself on slopes; a texel whose page is missing takes the centre's answer.

int vsmSunLod(vec2 dx, vec2 dy, float bias) {
  float px     = dot(dx, dx);
  float py     = dot(dy, dy);
  float minLod = 0.5 * log2(min(px, py));
  return max(0, int(minLod + bias));
  }

// texel of the virtual clipmap 'mip' (0..VSM_CLIPMAP_SIZE): stored depth, or -1 where no page holds it
float vsmSunTexel(ivec2 tx, int mip, in utexture3D pageTbl, in texture2D pageData) {
  if(any(lessThan(tx, ivec2(0))) || any(greaterThanEqual(tx, ivec2(VSM_CLIPMAP_SIZE))))
    return -1;
  const uint pageD = texelFetch(pageTbl, ivec3(tx/VSM_PAGE_SIZE, mip), 0).x;
  if(pageD==0)
    return -1;
  const ivec2 at = unpackVsmPageId(pageD >> 16u)*VSM_PAGE_SIZE + (tx%VSM_PAGE_SIZE);
  return texelFetch(pageData, at, 0).x;
  }

vec2 vsmSunTexelPos(vec2 shPos, int mip) {
  return (shPos/float(1 << mip)*0.5 + 0.5)*float(VSM_CLIPMAP_SIZE) - 0.5;
  }

// shPos0: the receiver in the shadow space of shadowMat; shPosDx, shPosDy: its neighbours one pixel aside;
// planeN: the receiver plane's normal in shadowMat's world space. 1 lit, 0 in shadow.
float vsmSunShadow(const vec3 shPos0, const vec2 shPosDx, const vec2 shPosDy, const mat4 shadowMat, const vec3 planeN,
                   const float mipBias, in utexture3D pageTbl, in texture2D pageData) {
  int mip = vsmSunLod((shPosDx - shPos0.xy)*VSM_CLIPMAP_SIZE, (shPosDy - shPos0.xy)*VSM_CLIPMAP_SIZE, mipBias);
  mip = vsmCalcMipIndex(shPos0.xy, mip);

  float zc = -1;
  [[unroll]]
  for(int i=0; i<5; ++i) {
    if(mip>=VSM_PAGE_MIPS)
      return 1.0;
    zc = vsmSunTexel(ivec2(floor(vsmSunTexelPos(shPos0.xy, mip) + 0.5)), mip, pageTbl, pageData);
    if(zc>=0)
      break;
    mip++;
    }
  if(zc<0)
    return 1.0;

  // receiver plane in shadow space: depth change per texel of this mip
  const vec3  r0 = vec3(shadowMat[0][0], shadowMat[1][0], shadowMat[2][0]);
  const vec3  r1 = vec3(shadowMat[0][1], shadowMat[1][1], shadowMat[2][1]);
  const vec3  r2 = vec3(shadowMat[0][2], shadowMat[1][2], shadowMat[2][2]);
  const vec3  pm = vec3(dot(r0,planeN)/dot(r0,r0), dot(r1,planeN)/dot(r1,r1), dot(r2,planeN)/dot(r2,r2));
  const float mz = max(pm.z, 0.1*length(pm)); // steeper than ~84 degrees counts as 84
  const vec2  dz = -(pm.xy/mz) * (2.0*float(1 << mip)/float(VSM_CLIPMAP_SIZE));
  const float bias = 2.0/65535.0;

  const vec2  t    = vsmSunTexelPos(shPos0.xy, mip);
  const ivec2 base = ivec2(floor(t));
  const vec2  f    = t - vec2(base);
  const float refZ = shPos0.z + bias;

  // centre texel: the answer for the taps without a page
  const ivec2 near = ivec2(floor(t + 0.5));
  const float lc   = (zc==0 || zc < refZ + dot(dz, vec2(near) - t)) ? 1.0 : 0.0;

  const vec4 wx = vec4(1.0-f.x, 1.0, 1.0, f.x);
  const vec4 wy = vec4(1.0-f.y, 1.0, 1.0, f.y);

  float sum = 0;
  [[unroll]]
  for(int y=0; y<4; ++y) {
    [[unroll]]
    for(int x=0; x<4; ++x) {
      const ivec2 tx = base + ivec2(x-1, y-1);
      const float z  = vsmSunTexel(tx, mip, pageTbl, pageData);
      float lit = lc;
      if(z>=0)
        lit = (z==0 || z < refZ + dot(dz, vec2(tx) - t)) ? 1.0 : 0.0;
      sum += lit*wx[x]*wy[y];
      }
    }
  return sum/9.0;
  }

#endif
